#include "../common/bench_router_compare_common.hpp"

#include <grpcpp/grpcpp.h>
#include "echo.grpc.pb.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace bench_rc;

struct thread_result_t
{
    long recv_count;
    double latency_sum_us;
    long latency_samples;
    double elapsed_s;
};

// Each thread measures its own time window independently.  The main thread
// uses the longest elapsed time across all threads for throughput calculation.
void client_thread_fn (std::shared_ptr<grpc::Channel> channel,
                       size_t msg_size,
                       int duration_s,
                       int settle_ms,
                       int drain_ms,
                       thread_result_t &result)
{
    result.recv_count = 0;
    result.latency_sum_us = 0.0;
    result.latency_samples = 0;
    result.elapsed_s = 0.0;

    auto stub = bench_echo::EchoService::NewStub (channel);

    grpc::ClientContext context;
    auto stream = stub->BidirectionalEcho (&context);
    if (!stream) {
        std::fprintf (stderr, "grpc client: failed to open stream\n");
        return;
    }

    if (settle_ms > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (settle_ms));

    const auto measure_start = std::chrono::steady_clock::now ();
    const auto measure_end = measure_start + std::chrono::seconds (duration_s);

    std::vector<unsigned char> payload_buf (std::max<size_t> (16, msg_size), 0xAB);

    bench_echo::EchoRequest request;
    bench_echo::EchoResponse response;

    while (std::chrono::steady_clock::now () < measure_end) {
        const uint64_t send_ts = now_ns ();
        store_u64_be (payload_buf.data (), send_ts);
        store_u64_be (payload_buf.data () + 8, send_ts ^ 0x5a5a5a5a5a5a5a5aULL);

        request.set_payload (payload_buf.data (), payload_buf.size ());

        if (!stream->Write (request))
            goto done;

        if (!stream->Read (&response))
            goto done;

        const std::string &resp_payload = response.payload ();
        if (resp_payload.size () >= 16) {
            const uint64_t wire_ts =
              load_u64_be (reinterpret_cast<const unsigned char *> (resp_payload.data ()));
            const uint64_t now = now_ns ();
            if (now >= wire_ts) {
                result.latency_sum_us += static_cast<double> (now - wire_ts) / 1000.0;
                ++result.latency_samples;
            }
        }
        ++result.recv_count;
    }

done:
    const auto measure_stop = std::chrono::steady_clock::now ();
    result.elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>> (measure_stop - measure_start)
        .count ();

    stream->WritesDone ();
    grpc::Status status = stream->Finish ();
    (void) status;

    (void) drain_ms;
}

} // namespace

int main (int argc, char **argv)
{
    const std::string lib_name = argc > 1 ? std::string (argv[1]) : std::string ("grpc");
    const std::string msg_sizes_raw = parse_string_env ("BENCH_MSG_SIZES", "");
    std::vector<size_t> msg_sizes;
    if (!msg_sizes_raw.empty () && !parse_size_list (msg_sizes_raw, msg_sizes)) {
        std::fprintf (stderr, "grpc client: invalid BENCH_MSG_SIZES\n");
        return 2;
    }
    if (msg_sizes.empty ())
        msg_sizes = {64, 256, 1024, 65536, 131072, 262144};

    const int clients = static_cast<int> (parse_long_env ("BENCH_CLIENTS", 100, 1));
    const int duration_s = static_cast<int> (parse_long_env ("BENCH_MULTI_DURATION_SECONDS", 5, 1));
    const int settle_ms = static_cast<int> (parse_long_env ("BENCH_MULTI_SETTLE_MS", 500, 0));
    const int drain_ms = static_cast<int> (parse_long_env ("BENCH_MULTI_DRAIN_MS", 300, 0));
    const int transition_drain_ms = resolve_size_transition_drain_ms (drain_ms);
    const int port = static_cast<int> (parse_long_env ("BENCH_PORT", 29200, 1));

    const std::string target = "127.0.0.1:" + std::to_string (port);

    // Create channels once, reuse across all msg_sizes
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxReceiveMessageSize (16 * 1024 * 1024);
    ch_args.SetMaxSendMessageSize (16 * 1024 * 1024);
    ch_args.SetInt (GRPC_ARG_HTTP2_MAX_FRAME_SIZE, 4 * 1024 * 1024);
    ch_args.SetInt (GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES, 4 * 1024 * 1024);
    ch_args.SetInt (GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE, 4 * 1024 * 1024);

    std::vector<std::shared_ptr<grpc::Channel>> channels (static_cast<size_t> (clients));
    for (int i = 0; i < clients; ++i) {
        channels[static_cast<size_t> (i)] =
          grpc::CreateCustomChannel (target, grpc::InsecureChannelCredentials (), ch_args);
    }

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];

        std::vector<thread_result_t> results (static_cast<size_t> (clients));
        std::vector<std::thread> threads;
        threads.reserve (static_cast<size_t> (clients));

        for (int i = 0; i < clients; ++i) {
            threads.emplace_back (client_thread_fn, channels[static_cast<size_t> (i)], msg_size,
                                  duration_s, settle_ms, drain_ms,
                                  std::ref (results[static_cast<size_t> (i)]));
        }

        for (auto &t : threads)
            t.join ();

        long total_recv = 0;
        double total_latency_sum = 0.0;
        long total_latency_samples = 0;
        double max_elapsed_s = 0.0;
        for (int i = 0; i < clients; ++i) {
            const thread_result_t &r = results[static_cast<size_t> (i)];
            total_recv += r.recv_count;
            total_latency_sum += r.latency_sum_us;
            total_latency_samples += r.latency_samples;
            if (r.elapsed_s > max_elapsed_s)
                max_elapsed_s = r.elapsed_s;
        }

        if (max_elapsed_s <= 0.0)
            max_elapsed_s = static_cast<double> (std::max (1, duration_s));

        const double throughput =
          total_recv > 0 ? static_cast<double> (total_recv) / max_elapsed_s : 0.0;
        const double latency = total_latency_samples > 0
                                 ? total_latency_sum / static_cast<double> (total_latency_samples)
                                 : 0.0;

        print_result (lib_name, "tcp", msg_size, throughput, latency);
        run_size_transition_drain_stage (transition_drain_ms, (si + 1) < msg_sizes.size ());
    }

    return 0;
}
