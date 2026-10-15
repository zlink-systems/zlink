/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#define main bench_server_entry
#include "../grpc/bench_grpc_cpp_server.cpp"
#undef main

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

static_assert (std::is_base_of_v<zlink::framework::bench::withgrpc::BenchService::CallbackService,
                                 bench_service_t>,
               "gRPC C++ must use the callback server model");

int main ()
{
    zlink_cpp_bench::server_metrics_t metrics;
    bench_service_t service (metrics);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort ("127.0.0.1:0", grpc::InsecureServerCredentials (), &port);
    builder.RegisterService (&service);
    auto server = builder.BuildAndStart ();
    if (!server || port == 0)
        throw std::runtime_error ("callback test server did not start");
    auto stub = zlink::framework::bench::withgrpc::BenchService::NewStub (grpc::CreateChannel (
      "127.0.0.1:" + std::to_string (port), grpc::InsecureChannelCredentials ()));
    for (auto phase : {zlink_cpp_bench::phase_warmup, zlink_cpp_bench::phase_active}) {
        std::string body (1024, '\0');
        zlink_cpp_bench::stamp_payload (body.data (), body.size (), 1, phase, 0);
        zlink::framework::bench::withgrpc::BenchPayload request, response;
        request.set_body (body);
        grpc::ClientContext echo_context;
        if (!stub->Echo (&echo_context, request, &response).ok () || response.body () != body)
            throw std::runtime_error ("callback Echo failed");
        grpc::ClientContext command_context;
        google::protobuf::Empty empty;
        if (!stub->Command (&command_context, request, &empty).ok ())
            throw std::runtime_error ("callback Command failed");
    }
    server->Shutdown ();
    const auto snapshot = nlohmann::json::parse (metrics.snapshot_json ());
    if (snapshot.at ("received") != 2 || snapshot.at ("allPhaseMessages") != 4)
        throw std::runtime_error ("callback receive metrics mismatch");
}
