#include "../common/bench_common.hpp"
#include "../zlink/send_metrics.hpp"
#include "bench.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

namespace
{
std::atomic<bool> g_stop{false};
void on_signal (int)
{
    g_stop.store (true);
}

class BenchService final : public zlink::framework::bench::withgrpc::BenchService::Service
{
    grpc::Status Echo (grpc::ServerContext *,
                       const zlink::framework::bench::withgrpc::BenchPayload *request,
                       zlink::framework::bench::withgrpc::BenchPayload *reply) override
    {
        const std::string &body = request->body ();
        try {
            const auto control = _metrics.control (body, zlink_c_bench::now_ns ());
            reply->set_body (control ? *control : body);
        }
        catch (const std::invalid_argument &error) {
            return grpc::Status (grpc::StatusCode::INVALID_ARGUMENT, error.what ());
        }
        return grpc::Status::OK;
    }

    grpc::Status Command (grpc::ServerContext *,
                          const zlink::framework::bench::withgrpc::BenchPayload *request,
                          google::protobuf::Empty *) override
    {
        _metrics.record (request->body ().data (), request->body ().size (),
                         zlink_c_bench::now_ns ());
        return grpc::Status::OK;
    }

    zlink_c_bench::send_metrics_t _metrics;
};
}

int main ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    const std::string address =
      zlink_c_bench::env_string ("GRPC_ADDRESS", zlink_c_bench::k_grpc_address);
    BenchService service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort (address, grpc::InsecureServerCredentials ());
    builder.RegisterService (&service);
    builder.SetMaxReceiveMessageSize (16 * 1024 * 1024);
    builder.SetMaxSendMessageSize (16 * 1024 * 1024);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart ();
    if (!server)
        return 2;
    while (!g_stop.load ())
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    server->Shutdown ();
    return 0;
}
