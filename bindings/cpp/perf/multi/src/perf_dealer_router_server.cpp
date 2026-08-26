// DEALER-ROUTER multi server benchmark: echo responder.
// Topology: client DEALER(connect, N) <-> server ROUTER(bind, 1)
// Measurement role: receive request payload and echo same payload back.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <deque>
#include <iostream>
#include <optional>
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

} // namespace

perf::async_task_t<bool> perf_dealer_router_server (const std::string &lib_name,
                                                    const std::string &transport,
                                                    size_t msg_size)
{
    perf::multi::set_perf_pattern_env ("DEALER_ROUTER_SENDSEND");

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",MULTI_DEALER_ROUTER_SENDSEND," << transport
                  << std::endl;
        co_return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    perf::multi::ctx_guard_t ctx;
    zlink::router_socket_t server (ctx.ctx ());
    if (!server.valid ())
        co_return false;

    perf::multi::apply_benchmark_socket_options (server, settings, transport);
    if (!perf::multi::setup_tls_server (server, transport))
        co_return false;

    const std::string endpoint = perf::multi::bind_and_resolve_endpoint (
      server, transport, "cpp_multi_dealer_router", settings.server_bind_port);
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
    std::vector<zlink::poll_event_t> events (1);
    poller.add (server, zlink::poll_event_flag_t::pollin, 0);

    // Bounded poll wait so the stdin/signal stop flag is observed promptly
    // without depending solely on a SIGTERM interrupting an infinite poll
    // (the C reference uses -1 + SIGTERM; this is the equivalent outcome,
    // just more responsive). 200ms keeps idle wakeups negligible.
    const std::chrono::milliseconds poll_timeout (200);

    while (!g_stop_requested.load (std::memory_order_acquire)) {
        try {
            const size_t ready_count = poller.wait (events.data (), events.size (), poll_timeout);
            if (ready_count == 0)
                continue;
            const auto revents_value = static_cast<int> (events[0].revents);
            const bool readable =
              (revents_value & static_cast<int> (zlink::poll_event_flag_t::pollin)) != 0;
            if (!readable)
                continue;
        }
        catch (const zlink::binding_error_t &err) {
            const int err_no = err.internal_errno ();
            if (err_no == EINTR)
                continue;
            failed = true;
            break;
        }

        // Receive one complete logical message so the benchmark can preserve
        // and validate its required empty trailing measurement frame.
        while (!g_stop_requested.load (std::memory_order_acquire)) {
            zlink::received_t received;
            const int recv_rc = server.recv (received, zlink::recv_flags_t::dontwait);
            if (recv_rc != 0) {
                const int err = errno;
                if (recv_rc == static_cast<int> (zlink::recv_result_t::no_data) || err == EAGAIN
                    || err == EWOULDBLOCK || err == EINTR)
                    break;
                failed = true;
                break;
            }

            if (!received.routing_id ().has_value ()
                || !perf::multi::measurement_parts_valid (received.parts ())) {
                continue;
            }
            try {
                zlink::message_t &part = received.parts ().front ();
                const zlink::routing_id_t source_rid = *received.routing_id ();
                if (perf::multi::measurement_part_count () == 2) {
                    zlink::message_t tail = perf::multi::measurement_empty_part ();
                    std::move (server.send (source_rid).message (part)).message (tail).submit ();
                } else {
                    std::move (server.send (source_rid)).message (part).submit ();
                }
            }
            catch (const zlink::submit_error_t &err) {
                const int err_no = err.internal_errno ();
                if (err_no == EINTR || err_no == EHOSTUNREACH || err_no == ENOTCONN) {
                    continue;
                }
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

    return perf_dealer_router_server (lib_name, transport, size).get () ? 0 : 1;
}
