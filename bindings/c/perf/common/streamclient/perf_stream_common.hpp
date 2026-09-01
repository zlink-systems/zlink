#ifndef PERF_STREAM_COMMON_HPP
#define PERF_STREAM_COMMON_HPP

// Shared utilities for the perf stream client.
// Provides: packet framing helpers, nanosecond clock, string helpers,
// percentile calculation, and CLI parsing for --sizes and --endpoint.
// Used by both async (bench_client_t) and sync (stream_client_t) paths.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace perf_stream_common
{

// Payload size constraints enforced on both send and receive.
inline constexpr size_t k_stream_min_chunk_size = 16;
inline constexpr size_t k_stream_max_chunk_size = 4 * 1024 * 1024;

// Monotonic nanosecond timestamp for latency measurement.
inline uint64_t perf_stream_now_ns ()
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    return static_cast<uint64_t> (
      std::chrono::duration_cast<std::chrono::nanoseconds> (now.time_since_epoch ()).count ());
}

// A STREAM echo contributes to active metrics only when its receive callback
// completes before the configured active deadline.  The lifecycle tail drain
// deliberately leaves this predicate false.
inline bool perf_stream_is_active_completion (bool collection_enabled,
                                               bool active_phase,
                                               uint64_t completion_ns,
                                               uint64_t phase_end_ns)
{
    return collection_enabled && active_phase && phase_end_ns > 0
           && completion_ns < phase_end_ns;
}

// The raw STREAM peer has no Core send/HWM admission point. Keep exactly one
// unresolved echo per connection so TCP, TLS, WS, and WSS are offered the same
// load instead of benchmarking their different local buffering depths.
inline bool perf_stream_can_submit_echo (size_t outstanding)
{
    return outstanding == 0;
}

inline double perf_stream_echo_latency_ns (uint64_t rtt_ns)
{
    return static_cast<double> (rtt_ns) * 0.5;
}

inline double perf_stream_echo_mean_ns (uint64_t rtt_sum_ns, uint64_t completion_count)
{
    if (completion_count == 0)
        return 0.0;
    return perf_stream_echo_latency_ns (rtt_sum_ns)
           / static_cast<double> (completion_count);
}

// Big-endian wire encoding helpers for the packet framing protocol.
// Wire format: [2-byte BE header length][4-byte BE body length][header][body]
inline uint16_t perf_stream_load_u16_be (const unsigned char *p)
{
    return (static_cast<uint16_t> (p[0]) << 8) | static_cast<uint16_t> (p[1]);
}

inline void perf_stream_store_u16_be (unsigned char *p, uint16_t v)
{
    p[0] = static_cast<unsigned char> ((v >> 8) & 0xFF);
    p[1] = static_cast<unsigned char> (v & 0xFF);
}

inline uint32_t perf_stream_load_u32_be (const unsigned char *p)
{
    return (static_cast<uint32_t> (p[0]) << 24) | (static_cast<uint32_t> (p[1]) << 16)
           | (static_cast<uint32_t> (p[2]) << 8) | static_cast<uint32_t> (p[3]);
}

inline void perf_stream_store_u32_be (unsigned char *p, uint32_t v)
{
    p[0] = static_cast<unsigned char> ((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char> ((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char> ((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char> (v & 0xFF);
}

inline constexpr size_t k_stream_packet_prefix_size = 6;
inline constexpr char k_stream_msg_name[] = "stream.echo";
inline constexpr size_t k_stream_msg_name_size = sizeof (k_stream_msg_name) - 1;

inline bool perf_stream_validate_frame_sizes (size_t header_size, size_t body_size)
{
    const size_t max_size = k_stream_max_chunk_size;
    return header_size <= max_size && body_size <= max_size && header_size <= max_size - body_size;
}

inline bool perf_stream_is_msg_name (const unsigned char *data, size_t size)
{
    return data && size == k_stream_msg_name_size
           && std::memcmp (data, k_stream_msg_name, k_stream_msg_name_size) == 0;
}

inline std::string lower_copy (const std::string &text)
{
    std::string out = text;
    std::transform (out.begin (), out.end (), out.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return out;
}

// Linear-interpolation percentile from a pre-sorted sample vector.
// q in [0.0, 1.0]: 0.5 = p50, 0.95 = p95, 0.99 = p99.
inline double percentile_from_sorted (const std::vector<double> &samples, double q)
{
    if (samples.empty ())
        return 0.0;

    if (q < 0.0)
        q = 0.0;
    if (q > 1.0)
        q = 1.0;

    const size_t last = samples.size () - 1;
    const double idx = q * static_cast<double> (last);
    const size_t lo = static_cast<size_t> (idx);
    const size_t hi = std::min<size_t> (last, lo + 1);
    const double frac = idx - static_cast<double> (lo);
    if (lo == hi)
        return samples[lo];
    return samples[lo] + (samples[hi] - samples[lo]) * frac;
}

// Parse comma-separated size list (e.g. "64,1024,65536").
// Each value must be within [k_stream_min_chunk_size, k_stream_max_chunk_size].
inline bool perf_stream_parse_size_list (const std::string &text, std::vector<size_t> &out)
{
    out.clear ();
    std::stringstream ss (text);
    std::string item;
    while (std::getline (ss, item, ',')) {
        if (item.empty ())
            continue;
        char *end = NULL;
        const unsigned long parsed = std::strtoul (item.c_str (), &end, 10);
        if (end == item.c_str ())
            return false;
        const size_t size = static_cast<size_t> (parsed);
        if (size < k_stream_min_chunk_size || size > k_stream_max_chunk_size)
            return false;
        out.push_back (size);
    }
    return !out.empty ();
}

// Parse endpoint URI "scheme://host:port" into components.
// Supports IPv6 bracket notation: "tcp://[::1]:5555".
inline bool perf_stream_parse_endpoint (const std::string &endpoint,
                                        std::string *transport_out,
                                        std::string *host_out,
                                        int *port_out)
{
    const size_t scheme_pos = endpoint.find ("://");
    if (scheme_pos == std::string::npos)
        return false;

    const std::string scheme = lower_copy (endpoint.substr (0, scheme_pos));
    std::string rest = endpoint.substr (scheme_pos + 3);
    if (rest.empty ())
        return false;

    const size_t slash_pos = rest.find ('/');
    if (slash_pos != std::string::npos)
        rest = rest.substr (0, slash_pos);
    if (rest.empty ())
        return false;

    std::string host;
    std::string port_text;
    if (!rest.empty () && rest[0] == '[') {
        const size_t close = rest.find (']');
        if (close == std::string::npos || close + 2 > rest.size () || rest[close + 1] != ':')
            return false;
        host = rest.substr (1, close - 1);
        port_text = rest.substr (close + 2);
    } else {
        const size_t colon = rest.rfind (':');
        if (colon == std::string::npos)
            return false;
        host = rest.substr (0, colon);
        port_text = rest.substr (colon + 1);
    }
    if (host.empty () || port_text.empty ())
        return false;

    char *end = NULL;
    const long port = std::strtol (port_text.c_str (), &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535)
        return false;

    if (transport_out)
        *transport_out = scheme;
    if (host_out)
        *host_out = host;
    if (port_out)
        *port_out = static_cast<int> (port);
    return true;
}

} // namespace perf_stream_common

#endif
