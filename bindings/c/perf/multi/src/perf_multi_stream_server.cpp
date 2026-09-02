#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_stream_session.hpp"
#include "bench_resource.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

struct stdin_watcher_state_t
{
    stdin_watcher_state_t () : done (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool done;
};

inline void print_server_metrics (const std::string &lib_name,
                                  const std::string &transport,
                                  const std::vector<size_t> &sizes,
                                  const bench_resource_metrics_t &metrics)
{
    print_server_metrics_for_sizes (lib_name, k_pattern, transport, sizes, metrics);
}

zlink_close_result_t close_stream_server_fenced (void *server)
{
    // CLOSE_BUSY is the public lifecycle handoff signal. Retry until the
    // accepted close can fence every admitted socket operation.
    for (;;) {
        const zlink_close_result_t result = zlink_close (server);
        if (result != ZLINK_CLOSE_BUSY || zlink_errno () != EBUSY)
            return result;
        std::this_thread::yield ();
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
    const zlink_stream_recv_mode_t packet_mode = ZLINK_STREAM_RECV_MODE_PACKET;
    if (zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE,
                                 &packet_mode, sizeof (packet_mode))
        != ZLINK_CONFIG_OK) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-stream-server] PACKET mode setup failed errno="
                      << zlink_errno () << std::endl;
        zlink_close (server);
        return 1;
    }

    if (!setup_tls_server (server, transport)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] tls setup failed transport=" << transport
                      << " errno=" << zlink_errno () << std::endl;
        }
        zlink_close (server);
        return 1;
    }

    // Open before bind so the START barrier can prove that every requested raw
    // transport connection reached Core CONNECTION_READY before HWM planning.
    connect_monitor_t connect_monitor;
    if (!open_connect_monitor (server, connect_monitor)) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-stream-server] connect monitor open failed" << std::endl;
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
        close_connect_monitor (connect_monitor);
        zlink_close (server);
        return 1;
    }

    perf_stop_requested ().store (false, std::memory_order_release);
    perf_multi_stream::reset_session (&g_stream_session, server);
    install_perf_signal_handlers ();

    std::cout << "READY," << endpoint << std::endl;
    // The runner sends this START only after the raw client has connected every
    // requested session and completed the size update. Keep context/socket APIs
    // on this thread; it becomes the server event-loop owner immediately below.
    if (!perf_multi_handshake::wait_for_start_from_stdin (msg_size)) {
        close_connect_monitor (connect_monitor);
        const zlink_close_result_t close_result = close_stream_server_fenced (server);
        if (close_result == ZLINK_CLOSE_OK)
            perf_multi_stream::clear_session (&g_stream_session);
        return 1;
    }
    if (!wait_connect_ready_count (connect_monitor, settings.clients,
                                   settings.connect_ready_timeout_ms)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] connection-ready barrier failed ready="
                      << poll_connect_ready_count (connect_monitor)
                      << " expected=" << settings.clients << std::endl;
        }
        close_connect_monitor (connect_monitor);
        const zlink_close_result_t close_result = close_stream_server_fenced (server);
        if (close_result == ZLINK_CLOSE_OK)
            perf_multi_stream::clear_session (&g_stream_session);
        return 1;
    }
    apply_benchmark_hwm (server, settings.hwm);
    if (zlink_ctx_auto_hwm_recalculate (ctx.get ()) != ZLINK_CONFIG_OK) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] ctx auto-hwm recalc failed err="
                      << zlink_errno () << " size=" << msg_size << std::endl;
        }
        close_connect_monitor (connect_monitor);
        const zlink_close_result_t close_result = close_stream_server_fenced (server);
        if (close_result == ZLINK_CLOSE_OK)
            perf_multi_stream::clear_session (&g_stream_session);
        return 1;
    }

    // Unlike the removed bind-time snapshot, this observes attached application
    // pipes and therefore reports their actual applied byte HWM.
    perf_print_auto_hwm_snapshot (server, false, "server-connected", transport, true,
                                  msg_size, ZLINK_SOCKET_STREAM);
    close_connect_monitor (connect_monitor);
    std::cout << "SERVER_START_READY," << msg_size << std::endl;

    const std::shared_ptr<stdin_watcher_state_t> stdin_state =
      std::make_shared<stdin_watcher_state_t> ();
    std::thread stdin_watcher ([stdin_state] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT") {
                perf_multi_stream::request_stop (&g_stream_session);
                break;
            }
        }
        perf_multi_stream::request_stop (&g_stream_session);
        {
            std::lock_guard<std::mutex> lock (stdin_state->mutex);
            stdin_state->done = true;
        }
        stdin_state->cv.notify_one ();
    });

    const int loop_rc =
      perf_multi_stream::run_server_event_loop (&g_stream_session, k_stop_token);

    bool stdin_done = false;
    {
        std::unique_lock<std::mutex> lock (stdin_state->mutex);
        stdin_state->cv.wait_for (lock, std::chrono::milliseconds (100),
                                  [stdin_state] () { return stdin_state->done; });
        stdin_done = stdin_state->done;
    }
    if (stdin_done)
        stdin_watcher.join ();
    else
        stdin_watcher.detach ();

    // Accepted close fences admitted socket operations before the session
    // socket pointer is cleared.
    const zlink_close_result_t close_rc = close_stream_server_fenced (server);
    const bool async_failed =
      g_stream_session.failed.load (std::memory_order_acquire)
      || perf_multi_stream::outstanding_size (&g_stream_session) != 0;
    if (close_rc == ZLINK_CLOSE_OK)
        perf_multi_stream::clear_session (&g_stream_session);

    const bench_resource_metrics_t metrics = bench_finish_resource_probe (cpu_start);
    print_server_metrics (lib_name, transport, sizes, metrics);
    return loop_rc == 0 && close_rc == ZLINK_CLOSE_OK && !async_failed ? 0 : 1;
}
