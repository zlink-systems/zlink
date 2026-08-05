#include "../common/bench_router_compare_common.hpp"

#include <grpcpp/grpcpp.h>
#include "echo.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

namespace
{

using namespace bench_rc;

static std::atomic<bool> g_stop (false);

void on_signal (int)
{
    g_stop.store (true, std::memory_order_release);
}

class EchoServiceImpl final : public bench_echo::EchoService::Service
{
    grpc::Status BidirectionalEcho (
      grpc::ServerContext * /*context*/,
      grpc::ServerReaderWriter<bench_echo::EchoResponse, bench_echo::EchoRequest> *stream) override
    {
        bench_echo::EchoRequest request;
        bench_echo::EchoResponse response;

        while (stream->Read (&request)) {
            response.mutable_payload ()->swap (*request.mutable_payload ());
            if (!stream->Write (response))
                break;
        }

        return grpc::Status::OK;
    }
};

} // namespace

int main (int /*argc*/, char ** /*argv*/)
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    const int port = static_cast<int> (parse_long_env ("BENCH_PORT", 29200, 1));
    const std::string address = "0.0.0.0:" + std::to_string (port);

    EchoServiceImpl service;

    grpc::ServerBuilder builder;
    builder.AddListeningPort (address, grpc::InsecureServerCredentials ());
    builder.RegisterService (&service);
    builder.SetMaxReceiveMessageSize (16 * 1024 * 1024);
    builder.SetMaxSendMessageSize (16 * 1024 * 1024);
    builder.AddChannelArgument (GRPC_ARG_HTTP2_MAX_FRAME_SIZE, 4 * 1024 * 1024);
    builder.AddChannelArgument (GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES, 4 * 1024 * 1024);
    builder.AddChannelArgument (GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE, 4 * 1024 * 1024);
    builder.SetSyncServerOption (grpc::ServerBuilder::MIN_POLLERS, 4);
    builder.SetSyncServerOption (grpc::ServerBuilder::MAX_POLLERS, 16);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart ();
    if (!server) {
        std::fprintf (stderr, "grpc server: failed to start on %s\n", address.c_str ());
        return 2;
    }

    while (!g_stop.load (std::memory_order_acquire)) {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }

    server->Shutdown ();
    return 0;
}
