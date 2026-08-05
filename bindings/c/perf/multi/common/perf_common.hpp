#ifndef PERF_RUNTIME_COMMON_HPP
#define PERF_RUNTIME_COMMON_HPP

#include "../../common/perf_infra.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <fstream>
#include <climits>
#include <mutex>
#include <new>
#include <zlink.h>
#include "perf_common_multi.hpp"
#include "../../common/perf_tls_setup.hpp"
#include "perf_multi_metrics.hpp"
#include "perf_multi_poll.hpp"
#include "perf_multi_runtime.hpp"

typedef std::chrono::steady_clock steady_clock_t;
typedef std::chrono::milliseconds milliseconds_t;
typedef std::chrono::seconds seconds_t;
typedef std::chrono::nanoseconds nanoseconds_t;
typedef std::chrono::duration<double> floating_seconds_t;

inline steady_clock_t::duration to_clock_duration (double seconds)
{
    return std::chrono::duration_cast<steady_clock_t::duration> (floating_seconds_t (seconds));
}

inline long remaining_milliseconds (const steady_clock_t::time_point &deadline,
                                    const steady_clock_t::time_point &now)
{
    return std::chrono::duration_cast<milliseconds_t> (deadline - now).count ();
}

// Wire-level stop token used by sender threads to signal phase end to a
// receiver waiting on a poller. PERF_MULTI_TEST_POLICY § 1.3.1 mandates
// this pattern instead of `std::atomic<bool> sender_done` + short polling.
// Mirrors perf_multi (cpp) common/perf_common.hpp helpers.
static const char *const k_stop_token = "__zlink_perf_stop__";

inline bool is_stop_token (const void *data_, size_t size_)
{
    const size_t token_size = std::strlen (k_stop_token);
    return data_ != NULL && size_ == token_size
           && std::memcmp (data_, k_stop_token, token_size) == 0;
}

inline bool is_stop_token_message (const zlink_msg_t &msg_)
{
    return is_stop_token (zlink_msg_data (const_cast<zlink_msg_t *> (&msg_)),
                          zlink_msg_size (const_cast<zlink_msg_t *> (&msg_)));
}

struct connect_monitor_state_t
{
    connect_monitor_state_t () : connection_ready_count (0), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    size_t connection_ready_count;
    int error_code;
};

struct connect_monitor_t
{
    connect_monitor_t () : owner (NULL), monitor (NULL), state (NULL) {}

    void *owner;
    void *monitor;
    connect_monitor_state_t *state;
};

struct ready_monitor_t
{
    ready_monitor_t () : owner (NULL), monitor (NULL) {}

    void *owner;
    void *monitor;
};

inline void connect_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    connect_monitor_state_t *state = static_cast<connect_monitor_state_t *> (userdata_);
    if (!state || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (state->sync);
        switch (event_->event) {
            case ZLINK_EVENT_CONNECTION_READY:
                ++state->connection_ready_count;
                break;

            case ZLINK_EVENT_BIND_FAILED:
            case ZLINK_EVENT_ACCEPT_FAILED:
            case ZLINK_EVENT_CLOSE_FAILED:
            case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
                if (state->error_code == 0)
                    state->error_code = event_->value > 0 ? static_cast<int> (event_->value) : EIO;
                break;

            default:
                break;
        }
    }
    state->cv.notify_all ();
}

inline size_t connect_ready_count (const connect_monitor_state_t *state_)
{
    if (!state_)
        return 0;
    return state_->connection_ready_count;
}

inline bool open_connect_monitor (void *socket_, connect_monitor_t &out_)
{
    out_.owner = socket_;
    out_.monitor = NULL;
    out_.state = NULL;
    if (!socket_)
        return false;

    connect_monitor_state_t *state = new (std::nothrow) connect_monitor_state_t ();
    if (!state)
        return false;

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events =
      ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
      | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
      | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &monitor_opts);
    if (!monitor) {
        delete state;
        return false;
    }
    if (zlink_socket_monitor_handler (monitor, &connect_monitor_handler, state) != 0) {
        zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    const uint64_t monitor_hwm = bench_hwm_from_env ("PERF_MONITOR_HWM", 4096000);
    set_sockopt_int (monitor, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
    if (monitor_hwm > 0) {
        set_sockopt_u64 (monitor, ZLINK_OPT_SNDHWM, monitor_hwm,
                         "ZLINK_OPT_SNDHWM");
        set_sockopt_u64 (monitor, ZLINK_OPT_RCVHWM, monitor_hwm,
                         "ZLINK_OPT_RCVHWM");
    }

    out_.monitor = monitor;
    out_.state = state;
    return true;
}

inline void stop_and_close_socket_monitor (void *owner_, void **monitor_p_)
{
    if (!monitor_p_ || !*monitor_p_)
        return;

    (void) owner_;
    void *monitor = *monitor_p_;
    *monitor_p_ = NULL;
    (void) zlink_monitor_close (&monitor);
}

inline void configure_perf_monitor_socket (void *monitor_)
{
    if (!monitor_)
        return;

    const uint64_t monitor_hwm = bench_hwm_from_env ("PERF_MONITOR_HWM", 4096000);
    set_sockopt_int (monitor_, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
    if (monitor_hwm > 0) {
        set_sockopt_u64 (monitor_, ZLINK_OPT_SNDHWM, monitor_hwm,
                         "ZLINK_OPT_SNDHWM");
        set_sockopt_u64 (monitor_, ZLINK_OPT_RCVHWM, monitor_hwm,
                         "ZLINK_OPT_RCVHWM");
    }
}

inline void close_ready_monitor (ready_monitor_t &monitor_)
{
    if (!monitor_.monitor)
        return;

    void *monitor = monitor_.monitor;
    monitor_.owner = NULL;
    monitor_.monitor = NULL;
    (void) zlink_monitor_close (&monitor);
}

inline bool open_configured_socket_monitor (void *socket_, uint64_t events_, ready_monitor_t *out_)
{
    if (!socket_ || events_ == 0 || !out_)
        return false;

    out_->owner = socket_;
    out_->monitor = NULL;

    zlink_socket_monitor_open_options_t opts;
    std::memset (&opts, 0, sizeof (opts));
    opts.events = events_;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor)
        return false;

    configure_perf_monitor_socket (monitor);
    out_->monitor = monitor;
    return true;
}

inline bool is_socket_monitor_error_event (uint64_t event_)
{
    switch (event_) {
        case ZLINK_EVENT_BIND_FAILED:
        case ZLINK_EVENT_ACCEPT_FAILED:
        case ZLINK_EVENT_CLOSE_FAILED:
        case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
            return true;

        default:
            return false;
    }
}

inline bool socket_monitor_event_ready (const zlink_socket_monitor_event_t &event_,
                                        uint64_t success_event_)
{
    return event_.event == success_event_;
}

inline bool
wait_for_socket_monitor_event (ready_monitor_t &monitor_, uint64_t success_event_, int timeout_ms_)
{
    if (!monitor_.monitor || success_event_ == 0)
        return false;

    const steady_clock_t::time_point deadline =
      steady_clock_t::now () + milliseconds_t (timeout_ms_ > 0 ? timeout_ms_ : 1);

    while (steady_clock_t::now () < deadline) {
        zlink_pollitem_t item = {monitor_.monitor, 0, ZLINK_POLLIN, 0};
        const long timeout_ms =
          std::chrono::duration_cast<milliseconds_t> (deadline - steady_clock_t::now ()).count ();
        const int poll_rc = perf_socket_poll (&item, 1, timeout_ms > 0 ? timeout_ms : 1);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (poll_rc == 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            zlink_socket_monitor_event_t event;
            if (zlink_socket_monitor_recv (monitor_.monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT)
                != 0) {
                const int err = zlink_errno ();
                if (err == EAGAIN || err == EINTR)
                    break;
                return false;
            }
            if (socket_monitor_event_ready (event, success_event_))
                return true;
            if (is_socket_monitor_error_event (event.event)) {
                errno = event.value > 0 ? static_cast<int> (event.value) : EIO;
                return false;
            }
        }
    }

    return false;
}

inline int poll_connect_ready_count (connect_monitor_t &monitor_)
{
    if (!monitor_.state)
        return 0;

    std::lock_guard<std::mutex> lock (monitor_.state->sync);
    return static_cast<int> (connect_ready_count (monitor_.state));
}

inline bool
wait_connect_ready_count (connect_monitor_t &monitor_, size_t expected_ready_, int timeout_ms_)
{
    if (expected_ready_ == 0)
        return true;
    if (!monitor_.state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_.state->sync);
    if (monitor_.state->error_code != 0)
        return false;
    if (connect_ready_count (monitor_.state) >= expected_ready_)
        return true;

    const int bounded_timeout = timeout_ms_ > 0 ? timeout_ms_ : 0;
    if (bounded_timeout == 0)
        return false;

    const bool signaled = monitor_.state->cv.wait_for (
      lock, milliseconds_t (bounded_timeout), [&monitor_, expected_ready_] () {
          return monitor_.state->error_code != 0
                 || connect_ready_count (monitor_.state) >= expected_ready_;
      });
    if (!signaled && bench_debug_enabled ()) {
        std::cerr << "[perf-multi] connect ready timeout ready="
                  << monitor_.state->connection_ready_count << " expected=" << expected_ready_
                  << std::endl;
    }
    return signaled && monitor_.state->error_code == 0
           && connect_ready_count (monitor_.state) >= expected_ready_;
}

inline void close_connect_monitor (connect_monitor_t &monitor_)
{
    connect_monitor_state_t *state = monitor_.state;
    void *owner = monitor_.owner;
    void *monitor = monitor_.monitor;

    if (!monitor && !state)
        return;

    if (monitor) {
        (void) owner;
        if (zlink_monitor_close (&monitor) == 0) {
            monitor_.owner = NULL;
            monitor_.monitor = NULL;
            monitor_.state = NULL;
            delete state;
            return;
        }

        if (bench_debug_enabled ()) {
            std::cerr << "[perf-multi] monitor close failed: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        }
        return;
    }

    monitor_.owner = NULL;
    monitor_.monitor = NULL;
    monitor_.state = NULL;
    delete state;
}

#endif
