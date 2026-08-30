#ifndef PERF_MULTI_COMMON_MULTI_HPP
#define PERF_MULTI_COMMON_MULTI_HPP

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>


namespace perf
{
namespace multi
{

inline constexpr int default_send_drain_timeout_ms = 1000;

struct multi_bench_settings_t
{
    size_t clients;
    uint64_t hwm;
    uint64_t sndhwm;
    uint64_t rcvhwm;
    // Debug-only manual SNDBUF/RCVBUF override in bytes; -1 means "leave the
    // OS default" (mirrors bindings/c/perf multi bench_manual_socket_overrides_allowed()
    // + bench_socket_buffer_bytes_from_env()).
    int sndbuf;
    int rcvbuf;
    int duration_seconds;
    int client_poll_timeout_ms;
    int connect_ready_timeout_ms;
    int send_drain_timeout_ms;
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

inline std::chrono::milliseconds remaining_bounded_timeout (
  const std::chrono::steady_clock::time_point &deadline)
{
    const auto now = std::chrono::steady_clock::now ();
    if (now >= deadline)
        return std::chrono::milliseconds::zero ();

    const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
    return std::chrono::milliseconds (std::max<int64_t> (1, remaining.count ()));
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

inline uint64_t parse_nonnegative_uint64_env (const char *name,
                                              uint64_t default_value)
{
    if (!name)
        return default_value;

    const char *value = std::getenv (name);
    if (!value || !*value
        || std::strspn (value, "0123456789") != std::strlen (value))
        return default_value;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0'
        || parsed > (std::numeric_limits<uint64_t>::max) ())
        return default_value;
    return static_cast<uint64_t> (parsed);
}

inline bool manual_socket_overrides_enabled ()
{
    return parse_positive_env ("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0
           || parse_positive_env ("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0;
}

// Mirrors bindings/c/perf multi parse_byte_size_token(): accepts an optional
// case-insensitive b/k/m/g[b] suffix. Returns default_value on any parse
// failure or overflow, matching the C reference byte-for-byte.
inline int parse_byte_size_token (const char *value, int default_value)
{
    if (!value || !*value)
        return default_value;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value)
        return default_value;

    unsigned long long multiplier = 1;
    if (end && *end) {
        char suffix[3] = {0, 0, 0};
        size_t suffix_len = 0;
        while (end[suffix_len] != '\0' && suffix_len < 2) {
            suffix[suffix_len] =
              static_cast<char> (std::tolower (static_cast<unsigned char> (end[suffix_len])));
            ++suffix_len;
        }
        if (end[suffix_len] != '\0')
            return default_value;

        if (suffix[0] == 'b' && suffix[1] == '\0')
            multiplier = 1;
        else if (suffix[0] == 'k' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL;
        else if (suffix[0] == 'm' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL;
        else if (suffix[0] == 'g' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        else
            return default_value;
    }

    const unsigned long long bytes = parsed * multiplier;
    if (bytes == 0)
        return default_value;
    if (bytes > static_cast<unsigned long long> (INT_MAX))
        return INT_MAX;
    return static_cast<int> (bytes);
}

inline int bench_socket_buffer_bytes_from_env (const char *name, int default_bytes)
{
    if (!name || !*name)
        return default_bytes;
    const char *value = std::getenv (name);
    if (!value || !*value)
        return default_bytes;
    return parse_byte_size_token (value, default_bytes);
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
    (void) pattern;
    return static_cast<size_t> (100);
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
        out.sndbuf = bench_socket_buffer_bytes_from_env ("PERF_MULTI_SNDBUF", -1);
        out.rcvbuf = bench_socket_buffer_bytes_from_env ("PERF_MULTI_RCVBUF", -1);
    } else {
        out.hwm = 0;
        out.sndhwm = 0;
        out.rcvhwm = 0;
        out.sndbuf = -1;
        out.rcvbuf = -1;
    }
    out.duration_seconds = std::max (1, parse_positive_env ("PERF_MULTI_DURATION_SECONDS", 5));
    out.client_poll_timeout_ms = 0;
    out.connect_ready_timeout_ms =
      std::max (0, parse_positive_env ("PERF_MULTI_CONNECT_READY_TIMEOUT_MS", 10000));
    out.send_drain_timeout_ms = std::max (
      1, parse_positive_env ("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS",
                             default_send_drain_timeout_ms));
    out.sndtimeo_ms = std::max (0, parse_positive_env ("PERF_MULTI_SNDTIMEO_MS", 200));
    out.rcvtimeo_ms = std::max (0, parse_positive_env ("PERF_MULTI_RCVTIMEO_MS", 200));
    out.monitor_hwm =
      parse_nonnegative_uint64_env ("PERF_MULTI_MONITOR_HWM", 4096000);
    out.server_bind_port = std::max (0, parse_positive_env ("PERF_MULTI_SERVER_BIND_PORT", 0));

    return out;
}

} // namespace multi
} // namespace perf

#endif
