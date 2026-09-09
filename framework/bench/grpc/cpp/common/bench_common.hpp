/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Shared measurement plumbing for the C++ with-grpc bench.
//
// Everything here is measurement scaffolding, never judgement: the shared
// aggregator (framework/bench/tools/) owns tables, medians, G5 and the spec 7.2
// ratios. This header only produces the `with-grpc-cell-v1` records it reads
// (FB-021) plus the spec 4 RESULT lines a human uses to eyeball one run.
#ifndef ZLINK_CPP_BENCH_WITH_GRPC_COMMON_HPP
#define ZLINK_CPP_BENCH_WITH_GRPC_COMMON_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace zlink_cpp_bench
{

// ---------------------------------------------------------------------------
// clocks
// ---------------------------------------------------------------------------

// Monotonic. WSL wall clock jumps by seconds; every duration in this bench is a
// steady_clock difference for that reason.
inline uint64_t now_ns ()
{
    return static_cast<uint64_t> (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
        .count ());
}

// ---------------------------------------------------------------------------
// spec 6 measurement header (29 bytes, identical layout in every language)
// ---------------------------------------------------------------------------

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
           | (static_cast<uint32_t> (src[2]) << 16)
           | (static_cast<uint32_t> (src[3]) << 24);
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
    uint32_t magic = 0;
    uint32_t run_id = 0;
    uint8_t phase = 0;
    uint32_t payload_size = 0;
    uint64_t seq = 0;
    uint64_t sent_ns = 0;
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

// ---------------------------------------------------------------------------
// protobuf BenchPayload{bytes body = 1} by hand
//
// FB-024: the raw ZLink row must put the same bytes on the wire as `zlink-c`,
// because formula 1 divides one by the other. The C bench hand-encodes field 1
// (`bench_zlink_client.cpp:130-140`); this does the same so the raw row never
// depends on which protobuf runtime a language happens to link.
// ---------------------------------------------------------------------------

inline size_t varint_size (size_t value)
{
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

inline unsigned char *write_varint (unsigned char *dst, size_t value)
{
    while (value >= 0x80) {
        *dst++ = static_cast<unsigned char> ((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *dst++ = static_cast<unsigned char> (value);
    return dst;
}

inline bool read_varint (const unsigned char *&p, const unsigned char *end, size_t *value)
{
    size_t result = 0;
    unsigned shift = 0;
    while (p < end && shift < sizeof (size_t) * 8) {
        const unsigned char byte = *p++;
        result |= static_cast<size_t> (byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

inline bool decode_bench_payload_body (const void *data,
                                       size_t size,
                                       const unsigned char **body,
                                       size_t *body_size)
{
    if (!data || !body || !body_size)
        return false;
    const unsigned char *p = static_cast<const unsigned char *> (data);
    const unsigned char *end = p + size;
    while (p < end) {
        size_t key = 0;
        if (!read_varint (p, end, &key))
            return false;
        const size_t field = key >> 3;
        const size_t wire_type = key & 0x07U;
        if (wire_type != 2)
            return false;
        size_t len = 0;
        if (!read_varint (p, end, &len) || static_cast<size_t> (end - p) < len)
            return false;
        if (field == 1) {
            *body = p;
            *body_size = len;
            return true;
        }
        p += len;
    }
    return false;
}

// Encodes BenchPayload{body} into `out` and stamps the spec 6 header into the
// body. Returns the offset of the body inside `out`.
inline size_t encode_bench_payload (std::vector<unsigned char> &out,
                                    size_t payload_size,
                                    uint32_t run_id,
                                    phase_t phase,
                                    uint64_t seq)
{
    const size_t body_size = std::max (payload_size, k_header_size);
    out.assign (1 + varint_size (body_size) + body_size, 0xab);
    out[0] = 0x0a;
    unsigned char *body = write_varint (out.data () + 1, body_size);
    std::memset (body, 0xab, body_size);
    stamp_payload (body, body_size, run_id, phase, seq);
    return static_cast<size_t> (body - out.data ());
}

// ---------------------------------------------------------------------------
// envelope headers (spec 3: header part + protobuf part, byte-identical to the
// C bench so that formula 1 divides two runs of the same experiment)
// ---------------------------------------------------------------------------

inline const char *request_envelope ()
{
    return "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}";
}

inline const char *response_envelope ()
{
    return "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}";
}

// ---------------------------------------------------------------------------
// latency
// ---------------------------------------------------------------------------

class latency_sampler_t
{
  public:
    explicit latency_sampler_t (size_t cap = 200000) : _cap (cap) { _samples.reserve (cap); }

    void add_us (double value)
    {
        _sum += value;
        ++_count;
        if (_samples.size () < _cap)
            _samples.push_back (value);
    }

    void reset ()
    {
        _sum = 0.0;
        _count = 0;
        _samples.clear ();
    }

    uint64_t count () const { return _count; }
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

// ---------------------------------------------------------------------------
// CPU accounting
//
// spec 5.1 / FB-023 / FB-032: a saturation instrument has to measure the
// execution resource USER CODE runs on. Two readings are taken for every cell so
// the choice can be made from evidence rather than assumed:
//
//   process cores  -- all threads of the client process, Core's native I/O
//                     threads included.
//   submit cores   -- only the threads this harness runs its own submit and
//                     completion loops on (CLOCK_THREAD_CPUTIME_ID).
//
// The difference between them is exactly "CPU that is not the client runtime",
// which is what made process cores the wrong instrument in node (FB-023) and in
// java (FB-032).
// ---------------------------------------------------------------------------

inline long logical_cores ()
{
    return std::max<long> (1, sysconf (_SC_NPROCESSORS_ONLN));
}

inline double process_cpu_seconds_self ()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return static_cast<double> (ru.ru_utime.tv_sec)
           + static_cast<double> (ru.ru_utime.tv_usec) / 1e6
           + static_cast<double> (ru.ru_stime.tv_sec)
           + static_cast<double> (ru.ru_stime.tv_usec) / 1e6;
}

inline double thread_cpu_seconds_self ()
{
    timespec ts {};
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0.0;
    return static_cast<double> (ts.tv_sec) + static_cast<double> (ts.tv_nsec) / 1e9;
}

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

inline double rss_mb_pid (int pid)
{
    const std::string path =
      pid > 0 ? "/proc/" + std::to_string (pid) + "/statm" : std::string ("/proc/self/statm");
    std::ifstream statm (path);
    long pages = 0;
    long resident = 0;
    statm >> pages >> resident;
    return static_cast<double> (resident) * static_cast<double> (sysconf (_SC_PAGESIZE)) / 1024.0
           / 1024.0;
}

inline double rss_mb () { return rss_mb_pid (0); }

// Counts the threads the process currently has. Reported so that "process cores
// exceeded the submit parallelism" can be read against how many threads existed
// to produce it.
inline int thread_count_self ()
{
    DIR *dir = opendir ("/proc/self/task");
    if (!dir)
        return 0;
    int count = 0;
    while (dirent *entry = readdir (dir)) {
        if (entry->d_name[0] != '.')
            ++count;
    }
    closedir (dir);
    return count;
}

// A CPU window over one measured segment. `submit_cpu_start` is filled by each
// driver from the threads it actually runs its submit loop on.
struct cpu_window_t
{
    double process_cpu_start = 0.0;
    double submit_cpu_start = 0.0;
    int server_pid = 0;
    double server_cpu_start = 0.0;
};

// ---------------------------------------------------------------------------
// tiny blocking HTTP/1.1 client for the server stats endpoints
// ---------------------------------------------------------------------------

inline std::optional<std::string> http_request (const std::string &host,
                                                int port,
                                                const std::string &method,
                                                const std::string &path,
                                                int timeout_ms = 5000)
{
    const int fd = ::socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return std::nullopt;
    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    ::setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons (static_cast<uint16_t> (port));
    if (::inet_pton (AF_INET, host.c_str (), &addr.sin_addr) != 1
        || ::connect (fd, reinterpret_cast<sockaddr *> (&addr), sizeof (addr)) != 0) {
        ::close (fd);
        return std::nullopt;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host
                      + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    size_t sent = 0;
    while (sent < req.size ()) {
        const ssize_t n = ::send (fd, req.data () + sent, req.size () - sent, 0);
        if (n <= 0) {
            ::close (fd);
            return std::nullopt;
        }
        sent += static_cast<size_t> (n);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv (fd, buffer, sizeof (buffer), 0);
        if (n <= 0)
            break;
        response.append (buffer, static_cast<size_t> (n));
    }
    ::close (fd);
    const size_t body = response.find ("\r\n\r\n");
    if (body == std::string::npos)
        return std::nullopt;
    return response.substr (body + 4);
}

// The stats payload the three servers publish. Flat JSON with numeric fields
// only, so a hand-rolled reader is enough and the bench pulls in no JSON
// dependency for four numbers.
struct server_snapshot_t
{
    long long active_messages = 0;
    long long any_phase_messages = 0;
    long long errors = 0;
    double mean_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double cpu_seconds = 0.0;
    double rss_mb = 0.0;
};

inline double json_number (const std::string &text, const char *key, double fallback = 0.0)
{
    const std::string needle = std::string ("\"") + key + "\":";
    const size_t at = text.find (needle);
    if (at == std::string::npos)
        return fallback;
    return std::strtod (text.c_str () + at + needle.size (), nullptr);
}

inline std::optional<server_snapshot_t> fetch_stats (const std::string &host, int port)
{
    const auto body = http_request (host, port, "GET", "/bench/stats");
    if (!body)
        return std::nullopt;
    server_snapshot_t s;
    s.active_messages = static_cast<long long> (json_number (*body, "activeMessages"));
    s.any_phase_messages = static_cast<long long> (json_number (*body, "anyPhaseMessages"));
    s.errors = static_cast<long long> (json_number (*body, "errors"));
    s.mean_us = json_number (*body, "meanMicros");
    s.p95_us = json_number (*body, "p95Micros");
    s.p99_us = json_number (*body, "p99Micros");
    s.cpu_seconds = json_number (*body, "cpuSeconds");
    s.rss_mb = json_number (*body, "workingSetMb");
    return s;
}

inline bool reset_stats (const std::string &host, int port)
{
    return http_request (host, port, "POST", "/bench/reset").has_value ();
}

inline bool wait_ready (const std::string &host, int port, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        if (http_request (host, port, "GET", "/ready", 500))
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    return false;
}

// ---------------------------------------------------------------------------
// one measured cell, in the shape framework/bench/tools reads (FB-021)
// ---------------------------------------------------------------------------

struct cell_t
{
    std::string implementation;
    std::string pattern;
    size_t payload_size = 0;

    double throughput_per_second = 0.0;
    double bandwidth_mb_s = 0.0;
    double latency_mean_ms = 0.0;
    double latency_p95_ms = 0.0;
    double latency_p99_ms = 0.0;
    double client_cpu_percent = 0.0;
    double client_memory_mb = 0.0;
    double server_cpu_percent = 0.0;
    double server_memory_mb = 0.0;

    // spec 5.1 / FB-023: both readings and the declared instrument.
    std::string client_saturation_metric = "submit_thread_cores";
    double submit_thread_cores = 0.0;
    double client_cores = 0.0;
    double non_submit_cores = 0.0;
    double client_parallelism_ceiling = 1.0;
    long logical_cores_value = 0;
    int client_threads = 0;

    // FB-017 / G8
    long long peak_in_flight = 0;
    long long request_window = 0;
    long long abandoned = 0;

    // FB-008 / spec 3
    std::optional<double> drain_ms;
    std::optional<bool> drain_bound_hit;
    // spec 5 / G3
    std::optional<long long> server_received_at_close;
    std::optional<long long> server_received_post_drain;

    long long errors = 0;
    long long submitted = 0;
    long long completed = 0;
    long long header_validation_failures = 0; // G2

    std::vector<double> warmup_segment_throughput; // spec 8.2 evidence

    bool contaminated = false;
    std::string contamination_reason;

    std::string unit () const
    {
        return pattern == "send-saturation" ? "KMSG/s" : "KOPS";
    }

    std::string scenario () const { return implementation + "-" + pattern; }
};

inline std::string json_escape (const std::string &value)
{
    std::string out;
    for (const char c : value) {
        if (c == '"' || c == '\\')
            out += std::string ("\\") + c;
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

inline std::string json_number_str (double value)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.6f", value);
    return buffer;
}

inline void write_cells_json (const std::string &path,
                              const std::vector<cell_t> &cells,
                              const std::map<std::string, std::string> &metadata)
{
    std::ofstream out (path);
    out << "{\n  \"schema\": \"with-grpc-cell-v1\",\n  \"metadata\": {\n";
    size_t i = 0;
    for (const auto &entry : metadata) {
        out << "    \"" << json_escape (entry.first) << "\": \"" << json_escape (entry.second)
            << "\"" << (++i < metadata.size () ? "," : "") << "\n";
    }
    out << "  },\n  \"cells\": [\n";
    for (size_t c = 0; c < cells.size (); ++c) {
        const cell_t &cell = cells[c];
        out << "    {\n";
        out << "      \"implementation\": \"" << cell.implementation << "\",\n";
        out << "      \"pattern\": \"" << cell.pattern << "\",\n";
        out << "      \"payload_size\": " << cell.payload_size << ",\n";
        out << "      \"contaminated\": " << (cell.contaminated ? "true" : "false") << ",\n";
        out << "      \"contamination_reason\": "
            << (cell.contamination_reason.empty ()
                  ? std::string ("null")
                  : "\"" + json_escape (cell.contamination_reason) + "\"")
            << ",\n";
        out << "      \"throughput_per_second\": " << json_number_str (cell.throughput_per_second)
            << ",\n";
        out << "      \"bandwidth_mb_s\": " << json_number_str (cell.bandwidth_mb_s) << ",\n";
        out << "      \"latency_mean_ms\": " << json_number_str (cell.latency_mean_ms) << ",\n";
        out << "      \"latency_p95_ms\": " << json_number_str (cell.latency_p95_ms) << ",\n";
        out << "      \"latency_p99_ms\": " << json_number_str (cell.latency_p99_ms) << ",\n";
        out << "      \"client_cpu_percent\": " << json_number_str (cell.client_cpu_percent)
            << ",\n";
        out << "      \"client_memory_mb\": " << json_number_str (cell.client_memory_mb) << ",\n";
        out << "      \"server_cpu_percent\": " << json_number_str (cell.server_cpu_percent)
            << ",\n";
        out << "      \"server_memory_mb\": " << json_number_str (cell.server_memory_mb) << ",\n";
        out << "      \"client_saturation_metric\": \"" << cell.client_saturation_metric
            << "\",\n";
        out << "      \"submit_thread_cores\": " << json_number_str (cell.submit_thread_cores)
            << ",\n";
        out << "      \"client_cores\": " << json_number_str (cell.client_cores) << ",\n";
        out << "      \"non_submit_cores\": " << json_number_str (cell.non_submit_cores) << ",\n";
        out << "      \"client_parallelism_ceiling\": "
            << json_number_str (cell.client_parallelism_ceiling) << ",\n";
        out << "      \"logical_cores\": " << cell.logical_cores_value << ",\n";
        out << "      \"client_threads\": " << cell.client_threads << ",\n";
        out << "      \"peak_in_flight\": " << cell.peak_in_flight << ",\n";
        out << "      \"request_window\": " << cell.request_window << ",\n";
        out << "      \"abandoned\": " << cell.abandoned << ",\n";
        if (cell.drain_ms)
            out << "      \"drain_ms\": " << json_number_str (*cell.drain_ms) << ",\n";
        if (cell.drain_bound_hit)
            out << "      \"drain_bound_hit\": " << (*cell.drain_bound_hit ? "true" : "false")
                << ",\n";
        if (cell.server_received_at_close)
            out << "      \"server_received_at_close\": " << *cell.server_received_at_close
                << ",\n";
        if (cell.server_received_post_drain)
            out << "      \"server_received_post_drain\": " << *cell.server_received_post_drain
                << ",\n";
        out << "      \"errors\": " << cell.errors << ",\n";
        out << "      \"submitted\": " << cell.submitted << ",\n";
        out << "      \"completed\": " << cell.completed << ",\n";
        out << "      \"header_validation_failures\": " << cell.header_validation_failures
            << ",\n";
        out << "      \"warmup_segment_throughput\": [";
        for (size_t s = 0; s < cell.warmup_segment_throughput.size (); ++s)
            out << (s ? ", " : "") << json_number_str (cell.warmup_segment_throughput[s]);
        out << "]\n";
        out << "    }" << (c + 1 < cells.size () ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

// spec 4 RESULT lines. Convenience for reading one run; never the basis of a
// judgement (spec 7.4, FB-020) -- the aggregator output is.
inline void print_result_lines (std::FILE *out, const cell_t &cell)
{
    const char *s = cell.scenario ().c_str ();
    const std::string scenario = cell.scenario ();
    const size_t size = cell.payload_size;
    (void) s;
    auto line = [&] (const char *metric, double value) {
        std::fprintf (out, "RESULT,current,%s,local,%zu,%s,%.3f\n", scenario.c_str (), size,
                      metric, value);
    };
    line ("throughput", cell.throughput_per_second);
    line ("bandwidth", cell.bandwidth_mb_s);
    line ("latency", cell.latency_mean_ms);
    line ("latency_p95", cell.latency_p95_ms);
    line ("latency_p99", cell.latency_p99_ms);
    line ("client_cpu_percent", cell.client_cpu_percent);
    line ("client_memory_mb", cell.client_memory_mb);
    line ("server_cpu_percent", cell.server_cpu_percent);
    line ("server_memory_mb", cell.server_memory_mb);
}

inline void print_table_row (std::FILE *out, const cell_t &cell)
{
    std::fprintf (out,
                  "      | %-26s | %-8zu | %10.3f %-7s | %9.3f MB/s | %10.3f ms | %10.3f ms |"
                  " %10.3f ms | %9.1f%% | %8.1f MB | %9.1f%% | %8.1f MB |\n",
                  cell.scenario ().c_str (), cell.payload_size,
                  cell.throughput_per_second / 1000.0, cell.unit ().c_str (),
                  cell.bandwidth_mb_s, cell.latency_mean_ms, cell.latency_p95_ms,
                  cell.latency_p99_ms, cell.client_cpu_percent, cell.client_memory_mb,
                  cell.server_cpu_percent, cell.server_memory_mb);
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------

inline std::string arg_value (int argc, char **argv, const char *name, const char *fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp (argv[i], name) == 0)
            return argv[i + 1];
    }
    return fallback;
}

inline std::vector<size_t> parse_sizes (const std::string &raw)
{
    std::vector<size_t> sizes;
    std::stringstream stream (raw);
    std::string token;
    while (std::getline (stream, token, ',')) {
        const long value = std::atol (token.c_str ());
        if (value > 0)
            sizes.push_back (static_cast<size_t> (value));
    }
    if (sizes.empty ())
        sizes = {1024, 4096};
    return sizes;
}

inline std::vector<std::string> split_csv (const std::string &raw)
{
    std::vector<std::string> parts;
    std::stringstream stream (raw);
    std::string token;
    while (std::getline (stream, token, ','))
        if (!token.empty ())
            parts.push_back (token);
    return parts;
}

inline bool contains (const std::vector<std::string> &values, const std::string &needle)
{
    return std::find (values.begin (), values.end (), needle) != values.end ();
}

inline double loadavg1 ()
{
    std::ifstream in ("/proc/loadavg");
    double value = 0.0;
    in >> value;
    return value;
}

inline std::string cpu_model ()
{
    std::ifstream in ("/proc/cpuinfo");
    std::string line;
    while (std::getline (in, line)) {
        if (line.rfind ("model name", 0) == 0) {
            const size_t colon = line.find (':');
            if (colon != std::string::npos)
                return line.substr (line.find_first_not_of (" \t", colon + 1));
        }
    }
    return "unknown";
}

} // namespace zlink_cpp_bench

#endif
