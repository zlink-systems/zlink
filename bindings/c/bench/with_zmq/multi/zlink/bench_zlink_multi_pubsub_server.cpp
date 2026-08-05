#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_metric_header.hpp"
#include "bench_multi_resource.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_PUBSUB";
static const char *k_token = "pubsub";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_PUB;
static const bool k_server_has_routing_id = false;
static const char *k_server_routing_id = "SERVER";
static const uint32_t k_metric_run_id = 1U;
static const char *k_pubsub_topic = "bench";

static std::atomic<bool> g_queue_probe_pending (false);
static std::atomic<size_t> g_queue_probe_size (0);
static std::atomic<int> g_debug_pub_logs (0);
std::mutex g_start_sync;
std::condition_variable g_start_cv;
size_t g_start_requested_size = 0;

inline void request_queue_probe (size_t msg_size)
{
    if (msg_size == 0)
        return;
    g_queue_probe_size.store (msg_size, std::memory_order_release);
    g_queue_probe_pending.store (true, std::memory_order_release);
}

inline void emit_requested_queue_probe (const std::string &lib_name,
                                        const std::string &transport,
                                        void *send_socket,
                                        void *recv_socket)
{
    if (!g_queue_probe_pending.exchange (false, std::memory_order_acq_rel))
        return;

    const size_t msg_size = g_queue_probe_size.load (std::memory_order_acquire);
    if (msg_size == 0 || !send_socket || !recv_socket)
        return;

    const server_queue_stats_t queue_stats = sample_server_queue_stats (send_socket, recv_socket);
    print_server_queue_metrics (lib_name, k_pattern, transport, msg_size, queue_stats);
}

bool parse_start_command (const std::string &line, size_t *msg_size_out)
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

bool wait_for_start_signal (size_t msg_size, int timeout_ms)
{
    std::unique_lock<std::mutex> lock (g_start_sync);
    if (g_start_requested_size == msg_size) {
        g_start_requested_size = 0;
        return true;
    }

    const bool signaled = g_start_cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms > 0 ? timeout_ms : 1), [msg_size] () {
          return perf_stop_requested ().load (std::memory_order_acquire)
                 || g_start_requested_size == msg_size;
      });
    if (!signaled || g_start_requested_size != msg_size) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-server] start gate timeout size=" << msg_size << std::endl;
        }
        return false;
    }

    g_start_requested_size = 0;
    return true;
}

inline bool publish_once (void *server,
                          std::vector<char> &payload,
                          size_t current_msg_size,
                          perf_multi_metric::phase_t phase,
                          uint64_t *seq)
{
    if (current_msg_size == 0)
        return true;
    if (!seq)
        return false;

    const size_t send_size =
      std::min (payload.size (), std::max<size_t> (static_cast<size_t> (1), current_msg_size));
    if (send_size < perf_multi_metric::header_size ())
        return false;
    if (!perf_multi_metric::stamp_payload (payload.data (), send_size, k_metric_run_id, phase,
                                           current_msg_size, (*seq)++,
                                           perf_multi_metric::now_us ())) {
        return false;
    }

    zlink_msg_t payload_part;
    if (zlink_msg_init_size (&payload_part, send_size) != 0)
        return false;
    std::memcpy (zlink_msg_data (&payload_part), payload.data (), send_size);

    if (::zlink_std_compat_publish (server, k_pubsub_topic, &payload_part, 1, ZLINK_DONTWAIT)
        == ZLINK_SUBMIT_OK) {
        if (bench_debug_enabled ()
            && g_debug_pub_logs.fetch_add (1, std::memory_order_acq_rel) < 8) {
            std::cerr << "[multi-pubsub-server] publish ok phase="
                      << static_cast<unsigned int> (phase) << " size=" << current_msg_size
                      << " seq=" << (*seq - 1) << std::endl;
        }
        return true;
    }

    zlink_msg_close (&payload_part);

    const int err = zlink_errno ();
    if (err == EAGAIN) {
        if (bench_debug_enabled ()
            && g_debug_pub_logs.fetch_add (1, std::memory_order_acq_rel) < 8) {
            std::cerr << "[multi-pubsub-server] publish blocked err=" << err
                      << " phase=" << static_cast<unsigned int> (phase)
                      << " size=" << current_msg_size << std::endl;
        }
        if (perf_socket_poll (NULL, 0, 1) < 0 && zlink_errno () != EINTR)
            return false;
        return true;
    }

    return perf_stop_requested ().load (std::memory_order_acquire);
}

inline size_t resolve_max_size (const std::vector<size_t> &sizes)
{
    size_t max_size = 64;
    for (size_t i = 0; i < sizes.size (); ++i) {
        if (sizes[i] > max_size)
            max_size = sizes[i];
    }
    return max_size;
}

struct one_way_phase_t
{
    one_way_phase_t (size_t msg_size_,
                     perf_multi_metric::phase_t phase_,
                     std::chrono::steady_clock::duration duration_,
                     bool send_active_) :
        msg_size (msg_size_), phase (phase_), duration (duration_), send_active (send_active_)
    {
    }

    size_t msg_size;
    perf_multi_metric::phase_t phase;
    std::chrono::steady_clock::duration duration;
    bool send_active;
};

inline void append_one_way_phase (std::vector<one_way_phase_t> *phases,
                                  size_t msg_size,
                                  perf_multi_metric::phase_t phase,
                                  double seconds,
                                  bool send_active)
{
    if (!phases || seconds <= 0.0)
        return;
    phases->push_back (
      one_way_phase_t (msg_size, phase,
                       std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                         std::chrono::duration<double> (seconds)),
                       send_active));
}

inline std::vector<one_way_phase_t> build_one_way_phases (const multi_bench_settings_t &settings,
                                                          const std::vector<size_t> &msg_sizes)
{
    std::vector<one_way_phase_t> phases;
    if (msg_sizes.empty ())
        return phases;

    const double warmup_s = static_cast<double> (std::max (0, settings.warmup_seconds));
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        const size_t msg_size = msg_sizes[i];
        append_one_way_phase (&phases, msg_size, perf_multi_metric::phase_warmup, warmup_s, true);
        append_one_way_phase (&phases, msg_size, perf_multi_metric::phase_active, active_s, true);
    }

    return phases;
}

inline void print_server_metrics (const std::string &lib_name,
                                  const std::string &transport,
                                  const std::vector<size_t> &sizes,
                                  const bench_multi_resource_metrics_t &metrics,
                                  const server_queue_stats_t &queue_stats)
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
        print_server_queue_metrics (lib_name, k_pattern, transport, sizes[i], queue_stats);
    }
}

inline bool run_server_loop (void *server,
                             const multi_bench_settings_t &settings,
                             const std::vector<size_t> &msg_sizes,
                             std::vector<char> *payload,
                             const std::string &lib_name,
                             const std::string &transport)
{
    if (!server || !payload)
        return false;

    const std::vector<one_way_phase_t> phases = build_one_way_phases (settings, msg_sizes);
    size_t phase_index = 0;
    auto phase_deadline = std::chrono::steady_clock::time_point ();
    bool phase_started = false;
    size_t current_phase_msg_size = 0;
    perf_multi_metric::phase_t current_phase = perf_multi_metric::phase_warmup;
    uint64_t phase_seq = 1;

    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        emit_requested_queue_probe (lib_name, transport, server, server);

        if (!phases.empty ()) {
            const auto now = std::chrono::steady_clock::now ();
            while (phase_started && phase_index < phases.size () && now >= phase_deadline) {
                ++phase_index;
                phase_started = false;
            }

            if (phase_index >= phases.size ()) {
                // The benchmark phases are fully scripted for PUBSUB, so once
                // warmup/active is complete there is nothing left to wait for.
                // Exiting here avoids an extra STOP-driven shutdown edge.
                return true;
            }

            if (phases[phase_index].msg_size != current_phase_msg_size
                || phases[phase_index].phase != current_phase) {
                const bool new_size = phases[phase_index].msg_size != current_phase_msg_size;
                current_phase_msg_size = phases[phase_index].msg_size;
                current_phase = phases[phase_index].phase;
                phase_seq = 1;
                if (new_size
                    && !wait_for_start_signal (current_phase_msg_size,
                                               settings.connect_ready_timeout_ms)) {
                    return false;
                }
                phase_started = false;
            }

            if (!phase_started) {
                phase_deadline = std::chrono::steady_clock::now () + phases[phase_index].duration;
                phase_started = true;
            }

            if (!phases[phase_index].send_active) {
                const long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                            phase_deadline - std::chrono::steady_clock::now ())
                                            .count ();
                const long wait_ms = remaining_ms > 0 ? remaining_ms : 0;
                if (perf_socket_poll (NULL, 0, wait_ms) < 0 && zlink_errno () != EINTR) {
                    return false;
                }
                continue;
            }

            if (!publish_once (server, *payload, phases[phase_index].msg_size,
                               phases[phase_index].phase, &phase_seq)) {
                return false;
            }
            continue;
        }

        current_phase = perf_multi_metric::phase_active;
        current_phase_msg_size = payload->size ();
        if (!publish_once (server, *payload, payload->size (), perf_multi_metric::phase_active,
                           &phase_seq)) {
            return false;
        }
    }

    return true;
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
    const int linger_ms = 0;
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    apply_benchmark_hwm (server, settings.hwm);
    if (k_server_has_routing_id) {
        zlink_set_routing_id (server, k_server_routing_id, std::strlen (k_server_routing_id));
    }

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
    g_queue_probe_pending.store (false, std::memory_order_release);
    g_queue_probe_size.store (0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock (g_start_sync);
        g_start_requested_size = 0;
    }
    install_perf_signal_handlers ();

    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t queue_size = 0;
            size_t start_size = 0;
            if (parse_queue_probe_command (line, &queue_size)) {
                request_queue_probe (queue_size);
                continue;
            }
            if (parse_start_command (line, &start_size)) {
                std::lock_guard<std::mutex> lock (g_start_sync);
                g_start_requested_size = start_size;
                g_start_cv.notify_all ();
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                g_start_cv.notify_all ();
                return;
            }
        }
        if (bench_debug_enabled ())
            std::cerr << "[multi-pubsub-server] stdin watcher eof" << std::endl;
        perf_stop_requested ().store (true, std::memory_order_release);
        g_start_cv.notify_all ();
    });

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);

    const size_t max_size = resolve_max_size (sizes);
    std::vector<char> payload (
      std::max<size_t> (static_cast<size_t> (1024),
                        std::max<size_t> (max_size, perf_multi_metric::header_size ())),
      's');

    const bench_multi_cpu_sample_t sample_start = bench_multi_capture_cpu_sample ();

    std::cout << "READY," << endpoint << std::endl;

    const bool loop_ok = run_server_loop (server, settings, sizes, &payload, lib_name, transport);

    perf_stop_requested ().store (true, std::memory_order_release);
    g_start_cv.notify_all ();
    if (stdin_watcher.joinable ()) {
        stdin_watcher.join ();
    }

    const bench_multi_resource_metrics_t metrics = bench_multi_finish_resource_probe (sample_start);
    const server_queue_stats_t queue_stats = sample_server_queue_stats (server, server);
    print_server_metrics (lib_name, transport, sizes, metrics, queue_stats);

    zlink_close (server);

    return loop_ok ? 0 : 1;
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
