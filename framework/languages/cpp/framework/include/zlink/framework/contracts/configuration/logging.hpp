/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

namespace zlink::framework
{

namespace detail
{
class logging_state_t;
} // namespace detail

enum class log_level_t
{
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
    off = 6
};

enum class logging_backend_t
{
    builtin = 0,
    structured = 1
};

enum class logging_overflow_policy_t
{
    drop_debug = 0,
    drop_oldest = 1,
    block = 2
};

struct log_field_t
{
    std::string key;
    std::string value;
};

struct log_record_t
{
    log_level_t level = log_level_t::info;
    std::string category;
    std::string message;
    std::vector<log_field_t> fields;
    std::chrono::system_clock::time_point timestamp;
    std::thread::id thread_id;
};

struct logging_async_options_t
{
    std::size_t queue_capacity = 8192;
    logging_overflow_policy_t overflow_policy = logging_overflow_policy_t::drop_debug;
};

struct rotating_file_options_t
{
    std::size_t max_file_size = 10 * 1024 * 1024;
    std::size_t max_files = 5;
};

namespace detail
{
bool is_log_enabled (const std::shared_ptr<logging_state_t> &state, int level) noexcept;

void emit_log (const std::shared_ptr<logging_state_t> &state,
               int level,
               const std::string &category,
               const std::string &message,
               std::vector<log_field_t> fields);
} // namespace detail

class logger_factory_t;

template <typename TCategory = void> class logger_t
{
  public:
    logger_t () = default;

    bool is_enabled (log_level_t level) const noexcept
    {
        return detail::is_log_enabled (_state, static_cast<int> (level));
    }

    void log (log_level_t level,
              std::string message,
              std::initializer_list<log_field_t> fields = {}) const
    {
        detail::emit_log (_state, static_cast<int> (level), _category, std::move (message),
                          std::vector<log_field_t> (fields));
    }

    // Structured form for dynamic field sets (e.g. message-flow tracing). Lets sinks
    // / collectors ingest key/value fields instead of parsing a formatted line.
    void
    log_with_fields (log_level_t level, std::string message, std::vector<log_field_t> fields) const
    {
        detail::emit_log (_state, static_cast<int> (level), _category, std::move (message),
                          std::move (fields));
    }

    void trace (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::trace, std::move (message), fields);
    }

    void debug (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::debug, std::move (message), fields);
    }

    void info (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::info, std::move (message), fields);
    }

    void warn (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::warn, std::move (message), fields);
    }

    void error (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::error, std::move (message), fields);
    }

    void critical (std::string message, std::initializer_list<log_field_t> fields = {}) const
    {
        log (log_level_t::critical, std::move (message), fields);
    }

    const std::string &category () const noexcept { return _category; }

  private:
    friend class logger_factory_t;

    logger_t (std::shared_ptr<detail::logging_state_t> state, std::string category) :
        _state (std::move (state)), _category (std::move (category))
    {
    }

    std::shared_ptr<detail::logging_state_t> _state;
    std::string _category = typeid (TCategory).name ();
};

class logger_factory_t
{
  public:
    logger_factory_t ();
    explicit logger_factory_t (std::shared_ptr<detail::logging_state_t> state);

    logger_t<> create (std::string category) const;

    template <typename TCategory> logger_t<TCategory> create () const
    {
        return logger_t<TCategory> (_state, typeid (TCategory).name ());
    }

  private:
    std::shared_ptr<detail::logging_state_t> _state;
};

class logging_builder_t
{
  public:
    using sink_t = std::function<void (const log_record_t &)>;

    logging_builder_t ();
    ~logging_builder_t ();

    logging_builder_t (logging_builder_t &&) noexcept;
    logging_builder_t &operator= (logging_builder_t &&) noexcept;
    logging_builder_t (const logging_builder_t &) = delete;
    logging_builder_t &operator= (const logging_builder_t &) = delete;

    logging_builder_t &use_console ();
    logging_builder_t &use_file (std::string path);
    logging_builder_t &use_rotating_file (std::string path, rotating_file_options_t options = {});
    logging_builder_t &use_callback_sink (sink_t sink);
    // Register an application logging backend ("provider"), .NET/SLF4J style: the
    // framework keeps its own logger facade and forwards every record (with
    // structured fields) to this provider. With no use_console()/use_file() the
    // framework emits nothing itself, so logs flow only to the app's backend.
    // `name` is informational (introspection/diagnostics).
    logging_builder_t &use_provider (std::string name, sink_t sink);
    logging_builder_t &use_async (logging_async_options_t options = {});
    logging_builder_t &use_backend (logging_backend_t backend);

    // In-memory record buffer control (the buffer is a test/inspection aid). It is
    // bounded by default; disable it in production when a sink/provider is used.
    logging_builder_t &disable_record_capture ();
    logging_builder_t &set_max_captured_records (std::size_t max);

    logging_builder_t &set_min_level (log_level_t level);
    logging_builder_t &set_level (std::string level);

    bool console_enabled () const noexcept;
    // True if any output sink is configured (console, file, callback, or provider) —
    // i.e. logs would actually go somewhere.
    bool has_output_sink () const noexcept;
    bool async_enabled () const noexcept;
    logging_backend_t backend () const noexcept;
    log_level_t min_level () const noexcept;
    const std::string &level () const noexcept;
    const std::vector<std::string> &file_paths () const noexcept;
    const std::vector<std::string> &provider_names () const noexcept;
    const std::vector<log_record_t> &captured_records () const noexcept;

    logger_factory_t factory () const;
    logger_t<> create_logger (std::string category) const;

  private:
    std::shared_ptr<detail::logging_state_t> _state;
};

} // namespace zlink::framework
