/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Target B: public RouteMesh typed protobuf echo/count handlers.
#include "bench_stats_server.hpp"
#include "bench.pb.h"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
#include <csignal>

std::atomic<bool> stop_target {false};
void target_signal (int) { stop_target = true; }

using namespace zlink_cpp_bench;
namespace fw = zlink::framework;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;

struct echo_handler_t
{
    explicit echo_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    payload_t handle (const payload_t &request)
    {
        metrics.record (request.body ().data (), request.body ().size ());
        return request;
    }
    server_metrics_t &metrics;
};

struct command_handler_t
{
    explicit command_handler_t (server_metrics_t &metrics) : metrics (metrics) {}
    void handle (const payload_t &request)
    {
        metrics.record (request.body ().data (), request.body ().size ());
    }
    server_metrics_t &metrics;
};

class capture_runtime_t final : public fw::hosted_service_t
{
  public:
    explicit capture_runtime_t (std::atomic<fw::route_mesh_runtime_t *> &runtime) : _runtime (runtime) {}
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _runtime.store (&services.get_required<fw::route_mesh_runtime_t> ());
        co_return;
    }
    void stop () noexcept override {}
  private:
    std::atomic<fw::route_mesh_runtime_t *> &_runtime;
};

int main (int argc, char **argv)
{
    const auto endpoint = arg_value (argc, argv, "--endpoint", "tcp://127.0.0.1:5294");
    const int port = std::stoi (arg_value (argc, argv, "--stats-port", "5295"));
    try {
        // The public RouteMesh snapshot exposes topology, but no drop counters.
        server_metrics_t metrics (std::nullopt);
        std::atomic<fw::route_mesh_runtime_t *> runtime{nullptr};
        stats_http_server_t server (metrics, port, [&] {
            if (auto *current = runtime.load ()) (void) current->snapshot ("bench");
        });
        server.app ().add_hosted_service (std::make_unique<capture_runtime_t> (runtime));
        server.app ().logging ().disable_record_capture ();
        auto &options = server.app ().add_zlink_framework ();
        options.codecs ().use (zlink::framework_codecs::protobuf ());
        options.services ().add_singleton<echo_handler_t> (std::make_unique<echo_handler_t> (metrics));
        options.services ().add_singleton<command_handler_t> (std::make_unique<command_handler_t> (metrics));
        auto mesh = options.add_route_mesh ("bench");
        mesh.set_object_role (fw::object_role_t::none).listen (endpoint).set_routing_id (zlink::routing_id_t::from ("bench-server"));
        mesh.add_route_request_handler<echo_handler_t, payload_t, payload_t> ("BenchPayload")
          .add_route_send_handler<command_handler_t, payload_t> ("BenchPayload");
        if (!server.start ())
            return 2;
        std::fprintf (stderr, "zlink-framework-cpp target: endpoint=%s stats=%d\n", endpoint.c_str (), port);
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
