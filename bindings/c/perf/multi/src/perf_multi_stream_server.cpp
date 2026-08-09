#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_stream_session.hpp"
#include "bench_resource.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#ifndef ZLINK_SOCKET_STREAM
#define ZLINK_SOCKET_STREAM ((zlink_socket_type_t) 0x1008)
#endif

namespace
{

#ifndef PERF_MULTI_STREAM_PATTERN_NAME
#define PERF_MULTI_STREAM_PATTERN_NAME "MULTI_STREAM"
#endif

static const char *k_pattern = PERF_MULTI_STREAM_PATTERN_NAME;
// k_stop_token is provided by perf_common.hpp (wire-level shutdown token).

static perf_multi_stream::session_t g_stream_session;

inline void print_server_metrics (const std::string &lib_name,
                                  const std::string &transport,
                                  const std::vector<size_t> &sizes,
                                  const bench_resource_metrics_t &metrics)
{
    print_server_metrics_for_sizes (lib_name, k_pattern, transport, sizes, metrics);
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
    if (!ctx.valid ()) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-stream-server] ctx invalid" << std::endl;
        return 1;
    }

    void *server = zlink_socket (ctx.get (), ZLINK_SOCKET_STREAM);
    if (!server) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] socket create failed errno=" << zlink_errno ()
                      << std::endl;
        }
        return 1;
    }

    const bench_cpu_sample_t cpu_start = bench_capture_cpu_sample ();
    const bench_settings_t settings = resolve_bench_settings ();
    const std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    const size_t msg_size = argc >= 4
                              ? static_cast<size_t> (std::strtoull (argv[3], NULL, 10))
                              : (sizes.empty () ? 64 : sizes.front ());
    if (msg_size == 0) {
        zlink_close (server);
        return 1;
    }

    const int linger_ms = 0;
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    apply_benchmark_hwm (server, settings.hwm);
    const int io_timeout_ms = resolve_bench_count ("PERF_STREAM_TIMEOUT_MS", 5000);
    set_sockopt_int (server, ZLINK_OPT_SNDTIMEO, io_timeout_ms, "ZLINK_OPT_SNDTIMEO");
    set_sockopt_int (server, ZLINK_OPT_RCVTIMEO, io_timeout_ms, "ZLINK_OPT_RCVTIMEO");
    const int nodelay = 1;
    set_sockopt_int (server, ZLINK_OPT_TCP_NODELAY, nodelay, "ZLINK_OPT_TCP_NODELAY");

    if (!setup_tls_server (server, transport)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] tls setup failed transport=" << transport
                      << " errno=" << zlink_errno () << std::endl;
        }
        zlink_close (server);
        return 1;
    }

    const std::string endpoint =
      bind_server_endpoint (server, transport, lib_name + "_stream_server");
    if (endpoint.empty ()) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] bind endpoint failed errno=" << zlink_errno ()
                      << std::endl;
        }
        zlink_close (server);
        return 1;
    }

    if (!apply_benchmark_context_auto_hwm_msg_unit (ctx.get (), msg_size)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] auto-HWM setup failed errno=" << zlink_errno ()
                      << std::endl;
        }
        zlink_close (server);
        return 1;
    }
    perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                  ZLINK_SOCKET_STREAM);

    perf_stop_requested ().store (false, std::memory_order_release);
    perf_multi_stream::reset_session (&g_stream_session, ctx.get (), server, transport);
    perf_multi_stream::packet_handler_context_t packet_handler_ctx;
    packet_handler_ctx.session = &g_stream_session;
    packet_handler_ctx.stop_token = k_stop_token;
    if (zlink_stream_packet_handler (server, &perf_multi_stream::stream_packet_handler_callback,
                                     &packet_handler_ctx)
        != ZLINK_HANDLER_OK) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] packet handler install failed errno="
                      << zlink_errno () << std::endl;
        }
        perf_multi_stream::clear_session (&g_stream_session);
        zlink_close (server);
        return 1;
    }
    install_perf_signal_handlers ();

    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
    });
    stdin_watcher.detach ();

    std::atomic<int> loop_rc (0);
    std::thread event_loop_thread ([&] () {
        loop_rc.store (perf_multi_stream::run_server_event_loop (&g_stream_session, server,
                                                                 k_stop_token, NULL, NULL),
                       std::memory_order_release);
    });

    std::cout << "READY," << endpoint << std::endl;
    event_loop_thread.join ();

    perf_multi_stream::clear_session (&g_stream_session);

    const bench_resource_metrics_t metrics = bench_finish_resource_probe (cpu_start);
    print_server_metrics (lib_name, transport, sizes, metrics);
    zlink_close (server);
    return loop_rc.load (std::memory_order_acquire);
}
