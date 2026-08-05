#include "../common/bench_common.hpp"
#include "bench.grpc.pb.h"

#include <grpcpp/grpcpp.h>

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
    zlink_c_bench_grpc::BenchPayload request;
    zlink_c_bench_grpc::BenchPayload reply;
    grpc::ClientContext context;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<zlink_c_bench_grpc::BenchPayload>> reader;
};

struct send_call_t
{
    zlink_c_bench_grpc::BenchPayload request;
    zlink_c_bench_grpc::BenchEmpty reply;
    grpc::ClientContext context;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<zlink_c_bench_grpc::BenchEmpty>> reader;
};

zlink_c_bench::result_t run_request_serial (
  zlink_c_bench_grpc::BenchService::Stub *stub, size_t size)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    zlink_c_bench::latency_sampler_t latency (200000);
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t completed = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_c_bench_grpc::BenchPayload request;
        zlink_c_bench_grpc::BenchPayload reply;
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
    const auto stop = std::chrono::steady_clock::now ();
    zlink_c_bench::result_t r;
    r.scenario = "grpc-c-request-serial";
    r.size = size;
    r.unit = "KOPS";
    r.completed = completed;
    r.errors = errors;
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.mean_us = latency.mean_us ();
    r.p95_us = latency.percentile (0.95);
    r.p99_us = latency.percentile (0.99);
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = completed + errors;
    r.max_outstanding = completed > 0 ? 1 : 0;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_request_async (
  zlink_c_bench_grpc::BenchService::Stub *stub, size_t size, int max_outstanding, const char *scenario)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    zlink_c_bench::latency_sampler_t latency (200000);
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
        if (outstanding.load (std::memory_order_relaxed) >= max_outstanding) {
            std::this_thread::yield ();
            continue;
        }
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
    while (outstanding.load (std::memory_order_relaxed) > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    const auto stop = std::chrono::steady_clock::now ();
    cq.Shutdown ();
    completion_thread.join ();

    zlink_c_bench::result_t r;
    r.scenario = scenario;
    r.size = size;
    r.unit = "KOPS";
    r.completed = completed.load ();
    r.errors = errors.load ();
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.mean_us = latency.mean_us ();
    r.p95_us = latency.percentile (0.95);
    r.p99_us = latency.percentile (0.99);
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = submitted;
    r.max_outstanding = max_outstanding_seen;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_send_blocking (
  zlink_c_bench_grpc::BenchService::Stub *stub, size_t size)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t completed = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_c_bench_grpc::BenchPayload request;
        zlink_c_bench_grpc::BenchEmpty reply;
        fill_payload (request.mutable_body (), size, run_id, completed + errors);
        grpc::ClientContext context;
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        const grpc::Status status = stub->Command (&context, request, &reply);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (status.ok ())
            ++completed;
        else
            ++errors;
    }
    const auto stop = std::chrono::steady_clock::now ();
    zlink_c_bench::result_t r;
    r.scenario = "grpc-c-send-blocking";
    r.size = size;
    r.unit = "KMSG/s";
    r.completed = completed;
    r.errors = errors;
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = completed + errors;
    r.max_outstanding = completed > 0 ? 1 : 0;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_send_async (
  zlink_c_bench_grpc::BenchService::Stub *stub, size_t size, int max_outstanding)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
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
        if (outstanding.load (std::memory_order_relaxed) >= max_outstanding) {
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
    while (outstanding.load (std::memory_order_relaxed) > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    const auto stop = std::chrono::steady_clock::now ();
    cq.Shutdown ();
    completion_thread.join ();

    zlink_c_bench::result_t r;
    r.scenario = "grpc-c-send-saturation";
    r.size = size;
    r.unit = "KMSG/s";
    r.completed = completed.load ();
    r.errors = errors.load ();
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = submitted;
    r.max_outstanding = max_outstanding_seen;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}
}

int main ()
{
    const std::string target = zlink_c_bench::env_string ("GRPC_TARGET", "127.0.0.1:6071");
    const int window = zlink_c_bench::env_int ("WINDOW_SIZE", 100);
    const int max_outstanding = zlink_c_bench::env_int ("MAX_OUTSTANDING", 4096);
    const std::string scenarios = zlink_c_bench::env_string ("GRPC_BENCH_SCENARIOS", "all");
    grpc::ChannelArguments args;
    args.SetInt (GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    args.SetMaxReceiveMessageSize (16 * 1024 * 1024);
    args.SetMaxSendMessageSize (16 * 1024 * 1024);
    auto channel = grpc::CreateCustomChannel (target, grpc::InsecureChannelCredentials (), args);
    auto stub = zlink_c_bench_grpc::BenchService::NewStub (channel);
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        if (zlink_c_bench::scenario_enabled (scenarios, "request-serial"))
            zlink_c_bench::print_result (run_request_serial (stub.get (), size));
        if (zlink_c_bench::scenario_enabled (scenarios, "request-window"))
            zlink_c_bench::print_result (
              run_request_async (stub.get (), size, window, "grpc-c-request-window"));
        if (zlink_c_bench::scenario_enabled (scenarios, "request-saturation"))
            zlink_c_bench::print_result (run_request_async (stub.get (), size, max_outstanding,
                                                            "grpc-c-request-saturation"));
        if (zlink_c_bench::scenario_enabled (scenarios, "send-blocking"))
            zlink_c_bench::print_result (run_send_blocking (stub.get (), size));
        if (zlink_c_bench::scenario_enabled (scenarios, "send-saturation"))
            zlink_c_bench::print_result (run_send_async (stub.get (), size, max_outstanding));
    }
    return 0;
}
