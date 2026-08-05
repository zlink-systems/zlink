#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "perf_multi_metric_header.hpp"
#include "bench_multi_resource.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_DEALER_DEALER";
static const char *k_token = "dealer_dealer";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_DEALER;

using perf_multi_client::normalize_latency_stats;

enum recv_result_t
{
    recv_ok = 0,
    recv_none = 1,
    recv_fatal = 2
};

inline bool decode_and_match_header (const zlink_msg_t *msg,
                                     size_t expected_msg_size,
                                     uint32_t expected_run_id,
                                     perf_multi_metric::phase_t expected_phase,
                                     perf_multi_metric::header_t *header_out)
{
    if (!msg || !header_out)
        return false;

    if (!perf_multi_metric::decode_payload_header (zlink_msg_data (const_cast<zlink_msg_t *> (msg)),
                                                   zlink_msg_size (const_cast<zlink_msg_t *> (msg)),
                                                   header_out)) {
        return false;
    }

    return header_out->magic == perf_multi_metric::k_magic && header_out->run_id == expected_run_id
           && header_out->phase == static_cast<uint32_t> (expected_phase)
           && header_out->msg_size == static_cast<uint32_t> (expected_msg_size);
}

inline recv_result_t receive_one_message (void *server,
                                          int flags,
                                          size_t expected_msg_size,
                                          uint32_t expected_run_id,
                                          perf_multi_metric::phase_t expected_phase,
                                          bool count_message,
                                          bool collect_latency,
                                          long *message_count,
                                          double *lat_sum,
                                          long *lat_count,
                                          bench_latency_sampler_t *lat_samples)
{
    if (!server)
        return recv_fatal;

    zlink_routing_id_t source_rid;
    source_rid.size = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_recv_result_t rc = ::zlink_std_compat_recv (
      server, &source_rid, &parts, &part_count, static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        const int err = zlink_errno ();
        if (err == EAGAIN || err == EINTR || err == ETIMEDOUT)
            return recv_none;
        return recv_fatal;
    }

    if (part_count < 1) {
        if (parts) {
            zlink_multipart_close (parts, part_count);
        }
        return recv_fatal;
    }

    perf_multi_metric::header_t header;
    const bool matched = decode_and_match_header (&parts[0], expected_msg_size, expected_run_id,
                                                  expected_phase, &header);

    if (matched && count_message && message_count)
        (*message_count)++;

    if (matched && collect_latency && lat_sum && lat_count) {
        const uint64_t now_us = perf_multi_metric::now_us ();
        if (header.sent_ts_us > 0 && now_us >= header.sent_ts_us) {
            const double sample_us = static_cast<double> (now_us - header.sent_ts_us);
            *lat_sum += sample_us;
            (*lat_count)++;
            if (lat_samples)
                lat_samples->add (sample_us);
        }
    }

    zlink_multipart_close (parts, part_count);
    return recv_ok;
}

inline bool drain_non_blocking_messages (void *server,
                                         size_t expected_msg_size,
                                         uint32_t expected_run_id,
                                         perf_multi_metric::phase_t expected_phase,
                                         bool count_message,
                                         bool collect_latency,
                                         long *message_count,
                                         double *lat_sum,
                                         long *lat_count,
                                         bench_latency_sampler_t *lat_samples)
{
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        const recv_result_t status = receive_one_message (
          server, ZLINK_DONTWAIT, expected_msg_size, expected_run_id, expected_phase, count_message,
          collect_latency, message_count, lat_sum, lat_count, lat_samples);
        if (status == recv_none)
            break;
        if (status == recv_fatal)
            return false;
    }
    return true;
}

inline bool run_receive_window (void *server,
                                size_t expected_msg_size,
                                uint32_t expected_run_id,
                                perf_multi_metric::phase_t expected_phase,
                                double duration_seconds,
                                bool count_message,
                                bool collect_latency,
                                long *message_count,
                                double *lat_sum,
                                long *lat_count,
                                bench_latency_sampler_t *lat_samples)
{
    if (!server)
        return false;
    if (duration_seconds <= 0.0)
        return true;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (duration_seconds));

    while (!perf_stop_requested ().load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        const recv_result_t status = receive_one_message (
          server, 0, expected_msg_size, expected_run_id, expected_phase, count_message,
          collect_latency, message_count, lat_sum, lat_count, lat_samples);
        if (status == recv_none)
            continue;
        if (status == recv_fatal)
            return false;

        if (!drain_non_blocking_messages (server, expected_msg_size, expected_run_id,
                                          expected_phase, count_message, collect_latency,
                                          message_count, lat_sum, lat_count, lat_samples)) {
            return false;
        }
    }

    return true;
}

inline bool run_one_size_benchmark (void *server,
                                    const multi_bench_settings_t &settings,
                                    size_t msg_size,
                                    uint32_t run_id,
                                    const std::string &lib_name,
                                    const std::string &transport)
{
    const double warmup_s = static_cast<double> (std::max (0, settings.warmup_seconds));
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));
    const bool warmup_ok =
      run_receive_window (server, msg_size, run_id, perf_multi_metric::phase_warmup, warmup_s,
                          false, false, NULL, NULL, NULL, NULL);
    if (!warmup_ok) {
        return false;
    }

    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;

    const bool active_ok =
      run_receive_window (server, msg_size, run_id, perf_multi_metric::phase_active, active_s, true,
                          true, &recv_count, &lat_sum, &lat_count, &lat_samples);
    if (!active_ok) {
        return false;
    }

    if (recv_count <= 0 || lat_count <= 0)
        return false;

    bench_latency_stats_t latency;
    normalize_latency_stats (lat_sum, lat_count, &lat_samples, &latency);

    const double throughput = static_cast<double> (recv_count)
                              / static_cast<double> (std::max (1, settings.duration_seconds));

    print_result (lib_name, k_pattern, transport, msg_size, throughput, latency.mean_us,
                  latency.p95_us, latency.p99_us);

    const server_queue_stats_t queue_stats = sample_server_queue_stats (server, server);
    print_server_queue_metrics (lib_name, k_pattern, transport, msg_size, queue_stats);

    return true;
}

inline bool parse_start_command (const std::string &line, size_t *msg_size_out)
{
    static const char prefix[] = "START,";
    if (!msg_size_out || line.compare (0, sizeof (prefix) - 1, prefix) != 0) {
        return false;
    }

    const char *value = line.c_str () + (sizeof (prefix) - 1);
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (!end || *end != '\0' || parsed == 0)
        return false;

    *msg_size_out = static_cast<size_t> (parsed);
    return true;
}

inline void print_server_resource_metrics (const std::string &lib_name,
                                           const std::string &transport,
                                           const std::vector<size_t> &sizes,
                                           const bench_multi_resource_metrics_t &metrics)
{
    for (size_t i = 0; i < sizes.size (); ++i) {
        if (metrics.has_cpu_pct) {
            std::cout << "RESULT," << lib_name << "," << k_pattern << "," << transport << ","
                      << sizes[i] << ",server_cpu_pct," << std::fixed << std::setprecision (2)
                      << metrics.cpu_pct << std::endl;
        }
        if (metrics.has_mem_mb) {
            std::cout << "RESULT," << lib_name << "," << k_pattern << "," << transport << ","
                      << sizes[i] << ",server_mem_mb," << std::fixed << std::setprecision (2)
                      << metrics.mem_mb << std::endl;
        }
    }
}

inline int run_server_benchmark (const std::string &lib_name, const std::string &transport)
{
    set_perf_multi_pattern_env (k_pattern);

    if (!is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }

    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    void *server = zlink_socket (ctx.get (), k_server_socket_type);
    if (!server)
        return 1;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    apply_benchmark_socket_options (server, settings.hwm, transport);

    if (!setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint =
      bind_server_endpoint (server, transport, lib_name + std::string ("_") + k_token + "_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }

    perf_stop_requested ().store (false, std::memory_order_release);
    install_perf_signal_handlers ();
    std::mutex start_sync;
    std::condition_variable start_cv;
    std::set<size_t> pending_start_sizes;

    std::thread stdin_watcher ([&] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t start_size = 0;
            if (parse_start_command (line, &start_size)) {
                {
                    std::lock_guard<std::mutex> lock (start_sync);
                    pending_start_sizes.insert (start_size);
                }
                start_cv.notify_all ();
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                start_cv.notify_all ();
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
        start_cv.notify_all ();
    });

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);

    std::cout << "READY," << endpoint << std::endl;

    const bench_multi_cpu_sample_t sample_start = bench_multi_capture_cpu_sample ();
    bool ok = true;
    for (size_t si = 0; si < sizes.size (); ++si) {
        if (perf_stop_requested ().load (std::memory_order_acquire)) {
            ok = false;
            break;
        }

        {
            std::unique_lock<std::mutex> lock (start_sync);
            const bool started = start_cv.wait_for (
              lock, std::chrono::milliseconds (std::max (1, settings.connect_ready_timeout_ms)),
              [&] () {
                  return perf_stop_requested ().load (std::memory_order_acquire)
                         || pending_start_sizes.count (sizes[si]) != 0;
              });
            if (!started || perf_stop_requested ().load (std::memory_order_acquire)) {
                ok = false;
                break;
            }
            pending_start_sizes.erase (sizes[si]);
        }

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        if (!run_one_size_benchmark (server, settings, sizes[si], run_id, lib_name, transport)) {
            ok = false;
            break;
        }
    }

    const bench_multi_resource_metrics_t metrics = bench_multi_finish_resource_probe (sample_start);
    print_server_resource_metrics (lib_name, transport, sizes, metrics);

    perf_stop_requested ().store (true, std::memory_order_release);
    start_cv.notify_all ();
    if (stdin_watcher.joinable ())
        stdin_watcher.join ();
    zlink_close (server);

    return ok ? 0 : 1;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    return run_server_benchmark (lib_name, transport);
}
