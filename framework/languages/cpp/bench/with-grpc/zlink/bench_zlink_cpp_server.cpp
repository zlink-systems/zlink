/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// `zlink-cpp` raw server: the ROUTER<->ROUTER echo and command endpoints.
//
// spec 1.3 / FB-001: both ZLink rows use ROUTER<->ROUTER, so this server
// announces a well-known routing id before bind and the client addresses it by
// that id. spec 3 keeps the request echo endpoint and the command endpoint on
// separate sockets so a command measurement never has echo replies mixed into
// it.
#include "../common/bench_common.hpp"
#include "../common/bench_stats_server.hpp"

#include <zlink.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

namespace
{
std::atomic<bool> g_stop {false};

void on_signal (int)
{
    g_stop.store (true);
}

zlink::routing_id_t make_routing_id (const std::string &value)
{
    return zlink::routing_id_t::from (value);
}

// The request echo loop. A ROUTER peer's request arrives with a reply token; the
// echo goes back through reply(). A DEALER peer (the legacy configuration kept
// reachable for comparison) arrives without one and is answered with send().
void request_loop (zlink::router_socket_t &router, zlink_cpp_bench::server_metrics_t &metrics)
{
    zlink::received_t received;
    while (!g_stop.load ()) {
        const int rc = router.recv (received, zlink::recv_flags_t::none);
        if (rc != 0) {
            std::this_thread::sleep_for (std::chrono::microseconds (50));
            continue;
        }
        try {
            auto &parts = received.parts ();
            if (parts.empty ())
                continue;
            zlink::message_t &body = parts.back ();
            const unsigned char *payload = nullptr;
            size_t payload_size = 0;
            if (zlink_cpp_bench::decode_bench_payload_body (
                  static_cast<const void *> (body.data ()), body.size (), &payload, &payload_size))
                metrics.record (payload, payload_size);

            const char *envelope = zlink_cpp_bench::response_envelope ();
            zlink::message_t header = zlink::message_t::from (std::as_bytes (
              std::span<const char> (envelope, std::strlen (envelope))));
            zlink::message_t echo =
              zlink::message_t::from (std::span<const std::byte> (body.data (), body.size ()));
            if (received.reply_token ())
                std::move (received.reply ()).message (header).message (echo).submit ();
            else
                std::move (received.send ()).message (header).message (echo).submit ();
        }
        catch (const std::exception &error) {
            std::fprintf (stderr, "zlink-cpp request loop: %s\n", error.what ());
            metrics.record_error ();
        }
    }
}

// The command loop. No reply: spec 2 measures the one-way path, and the
// throughput of that pattern is this server's received count (spec 5, G3).
void command_loop (zlink::router_socket_t &router, zlink_cpp_bench::server_metrics_t &metrics)
{
    zlink::received_t received;
    while (!g_stop.load ()) {
        const int rc = router.recv (received, zlink::recv_flags_t::none);
        if (rc != 0) {
            std::this_thread::sleep_for (std::chrono::microseconds (50));
            continue;
        }
        try {
            auto &parts = received.parts ();
            if (parts.empty ())
                continue;
            zlink::message_t &body = parts.back ();
            const unsigned char *payload = nullptr;
            size_t payload_size = 0;
            if (zlink_cpp_bench::decode_bench_payload_body (
                  static_cast<const void *> (body.data ()), body.size (), &payload, &payload_size))
                metrics.record (payload, payload_size);
        }
        catch (const std::exception &error) {
            std::fprintf (stderr, "zlink-cpp command loop: %s\n", error.what ());
            metrics.record_error ();
        }
    }
}
} // namespace

int main (int argc, char **argv)
{
    using namespace zlink_cpp_bench;
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    const std::string request_endpoint =
      arg_value (argc, argv, "--endpoint", "tcp://127.0.0.1:5115");
    const std::string command_endpoint =
      arg_value (argc, argv, "--command-endpoint", "tcp://127.0.0.1:5117");
    const int stats_port = std::atoi (arg_value (argc, argv, "--stats-port", "5116").c_str ());
    const std::string request_rid =
      arg_value (argc, argv, "--request-routing-id", "zlink-cpp-bench-request-server");
    const std::string command_rid =
      arg_value (argc, argv, "--command-routing-id", "zlink-cpp-bench-command-server");

    server_metrics_t metrics;
    stats_http_server_t stats (metrics, stats_port);
    if (!stats.start ()) {
        std::fprintf (stderr, "zlink-cpp server: stats endpoint %d unavailable\n", stats_port);
        return 2;
    }

    try {
        zlink::context_t ctx;
        zlink::router_socket_t request_router (ctx);
        zlink::router_socket_t command_router (ctx);
        request_router.set_routing_id (make_routing_id (request_rid));
        command_router.set_routing_id (make_routing_id (command_rid));
        request_router.bind (request_endpoint);
        command_router.bind (command_endpoint);
        std::fprintf (stderr, "zlink-cpp server: request=%s command=%s stats=%d\n",
                      request_endpoint.c_str (), command_endpoint.c_str (), stats_port);
        std::fflush (stderr);

        std::thread request_thread ([&] { request_loop (request_router, metrics); });
        std::thread command_thread ([&] { command_loop (command_router, metrics); });
        while (!g_stop.load ())
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        request_thread.join ();
        command_thread.join ();
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "zlink-cpp server failed: %s\n", error.what ());
        return 2;
    }
    return 0;
}
