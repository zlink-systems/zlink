#ifndef PERF_MULTI_COMMON_MULTI_HPP
#define PERF_MULTI_COMMON_MULTI_HPP

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace perf
{
namespace multi
{

struct multi_bench_settings_t
{
    size_t clients;
    uint64_t hwm;
    uint64_t sndhwm;
    uint64_t rcvhwm;
    int duration_seconds;
    int client_poll_timeout_ms;
    int connect_ready_timeout_ms;
    int sndtimeo_ms;
    int rcvtimeo_ms;
    uint64_t monitor_hwm;
    int server_bind_port;
};

inline int parse_positive_env (const char *name, int default_value)
{
    if (!name)
        return default_value;

    const char *value = std::getenv (name);
    if (!value || !*value)
        return default_value;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value || parsed <= 0)
        return default_value;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline uint64_t parse_positive_uint64_env (const char *name, uint64_t default_value)
{
    if (!name)
        return default_value;

    const char *value = std::getenv (name);
    if (!value || !*value || *value == '-')
        return default_value;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0)
        return default_value;
    return static_cast<uint64_t> (parsed);
}

inline bool manual_socket_overrides_enabled ()
{
    return parse_positive_env ("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0
           || parse_positive_env ("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0;
}

inline bool is_stream_pattern (const char *pattern)
{
    if (!pattern || !*pattern)
        return false;
    return std::strcmp (pattern, "STREAM") == 0 || std::strcmp (pattern, "MULTI_STREAM") == 0;
}

inline bool validate_multi_perf_pattern (const char *pattern)
{
    return pattern && *pattern;
}

inline size_t resolve_multi_default_clients (const std::string &pattern)
{
    return is_stream_pattern (pattern.c_str ()) ? static_cast<size_t> (10000)
                                                : static_cast<size_t> (100);
}

inline uint64_t resolve_multi_default_hwm (const std::string &pattern, size_t)
{
    (void) pattern;
    return 1000;
}

inline multi_bench_settings_t resolve_multi_bench_settings ()
{
    const char *pattern_env = std::getenv ("PERF_PATTERN");
    const std::string pattern = pattern_env ? pattern_env : "";

    const size_t default_clients = resolve_multi_default_clients (pattern);
    const int clients =
      std::max (1, parse_positive_env ("PERF_MULTI_CLIENTS", static_cast<int> (default_clients)));

    const uint64_t default_hwm = resolve_multi_default_hwm (pattern, static_cast<size_t> (clients));

    multi_bench_settings_t out;
    out.clients = static_cast<size_t> (clients);
    if (manual_socket_overrides_enabled ()) {
        out.hwm = parse_positive_uint64_env ("PERF_MULTI_HWM", default_hwm);
        out.sndhwm = parse_positive_uint64_env ("PERF_MULTI_SNDHWM", out.hwm);
        out.rcvhwm = parse_positive_uint64_env ("PERF_MULTI_RCVHWM", out.hwm);
    } else {
        out.hwm = 0;
        out.sndhwm = 0;
        out.rcvhwm = 0;
    }
    out.duration_seconds = std::max (1, parse_positive_env ("PERF_MULTI_DURATION_SECONDS", 5));
    out.client_poll_timeout_ms = 0;
    out.connect_ready_timeout_ms =
      std::max (0, parse_positive_env ("PERF_MULTI_CONNECT_READY_TIMEOUT_MS", 5000));
    out.sndtimeo_ms = std::max (0, parse_positive_env ("PERF_MULTI_SNDTIMEO_MS", 200));
    out.rcvtimeo_ms = std::max (0, parse_positive_env ("PERF_MULTI_RCVTIMEO_MS", 200));
    out.monitor_hwm = parse_positive_uint64_env ("PERF_MULTI_MONITOR_HWM", 1000);
    out.server_bind_port = std::max (0, parse_positive_env ("PERF_MULTI_SERVER_BIND_PORT", 0));

    return out;
}

} // namespace multi
} // namespace perf

#endif
