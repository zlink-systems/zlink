#ifndef ZLINK_C_BENCH_WITH_GRPC_COMMON_HPP
#define ZLINK_C_BENCH_WITH_GRPC_COMMON_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace zlink_c_bench
{

inline uint64_t now_ns ()
{
    return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::steady_clock::now ().time_since_epoch ())
                                    .count ());
}

inline void write_u32_le (unsigned char *dst, uint32_t value)
{
    dst[0] = static_cast<unsigned char> (value & 0xffu);
    dst[1] = static_cast<unsigned char> ((value >> 8) & 0xffu);
    dst[2] = static_cast<unsigned char> ((value >> 16) & 0xffu);
    dst[3] = static_cast<unsigned char> ((value >> 24) & 0xffu);
}

inline void write_u64_le (unsigned char *dst, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        dst[i] = static_cast<unsigned char> ((value >> (i * 8)) & 0xffu);
}

inline uint32_t read_u32_le (const unsigned char *src)
{
    return static_cast<uint32_t> (src[0]) | (static_cast<uint32_t> (src[1]) << 8)
           | (static_cast<uint32_t> (src[2]) << 16) | (static_cast<uint32_t> (src[3]) << 24);
}

inline uint64_t read_u64_le (const unsigned char *src)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= static_cast<uint64_t> (src[i]) << (i * 8);
    return value;
}

static const uint32_t k_magic = 0x5a4c4e4bU;
static const size_t k_header_size = 29;

enum phase_t : uint8_t
{
    phase_warmup = 0,
    phase_active = 1
};

inline bool stamp_payload (
  void *payload, size_t payload_size, uint32_t run_id, phase_t phase, uint64_t seq)
{
    if (!payload || payload_size < k_header_size)
        return false;
    unsigned char *p = static_cast<unsigned char *> (payload);
    write_u32_le (p + 0, k_magic);
    write_u32_le (p + 4, run_id);
    p[8] = static_cast<uint8_t> (phase);
    write_u32_le (p + 9, static_cast<uint32_t> (payload_size));
    write_u64_le (p + 13, seq);
    write_u64_le (p + 21, now_ns ());
    return true;
}

struct decoded_header_t
{
    uint32_t magic;
    uint32_t run_id;
    uint8_t phase;
    uint32_t payload_size;
    uint64_t seq;
    uint64_t sent_ns;
};

inline bool decode_payload (const void *payload, size_t payload_size, decoded_header_t *out)
{
    if (!payload || payload_size < k_header_size || !out)
        return false;
    const unsigned char *p = static_cast<const unsigned char *> (payload);
    out->magic = read_u32_le (p + 0);
    out->run_id = read_u32_le (p + 4);
    out->phase = p[8];
    out->payload_size = read_u32_le (p + 9);
    out->seq = read_u64_le (p + 13);
    out->sent_ns = read_u64_le (p + 21);
    return out->magic == k_magic;
}

inline std::vector<size_t> parse_sizes ()
{
    const char *raw = std::getenv ("PAYLOAD_SIZES");
    if (!raw || !*raw)
        return {1024, 4096};
    std::vector<size_t> sizes;
    const char *p = raw;
    while (*p) {
        char *end = nullptr;
        const unsigned long value = std::strtoul (p, &end, 10);
        if (value > 0)
            sizes.push_back (static_cast<size_t> (value));
        if (!end || *end == '\0')
            break;
        p = end + 1;
    }
    return sizes.empty () ? std::vector<size_t> {1024, 4096} : sizes;
}

inline int env_int (const char *name, int fallback)
{
    const char *value = std::getenv (name);
    if (!value || !*value)
        return fallback;
    return std::max (1, std::atoi (value));
}

inline std::string env_string (const char *name, const char *fallback)
{
    const char *value = std::getenv (name);
    return value && *value ? std::string (value) : std::string (fallback);
}

inline bool pattern_enabled (const std::string &enabled, const char *pattern)
{
    size_t start = 0;
    while (start <= enabled.size ()) {
        const size_t comma = enabled.find (',', start);
        const size_t end = comma == std::string::npos ? enabled.size () : comma;
        if (enabled.compare (start, end - start, pattern) == 0)
            return true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

class latency_sampler_t
{
  public:
    explicit latency_sampler_t (size_t cap) : _cap (cap) { _samples.reserve (cap); }

    void add_us (double value)
    {
        _sum += value;
        ++_count;
        if (_samples.size () < _cap)
            _samples.push_back (value);
    }

    double mean_us () const { return _count == 0 ? 0.0 : _sum / static_cast<double> (_count); }

    double percentile (double q) const
    {
        if (_samples.empty ())
            return mean_us ();
        std::vector<double> sorted = _samples;
        std::sort (sorted.begin (), sorted.end ());
        const size_t index = static_cast<size_t> (std::min<double> (
          static_cast<double> (sorted.size () - 1), q * static_cast<double> (sorted.size () - 1)));
        return sorted[index];
    }

  private:
    size_t _cap;
    uint64_t _count = 0;
    double _sum = 0.0;
    std::vector<double> _samples;
};

struct resource_sample_t
{
    double cpu_start = 0.0;
    int server_pid = 0;
    double server_cpu_start = 0.0;
};

inline double process_cpu_seconds_pid (int pid)
{
    if (pid <= 0)
        return 0.0;
    std::ifstream stat ("/proc/" + std::to_string (pid) + "/stat");
    std::string line;
    if (!std::getline (stat, line))
        return 0.0;
    const size_t end_comm = line.rfind (')');
    if (end_comm == std::string::npos || end_comm + 2 >= line.size ())
        return 0.0;
    std::istringstream fields (line.substr (end_comm + 2));
    std::string token;
    unsigned long long utime_ticks = 0;
    unsigned long long stime_ticks = 0;
    for (int field = 3; fields >> token; ++field) {
        if (field == 14)
            utime_ticks = std::strtoull (token.c_str (), nullptr, 10);
        else if (field == 15) {
            stime_ticks = std::strtoull (token.c_str (), nullptr, 10);
            break;
        }
    }
    const long ticks = std::max<long> (1, sysconf (_SC_CLK_TCK));
    return static_cast<double> (utime_ticks + stime_ticks) / static_cast<double> (ticks);
}

inline double process_cpu_seconds_self ()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return static_cast<double> (ru.ru_utime.tv_sec) + static_cast<double> (ru.ru_utime.tv_usec) / 1e6
           + static_cast<double> (ru.ru_stime.tv_sec)
           + static_cast<double> (ru.ru_stime.tv_usec) / 1e6;
}

inline double rss_mb_pid (int pid)
{
    const std::string path = pid > 0 ? "/proc/" + std::to_string (pid) + "/statm" : "/proc/self/statm";
    std::ifstream statm (path);
    long pages = 0;
    long resident = 0;
    statm >> pages >> resident;
    const long page_size = sysconf (_SC_PAGESIZE);
    return static_cast<double> (resident) * static_cast<double> (page_size) / 1024.0 / 1024.0;
}

inline double rss_mb () { return rss_mb_pid (0); }

inline int env_pid (const char *name)
{
    const char *value = std::getenv (name);
    if (!value || !*value)
        return 0;
    return std::max (0, std::atoi (value));
}

inline resource_sample_t resource_start ()
{
    const int server_pid = env_pid ("SERVER_PID");
    return resource_sample_t {process_cpu_seconds_self (), server_pid,
                              process_cpu_seconds_pid (server_pid)};
}

inline double cpu_percent (const resource_sample_t &start, double elapsed_s)
{
    const long cores = std::max<long> (1, sysconf (_SC_NPROCESSORS_ONLN));
    return (process_cpu_seconds_self () - start.cpu_start) / std::max (0.001, elapsed_s)
           / static_cast<double> (cores) * 100.0;
}

inline double server_cpu_percent (const resource_sample_t &start, double elapsed_s)
{
    if (start.server_pid <= 0)
        return 0.0;
    const long cores = std::max<long> (1, sysconf (_SC_NPROCESSORS_ONLN));
    return (process_cpu_seconds_pid (start.server_pid) - start.server_cpu_start)
           / std::max (0.001, elapsed_s) / static_cast<double> (cores) * 100.0;
}

inline double server_mem_mb (const resource_sample_t &start)
{
    return start.server_pid > 0 ? rss_mb_pid (start.server_pid) : 0.0;
}

// Values fixed by the specification (README §3). They are not inputs.
static const uint64_t k_drain_bound_ms = 30000;
static const int k_route_ready_ms = 30000;
static const size_t k_latency_sample_limit = 200000;

inline int duration_seconds () { return env_int ("DURATION_SECONDS", 5); }

struct result_t
{
    std::string implementation;
    std::string pattern;
    size_t size = 0;
    uint64_t completed = 0;
    uint64_t errors = 0;
    double elapsed_s = 0.0;
    double mean_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double cpu_percent = 0.0;
    double mem_mb = 0.0;
    double server_cpu_percent = 0.0;
    double server_mem_mb = 0.0;
    uint64_t submitted = 0;
    uint64_t blocked = 0;
    uint64_t peak_in_flight = 0;
    double submit_wait_ms = 0.0;
    // request-backpressure only: requests still unanswered when the bounded drain ended.
    bool has_drain = false;
    uint64_t abandoned = 0;
    double drain_ms = 0.0;
    bool drain_bound_hit = false;
};

inline std::string json_escape (const std::string &value)
{
    std::string out;
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char> (c) < 0x20)
            out += ' ';
        else
            out += c;
    }
    return out;
}

inline std::string json_number (double value)
{
    char number[64];
    std::snprintf (number, sizeof (number), "%.6f", value);
    return number;
}

// Writes <BENCH_RUN_DIR>/<implementation>-<pattern>-<size>/results.json
// (`with-grpc-cell-v1`, README §11) through a temporary file and a rename.
// `grpc_version` is empty for a client that does not link gRPC.
inline bool write_cell_json (const result_t &r, const std::string &grpc_version)
{
    const std::string run_dir = env_string ("BENCH_RUN_DIR", "");
    if (run_dir.empty ()) {
        std::fprintf (stderr, "BENCH_RUN_DIR is not set\n");
        return false;
    }
    const std::string cell_dir =
      run_dir + "/" + r.implementation + "-" + r.pattern + "-" + std::to_string (r.size);
    std::filesystem::create_directories (cell_dir);
    const double ops_per_s = static_cast<double> (r.completed) / std::max (0.001, r.elapsed_s);
    std::ostringstream out;
    out << "{\n  \"schema\": \"with-grpc-cell-v1\",\n  \"metadata\": {\n"
        << "    \"language\": \"c\",\n"
        << "    \"coreVersion\": \"" << json_escape (env_string ("BENCH_CORE_VERSION", "")) << "\",\n"
        << "    \"compiler\": \"" << json_escape (__VERSION__) << "\",\n"
        << "    \"durationSeconds\": " << duration_seconds () << ",\n"
        << "    \"warmup\": \"none\"";
    if (!grpc_version.empty ())
        out << ",\n    \"grpcVersion\": \"" << json_escape (grpc_version) << "\"";
    out << "\n  },\n  \"cells\": [\n    {\n"
        << "      \"implementation\": \"" << r.implementation << "\",\n"
        << "      \"pattern\": \"" << r.pattern << "\",\n"
        << "      \"payload_size\": " << r.size << ",\n"
        << "      \"throughput_per_second\": " << json_number (ops_per_s) << ",\n"
        << "      \"bandwidth_mb_s\": "
        << json_number (ops_per_s * static_cast<double> (r.size) / 1000000.0) << ",\n"
        << "      \"latency_mean_ms\": " << json_number (r.mean_us / 1000.0) << ",\n"
        << "      \"latency_p95_ms\": " << json_number (r.p95_us / 1000.0) << ",\n"
        << "      \"latency_p99_ms\": " << json_number (r.p99_us / 1000.0) << ",\n"
        << "      \"client_cpu_percent\": " << json_number (r.cpu_percent) << ",\n"
        << "      \"client_memory_mb\": " << json_number (r.mem_mb) << ",\n"
        << "      \"server_cpu_percent\": " << json_number (r.server_cpu_percent) << ",\n"
        << "      \"server_memory_mb\": " << json_number (r.server_mem_mb) << ",\n"
        << "      \"peak_in_flight\": " << r.peak_in_flight << ",\n";
    if (r.has_drain)
        out << "      \"abandoned\": " << r.abandoned << ",\n"
            << "      \"drain_ms\": " << json_number (r.drain_ms) << ",\n"
            << "      \"drain_bound_hit\": " << (r.drain_bound_hit ? "true" : "false") << ",\n";
    out << "      \"errors\": " << r.errors << ",\n"
        << "      \"contaminated\": false,\n"
        << "      \"extra\": {\"submitted\": " << r.submitted << ", \"completed\": " << r.completed
        << ", \"blocked\": " << r.blocked
        << ", \"submit_wait_ms\": " << json_number (r.submit_wait_ms) << "}\n    }\n  ]\n}\n";
    const std::string path = cell_dir + "/results.json";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file (tmp, std::ios::binary | std::ios::trunc);
        file << out.str ();
        if (!file.good ())
            return false;
    }
    std::filesystem::rename (tmp, path);
    std::fprintf (stderr, "[bench] cell %s-%s-%zu done: %.1f ops/s errors=%llu\n",
                  r.implementation.c_str (), r.pattern.c_str (), r.size, ops_per_s,
                  static_cast<unsigned long long> (r.errors));
    return true;
}

} // namespace zlink_c_bench

#endif
