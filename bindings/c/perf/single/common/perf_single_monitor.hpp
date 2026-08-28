#ifndef PERF_SINGLE_MONITOR_HPP
#define PERF_SINGLE_MONITOR_HPP

#include "../../common/perf_infra.hpp"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>

#include <zlink.h>

inline uint64_t perf_single_monitor_hwm_bytes ()
{
    const uint64_t default_hwm_bytes = 4096000;
    const char *value = std::getenv ("PERF_MONITOR_HWM_BYTES");
    if (!value || !*value)
        return default_hwm_bytes;
    if (std::strspn (value, "0123456789") != std::strlen (value))
        return default_hwm_bytes;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0'
        || parsed > (std::numeric_limits<uint64_t>::max) ())
        return default_hwm_bytes;
    return static_cast<uint64_t> (parsed);
}

inline void configure_perf_monitor_socket (void *monitor_)
{
    if (!monitor_)
        return;

    set_sockopt_int (monitor_, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
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
    if (event_.event != success_event_)
        return false;
    if (success_event_ == ZLINK_EVENT_CONNECTION_READY)
        return true;
    return event_.value > 0;
}

inline void *open_configured_socket_monitor (void *socket_, uint64_t events_)
{
    if (!socket_ || events_ == 0)
        return NULL;

    const uint64_t monitor_hwm = perf_single_monitor_hwm_bytes ();
    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = events_;
    monitor_opts.monitor_hwm_bytes = monitor_hwm;
    void *monitor = zlink_socket_monitor_open (socket_, &monitor_opts);
    if (!monitor)
        return NULL;
    configure_perf_monitor_socket (monitor);
    return monitor;
}

inline bool wait_for_socket_monitor_event_with_activity (void *monitor_,
                                                         void *activity_socket_,
                                                         uint64_t success_event_,
                                                         int timeout_ms_);

inline bool wait_for_socket_monitor_event (void *monitor_, uint64_t success_event_, int timeout_ms_)
{
    return wait_for_socket_monitor_event_with_activity (monitor_, NULL, success_event_,
                                                        timeout_ms_);
}

inline bool wait_for_socket_monitor_event_with_activity (void *monitor_,
                                                         void *activity_socket_,
                                                         uint64_t success_event_,
                                                         int timeout_ms_)
{
    if (!monitor_ || success_event_ == 0)
        return false;

    bool ready = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1);
    void *poller = zlink_poller_new ();
    if (!poller)
        return false;
    if (zlink_poller_add (poller, monitor_, monitor_, ZLINK_POLLIN) != 0) {
        (void) zlink_poller_destroy (&poller);
        return false;
    }
    if (activity_socket_
        && zlink_poller_add (poller, activity_socket_, activity_socket_, ZLINK_POLLIN) != 0) {
        (void) zlink_poller_destroy (&poller);
        return false;
    }

    while (std::chrono::steady_clock::now () < deadline && !ready) {
        const long timeout_ms =
          static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                               deadline - std::chrono::steady_clock::now ())
                               .count ());
        zlink_poller_event_t event_info;
        const int poll_rc =
          zlink_poller_wait (poller, &event_info, 1, timeout_ms > 0 ? timeout_ms : 1, NULL);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            break;
        }
        if (poll_rc == 0)
            continue;
        if (event_info.socket != monitor_)
            continue;

        zlink_socket_monitor_event_t event;
        if (zlink_socket_monitor_recv (monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT) != 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            (void) zlink_poller_destroy (&poller);
            return false;
        }
        if (socket_monitor_event_ready (event, success_event_)) {
            ready = true;
            break;
        }
        if (is_socket_monitor_error_event (event.event)) {
            errno = event.value > 0 ? static_cast<int> (event.value) : EIO;
            (void) zlink_poller_destroy (&poller);
            return false;
        }
    }
    (void) zlink_poller_destroy (&poller);
    return ready;
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency_ns,
                          double latency_p95_ns,
                          double latency_p99_ns)
{
    const double bandwidth_mb_s = (throughput * static_cast<double> (size)) / 1000000.0;
    const double latency_ms = latency_ns / 1000000.0;
    const double latency_p95_ms = latency_p95_ns / 1000000.0;
    const double latency_p99_ms = latency_p99_ns / 1000000.0;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (3) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (3) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (3) << latency_ms << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (3) << latency_p95_ms
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (3) << latency_p99_ms
              << std::endl;
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency)
{
    print_result (lib_type, pattern, transport, size, throughput, latency, latency, latency);
}

inline void print_failure_diagnostics (const std::string &lib_type,
                                       const std::string &pattern,
                                       const std::string &transport,
                                       size_t size,
                                       const char *detail_ = NULL)
{
    std::cerr << "FAIL," << lib_type << "," << pattern << "," << transport << "," << size;
    if (detail_ && *detail_)
        std::cerr << "," << detail_;
    std::cerr << std::endl;
}

inline void print_fail_result (const std::string &lib_type,
                               const std::string &pattern,
                               const std::string &transport,
                               size_t size)
{
    print_failure_diagnostics (lib_type, pattern, transport, size);
}

#endif
