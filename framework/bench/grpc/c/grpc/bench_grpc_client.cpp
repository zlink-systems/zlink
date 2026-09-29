#include "../common/bench_common.hpp"
#include "bench.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
void fill_payload (std::string *body, size_t size, uint32_t run_id, uint64_t seq)
{
    body->assign (std::max (size, zlink_c_bench::k_header_size), static_cast<char> (0xab));
    zlink_c_bench::stamp_payload (body->data (), body->size (), run_id,
                                  zlink_c_bench::phase_active, seq);
}

struct request_call_t
{
    zlink::framework::bench::withgrpc::BenchPayload request;
    zlink::framework::bench::withgrpc::BenchPayload reply;
    grpc::ClientContext context;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<zlink::framework::bench::withgrpc::BenchPayload>> reader;
};

// Depth of the unary `Command` calls kept in flight for send-saturation (README §2.1).
constexpr int k_send_saturation_outstanding = 4096;

struct send_call_t
{
    zlink::framework::bench::withgrpc::BenchPayload request;
    google::protobuf::Empty reply;
    grpc::ClientContext context;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<google::protobuf::Empty>> reader;
};

zlink_c_bench::result_t run_request_serial (
  zlink::framework::bench::withgrpc::BenchService::Stub *stub, size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t completed = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink::framework::bench::withgrpc::BenchPayload request;
        zlink::framework::bench::withgrpc::BenchPayload reply;
        fill_payload (request.mutable_body (), size, run_id, completed + errors);
        grpc::ClientContext context;
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        const grpc::Status status = stub->Echo (&context, request, &reply);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (!status.ok ()) {
            ++errors;
            continue;
        }
        zlink_c_bench::decoded_header_t header {};
        if (zlink_c_bench::decode_payload (reply.body ().data (), reply.body ().size (), &header)) {
            const uint64_t now = zlink_c_bench::now_ns ();
            latency.add_us (now >= header.sent_ns ? static_cast<double> (now - header.sent_ns) / 1000.0 : 0.0);
        }
        ++completed;
    }
    zlink_c_bench::result_t r;
    zlink_c_bench::capture_active_close (&r, start, resources, completed, &latency);
    r.implementation = "grpc-c";
    r.pattern = "request-serial";
    r.size = size;
    r.errors = errors;
    r.submitted = completed + errors;
    r.peak_in_flight = completed > 0 ? 1 : 0;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_request_backpressure (
  zlink::framework::bench::withgrpc::BenchService::Stub *stub, size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    std::mutex latency_gate;
    grpc::CompletionQueue cq;
    std::atomic<uint64_t> completed {0};
    std::atomic<uint64_t> errors {0};
    std::atomic<int> outstanding {0};
    uint64_t submitted = 0;
    uint64_t max_outstanding_seen = 0;
    double submit_wait_ms = 0.0;

    std::thread completion_thread ([&] {
        void *tag = nullptr;
        bool ok = false;
        while (cq.Next (&tag, &ok)) {
            std::unique_ptr<request_call_t> call (static_cast<request_call_t *> (tag));
            if (ok && call->status.ok ()) {
                zlink_c_bench::decoded_header_t header {};
                if (zlink_c_bench::decode_payload (call->reply.body ().data (),
                                                   call->reply.body ().size (), &header)) {
                    const uint64_t now = zlink_c_bench::now_ns ();
                    const double us =
                      now >= header.sent_ns ? static_cast<double> (now - header.sent_ns) / 1000.0 : 0.0;
                    std::lock_guard<std::mutex> lock (latency_gate);
                    latency.add_us (us);
                    completed.fetch_add (1, std::memory_order_relaxed);
                } else {
                    errors.fetch_add (1, std::memory_order_relaxed);
                }
            } else {
                errors.fetch_add (1, std::memory_order_relaxed);
            }
            outstanding.fetch_sub (1, std::memory_order_relaxed);
        }
    });

    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    while (std::chrono::steady_clock::now () < deadline) {
        auto *call = new request_call_t ();
        fill_payload (call->request.mutable_body (), size, run_id, submitted++);
        const int now_outstanding = outstanding.fetch_add (1, std::memory_order_relaxed) + 1;
        max_outstanding_seen = std::max<uint64_t> (max_outstanding_seen,
                                                   static_cast<uint64_t> (now_outstanding));
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        call->reader = stub->AsyncEcho (&call->context, call->request, &cq);
        call->reader->Finish (&call->reply, &call->status, call);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
    }
    // Close the active window before the drain: throughput and CPU% must not include it.
    zlink_c_bench::result_t r;
    {
        std::lock_guard<std::mutex> lock (latency_gate);
        zlink_c_bench::capture_active_close (&r, start, resources, completed.load (), &latency);
    }
    // README §3: the drain is bounded. Without a bound an uncapped submission phase can leave
    // this loop waiting indefinitely, and a wedged cell would hang the run.
    const auto drain_begin = std::chrono::steady_clock::now ();
    const auto drain_deadline =
      drain_begin + std::chrono::milliseconds (zlink_c_bench::k_drain_bound_ms);
    while (outstanding.load (std::memory_order_relaxed) > 0
           && std::chrono::steady_clock::now () < drain_deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    const auto drain_end = std::chrono::steady_clock::now ();
    const int abandoned = outstanding.load (std::memory_order_relaxed);
    cq.Shutdown ();
    completion_thread.join ();

    r.implementation = "grpc-c";
    r.pattern = "request-backpressure";
    r.size = size;
    r.errors = errors.load ();
    r.submitted = submitted;
    r.peak_in_flight = max_outstanding_seen;
    r.submit_wait_ms = submit_wait_ms;
    r.has_drain = true;
    r.abandoned = static_cast<uint64_t> (abandoned);
    r.drain_ms = std::chrono::duration<double, std::milli> (drain_end - drain_begin).count ();
    r.drain_bound_hit = abandoned > 0;
    return r;
}

zlink_c_bench::result_t run_send_saturation (
  zlink::framework::bench::withgrpc::BenchService::Stub *stub, size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    grpc::CompletionQueue cq;
    std::atomic<uint64_t> completed {0};
    std::atomic<uint64_t> errors {0};
    std::atomic<int> outstanding {0};
    uint64_t submitted = 0;
    uint64_t max_outstanding_seen = 0;
    double submit_wait_ms = 0.0;

    std::thread completion_thread ([&] {
        void *tag = nullptr;
        bool ok = false;
        while (cq.Next (&tag, &ok)) {
            std::unique_ptr<send_call_t> call (static_cast<send_call_t *> (tag));
            if (ok && call->status.ok ())
                completed.fetch_add (1, std::memory_order_relaxed);
            else
                errors.fetch_add (1, std::memory_order_relaxed);
            outstanding.fetch_sub (1, std::memory_order_relaxed);
        }
    });

    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    while (std::chrono::steady_clock::now () < deadline) {
        if (outstanding.load (std::memory_order_relaxed) >= k_send_saturation_outstanding) {
            std::this_thread::yield ();
            continue;
        }
        auto *call = new send_call_t ();
        fill_payload (call->request.mutable_body (), size, run_id, submitted++);
        const int now_outstanding = outstanding.fetch_add (1, std::memory_order_relaxed) + 1;
        max_outstanding_seen = std::max<uint64_t> (max_outstanding_seen,
                                                   static_cast<uint64_t> (now_outstanding));
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        call->reader = stub->AsyncCommand (&call->context, call->request, &cq);
        call->reader->Finish (&call->reply, &call->status, call);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
    }
    // Unary Command calls that finished OK inside the window; the wait below is the drain.
    zlink_c_bench::result_t r;
    zlink_c_bench::capture_active_close (&r, start, resources, completed.load (), nullptr);
    while (outstanding.load (std::memory_order_relaxed) > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    cq.Shutdown ();
    completion_thread.join ();

    r.implementation = "grpc-c";
    r.pattern = "send-saturation";
    r.size = size;
    r.errors = errors.load ();
    r.submitted = submitted;
    r.peak_in_flight = max_outstanding_seen;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}
}

int main ()
{
    const std::string target = zlink_c_bench::env_string ("GRPC_TARGET", "127.0.0.1:6071");
    const std::string patterns = zlink_c_bench::env_string (
      "PATTERNS", "request-serial,request-backpressure,send-saturation");
    grpc::ChannelArguments args;
    args.SetInt (GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    args.SetMaxReceiveMessageSize (16 * 1024 * 1024);
    args.SetMaxSendMessageSize (16 * 1024 * 1024);
    auto channel = grpc::CreateCustomChannel (target, grpc::InsecureChannelCredentials (), args);
    auto stub = zlink::framework::bench::withgrpc::BenchService::NewStub (channel);
    int status = 0;
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        zlink_c_bench::result_t results[3];
        int count = 0;
        if (zlink_c_bench::pattern_enabled (patterns, "request-serial"))
            results[count++] = run_request_serial (stub.get (), size);
        if (zlink_c_bench::pattern_enabled (patterns, "request-backpressure"))
            results[count++] = run_request_backpressure (stub.get (), size);
        if (zlink_c_bench::pattern_enabled (patterns, "send-saturation"))
            results[count++] = run_send_saturation (stub.get (), size);
        for (int i = 0; i < count; ++i)
            if (!zlink_c_bench::write_cell_json (results[i], grpc::Version ()))
                status = 1;
    }
    return status;
}
