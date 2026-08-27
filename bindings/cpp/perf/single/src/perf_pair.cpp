// PAIR benchmark: one-way sender->receiver loop inside one process.
// Topology: sender(PAIR connect) -> receiver(PAIR bind)

#include "../common/perf_single_common.hpp"
#include "../common/perf_single_runner.hpp"

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

namespace
{

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

int recv_pair_payload (zlink::pair_socket_t &socket_,
                       zlink::received_t &received_,
                       zlink::recv_flags_t flags_)
{
    try {
        return socket_.recv (received_, flags_);
    }
    catch (const zlink::recv_error_t &err) {
        errno = err.internal_errno ();
        return -1;
    }
}

bool record_pair_payload (zlink::received_t &received,
                          const zlink::message_t &payload,
                          uint32_t run_id,
                          size_t msg_size,
                          size_t payload_size,
                          const std::chrono::steady_clock::time_point &active_deadline,
                          std::atomic<unsigned long long> &received_count,
                          perf::single::latency_stats_builder_t &latency_builder)
{
    if (payload.size () != payload_size) {
        received.close ();
        return true;
    }

    perf_single_metric::header_t header;
    if (!perf_single_metric::decode_payload_header (payload.data (), payload.size (), &header)) {
        received.close ();
        return true;
    }
    // The C reference closes both raw parts before it checks the active window
    // and records now - sent_ts. Release this envelope at the same endpoint;
    // its vector capacity remains available for the next receive.
    received.close ();
    if (!perf_single_metric::is_expected (header, run_id, perf_single_metric::phase_active,
                                          msg_size)) {
        return true;
    }
    if (std::chrono::steady_clock::now () >= active_deadline)
        return true;

    // The receiver thread is joined before this counter is read, matching the
    // C reference harness; no release operation is needed for synchronization.
    received_count.fetch_add (1, std::memory_order_relaxed);
    const uint64_t now = perf_single_metric::now_ns ();
    latency_builder.add (perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
    return true;
}

} // namespace

bool run_pattern_pair (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        if (perf_debug_enabled ())
            std::cerr << "pair: transport unavailable" << std::endl;
        std::cout << "UNSUPPORTED," << lib_name << ",PAIR," << transport << std::endl;
        return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        if (perf_debug_enabled ())
            std::cerr << "pair: invalid context" << std::endl;
        return false;
    }

    zlink::pair_socket_t bind_socket (ctx.ctx ());
    zlink::pair_socket_t conn_socket (ctx.ctx ());
    if (!bind_socket.valid () || !conn_socket.valid ()) {
        if (perf_debug_enabled ())
            std::cerr << "pair: invalid sockets" << std::endl;
        return false;
    }

    // Measurement semantics are owned by the C reference harness
    // (bindings/c/perf/single/src/perf_pair.cpp): it sets neither
    // tcp_no_delay nor a context auto-HWM recalculation for this pattern.
    // Adding them here would make the C++ cell measure a different socket and
    // context configuration than the paired C cell.

    if (!perf::single::setup_connected_pair (bind_socket, conn_socket, transport,
                                             lib_name + "_pair")) {
        if (perf_debug_enabled ())
            std::cerr << "pair: setup_connected_pair failed errno=" << errno << std::endl;
        return false;
    }

    const int recv_timeout = perf::single::resolve_single_recv_timeout_ms ();
    bind_socket.options ().recv_timeout (std::chrono::milliseconds (recv_timeout));
    conn_socket.options ().send_timeout (
      std::chrono::milliseconds (perf::single::resolve_single_send_timeout_ms ()));

    const int duration_s = perf::single::resolve_single_duration_seconds ();
    std::vector<char> payload (std::max<size_t> (msg_size, perf_single_metric::header_size ()),
                               '\0');
    const size_t payload_size = payload.size ();

    const uint32_t run_id = 1U;
    std::atomic<unsigned long long> sent_count (0);
    std::atomic<unsigned long long> received_count (0);
    std::atomic<bool> sender_ok (true);
    perf::single::latency_stats_builder_t latency_builder (
      perf::single::resolve_single_latency_sample_cap ());
    const auto active_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);

    std::thread receiver_thread ([&] () {
        zlink::poller_t poller;
        // receive() reuses caller-provided envelope storage. Keep this object
        // for the entire drain loop so the two-part measurement message does
        // not allocate a new parts vector for every received message.
        zlink::received_t received;
        try {
            poller.add (bind_socket, zlink::poll_event_flag_t::pollin, 0);
        }
        catch (const zlink::config_error_t &) {
            sender_ok.store (false, std::memory_order_release);
            return;
        }

        while (true) {
            try {
                zlink::poll_event_t event;
                if (poller.wait (&event, 1, std::chrono::milliseconds (-1)) == 0)
                    continue;
            }
            catch (const zlink::recv_error_t &) {
                sender_ok.store (false, std::memory_order_release);
                return;
            }

            for (;;) {
                const int recv_rc =
                  recv_pair_payload (bind_socket, received, zlink::recv_flags_t::dontwait);
                if (recv_rc != 0) {
                    if (errno == EAGAIN || errno == EINTR)
                        break;
                    sender_ok.store (false, std::memory_order_release);
                    return;
                }
                const std::vector<zlink::message_t> &parts = received.parts ();
                if (parts.size () == 1 && perf::single::is_stop_token_message (parts.front ())) {
                    return;
                }
                const zlink::message_t *part = perf::single::measurement_payload_part (parts);
                if (!part)
                    continue;
                if (!record_pair_payload (received, *part, run_id, msg_size, payload_size,
                                          active_deadline, received_count, latency_builder)) {
                    sender_ok.store (false, std::memory_order_release);
                    return;
                }
            }
        }
    });

    std::thread sender_thread ([&] () {
        uint64_t seq = 0;
        // C-faithful send model (bindings/c/perf single
        // perf_single_one_way.hpp send_active_samples +
        // send_socket_active_message with ZLINK_SEND_FLAGS_NONE,
        // retry_on_eagain=true): on transient backpressure, re-stamp a
        // fresh timestamp, wait 1 ms, and retry.
        while (std::chrono::steady_clock::now () < active_deadline) {
            if (!perf_single_metric::stamp_payload (payload.data (), payload.size (), run_id,
                                                    perf_single_metric::phase_active, msg_size, seq,
                                                    perf_single_metric::now_ns ())) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            const int send_rc =
              perf::single::send_payload_active (conn_socket, payload.data (), payload.size ());
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
        if (!perf::single::send_stop_token_blocking (conn_socket))
            sender_ok.store (false, std::memory_order_release);
    });

    sender_thread.join ();
    receiver_thread.join ();

    const unsigned long long active_received = received_count.load (std::memory_order_acquire);
    if (!sender_ok.load (std::memory_order_acquire) || active_received == 0
        || latency_builder.count () == 0) {
        if (perf_debug_enabled ())
            std::cerr << "pair: active phase failed received=" << active_received << std::endl;
        return false;
    }
    const perf::single::latency_stats_t latency = latency_builder.snapshot ();

    perf::single::emit_single_socket_hwm_detail (bind_socket, "PAIR", transport, "receiver", "pair",
                                                 msg_size);
    perf::single::emit_single_socket_hwm_detail (conn_socket, "PAIR", transport, "sender", "pair",
                                                 msg_size);

    const double throughput =
      duration_s > 0 ? static_cast<double> (active_received) / duration_s : 0.0;
    perf::single::print_result (lib_name, "PAIR", transport, msg_size, throughput, latency.mean_ns,
                                latency.p95_ns, latency.p99_ns);
    return true;
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_pair);
}
