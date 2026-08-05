#ifndef PERF_MULTI_HANDSHAKE_HPP
#define PERF_MULTI_HANDSHAKE_HPP

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

namespace perf_multi_handshake
{

struct start_signal_state_t
{
    start_signal_state_t () : pending_sizes (), stopped (false), mutex (), cv () {}

    std::set<size_t> pending_sizes;
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
                                           size_t *count_out)
{
    if (!prefix || !*prefix || !size_out || !count_out)
        return false;

    const size_t prefix_len = std::strlen (prefix);
    if (line.compare (0, prefix_len, prefix) != 0)
        return false;

    const size_t comma = line.find (',', prefix_len);
    if (comma == std::string::npos)
        return false;

    char *end = NULL;
    const unsigned long long parsed_size = std::strtoull (line.c_str () + prefix_len, &end, 10);
    if (!end || static_cast<size_t> (end - line.c_str ()) != comma || parsed_size == 0) {
        return false;
    }

    const unsigned long long parsed_count = std::strtoull (line.c_str () + comma + 1, &end, 10);
    if (!end || *end != '\0' || parsed_count == 0)
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

inline void signal_start (start_signal_state_t *state, size_t msg_size)
{
    if (!state || msg_size == 0)
        return;

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->pending_sizes.insert (msg_size);
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

inline bool wait_for_start (start_signal_state_t *state, size_t msg_size, int timeout_ms)
{
    if (!state || msg_size == 0) {
        errno = EINVAL;
        return false;
    }

    std::unique_lock<std::mutex> lock (state->mutex);
    std::set<size_t>::iterator ready = state->pending_sizes.find (msg_size);
    if (ready != state->pending_sizes.end ()) {
        state->pending_sizes.erase (ready);
        return true;
    }
    if (state->stopped) {
        errno = ECANCELED;
        return false;
    }

    const bool signaled = state->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms > 0 ? timeout_ms : 1), [state, msg_size] () {
          return state->stopped
                 || state->pending_sizes.find (msg_size) != state->pending_sizes.end ();
      });
    ready = state->pending_sizes.find (msg_size);
    if (!signaled || ready == state->pending_sizes.end ()) {
        errno = state->stopped ? ECANCELED : ETIMEDOUT;
        return false;
    }

    state->pending_sizes.erase (ready);
    return true;
}

inline bool wait_for_start_from_stdin (size_t msg_size)
{
    std::string line;
    while (std::getline (std::cin, line)) {
        if (line == "STOP" || line == "QUIT") {
            errno = ECANCELED;
            return false;
        }

        size_t start_size = 0;
        if (!parse_size_command_line (line, "START,", &start_size))
            continue;
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
