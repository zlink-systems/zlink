#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "bench_multi_resource.hpp"
#include "../../../../perf/multi/common/perf_multi_stream_session.hpp"

#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef ZLINK_SOCKET_STREAM
#define ZLINK_SOCKET_STREAM ((zlink_socket_type_t) 0x1008)
#endif

namespace
{

static const char *k_pattern = "MULTI_STREAM";
static perf_multi_stream::session_t g_stream_session;

inline void on_signal (int)
{
    perf_stop_requested ().store (true, std::memory_order_release);
}

inline void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

inline bool parse_queue_probe_command (const std::string &line, size_t *msg_size_out)
{
    if (!msg_size_out)
        return false;

    static const char k_prefix[] = "QUEUE,";
    if (line.compare (0, sizeof (k_prefix) - 1, k_prefix) != 0)
        return false;

    const char *value = line.c_str () + (sizeof (k_prefix) - 1);
    if (!value || *value == '\0')
        return false;

    errno = 0;
    char *end = NULL;
    const unsigned long parsed = std::strtoul (value, &end, 10);
    if (errno != 0 || end == value || !end || *end != '\0' || parsed == 0)
        return false;

    *msg_size_out = static_cast<size_t> (parsed);
    return true;
}

inline void start_control_stdin_watcher ()
{
    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t queue_size = 0;
            if (parse_queue_probe_command (line, &queue_size))
                continue;
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
    });
    stdin_watcher.detach ();
}

inline void print_server_metrics (const std::string &lib_name,
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

    const zlink_stream_recv_mode_t packet_mode = ZLINK_STREAM_RECV_MODE_PACKET;
    if (zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &packet_mode,
                                 sizeof (packet_mode))
        != ZLINK_CONFIG_OK) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-stream-server] packet receive mode failed errno="
                      << zlink_errno () << std::endl;
        zlink_close (server);
        return 1;
    }

    const bench_multi_cpu_sample_t cpu_start = bench_multi_capture_cpu_sample ();
    const bench_settings_t settings = resolve_bench_settings ();
    const std::vector<size_t> sizes = resolve_bench_msg_sizes (64);

    set_sockopt_int (server, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
    apply_benchmark_hwm (server, settings.hwm);

    const int io_timeout_ms = resolve_bench_count ("PERF_STREAM_TIMEOUT_MS", 5000);
    set_sockopt_int (server, ZLINK_OPT_SNDTIMEO, io_timeout_ms, "ZLINK_OPT_SNDTIMEO");
    set_sockopt_int (server, ZLINK_OPT_RCVTIMEO, io_timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (server, ZLINK_OPT_TCP_NODELAY, 1, "ZLINK_OPT_TCP_NODELAY");

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

    perf_stop_requested ().store (false, std::memory_order_release);
    perf_multi_stream::reset_session (&g_stream_session, server);
    install_signal_handlers ();
    start_control_stdin_watcher ();

    std::cout << "READY," << endpoint << std::endl;

    const int rc = perf_multi_stream::run_server_event_loop (&g_stream_session, k_stop_token);

    perf_multi_stream::clear_session (&g_stream_session);

    const bench_multi_resource_metrics_t metrics = bench_multi_finish_resource_probe (cpu_start);
    print_server_metrics (lib_name, transport, sizes, metrics);
    zlink_close (server);
    return rc;
}
