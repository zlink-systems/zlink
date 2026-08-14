// PUBSUB benchmark: one-way publisher->subscriber loop.
// Topology: publisher(PUB bind) -> subscriber(SUB connect)

#include "../common/perf_single_common.hpp"
#include "../common/perf_single_runner.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

namespace
{

const std::string k_topic ("bench");

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

bool record_subscribed_payload (const zlink::topic_message_t &message,
                                uint32_t run_id,
                                size_t msg_size,
                                size_t payload_size,
                                std::atomic<unsigned long long> &received_count,
                                perf::single::latency_stats_builder_t &latency_builder)
{
    if (message.topic () != k_topic || message.parts ().size () != 1
        || message.parts ()[0].size () != payload_size) {
        return true;
    }

    perf_single_metric::header_t header;
    if (!perf_single_metric::decode_payload_header (message.parts ()[0].data (),
                                                    message.parts ()[0].size (), &header)) {
        return true;
    }
    if (!perf_single_metric::is_expected (header, run_id, perf_single_metric::phase_active,
                                          msg_size)) {
        return true;
    }

    received_count.fetch_add (1, std::memory_order_relaxed);
    const uint64_t now = perf_single_metric::now_ns ();
    latency_builder.add (perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
    return true;
}

} // namespace

bool run_pattern_pubsub (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: transport unavailable" << std::endl;
        std::cout << "UNSUPPORTED," << lib_name << ",PUBSUB," << transport << std::endl;
        return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: invalid context" << std::endl;
        return false;
    }

    zlink::pub_socket_t publisher (ctx.ctx ());
    zlink::sub_socket_t subscriber (ctx.ctx ());
    if (!publisher.valid () || !subscriber.valid ()) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: invalid sockets" << std::endl;
        return false;
    }

    publisher.options ().no_drop (true);
    const auto set_subscription =
        static_cast<void (zlink::sub_socket_t::*)(
            const std::string &)>(&zlink::sub_socket_t::set_subscription);
    (subscriber.*set_subscription) (std::string ());
    if (!perf::single::recalculate_single_auto_hwm (ctx)) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: auto-hwm recalculation failed errno=" << errno << std::endl;
        return false;
    }

    if (!perf::single::setup_connected_pair (publisher, subscriber, transport,
                                             lib_name + "_pubsub")) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: setup_connected_pair failed errno=" << errno << std::endl;
        return false;
    }

    std::this_thread::sleep_for (
      std::chrono::milliseconds (perf::single::resolve_single_pubsub_ready_settle_ms ()));
    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');

    const uint32_t run_id = 1U;
    const int duration_s = std::max (1, perf::single::resolve_single_duration_seconds ());
    std::atomic<unsigned long long> sent_count (0);
    std::atomic<unsigned long long> received_count (0);
    std::atomic<bool> sender_ok (true);
    perf::single::latency_stats_builder_t latency_builder (
      perf::single::resolve_single_latency_sample_cap ());
    const auto active_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);

    // PERF_SINGLE_TEST_POLICY § 1.4: match the C reference runner by
    // waiting for subscriber readiness through the public poller and
    // draining messages with DONTWAIT.  A blocking subscribe with a
    // receive timeout is not equivalent to this policy.
    zlink::poller_t recv_poller;
    try {
        recv_poller.add (subscriber, zlink::poll_event_flag_t::pollin, 0);
    }
    catch (const zlink::config_error_t &) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: subscriber poller setup failed" << std::endl;
        return false;
    }

    std::thread sender_thread ([&] () {
        uint64_t seq = 1;
        while (std::chrono::steady_clock::now () < active_deadline) {
            if (!perf_single_metric::stamp_payload (payload.data (), payload.size (), run_id,
                                                    perf_single_metric::phase_active, msg_size,
                                                    seq++, perf_single_metric::now_ns ())
                || !perf::single::publish_payload_blocking (publisher, k_topic, payload.data (),
                                                            payload.size ())) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            sent_count.fetch_add (1, std::memory_order_relaxed);
        }
        // PERF_SINGLE_TEST_POLICY § 1.4: publish one wire-level blocking
        // stop token under k_topic.
        if (!perf::single::publish_stop_token_blocking (publisher, k_topic))
            sender_ok.store (false, std::memory_order_release);
    });

    auto record_if_active = [&] (const zlink::topic_message_t &message_) {
        if (std::chrono::steady_clock::now () >= active_deadline)
            return true;
        return record_subscribed_payload (message_, run_id, msg_size, payload_size, received_count,
                                          latency_builder);
    };

    auto is_stop_message = [] (const zlink::topic_message_t &message_) {
        return message_.parts ().size () == 1
               && perf::single::is_stop_token_message (message_.parts ()[0]);
    };

    zlink::topic_message_t message;
    bool stop_received = false;
    while (!stop_received) {
        try {
            zlink::poll_event_t event;
            if (recv_poller.wait (&event, 1, std::chrono::milliseconds (-1)) == 0)
                continue;
        }
        catch (const zlink::recv_error_t &) {
            sender_ok.store (false, std::memory_order_release);
            break;
        }

        for (;;) {
            try {
                const int rc = subscriber.subscribe (
                  message, static_cast<int> (zlink::recv_flags_t::dontwait));
                if (rc == static_cast<int> (zlink::recv_result_t::no_data))
                    break;
                if (rc != static_cast<int> (zlink::recv_result_t::ok)) {
                    sender_ok.store (false, std::memory_order_release);
                    break;
                }
            }
            catch (const zlink::recv_error_t &error) {
                const int err = error.internal_errno ();
                if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK)
                    break;
                sender_ok.store (false, std::memory_order_release);
                break;
            }

            if (is_stop_message (message)) {
                stop_received = true;
                break;
            }

            if (!record_if_active (message)) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
        }
    }

    sender_thread.join ();
    // Stop token is the last in-flight message, so any earlier payloads
    // have already been recorded above. No bounded drain loop is needed.

    const unsigned long long received = received_count.load (std::memory_order_acquire);
    if (!sender_ok.load (std::memory_order_acquire) || received == 0
        || latency_builder.count () == 0) {
        if (perf_debug_enabled ())
            std::cerr << "pubsub: active phase failed received=" << received << std::endl;
        return false;
    }
    const perf::single::latency_stats_t latency = latency_builder.snapshot ();

    perf::single::emit_single_socket_hwm_detail (publisher, "PUBSUB", transport, "publisher", "pub",
                                                 msg_size);
    perf::single::emit_single_socket_hwm_detail (subscriber, "PUBSUB", transport, "subscriber",
                                                 "sub", msg_size);

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    perf::single::print_result (lib_name, "PUBSUB", transport, msg_size, throughput,
                                latency.mean_ns, latency.p95_ns, latency.p99_ns);
    return true;
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_pubsub);
}
