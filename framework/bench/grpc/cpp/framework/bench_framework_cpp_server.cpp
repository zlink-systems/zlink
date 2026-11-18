/* SPDX-License-Identifier: Elastic-2.0 */
// Target B: public RouteMesh protobuf and default typed JSON echo/count handlers.
#include "bench_metric_reader.hpp"
#include "bench_stats_server.hpp"
#include "../common/game_message.hpp"
#include "bench.pb.h"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
#include <csignal>
#if defined(ZLINK_CPP_ABLATION_LADDER)
#include "../ladder/ladder_wire.hpp"
#endif

std::atomic<bool> stop_target{false};
void target_signal (int)
{
    stop_target = true;
}

using namespace zlink_cpp_bench;
namespace fw = zlink::framework;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;

template <typename Payload = payload_t> struct echo_handler_t
{
    explicit echo_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    Payload handle (const Payload &request)
    {
        metrics.record (request.body ().data (), request.body ().size ());
        return request;
    }
    server_metrics_t &metrics;
};

template <typename Payload = payload_t> struct command_handler_t
{
    explicit command_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    void handle (const Payload &request)
    {
        metrics.record (request.body ().data (), request.body ().size ());
    }
    server_metrics_t &metrics;
};

struct game_echo_handler_t
{
    explicit game_echo_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    game_message_t handle (const game_message_t &request)
    {
        metrics.record_game (request);
        return request;
    }
    server_metrics_t &metrics;
};

struct game_command_handler_t
{
    explicit game_command_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    void handle (const game_message_t &request) { metrics.record_game (request); }
    server_metrics_t &metrics;
};

int main (int argc, char **argv)
{
    const auto endpoint = arg_value (argc, argv, "--endpoint", "tcp://127.0.0.1:5294");
    const int port = std::stoi (arg_value (argc, argv, "--stats-port", "5295"));
    try {
#if defined(ZLINK_CPP_ABLATION_LADDER)
        if (arg_value (argc, argv, "--ladder-wire-check", "0") == "1") {
            ladder::verify_wire ();
            return 0;
        }
        const auto level = fw::detail::ablation::level ();
        const auto shape = ladder::read_payload_shape ();
        const bool raw_mesh = arg_value (argc, argv, "--ladder-raw-mesh", "0") == "1";
        if (level <= 4 || raw_mesh) {
            server_metrics_t metrics;
            stats_http_server_t stats (metrics, port);
            if (!stats.start ())
                return 2;
            std::signal (SIGINT, target_signal);
            std::signal (SIGTERM, target_signal);
            if (level == 4 && !raw_mesh && shape == ladder::payload_shape_t::mixed)
                return ladder::direct_server<server_metrics_t, ladder::game_message_t> (
                  endpoint, metrics, stop_target, level, false, shape);
            if (level == 4 && !raw_mesh)
                return ladder::direct_server<server_metrics_t, ladder::typed_payload_t> (
                  endpoint, metrics, stop_target, level, false, shape);
            return ladder::direct_server (endpoint, metrics, stop_target, level, raw_mesh, shape);
        }
#endif
        bench_metric_reader_t metric_reader;
        server_metrics_t metrics (std::nullopt);
        stats_http_server_t server (metrics, port, [&] {
            const auto rejected = metric_reader.collect_cumulative_sum (
              "zlink.mesh_node.messages.dropped", {{"mesh_name", "bench"},
                                                   {"surface", "node"},
                                                   {"message_kind", "send"},
                                                   {"reason", "backpressure"}});
            metrics.observe_rejected_total (rejected);
        });
        server.app ().logging ().disable_record_capture ();
        auto &options = server.app ().add_zlink_framework ();
#if defined(ZLINK_CPP_ABLATION_LADDER)
        if (level != 7)
#endif
            options.codecs ().use (zlink::framework_codecs::protobuf ());
        // Metrics outlive the server; handlers get them by constructor injection (#4145).
        options.services ().add_factory<server_metrics_t> (
          [&metrics] (fw::service_provider_t &) {
              return std::shared_ptr<server_metrics_t> (&metrics, [] (auto *) {});
          },
          fw::service_lifetime_t::singleton);
        auto mesh = options.add_route_mesh ("bench");
        mesh.set_object_role (fw::object_role_t::none)
          .listen (endpoint)
          .set_routing_id (zlink::routing_id_t::from ("bench-server"));
#if defined(ZLINK_CPP_ABLATION_LADDER)
        if (level == 7) {
            mesh.add_route_request_handler<echo_handler_t<ladder::typed_payload_t>,
                                          ladder::typed_payload_t, ladder::typed_payload_t> ("BenchPayload")
              .add_route_send_handler<command_handler_t<ladder::typed_payload_t>,
                                      ladder::typed_payload_t> ("BenchPayload");
        } else
#endif
        {
            mesh.add_route_request_handler<echo_handler_t<>, payload_t, payload_t> ("BenchPayload")
              .add_route_send_handler<command_handler_t<>, payload_t> ("BenchPayload");
        }
        mesh
          .add_route_request_handler<game_echo_handler_t, game_message_t, game_message_t> (
            "GameMessage")
          .add_route_send_handler<game_command_handler_t, game_message_t> ("GameMessage");
        if (!server.start ())
            return 2;
        std::fprintf (stderr, "zlink-framework-cpp target: endpoint=%s stats=%d\n",
                      endpoint.c_str (), port);
        std::signal (SIGINT, target_signal);
        std::signal (SIGTERM, target_signal);
        while (!stop_target)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        server.stop ();
        return 0;
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "zlink-framework-cpp target failed: %s\n", error.what ());
        return 2;
    }
}
