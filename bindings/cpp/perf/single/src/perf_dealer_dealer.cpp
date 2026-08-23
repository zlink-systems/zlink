// DEALER-DEALER benchmark: one-way sender->receiver loop inside one process.

#include "../common/perf_single_common.hpp"
#include "../common/perf_single_runner.hpp"

#include <atomic>
#include <thread>
#include <vector>


perf::async_task_t<bool> run_pattern_dealer_dealer_async (const std::string &transport,
                                                          size_t msg_size,
                                                          const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",DEALER_DEALER," << transport << std::endl;
        co_return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        co_return false;
    }

    perf::single::socket_guard_t bind_socket (ctx, zlink::socket_type::dealer);
    perf::single::socket_guard_t conn_socket (ctx, zlink::socket_type::dealer);
    if (!bind_socket.valid () || !conn_socket.valid ()) {
        co_return false;
    }

    (void) bind_socket.sock ().set_option (perf::options::socket_options::tcp_nodelay, 1);
    (void) conn_socket.sock ().set_option (perf::options::socket_options::tcp_nodelay, 1);
    if (!perf::single::recalculate_single_auto_hwm (ctx)) {
        co_return false;
    }

    if (!perf::single::setup_connected_pair (bind_socket.sock (), conn_socket.sock (), transport,
                                             lib_name + "_dealer_dealer")) {
        co_return false;
    }

    const int recv_timeout = perf::single::resolve_single_recv_timeout_ms ();
    (void) bind_socket.sock ().set_option (perf::options::socket_options::rcvtimeo, recv_timeout);
    (void) conn_socket.sock ().set_option (perf::options::socket_options::sndtimeo,
                                           perf::single::resolve_single_send_timeout_ms ());

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
    // waiting for receiver readiness through the public poller and then
    // draining available messages with DONTWAIT. A blocking receive with a
    // timeout is not equivalent to the canonical C harness.
    zlink::poller_t recv_poller;
    try {
        bind_socket.sock ().poller_add (recv_poller, zlink::poll_event_flag_t::pollin, 0);
    }
    catch (const zlink::config_error_t &) {
        co_return false;
    }

    auto sender_work = [&] () -> void {
        uint64_t seq = 1;
        // C-faithful send model (bindings/c/perf single
        // perf_single_one_way.hpp send_active_samples +
        // send_socket_active_message with ZLINK_SEND_FLAGS_NONE,
        // retry_on_eagain=true): on transient backpressure, re-stamp a
        // fresh timestamp and retry.
        while (std::chrono::steady_clock::now () < active_deadline) {
            if (!perf_single_metric::stamp_payload (payload.data (), payload.size (), run_id,
                                                    perf_single_metric::phase_active, msg_size, seq,
                                                    perf_single_metric::now_ns ())) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            const int send_rc = perf::single::send_payload_active (
              conn_socket.sock (), payload.data (), payload.size ());
            if (send_rc < 0) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            if (send_rc == 0) {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                continue; // backpressure: re-stamp + wait + retry
            }
            ++seq;
            sent_count.fetch_add (1, std::memory_order_release);
        }
        // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with one
        // wire-level blocking stop token.
        if (!perf::single::send_stop_token_active (conn_socket.sock ()))
            sender_ok.store (false, std::memory_order_release);
    };

    // C-faithful receiver (bindings/c/perf single
    // perf_single_one_way.hpp run_active_phase): wait through the public
    // poller, receive the first ready part with DONTWAIT, and drain the
    // remaining ready parts until EAGAIN. The stop token is the only phase
    // terminator, matching the C harness.
    std::thread receiver_thread ([&] () {
        auto handle_part = [&] (zlink::message_t &part_, bool *stop_out_) -> bool {
            *stop_out_ = false;
            if (perf::single::is_stop_token_message (part_)) {
                *stop_out_ = true;
                return true;
            }
            if (part_.size () != payload_size)
                return true;
            perf_single_metric::header_t header;
            if (!perf_single_metric::decode_payload_header (part_.data (), part_.size (), &header))
                return true;
            if (!perf_single_metric::is_expected (header, run_id, perf_single_metric::phase_active,
                                                  msg_size))
                return true;
            if (std::chrono::steady_clock::now () < active_deadline) {
                received_count.fetch_add (1, std::memory_order_release);
                const uint64_t now = perf_single_metric::now_ns ();
                latency_builder.add (
                  perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
            }
            return true;
        };

        while (true) {
            try {
                zlink::poll_event_t event;
                if (recv_poller.wait (&event, 1, std::chrono::milliseconds (-1)) == 0)
                    continue;
            }
            catch (const zlink::recv_error_t &) {
                sender_ok.store (false, std::memory_order_release);
                return;
            }

            zlink::message_t part;
            const int recv_rc = bind_socket.sock ().recv (
              part, static_cast<int> (zlink::recv_flags_t::dontwait));
            if (recv_rc != 0) {
                if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
                    continue;
                sender_ok.store (false, std::memory_order_release);
                return;
            }
            bool stop = false;
            if (!handle_part (part, &stop)) {
                sender_ok.store (false, std::memory_order_release);
                return;
            }
            if (stop)
                return;

            for (;;) {
                zlink::message_t burst;
                const int burst_rc = bind_socket.sock ().recv (
                  burst, static_cast<int> (zlink::recv_flags_t::dontwait));
                if (burst_rc != 0) {
                    if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
                        break;
                    sender_ok.store (false, std::memory_order_release);
                    return;
                }
                bool burst_stop = false;
                if (!handle_part (burst, &burst_stop)) {
                    sender_ok.store (false, std::memory_order_release);
                    return;
                }
                if (burst_stop)
                    return;
            }
        }
    });

    sender_work ();
    receiver_thread.join ();

    const unsigned long long received = received_count.load (std::memory_order_acquire);
    if (!sender_ok.load (std::memory_order_acquire) || received == 0
        || latency_builder.count () == 0) {
        co_return false;
    }
    const perf::single::latency_stats_t latency = latency_builder.snapshot ();

    perf::single::emit_single_socket_hwm_detail (bind_socket.sock (), "DEALER_DEALER", transport,
                                                 "receiver", "dealer", msg_size);
    perf::single::emit_single_socket_hwm_detail (conn_socket.sock (), "DEALER_DEALER", transport,
                                                 "sender", "dealer", msg_size);

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    perf::single::print_result (lib_name, "DEALER_DEALER", transport, msg_size, throughput,
                                latency.mean_ns, latency.p95_ns, latency.p99_ns);
    co_return true;
}

bool run_pattern_dealer_dealer (const std::string &transport,
                                size_t msg_size,
                                const std::string &lib_name)
{
    return run_pattern_dealer_dealer_async (transport, msg_size, lib_name).get ();
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_dealer_dealer);
}
