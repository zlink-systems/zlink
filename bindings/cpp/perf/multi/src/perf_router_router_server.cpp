// ROUTER-ROUTER multi server benchmark: routed echo responder.
// Topology: client ROUTER(connect, N) <-> server ROUTER(bind, routing_id=SERVER)
// Measurement role: echo payload to sender routing id and emit queue metrics.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

// Termination mirrors the C reference relay server
// (bindings/c/perf/multi/common/perf_multi_relay_server.hpp): the server has
// NO socket stop-token path; it stops only via the stdin STOP/QUIT watcher
// (run_comparison.py writes "STOP\n" then closes stdin) and SIGINT/SIGTERM
// (run_comparison.py terminate() fallback that interrupts the blocked poll).
static std::atomic<bool> g_stop_requested (false);

inline void request_stop ()
{
    g_stop_requested.store (true, std::memory_order_release);
}

inline void on_signal (int)
{
    request_stop ();
}

inline void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

inline void wait_for_stop_stdin ()
{
    std::string line;
    while (std::getline (std::cin, line)) {
        if (line == "STOP" || line == "QUIT") {
            request_stop ();
            return;
        }
    }
    // stdin EOF (run_comparison.py closed the pipe) also means stop.
    request_stop ();
}

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

void debug_log (const std::string &message_)
{
    if (!perf_debug_enabled ())
        return;
    std::cerr << "router_router server: " << message_ << std::endl;
}

} // namespace

perf::async_task_t<bool> perf_router_router_server (const std::string &lib_name,
                                                    const std::string &transport,
                                                    size_t msg_size)
{
    perf::multi::set_perf_pattern_env ("ROUTER_ROUTER_SENDSEND");

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",MULTI_ROUTER_ROUTER_SENDSEND," << transport
                  << std::endl;
        co_return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    perf::multi::ctx_guard_t ctx;
    // Hold the typed router_socket_t directly; matches dealer_router_server
    // and avoids the perf::socket_t variant visit overhead on every hot
    // send/recv (cpp.md round 21).
    zlink::router_socket_t server (ctx.ctx ());
    if (!server.valid ())
        co_return false;

    server.set_routing_id (
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("SERVER"), 6));
    perf::multi::apply_benchmark_socket_options (server, settings, transport);
    if (!perf::multi::setup_tls_server (server, transport))
        co_return false;

    const std::string endpoint = perf::multi::bind_and_resolve_endpoint (
      server, transport, "cpp_multi_router_router", settings.server_bind_port);
    if (endpoint.empty ())
        co_return false;
    if (!perf::multi::recalculate_auto_hwm (ctx))
        co_return false;
    perf::multi::emit_auto_hwm_detail (server, "server", "server", transport, msg_size, "router");

    g_stop_requested.store (false, std::memory_order_release);
    install_signal_handlers ();
    std::thread stdin_watcher (&wait_for_stop_stdin);
    stdin_watcher.detach ();

    perf::multi::print_ready (endpoint);

    bool failed = false;
    zlink::poller_t poller;
    poller.add (server, zlink::poll_event_flag_t::pollin, 0);
    std::vector<zlink::poll_event_t> events (1);

    while (!g_stop_requested.load (std::memory_order_acquire)) {
        const size_t poll_rc =
          poller.wait (events.data (), events.size (), std::chrono::milliseconds (-1));
        if (poll_rc == 0)
            continue;

        const short revents = static_cast<short> (events[0].revents);
        const bool readable =
          (revents & static_cast<short> (zlink::poll_event_flag_t::pollin)) != 0;
        if (!readable)
            continue;

        // Drain available single-part routed messages without blocking.
        while (!g_stop_requested.load (std::memory_order_acquire)) {
            zlink::received_t received;
            const int recv_rc = server.recv (received, zlink::recv_flags_t::dontwait);
            if (recv_rc != 0) {
                const int err = errno;
                if (recv_rc == static_cast<int> (zlink::recv_result_t::no_data) || err == EAGAIN
                    || err == EWOULDBLOCK || err == EINTR)
                    break;
                debug_log ("recv failed errno=" + std::to_string (err));
                failed = true;
                break;
            }
            if (!received.routing_id ().has_value () || received.routing_id ()->size () == 0
                || received.request_seq ().has_value () || !received.is_single_part ()) {
                debug_log ("recv envelope mismatch");
                failed = true;
                break;
            }
            zlink::message_t &part = received.first_part ();

            // No socket stop-token handling: matches the C reference relay
            // server, which terminates only via stdin STOP/QUIT + signals.
            if (part.size () == 0) {
                continue;
            }

            const zlink::routing_id_t source_node_rid = *received.routing_id ();
            try {
                co_await std::move (server.send (source_node_rid)).message (part).async ();
            }
            catch (const zlink::submit_error_t &err) {
                const int err_no = err.internal_errno ();
                if (err_no == EAGAIN || err_no == EWOULDBLOCK || err_no == EINTR
                    || err_no == EHOSTUNREACH || err_no == ENOTCONN) {
                    continue;
                }
                errno = err_no;
                debug_log ("send failed errno=" + std::to_string (err_no));
                failed = true;
                break;
            }
        }
        if (failed)
            break;
    }

    co_return !failed;
}

int main (int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: <lib_name> <transport> <size>" << std::endl;
        return 1;
    }

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));
    if (size == 0)
        return 1;

    return perf_router_router_server (lib_name, transport, size).get () ? 0 : 1;
}
