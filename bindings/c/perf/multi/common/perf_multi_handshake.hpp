#ifndef PERF_MULTI_HANDSHAKE_HPP
#define PERF_MULTI_HANDSHAKE_HPP

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <map>
#include <limits>
#include "perf_multi_metric_header.hpp"
#include <string>

namespace perf_multi_handshake
{

struct start_signal_state_t
{
    start_signal_state_t () : pending_sizes (), stopped (false), mutex (), cv () {}

    typedef std::pair<size_t, perf_multi_metric::phase_t> key_t;
    struct signal_t
    {
        std::chrono::steady_clock::time_point received_at;
        size_t count;
    };
    typedef std::map<key_t, signal_t> signals_t;
    signals_t pending_sizes;
    bool stopped;
    std::mutex mutex;
    std::condition_variable cv;
};

inline bool parse_size_command_line (const std::string &line, const char *prefix, size_t *value_out)
{
    if (!prefix || !*prefix || !value_out)
        return false;

    const size_t prefix_len = std::strlen (prefix);
    if (line.compare (0, prefix_len, prefix) != 0)
        return false;

    const char *value = line.c_str () + prefix_len;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (!end || *end != '\0' || parsed == 0)
        return false;

    *value_out = static_cast<size_t> (parsed);
    return true;
}

inline bool
parse_endpoint_command_line (const std::string &line, const char *prefix, std::string *endpoint_out)
{
    if (!prefix || !*prefix || !endpoint_out)
        return false;

    const size_t prefix_len = std::strlen (prefix);
    if (line.compare (0, prefix_len, prefix) != 0)
        return false;

    *endpoint_out = line.substr (prefix_len);
    return !endpoint_out->empty ();
}

inline bool parse_size_count_command_line (const std::string &line,
                                           const char *prefix,
                                           size_t *size_out,
                                           size_t *count_out,
                                           bool allow_zero_count = false)
{
    if (!prefix || !*prefix || !size_out || !count_out)
        return false;

    const size_t prefix_len = std::strlen (prefix);
    if (line.compare (0, prefix_len, prefix) != 0)
        return false;

    const size_t comma = line.find (',', prefix_len);
    if (comma == std::string::npos)
        return false;

    if (comma == prefix_len || comma + 1 == line.size ())
        return false;
    for (size_t i = prefix_len; i < line.size (); ++i) {
        if (i != comma && (line[i] < '0' || line[i] > '9'))
            return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed_size = std::strtoull (line.c_str () + prefix_len, &end, 10);
    if (!end || static_cast<size_t> (end - line.c_str ()) != comma || parsed_size == 0
        || errno == ERANGE || parsed_size > std::numeric_limits<size_t>::max ()) {
        return false;
    }

    const unsigned long long parsed_count = std::strtoull (line.c_str () + comma + 1, &end, 10);
    if (!end || *end != '\0' || (!allow_zero_count && parsed_count == 0) || errno == ERANGE
        || parsed_count > std::numeric_limits<size_t>::max ())
        return false;

    *size_out = static_cast<size_t> (parsed_size);
    *count_out = static_cast<size_t> (parsed_count);
    return true;
}

inline bool parse_size_count_command_bytes (
  const void *data, size_t size, const char *prefix, size_t *size_out, size_t *count_out)
{
    if (!data)
        return false;
    return parse_size_count_command_line (std::string (static_cast<const char *> (data), size),
                                          prefix, size_out, count_out);
}

inline void reset_start_signal_state (start_signal_state_t *state)
{
    if (!state)
        return;

    std::lock_guard<std::mutex> lock (state->mutex);
    state->pending_sizes.clear ();
    state->stopped = false;
}

inline void signal_start (start_signal_state_t *state,
                          size_t msg_size,
                          perf_multi_metric::phase_t phase = perf_multi_metric::phase_unknown,
                          size_t count = 0)
{
    if (!state || msg_size == 0)
        return;

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->pending_sizes.insert (std::make_pair (
          start_signal_state_t::key_t (msg_size, phase),
          start_signal_state_t::signal_t{std::chrono::steady_clock::now (), count}));
    }
    state->cv.notify_all ();
}

inline void signal_stop (start_signal_state_t *state)
{
    if (!state)
        return;

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->stopped = true;
    }
    state->cv.notify_all ();
}

inline bool wait_for_start (start_signal_state_t *state,
                            size_t msg_size,
                            int timeout_ms,
                            perf_multi_metric::phase_t phase = perf_multi_metric::phase_unknown,
                            size_t *count_out = NULL)
{
    if (!state || msg_size == 0) {
        errno = EINVAL;
        return false;
    }

    std::unique_lock<std::mutex> lock (state->mutex);
    const start_signal_state_t::key_t key (msg_size, phase);
    start_signal_state_t::signals_t::iterator ready = state->pending_sizes.find (key);
    if (ready != state->pending_sizes.end ()) {
        if (count_out)
            *count_out = ready->second.count;
        state->pending_sizes.erase (ready);
        return true;
    }
    if (state->stopped) {
        errno = ECANCELED;
        return false;
    }

    const bool signaled = state->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms > 0 ? timeout_ms : 1), [state, key] () {
          return state->stopped || state->pending_sizes.find (key) != state->pending_sizes.end ();
      });
    ready = state->pending_sizes.find (key);
    if (!signaled || ready == state->pending_sizes.end ()) {
        errno = state->stopped ? ECANCELED : ETIMEDOUT;
        return false;
    }

    if (count_out)
        *count_out = ready->second.count;
    state->pending_sizes.erase (ready);
    return true;
}

inline bool query_start (start_signal_state_t *state,
                         size_t msg_size,
                         perf_multi_metric::phase_t phase,
                         std::chrono::steady_clock::time_point *received_at)
{
    if (!state || !received_at)
        return false;
    std::lock_guard<std::mutex> lock (state->mutex);
    const start_signal_state_t::signals_t::const_iterator found =
      state->pending_sizes.find (start_signal_state_t::key_t (msg_size, phase));
    if (found == state->pending_sizes.end ())
        return false;
    *received_at = found->second.received_at;
    return true;
}

inline bool is_phase_end_marker (const perf_multi_metric::header_t &header,
                                 uint32_t run_id,
                                 perf_multi_metric::phase_t phase,
                                 size_t msg_size)
{
    return (phase == perf_multi_metric::phase_active || phase == perf_multi_metric::phase_latency)
           && perf_multi_metric::is_expected (header, run_id, phase, msg_size) && header.seq == 0
           && header.sent_ts_ns == 0;
}

inline bool
parse_phase_done_line (const std::string &line, size_t *msg_size, perf_multi_metric::phase_t *phase)
{
    size_t phase_value = 0;
    if (!phase || !parse_size_count_command_line (line, "PHASE_DONE,", msg_size, &phase_value)
        || (phase_value != perf_multi_metric::phase_active
            && phase_value != perf_multi_metric::phase_latency))
        return false;
    *phase = static_cast<perf_multi_metric::phase_t> (phase_value);
    return true;
}

inline bool
wait_for_start_from_stdin (size_t msg_size, const char *prefix = "START,", size_t *count_out = NULL)
{
    std::string line;
    while (std::getline (std::cin, line)) {
        if (line == "STOP" || line == "QUIT") {
            errno = ECANCELED;
            return false;
        }

        size_t start_size = 0;
        const bool valid =
          count_out ? parse_size_count_command_line (line, prefix, &start_size, count_out,
                                                     std::strcmp (prefix, "PHASE_COMPLETE,") == 0)
                    : parse_size_command_line (line, prefix, &start_size);
        if (!valid) {
            if (count_out && line.compare (0, std::strlen (prefix), prefix) == 0) {
                errno = EPROTO;
                return false;
            }
            continue;
        }
        if (start_size == msg_size)
            return true;
    }

    errno = ECANCELED;
    return false;
}

inline std::string
make_size_count_command (const char *prefix, size_t size_value, size_t count_value)
{
    return std::string (prefix) + std::to_string (size_value) + "," + std::to_string (count_value);
}

inline std::string make_size_command (const char *prefix, size_t size_value)
{
    return std::string (prefix) + std::to_string (size_value);
}

} // namespace perf_multi_handshake

#endif
