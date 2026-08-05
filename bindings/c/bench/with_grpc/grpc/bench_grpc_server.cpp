#include "../common/bench_common.hpp"
#include "bench.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

namespace
{
std::atomic<bool> g_stop {false};
void on_signal (int) { g_stop.store (true); }

class BenchService final : public zlink_c_bench_grpc::BenchService::Service
{
    grpc::Status Echo (grpc::ServerContext *,
                       const zlink_c_bench_grpc::BenchPayload *request,
                       zlink_c_bench_grpc::BenchPayload *reply) override
    {
        reply->set_body (request->body ());
        return grpc::Status::OK;
    }

    grpc::Status Command (grpc::ServerContext *,
                          const zlink_c_bench_grpc::BenchPayload *,
                          zlink_c_bench_grpc::BenchEmpty *) override
    {
        return grpc::Status::OK;
    }
};
}

int main ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    const std::string address = zlink_c_bench::env_string ("GRPC_ADDRESS", "0.0.0.0:6071");
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
