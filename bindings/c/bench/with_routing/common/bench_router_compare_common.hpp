#ifndef BENCH_ROUTER_COMPARE_COMMON_HPP
#define BENCH_ROUTER_COMPARE_COMMON_HPP

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace bench_rc
{

inline uint64_t now_ns ()
{
    return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::steady_clock::now ().time_since_epoch ())
                                    .count ());
}

inline void store_u64_be (unsigned char *dst, uint64_t value)
{
    dst[0] = static_cast<unsigned char> ((value >> 56) & 0xFFu);
    dst[1] = static_cast<unsigned char> ((value >> 48) & 0xFFu);
    dst[2] = static_cast<unsigned char> ((value >> 40) & 0xFFu);
    dst[3] = static_cast<unsigned char> ((value >> 32) & 0xFFu);
    dst[4] = static_cast<unsigned char> ((value >> 24) & 0xFFu);
    dst[5] = static_cast<unsigned char> ((value >> 16) & 0xFFu);
    dst[6] = static_cast<unsigned char> ((value >> 8) & 0xFFu);
    dst[7] = static_cast<unsigned char> (value & 0xFFu);
}

inline uint64_t load_u64_be (const unsigned char *src)
{
    return (static_cast<uint64_t> (src[0]) << 56) | (static_cast<uint64_t> (src[1]) << 48)
           | (static_cast<uint64_t> (src[2]) << 40) | (static_cast<uint64_t> (src[3]) << 32)
           | (static_cast<uint64_t> (src[4]) << 24) | (static_cast<uint64_t> (src[5]) << 16)
           | (static_cast<uint64_t> (src[6]) << 8) | static_cast<uint64_t> (src[7]);
}

inline long parse_long_env (const char *name, long fallback, long min_value)
{
    const char *raw = std::getenv (name);
    if (!raw || !*raw)
        return fallback;

    char *end = NULL;
    errno = 0;
    const long value = std::strtol (raw, &end, 10);
    if (errno != 0 || end == raw)
        return fallback;
    if (value < min_value)
        return min_value;
    return value;
}

inline std::string parse_string_env (const char *name, const char *fallback)
{
    const char *raw = std::getenv (name);
    if (!raw || !*raw)
        return std::string (fallback ? fallback : "");
    return std::string (raw);
}

inline std::string endpoint_from_port (int port)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "tcp://127.0.0.1:%d", port);
    return std::string (buf);
}

inline int resolve_size_transition_drain_ms (int fallback_ms)
{
    return static_cast<int> (
      parse_long_env ("BENCH_MULTI_SIZE_TRANSITION_DRAIN_MS", fallback_ms, 0));
}

inline void run_size_transition_drain_stage (int transition_drain_ms, bool has_next_size)
{
    if (!has_next_size)
        return;

    if (transition_drain_ms <= 0)
        return;

    std::this_thread::sleep_for (std::chrono::milliseconds (transition_drain_ms));
}

inline void print_result (const std::string &lib_name,
                          const std::string &transport,
                          size_t msg_size,
                          double throughput,
                          double latency_us)
{
    std::printf ("RESULT,%s,ROUTER_ECHO,%s,%zu,throughput,%.2f\n", lib_name.c_str (),
                 transport.c_str (), msg_size, throughput);
    std::printf ("RESULT,%s,ROUTER_ECHO,%s,%zu,latency,%.2f\n", lib_name.c_str (),
                 transport.c_str (), msg_size, latency_us);
    std::fflush (stdout);
}

inline bool parse_size_list (const std::string &raw, std::vector<size_t> &out)
{
    out.clear ();
    if (raw.empty ())
        return false;

    const char *cur = raw.c_str ();
    while (*cur) {
        while (*cur == ',' || std::isspace (static_cast<unsigned char> (*cur)))
            ++cur;
        if (*cur == '\0')
            break;

        char *end = NULL;
        const unsigned long value = std::strtoul (cur, &end, 10);
        if (end == cur || value == 0)
            return false;

        out.push_back (static_cast<size_t> (value));
        cur = end;

        while (*cur && *cur != ',') {
            if (!std::isspace (static_cast<unsigned char> (*cur)))
                return false;
            ++cur;
        }
        if (*cur == ',')
            ++cur;
    }
    return !out.empty ();
}

} // namespace bench_rc

#endif
