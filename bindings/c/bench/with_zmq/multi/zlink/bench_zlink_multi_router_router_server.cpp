#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "bench_multi_resource.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_ROUTER_ROUTER";
static const char *k_token = "router_router";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_ROUTER;
static const bool k_server_has_routing_id = true;
static const char *k_server_routing_id = "SERVER";

static std::atomic<bool> g_queue_probe_pending (false);
static std::atomic<size_t> g_queue_probe_size (0);

inline void request_queue_probe (size_t msg_size)
{
    if (msg_size == 0)
        return;

    g_queue_probe_size.store (msg_size, std::memory_order_release);
    g_queue_probe_pending.store (true, std::memory_order_release);
}

inline void
emit_requested_queue_probe (const std::string &lib_name, const std::string &transport, void *server)
{
    if (!g_queue_probe_pending.exchange (false, std::memory_order_acq_rel))
        return;

    const size_t msg_size = g_queue_probe_size.load (std::memory_order_acquire);
    if (msg_size == 0 || !server)
        return;

    const server_queue_stats_t queue_stats = sample_server_queue_stats (server, server);
    print_server_queue_metrics (lib_name, k_pattern, transport, msg_size, queue_stats);
}

inline bool relay_router_once (void *server)
{
    zlink_routing_id_t source_rid;
    source_rid.size = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_recv_result_t rc =
      ::zlink_std_compat_recv (server, &source_rid, &parts, &part_count, 0);
    if (rc != ZLINK_RECV_OK) {
        const int err = zlink_errno ();
        return err == EAGAIN || err == EINTR;
    }

    if (part_count == 0 || !parts) {
        zlink_msg_t empty_part;
        if (zlink_msg_init_size (&empty_part, 0) != 0)
            return false;

        const zlink_submit_result_t send_rc =
          ::zlink_std_compat_send_rid (server, &source_rid, &empty_part, 1, 0);
        if (send_rc == ZLINK_SUBMIT_OK)
            return true;

        const int err = zlink_errno ();
        zlink_msg_close (&empty_part);
        return err == EAGAIN || err == EINTR;
    }

    const zlink_submit_result_t send_rc =
      ::zlink_std_compat_send_rid (server, &source_rid, parts, part_count, 0);
    if (send_rc == ZLINK_SUBMIT_OK)
        return true;

    const int err = zlink_errno ();
    zlink_multipart_close (parts, part_count);
    return err == EAGAIN || err == EINTR;
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

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];

    set_perf_multi_pattern_env (k_pattern);
    if (!perf_multi_client::is_supported_transport (transport)) {
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
    if (k_server_has_routing_id && k_server_routing_id) {
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
    install_perf_signal_handlers ();

    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t queue_size = 0;
            if (parse_queue_probe_command (line, &queue_size)) {
                request_queue_probe (queue_size);
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
    });
    stdin_watcher.detach ();

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);

    const bench_multi_cpu_sample_t sample_start = bench_multi_capture_cpu_sample ();

    std::cout << "READY," << endpoint << std::endl;

    bool loop_ok = true;
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        emit_requested_queue_probe (lib_name, transport, server);
        if (!relay_router_once (server)) {
            loop_ok = false;
            break;
        }
    }

    const bench_multi_resource_metrics_t metrics = bench_multi_finish_resource_probe (sample_start);
    const server_queue_stats_t queue_stats = sample_server_queue_stats (server, server);
    print_server_metrics (lib_name, transport, sizes, metrics, queue_stats);

    zlink_close (server);
    return loop_ok ? 0 : 1;
}
