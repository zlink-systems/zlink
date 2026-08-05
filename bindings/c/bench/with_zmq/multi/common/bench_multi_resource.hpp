#ifndef BENCH_MULTI_RESOURCE_HPP
#define BENCH_MULTI_RESOURCE_HPP

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

struct bench_multi_resource_metrics_t
{
    bool has_cpu_pct;
    double cpu_pct;
    bool has_mem_mb;
    double mem_mb;

    bench_multi_resource_metrics_t () :
        has_cpu_pct (false), cpu_pct (0.0), has_mem_mb (false), mem_mb (0.0)
    {
    }
};

struct server_queue_stats_t
{
    server_queue_stats_t () :
        has_send_queue_depth (false),
        send_queue_depth (0.0),
        has_recv_queue_depth (false),
        recv_queue_depth (0.0)
    {
    }

    bool has_send_queue_depth;
    double send_queue_depth;
    bool has_recv_queue_depth;
    double recv_queue_depth;
};

struct bench_multi_cpu_sample_t
{
    bool valid;
    unsigned long long ticks;
    std::chrono::steady_clock::time_point timestamp;

    bench_multi_cpu_sample_t () : valid (false), ticks (0), timestamp () {}
};

inline bool bench_multi_resource_metrics_enabled ()
{
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;

    const char *disabled = std::getenv ("PERF_DISABLE_RESOURCE_METRICS");
    if (disabled && std::strcmp (disabled, "1") == 0)
        cached = 0;
    else
        cached = 1;
    return cached == 1;
}

#if defined(_WIN32)
inline bool bench_multi_read_self_cpu_ticks (unsigned long long *ticks_out_)
{
    if (!ticks_out_)
        return false;

    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (!GetProcessTimes (GetCurrentProcess (), &creation, &exit_time, &kernel_time, &user_time)) {
        return false;
    }

    const unsigned long long kernel_ticks =
      (static_cast<unsigned long long> (kernel_time.dwHighDateTime) << 32)
      | static_cast<unsigned long long> (kernel_time.dwLowDateTime);
    const unsigned long long user_ticks =
      (static_cast<unsigned long long> (user_time.dwHighDateTime) << 32)
      | static_cast<unsigned long long> (user_time.dwLowDateTime);
    *ticks_out_ = kernel_ticks + user_ticks;
    return true;
}

inline bool bench_multi_read_self_mem_mb (double *mem_mb_out_)
{
    if (!mem_mb_out_)
        return false;

    PROCESS_MEMORY_COUNTERS counters;
    std::memset (&counters, 0, sizeof (counters));
    counters.cb = sizeof (counters);
    if (!GetProcessMemoryInfo (GetCurrentProcess (), &counters, sizeof (counters)))
        return false;

    *mem_mb_out_ = static_cast<double> (counters.WorkingSetSize) / (1024.0 * 1024.0);
    return true;
}
#else
inline bool bench_multi_read_self_cpu_ticks (unsigned long long *ticks_out_)
{
    if (!ticks_out_)
        return false;

    std::ifstream stat_file ("/proc/self/stat");
    if (!stat_file)
        return false;

    std::string line;
    if (!std::getline (stat_file, line))
        return false;

    const std::size_t rparen = line.rfind (')');
    if (rparen == std::string::npos || rparen + 2 >= line.size ())
        return false;

    std::istringstream iss (line.substr (rparen + 2));
    char state = '\0';
    if (!(iss >> state))
        return false;

    unsigned long long discard = 0;
    for (int i = 0; i < 10; ++i) {
        if (!(iss >> discard))
            return false;
    }

    unsigned long long utime = 0;
    unsigned long long stime = 0;
    if (!(iss >> utime >> stime))
        return false;

    *ticks_out_ = utime + stime;
    return true;
}

inline bool bench_multi_read_self_mem_mb (double *mem_mb_out_)
{
    if (!mem_mb_out_)
        return false;

    std::ifstream status_file ("/proc/self/status");
    if (!status_file)
        return false;

    std::string line;
    while (std::getline (status_file, line)) {
        if (line.compare (0, 6, "VmRSS:") != 0)
            continue;

        std::istringstream iss (line.substr (6));
        unsigned long long rss_kb = 0;
        if (!(iss >> rss_kb))
            return false;

        *mem_mb_out_ = static_cast<double> (rss_kb) / 1024.0;
        return true;
    }

    return false;
}
#endif

inline bench_multi_cpu_sample_t bench_multi_capture_cpu_sample ()
{
    bench_multi_cpu_sample_t sample;
    if (!bench_multi_resource_metrics_enabled ())
        return sample;
    if (!bench_multi_read_self_cpu_ticks (&sample.ticks))
        return sample;
    sample.timestamp = std::chrono::steady_clock::now ();
    sample.valid = true;
    return sample;
}

inline void bench_multi_fill_cpu_pct (const bench_multi_cpu_sample_t &start_,
                                      const bench_multi_cpu_sample_t &end_,
                                      bench_multi_resource_metrics_t *metrics_)
{
    if (!metrics_ || !start_.valid || !end_.valid)
        return;
    if (end_.ticks < start_.ticks)
        return;

#if defined(_WIN32)
    SYSTEM_INFO system_info;
    std::memset (&system_info, 0, sizeof (system_info));
    GetSystemInfo (&system_info);
    unsigned int nproc = system_info.dwNumberOfProcessors;
    if (nproc == 0)
        nproc = 1;
#else
    const long clk_tck = sysconf (_SC_CLK_TCK);
    if (clk_tck <= 0)
        return;
    long nproc = sysconf (_SC_NPROCESSORS_ONLN);
    if (nproc <= 0)
        nproc = 1;
#endif

    const double elapsed_sec =
      std::chrono::duration<double> (end_.timestamp - start_.timestamp).count ();
    if (elapsed_sec <= 0.0)
        return;

    const unsigned long long delta_ticks = end_.ticks - start_.ticks;
#if defined(_WIN32)
    const double cpu_sec = static_cast<double> (delta_ticks) / 10000000.0;
#else
    const double cpu_sec = static_cast<double> (delta_ticks) / static_cast<double> (clk_tck);
#endif
    const double denom = elapsed_sec * static_cast<double> (nproc);
    if (denom <= 0.0)
        return;

    metrics_->cpu_pct = (cpu_sec / denom) * 100.0;
    metrics_->has_cpu_pct = true;
}

inline bench_multi_resource_metrics_t
bench_multi_finish_resource_probe (const bench_multi_cpu_sample_t &start_)
{
    bench_multi_resource_metrics_t metrics;
    if (!bench_multi_resource_metrics_enabled ())
        return metrics;
    const bench_multi_cpu_sample_t end = bench_multi_capture_cpu_sample ();
    bench_multi_fill_cpu_pct (start_, end, &metrics);

    double mem_mb = 0.0;
    if (bench_multi_read_self_mem_mb (&mem_mb)) {
        metrics.mem_mb = mem_mb;
        metrics.has_mem_mb = true;
    }

    return metrics;
}

inline server_queue_stats_t sample_server_queue_stats (void *send_socket_, void *recv_socket_)
{
    (void) send_socket_;
    (void) recv_socket_;
    return server_queue_stats_t ();
}

inline void print_server_queue_metrics (const std::string &lib_name_,
                                        const std::string &pattern_,
                                        const std::string &transport_,
                                        size_t msg_size_,
                                        const server_queue_stats_t &stats_)
{
    if (stats_.has_send_queue_depth) {
        std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
                  << msg_size_ << ",server_send_queue_depth," << std::fixed << std::setprecision (2)
                  << stats_.send_queue_depth << std::endl;
    }
    if (stats_.has_recv_queue_depth) {
        std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
                  << msg_size_ << ",server_recv_queue_depth," << std::fixed << std::setprecision (2)
                  << stats_.recv_queue_depth << std::endl;
    }
}

inline bool parse_queue_probe_command (const std::string &line_, size_t *msg_size_out_)
{
    if (!msg_size_out_)
        return false;

    static const char k_prefix[] = "QUEUE,";
    if (line_.compare (0, sizeof (k_prefix) - 1, k_prefix) != 0)
        return false;

    const char *value = line_.c_str () + (sizeof (k_prefix) - 1);
    if (!value || *value == '\0')
        return false;

    errno = 0;
    char *end = NULL;
    const unsigned long parsed = std::strtoul (value, &end, 10);
    if (errno != 0 || end == value || !end || *end != '\0' || parsed == 0)
        return false;

    *msg_size_out_ = static_cast<size_t> (parsed);
    return true;
}

#endif
