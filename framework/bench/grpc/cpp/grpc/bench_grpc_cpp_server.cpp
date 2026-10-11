/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// `grpc-cpp` server.
//
// README 8.2 records the gRPC server model and default configuration.
// CallbackService uses the default ServerBuilder and insecure loopback credentials.
// Unary handlers finish through the default reactor without a thread per call.
#include "../common/bench_common.hpp"
#include "../common/bench_stats_server.hpp"

#include "bench.grpc.pb.h"
#include "bench.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>

namespace
{
std::unique_ptr<grpc::Server> g_server;
std::atomic<bool> g_stop{false};

void on_signal (int)
{
    g_stop.store (true);
}

class bench_service_t final
    : public zlink::framework::bench::withgrpc::BenchService::CallbackService
{
  public:
    explicit bench_service_t (zlink_cpp_bench::server_metrics_t &metrics) : _metrics (metrics) {}

    grpc::ServerUnaryReactor *
    Echo (grpc::CallbackServerContext *context,
          const zlink::framework::bench::withgrpc::BenchPayload *request,
          zlink::framework::bench::withgrpc::BenchPayload *response) override
    {
        _metrics.record (request->body ().data (), request->body ().size ());
        response->set_body (request->body ());
        auto *reactor = context->DefaultReactor ();
        reactor->Finish (grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *
    Command (grpc::CallbackServerContext *context,
             const zlink::framework::bench::withgrpc::BenchPayload *request,
             google::protobuf::Empty *) override
    {
        _metrics.record (request->body ().data (), request->body ().size ());
        auto *reactor = context->DefaultReactor ();
        reactor->Finish (grpc::Status::OK);
        return reactor;
    }

  private:
    zlink_cpp_bench::server_metrics_t &_metrics;
};
} // namespace

int main (int argc, char **argv)
{
    using namespace zlink_cpp_bench;

    const std::string endpoint = arg_value (argc, argv, "--endpoint", "127.0.0.1:5282");
    const int stats_port = std::atoi (arg_value (argc, argv, "--stats-port", "5283").c_str ());

    server_metrics_t metrics;
    stats_http_server_t stats (metrics, stats_port);
    if (!stats.start ()) {
        std::fprintf (stderr, "grpc-cpp server: stats endpoint %d unavailable\n", stats_port);
        return 2;
    }

    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    bench_service_t service (metrics);
    grpc::ServerBuilder builder;
    builder.AddListeningPort (endpoint, grpc::InsecureServerCredentials ());
    builder.RegisterService (&service);
    g_server = builder.BuildAndStart ();
    if (!g_server) {
        std::fprintf (stderr, "grpc-cpp server: failed to bind %s\n", endpoint.c_str ());
        return 2;
    }
    std::fprintf (stderr, "grpc-cpp server: endpoint=%s stats=%d grpc=%s\n", endpoint.c_str (),
                  stats_port, grpc::Version ().c_str ());
    std::fflush (stderr);
    while (!g_stop.load ())
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    g_server->Shutdown ();
    return 0;
}
