#include "../common/bench_common.hpp"

#include <zmq.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
bool make_msg (size_t size, uint32_t run_id, uint64_t seq, zmq_msg_t *msg)
{
    const size_t payload_size = std::max (size, zlink_c_bench::k_header_size);
    if (zmq_msg_init_size (msg, payload_size) != 0)
        return false;
    std::memset (zmq_msg_data (msg), 0xab, payload_size);
    return zlink_c_bench::stamp_payload (zmq_msg_data (msg), payload_size,
                                         run_id, zlink_c_bench::phase_active, seq);
}

zlink_c_bench::result_t run_send_send_serial (void *dealer, size_t size)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    zlink_c_bench::latency_sampler_t latency (200000);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;

    while (std::chrono::steady_clock::now () < deadline) {
        zmq_msg_t msg;
        if (!make_msg (size, run_id, seq, &msg)) {
            ++errors;
            continue;
        }

        const uint64_t submit_start = zlink_c_bench::now_ns ();
        const int send_rc = zmq_msg_send (&msg, dealer, 0);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (send_rc < 0) {
            zmq_msg_close (&msg);
            ++errors;
            continue;
        }
        ++submitted;

        zmq_msg_t reply;
        if (zmq_msg_init (&reply) != 0) {
            ++errors;
            continue;
        }
        if (zmq_msg_recv (&reply, dealer, 0) < 0 || zmq_msg_more (&reply)) {
            zmq_msg_close (&reply);
            ++errors;
            continue;
        }

        zlink_c_bench::decoded_header_t header {};
        if (zlink_c_bench::decode_payload (zmq_msg_data (&reply), zmq_msg_size (&reply), &header)
            && header.run_id == run_id && header.seq == seq) {
            const uint64_t now = zlink_c_bench::now_ns ();
            const double us = now >= header.sent_ns
                                ? static_cast<double> (now - header.sent_ns) / 1000.0
                                : 0.0;
            latency.add_us (us);
            ++completed;
            ++seq;
        } else {
            ++errors;
        }
        zmq_msg_close (&reply);
    }

    const auto stop = std::chrono::steady_clock::now ();
    zlink_c_bench::result_t r;
    r.scenario = "zmq-c-send-send-serial";
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
    r.submitted = submitted;
    r.max_outstanding = 1;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}
}

int main ()
{
    const std::string endpoint =
      zlink_c_bench::env_string ("ZMQ_SEND_ENDPOINT", "tcp://127.0.0.1:6079");
    const std::string scenarios = zlink_c_bench::env_string ("ZMQ_BENCH_SCENARIOS",
                                                             "send-send-serial");

    void *ctx = zmq_ctx_new ();
    void *dealer = ctx ? zmq_socket (ctx, ZMQ_DEALER) : nullptr;
    if (!ctx || !dealer)
        return 2;

    const int linger = 0;
    (void) zmq_setsockopt (dealer, ZMQ_LINGER, &linger, sizeof (linger));
    const char routing_id[] = "ZMQ-CLIENT";
    (void) zmq_setsockopt (dealer, ZMQ_ROUTING_ID, routing_id, sizeof (routing_id) - 1);
    if (zmq_connect (dealer, endpoint.c_str ()) != 0)
        return 2;

    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        if (zlink_c_bench::scenario_enabled (scenarios, "send-send-serial"))
            zlink_c_bench::print_result (run_send_send_serial (dealer, size));
    }

    zmq_close (dealer);
    zmq_ctx_term (ctx);
    return 0;
}
