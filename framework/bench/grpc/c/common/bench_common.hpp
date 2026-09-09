#ifndef ZLINK_C_BENCH_WITH_GRPC_COMMON_HPP
#define ZLINK_C_BENCH_WITH_GRPC_COMMON_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

inline bool scenario_enabled (const std::string &enabled, const char *scenario)
{
    if (enabled == "all")
        return true;

    size_t start = 0;
    while (start <= enabled.size ()) {
        const size_t comma = enabled.find (',', start);
        const size_t end = comma == std::string::npos ? enabled.size () : comma;
        if (enabled.compare (start, end - start, scenario) == 0)
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

struct result_t
{
    std::string scenario;
    size_t size = 0;
    std::string unit;
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
    uint64_t max_outstanding = 0;
    double submit_wait_ms = 0.0;
};

inline void print_result (const result_t &r)
{
    const double ops_per_s = static_cast<double> (r.completed) / std::max (0.001, r.elapsed_s);
    const double throughput = ops_per_s / 1000.0;
    const double bandwidth = ops_per_s * static_cast<double> (r.size) / 1000000.0;
    std::printf (
      "| %-28s | %8zu | %14.3f %-6s | %10.3f MB/s | %12.3f | %11.3f | %11.3f |"
      " %8.3f | %8.3f | %8.3f | %8.3f | %10llu | %10llu | %8llu | %8llu | %8llu | %12.3f |\n",
      r.scenario.c_str (), r.size, throughput, r.unit.c_str (), bandwidth, r.mean_us / 1000.0,
      r.p95_us / 1000.0, r.p99_us / 1000.0, r.cpu_percent, r.mem_mb,
      r.server_cpu_percent, r.server_mem_mb, static_cast<unsigned long long> (r.submitted),
      static_cast<unsigned long long> (r.completed), static_cast<unsigned long long> (r.errors),
      static_cast<unsigned long long> (r.blocked),
      static_cast<unsigned long long> (r.max_outstanding), r.submit_wait_ms);
    std::printf (
      "RESULT,current,%s,local,%zu,throughput,%.3f\n"
      "RESULT,current,%s,local,%zu,bandwidth,%.3f\n"
      "RESULT,current,%s,local,%zu,latency,%.3f\n"
      "RESULT,current,%s,local,%zu,latency_p95,%.3f\n"
      "RESULT,current,%s,local,%zu,latency_p99,%.3f\n"
      "RESULT,current,%s,local,%zu,client_cpu_percent,%.3f\n"
      "RESULT,current,%s,local,%zu,client_memory_mb,%.3f\n"
      "RESULT,current,%s,local,%zu,server_cpu_percent,%.3f\n"
      "RESULT,current,%s,local,%zu,server_memory_mb,%.3f\n"
      "RESULT,current,%s,local,%zu,submitted,%.3f\n"
      "RESULT,current,%s,local,%zu,completed,%.3f\n"
      "RESULT,current,%s,local,%zu,errors,%.3f\n"
      "RESULT,current,%s,local,%zu,blocked,%.3f\n"
      "RESULT,current,%s,local,%zu,max_outstanding,%.3f\n"
      "RESULT,current,%s,local,%zu,submit_wait_ms,%.3f\n",
      r.scenario.c_str (), r.size, throughput, r.scenario.c_str (), r.size,
      bandwidth, r.scenario.c_str (), r.size,
      r.mean_us / 1000.0, r.scenario.c_str (), r.size, r.p95_us / 1000.0, r.scenario.c_str (),
      r.size, r.p99_us / 1000.0, r.scenario.c_str (), r.size, r.cpu_percent,
      r.scenario.c_str (), r.size, r.mem_mb, r.scenario.c_str (), r.size,
      r.server_cpu_percent, r.scenario.c_str (), r.size, r.server_mem_mb,
      r.scenario.c_str (), r.size, static_cast<double> (r.submitted), r.scenario.c_str (),
      r.size, static_cast<double> (r.completed), r.scenario.c_str (), r.size,
      static_cast<double> (r.errors), r.scenario.c_str (), r.size, static_cast<double> (r.blocked),
      r.scenario.c_str (), r.size, static_cast<double> (r.max_outstanding), r.scenario.c_str (),
      r.size, r.submit_wait_ms);
}

} // namespace zlink_c_bench

#endif
