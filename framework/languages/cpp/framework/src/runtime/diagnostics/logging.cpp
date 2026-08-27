/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/configuration/logging.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace zlink::framework::detail
{

class logging_state_t
{
  public:
    bool console_enabled = false;
    bool async_enabled = false;
    logging_backend_t backend = logging_backend_t::builtin;
    log_level_t min_level = log_level_t::info;
    std::string level_name = "info";
    logging_async_options_t async_options{};
    std::vector<std::string> file_paths;
    std::vector<rotating_file_options_t> rotating_options;
    std::vector<logging_builder_t::sink_t> callback_sinks;
    std::vector<std::string> provider_names;
    std::vector<log_record_t> captured_records;
    // The in-memory record buffer is a test/inspection aid; bounded by default so it
    // never grows without limit in production. capture_records=false disables it.
    bool capture_records = true;
    std::size_t max_captured_records = 4096;
    runtime::offload_executor_t lane_executor;
    mutable runtime::state_lane_t lane{lane_executor};
};

const char *log_level_name (log_level_t level) noexcept
{
    switch (level) {
        case log_level_t::trace:
            return "trace";
        case log_level_t::debug:
            return "debug";
        case log_level_t::info:
            return "info";
        case log_level_t::warn:
            return "warn";
        case log_level_t::error:
            return "error";
        case log_level_t::critical:
            return "critical";
        case log_level_t::off:
            return "off";
    }
    return "info";
}

log_level_t parse_log_level (std::string level)
{
    std::transform (level.begin (), level.end (), level.begin (), [] (char ch) {
        return static_cast<char> (std::tolower (static_cast<unsigned char> (ch)));
    });
    if (level == "trace") {
        return log_level_t::trace;
    }
    if (level == "debug") {
        return log_level_t::debug;
    }
    if (level == "info") {
        return log_level_t::info;
    }
    if (level == "warn" || level == "warning") {
        return log_level_t::warn;
    }
    if (level == "error") {
        return log_level_t::error;
    }
    if (level == "critical") {
        return log_level_t::critical;
    }
    if (level == "off") {
        return log_level_t::off;
    }
    return log_level_t::info;
}

std::string format_record (const log_record_t &record)
{
    const auto time = std::chrono::system_clock::to_time_t (record.timestamp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s (&tm, &time);
#else
    localtime_r (&time, &tm);
#endif
    std::ostringstream output;
    output << std::put_time (&tm, "%Y-%m-%dT%H:%M:%S") << ' ' << log_level_name (record.level)
           << ' ' << record.category << " - " << record.message;
    for (const auto &field : record.fields) {
        output << ' ' << field.key << '=' << field.value;
    }
    return output.str ();
}

void ensure_parent_directory (const std::string &path) noexcept
{
    std::error_code ignored;
    const auto parent = std::filesystem::path (path).parent_path ();
    if (!parent.empty ()) {
        std::filesystem::create_directories (parent, ignored);
    }
}

void rotate_if_needed (const std::string &path, const rotating_file_options_t &options)
{
    if (options.max_file_size == 0 || options.max_files == 0 || !std::filesystem::exists (path)
        || std::filesystem::file_size (path) < options.max_file_size) {
        return;
    }

    for (std::size_t index = options.max_files; index > 0; --index) {
        const auto from = index == 1
                            ? std::filesystem::path (path)
                            : std::filesystem::path (path + "." + std::to_string (index - 1));
        const auto to = std::filesystem::path (path + "." + std::to_string (index));
        if (std::filesystem::exists (from)) {
            std::error_code ignored;
            std::filesystem::rename (from, to, ignored);
        }
    }
}

bool is_log_enabled (const std::shared_ptr<logging_state_t> &state, int level) noexcept
{
    if (!state) {
        return false;
    }
    return state->lane.run ([&] {
        return state->min_level != log_level_t::off
               && level >= static_cast<int> (state->min_level);
    }).get ();
}

void emit_log (const std::shared_ptr<logging_state_t> &state,
               int level,
               const std::string &category,
               const std::string &message,
               std::vector<log_field_t> fields)
{
    if (!is_log_enabled (state, level)) {
        return;
    }

    log_record_t record{static_cast<log_level_t> (level),
                        category,
                        message,
                        std::move (fields),
                        std::chrono::system_clock::now (),
                        std::this_thread::get_id ()};

    std::vector<logging_builder_t::sink_t> callbacks;
    std::vector<std::string> files;
    std::vector<rotating_file_options_t> rotations;
    bool console = false;
    state->lane.run ([&] {
        if (state->capture_records) {
            // Bounded ring with amortized-O(1) bulk trim: keep the most recent half
            // when the cap is reached so steady-state logging does not pay O(n) per
            // record. Production typically disables capture entirely.
            if (state->max_captured_records > 0
                && state->captured_records.size () >= state->max_captured_records) {
                const auto keep = state->max_captured_records / 2;
                state->captured_records.erase (state->captured_records.begin (),
                                               state->captured_records.end ()
                                                 - static_cast<std::ptrdiff_t> (keep));
            }
            state->captured_records.push_back (record);
        }
        callbacks = state->callback_sinks;
        files = state->file_paths;
        rotations = state->rotating_options;
        console = state->console_enabled;
    }).get ();

    const auto line = format_record (record);
    if (console) {
        std::clog << line << '\n';
    }
    for (std::size_t i = 0; i < files.size (); ++i) {
        if (i < rotations.size ()) {
            rotate_if_needed (files[i], rotations[i]);
        }
        std::ofstream output (files[i], std::ios::app);
        output << line << '\n';
    }
    for (const auto &callback : callbacks) {
        try {
            callback (record);
        }
        catch (...) {
            // A logging provider is an observation boundary.  Its failure
            // must not escape into the dispatch or lifecycle owner.
        }
    }
}

} // namespace zlink::framework::detail

namespace zlink::framework
{

logger_factory_t::logger_factory_t () : _state (std::make_shared<detail::logging_state_t> ())
{
}

logger_factory_t::logger_factory_t (std::shared_ptr<detail::logging_state_t> state) :
    _state (std::move (state))
{
}

logger_t<> logger_factory_t::create (std::string category) const
{
    return logger_t<> (_state, std::move (category));
}

logging_builder_t::logging_builder_t () : _state (std::make_shared<detail::logging_state_t> ())
{
}

logging_builder_t::~logging_builder_t () = default;
logging_builder_t::logging_builder_t (logging_builder_t &&) noexcept = default;
logging_builder_t &logging_builder_t::operator= (logging_builder_t &&) noexcept = default;

logging_builder_t &logging_builder_t::use_console ()
{
    _state->lane.run ([&] {
    _state->console_enabled = true;
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_file (std::string path)
{
    _state->lane.run ([&] {
    detail::ensure_parent_directory (path);
    _state->file_paths.push_back (std::move (path));
    _state->rotating_options.push_back ({});
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_rotating_file (std::string path,
                                                         rotating_file_options_t options)
{
    _state->lane.run ([&] {
    detail::ensure_parent_directory (path);
    _state->file_paths.push_back (std::move (path));
    _state->rotating_options.push_back (options);
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_callback_sink (sink_t sink)
{
    _state->lane.run ([&] {
    _state->callback_sinks.push_back (std::move (sink));
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_provider (std::string name, sink_t sink)
{
    _state->lane.run ([&] {
    _state->provider_names.push_back (std::move (name));
    _state->callback_sinks.push_back (std::move (sink));
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::disable_record_capture ()
{
    _state->lane.run ([&] {
    _state->capture_records = false;
    _state->captured_records.clear ();
    _state->captured_records.shrink_to_fit ();
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::set_max_captured_records (std::size_t max)
{
    _state->lane.run ([&] {
    _state->max_captured_records = max;
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_async (logging_async_options_t options)
{
    _state->lane.run ([&] {
    _state->async_enabled = true;
    _state->async_options = options;
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::use_backend (logging_backend_t backend)
{
    _state->lane.run ([&] {
    _state->backend = backend;
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::set_min_level (log_level_t level)
{
    _state->lane.run ([&] {
    _state->min_level = level;
    _state->level_name = detail::log_level_name (level);
    }).get ();
    return *this;
}

logging_builder_t &logging_builder_t::set_level (std::string level)
{
    return set_min_level (detail::parse_log_level (std::move (level)));
}

bool logging_builder_t::console_enabled () const noexcept
{
    return _state->lane.run ([&] {
        return _state->console_enabled;
    }).get ();
}

bool logging_builder_t::has_output_sink () const noexcept
{
    return _state->lane.run ([&] {
        return _state->console_enabled || !_state->file_paths.empty ()
               || !_state->callback_sinks.empty ();
    }).get ();
}

bool logging_builder_t::async_enabled () const noexcept
{
    return _state->lane.run ([&] {
        return _state->async_enabled;
    }).get ();
}

logging_backend_t logging_builder_t::backend () const noexcept
{
    return _state->lane.run ([&] {
        return _state->backend;
    }).get ();
}

log_level_t logging_builder_t::min_level () const noexcept
{
    return _state->lane.run ([&] {
        return _state->min_level;
    }).get ();
}

const std::string &logging_builder_t::level () const noexcept
{
    return _state->lane.run ([&] -> const std::string & {
        return _state->level_name;
    }).get ();
}

const std::vector<std::string> &logging_builder_t::file_paths () const noexcept
{
    return _state->lane.run ([&] -> const std::vector<std::string> & {
        return _state->file_paths;
    }).get ();
}

const std::vector<std::string> &logging_builder_t::provider_names () const noexcept
{
    return _state->lane.run ([&] -> const std::vector<std::string> & {
        return _state->provider_names;
    }).get ();
}

const std::vector<log_record_t> &logging_builder_t::captured_records () const noexcept
{
    return _state->lane.run ([&] -> const std::vector<log_record_t> & {
        return _state->captured_records;
    }).get ();
}

logger_factory_t logging_builder_t::factory () const
{
    return logger_factory_t (_state);
}

logger_t<> logging_builder_t::create_logger (std::string category) const
{
    return factory ().create (std::move (category));
}

} // namespace zlink::framework
