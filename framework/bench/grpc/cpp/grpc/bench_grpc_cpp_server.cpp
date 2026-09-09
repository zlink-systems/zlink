/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// `grpc-cpp` server.
//
// spec 8.2 / plan 2.3: the gRPC side runs each language's DEFAULT server
// configuration and the configuration is recorded rather than tuned. This is
// grpc++ 1.51.1's synchronous ServerBuilder with an insecure loopback credential
// and no other option set; nothing here competes with the ZLink rows on tuning.
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

void on_signal (int)
{
    if (g_server)
        g_server->Shutdown ();
}

class bench_service_t final : public zlink_cpp_bench_grpc::BenchService::Service
{
  public:
    explicit bench_service_t (zlink_cpp_bench::server_metrics_t &metrics) : _metrics (metrics) {}

    grpc::Status Echo (grpc::ServerContext *,
                       const zlink_cpp_bench_grpc::BenchPayload *request,
                       zlink_cpp_bench_grpc::BenchPayload *response) override
    {
        _metrics.record (request->body ().data (), request->body ().size ());
        response->set_body (request->body ());
        return grpc::Status::OK;
    }

    grpc::Status Command (grpc::ServerContext *,
                          const zlink_cpp_bench_grpc::BenchPayload *request,
                          zlink_cpp_bench_grpc::BenchEmpty *) override
    {
        _metrics.record (request->body ().data (), request->body ().size ());
        return grpc::Status::OK;
    }

  private:
    zlink_cpp_bench::server_metrics_t &_metrics;
};
} // namespace

int main (int argc, char **argv)
{
    using namespace zlink_cpp_bench;
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    const std::string endpoint = arg_value (argc, argv, "--endpoint", "127.0.0.1:5111");
    const int stats_port = std::atoi (arg_value (argc, argv, "--stats-port", "5114").c_str ());

    server_metrics_t metrics;
    stats_http_server_t stats (metrics, stats_port);
    if (!stats.start ()) {
        std::fprintf (stderr, "grpc-cpp server: stats endpoint %d unavailable\n", stats_port);
        return 2;
    }

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
    g_server->Wait ();
    return 0;
}
