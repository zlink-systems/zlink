#ifndef PERF_HANDSHAKE_HPP
#define PERF_HANDSHAKE_HPP

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

namespace perf
{
namespace multi
{

struct start_signal_state_t
{
    start_signal_state_t () : pending_sizes (), stopped (false), mutex (), cv () {}

    std::set<size_t> pending_sizes;
    bool stopped;
    std::mutex mutex;
    std::condition_variable cv;
};

inline bool
parse_size_command_line (const std::string &line_, const char *prefix_, size_t *value_out_)
{
    if (!prefix_ || !*prefix_ || !value_out_)
        return false;

    const size_t prefix_len = std::strlen (prefix_);
    if (line_.compare (0, prefix_len, prefix_) != 0)
        return false;

    const char *value = line_.c_str () + prefix_len;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (!end || *end != '\0' || parsed == 0)
        return false;

    *value_out_ = static_cast<size_t> (parsed);
    return true;
}

inline bool parse_endpoint_command_line (const std::string &line_,
                                         const char *prefix_,
                                         std::string *endpoint_out_)
{
    if (!prefix_ || !*prefix_ || !endpoint_out_)
        return false;

    const size_t prefix_len = std::strlen (prefix_);
    if (line_.compare (0, prefix_len, prefix_) != 0)
        return false;

    *endpoint_out_ = line_.substr (prefix_len);
    return !endpoint_out_->empty ();
}

inline bool parse_size_endpoint_command_line (const std::string &line_,
                                              const char *prefix_,
                                              size_t *size_out_,
                                              std::string *endpoint_out_)
{
    if (!prefix_ || !*prefix_ || !size_out_ || !endpoint_out_)
        return false;

    const size_t prefix_len = std::strlen (prefix_);
    if (line_.compare (0, prefix_len, prefix_) != 0)
        return false;

    const size_t comma = line_.find (',', prefix_len);
    if (comma == std::string::npos)
        return false;

    char *end = NULL;
    const unsigned long long parsed_size = std::strtoull (line_.c_str () + prefix_len, &end, 10);
    if (!end || static_cast<size_t> (end - line_.c_str ()) != comma || parsed_size == 0) {
        return false;
    }

    *size_out_ = static_cast<size_t> (parsed_size);
    *endpoint_out_ = line_.substr (comma + 1);
    return !endpoint_out_->empty ();
}

inline bool parse_size_count_command_line (const std::string &line_,
                                           const char *prefix_,
                                           size_t *size_out_,
                                           size_t *count_out_)
{
    if (!prefix_ || !*prefix_ || !size_out_ || !count_out_)
        return false;

    const size_t prefix_len = std::strlen (prefix_);
    if (line_.compare (0, prefix_len, prefix_) != 0)
        return false;

    const size_t comma = line_.find (',', prefix_len);
    if (comma == std::string::npos)
        return false;

    char *end = NULL;
    const unsigned long long parsed_size = std::strtoull (line_.c_str () + prefix_len, &end, 10);
    if (!end || static_cast<size_t> (end - line_.c_str ()) != comma || parsed_size == 0) {
        return false;
    }

    const unsigned long long parsed_count = std::strtoull (line_.c_str () + comma + 1, &end, 10);
    if (!end || *end != '\0' || parsed_count == 0)
        return false;

    *size_out_ = static_cast<size_t> (parsed_size);
    *count_out_ = static_cast<size_t> (parsed_count);
    return true;
}

inline void reset_start_signal_state (start_signal_state_t *state_)
{
    if (!state_)
        return;

    std::lock_guard<std::mutex> lock (state_->mutex);
    state_->pending_sizes.clear ();
    state_->stopped = false;
}

inline void signal_start (start_signal_state_t *state_, size_t msg_size_)
{
    if (!state_ || msg_size_ == 0)
        return;

    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->pending_sizes.insert (msg_size_);
    }
    state_->cv.notify_all ();
}

inline void signal_stop (start_signal_state_t *state_)
{
    if (!state_)
        return;

    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        state_->stopped = true;
    }
    state_->cv.notify_all ();
}

inline bool wait_for_start (start_signal_state_t *state_, size_t msg_size_, int timeout_ms_)
{
    if (!state_ || msg_size_ == 0) {
        errno = EINVAL;
        return false;
    }

    std::unique_lock<std::mutex> lock (state_->mutex);
    std::set<size_t>::iterator ready = state_->pending_sizes.find (msg_size_);
    if (ready != state_->pending_sizes.end ()) {
        state_->pending_sizes.erase (ready);
        return true;
    }
    if (state_->stopped) {
        errno = ECANCELED;
        return false;
    }

    const bool signaled = state_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1), [state_, msg_size_] () {
          return state_->stopped
                 || state_->pending_sizes.find (msg_size_) != state_->pending_sizes.end ();
      });
    ready = state_->pending_sizes.find (msg_size_);
    if (!signaled || ready == state_->pending_sizes.end ()) {
        errno = state_->stopped ? ECANCELED : ETIMEDOUT;
        return false;
    }

    state_->pending_sizes.erase (ready);
    return true;
}

inline bool wait_for_start_from_stdin (size_t msg_size_)
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
        if (start_size == msg_size_)
            return true;
    }

    errno = ECANCELED;
    return false;
}

inline std::string
make_size_count_command (const char *prefix_, size_t size_value_, size_t count_value_)
{
    return std::string (prefix_) + std::to_string (size_value_) + ","
           + std::to_string (count_value_);
}

inline std::string make_size_command (const char *prefix_, size_t size_value_)
{
    return std::string (prefix_) + std::to_string (size_value_);
}

} // namespace multi
} // namespace perf

#endif
