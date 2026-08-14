// DEALER-ROUTER benchmark: one-way dealer->router.

#include "../common/perf_single_common.hpp"
#include "../common/perf_single_runner.hpp"

#include <string>
#include <thread>
#include <vector>

namespace
{

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

bool wait_dealer_router_monitor_event (zlink::socket_monitor_t &monitor_,
                                       perf::single::perf_socket_t *activity_socket_,
                                       uint64_t success_event_,
                                       int timeout_ms_)
{
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1);

    zlink::poller_t poller;
    static const std::uintptr_t monitor_slot = 1;
    static const std::uintptr_t activity_slot = 2;
    try {
        poller.add (monitor_, zlink::poll_event_flag_t::pollin, monitor_slot);
        if (activity_socket_)
            activity_socket_->poller_add (poller, zlink::poll_event_flag_t::pollin, activity_slot);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    while (std::chrono::steady_clock::now () < deadline) {
        long wait_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                         deadline - std::chrono::steady_clock::now ())
                         .count ();
        if (wait_ms < 1)
            wait_ms = 1;

        zlink::poll_event_t poll_event;
        if (poller.wait (&poll_event, 1, std::chrono::milliseconds (wait_ms)) == 0)
            continue;
        if (poll_event.slot != monitor_slot)
            continue;

        for (;;) {
            const std::optional<zlink::monitor_event_t> event =
              monitor_.recv (static_cast<int> (zlink::send_flags_t::dontwait));
            if (!event)
                break;
            if (static_cast<uint64_t> (event->event) == success_event_)
                return true;
        }
    }
    return false;
}

bool setup_dealer_router_session (perf::single::perf_socket_t &router_,
                                  perf::single::perf_socket_t &dealer_,
                                  const std::string &transport_,
                                  const std::string &id_)
{
    if (!perf::setup_tls_server (router_, transport_)
        || !perf::setup_tls_client (dealer_, transport_)) {
        return false;
    }

    perf::single::apply_single_hwm (router_);
    perf::single::apply_single_hwm (dealer_);

    const std::string endpoint = perf::single::bind_and_resolve_endpoint (router_, transport_, id_);
    if (endpoint.empty ())
        return false;

    zlink::socket_monitor_t router_monitor =
      router_.monitor_open (zlink::monitor_event::connection_ready);
    zlink::socket_monitor_t dealer_monitor =
      dealer_.monitor_open (zlink::monitor_event::connection_ready);
    if (!router_monitor.valid () || !dealer_monitor.valid ())
        return false;

    try {
        dealer_.connect (endpoint);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    perf::single::apply_single_benchmark_socket_options (router_, transport_);
    perf::single::apply_single_benchmark_socket_options (dealer_, transport_);

    const int ready_timeout_ms = perf::single::resolve_single_connect_ready_timeout_ms ();
    return wait_dealer_router_monitor_event (
             router_monitor, &router_,
             static_cast<uint64_t> (zlink::monitor_event::connection_ready), ready_timeout_ms)
           && wait_dealer_router_monitor_event (
             dealer_monitor, NULL, static_cast<uint64_t> (zlink::monitor_event::connection_ready),
             ready_timeout_ms);
}

} // namespace

perf::async_task_t<bool> run_pattern_dealer_router_async (const std::string &transport,
                                                          size_t msg_size,
                                                          const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",DEALER_ROUTER," << transport << std::endl;
        co_return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        co_return false;
    }

    perf::single::socket_guard_t router (ctx, zlink::socket_type::router);
    perf::single::socket_guard_t dealer (ctx, zlink::socket_type::dealer);
    if (!router.valid () || !dealer.valid ()) {
        co_return false;
    }

    if (!perf::single::recalculate_single_auto_hwm (ctx)) {
        co_return false;
    }
    if (!setup_dealer_router_session (router.sock (), dealer.sock (), transport,
                                      lib_name + "_dealer_router")) {
        co_return false;
    }

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');

    const uint32_t run_id = 1U;
    const int duration_s = std::max (1, perf::single::resolve_single_duration_seconds ());
    std::atomic<unsigned long long> sent_count (0);
    std::atomic<bool> sender_ok (true);
    perf::single::latency_stats_builder_t latency_builder (
      perf::single::resolve_single_latency_sample_cap ());
    const auto active_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);

    // PERF_SINGLE_TEST_POLICY § 1.1 and § 1.4: use the public poller with an
    // infinite wait, receive routed parts with DONTWAIT, and drain all ready
    // parts before waiting again. The wire-level stop token terminates the
    // receiver after the active samples have arrived.
    zlink::poller_t recv_poller;
    try {
        router.sock ().poller_add (recv_poller, zlink::poll_event_flag_t::pollin, 0);
    }
    catch (const zlink::config_error_t &) {
        co_return false;
    }

    auto sender_work = [&] () -> perf::async_task_t<void> {
        uint64_t seq = 1;
        while (std::chrono::steady_clock::now () < active_deadline) {
            // Keep this measured send hot path aligned with the C reference:
            // stamp reusable caller storage, then copy the full payload into
            // the message submitted by the binding.
            if (!perf_single_metric::stamp_payload (
                  payload.data (), payload.size (), run_id, perf_single_metric::phase_active,
                  msg_size, seq, perf_single_metric::now_ns ())) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            zlink::message_t msg =
              perf::single::message_from_payload (payload.data (), payload.size ());
            if (!msg.valid ()) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }

            int send_rc = -1;
            try {
                co_await dealer.sock ().send_async (msg);
                send_rc = 1;
            }
            catch (const zlink::binding_error_t &err) {
                errno = err.internal_errno ();
                send_rc = perf::single::is_transient_send_errno (errno) ? 0 : -1;
            }
            if (send_rc <= 0) {
                const int err = errno;
                if (perf::single::is_transient_send_errno (err)
                    && std::chrono::steady_clock::now () < active_deadline) {
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
                    continue;
                }
                if (perf::single::is_transient_send_errno (err))
                    break;
                if (perf_debug_enabled ())
                    std::cerr << "dealer_router: send failed errno=" << err << std::endl;
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            sent_count.fetch_add (1, std::memory_order_release);
            ++seq;
        }
        // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with one
        // wire-level blocking stop token.
        if (!co_await perf::single::send_stop_token_async (dealer.sock ()))
            sender_ok.store (false, std::memory_order_release);
    };

    perf::async_task_t<void> sender_task = sender_work ();

    unsigned long long received_count = 0;
    {
        zlink::routing_id_t source_rid = zlink::routing_id_t::from (std::string ("placeholder"));
        bool stop_received = false;
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
            ++received_count;
            const uint64_t now = perf_single_metric::now_ns ();
            latency_builder.add (perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
            return true;
        };

        while (!stop_received) {
            try {
                zlink::poll_event_t poll_event;
                if (recv_poller.wait (&poll_event, 1, std::chrono::milliseconds (-1)) == 0)
                    continue;
            }
            catch (const zlink::recv_error_t &) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }

            zlink::message_t part;
            const int recv_rc = router.sock ().recv (
              source_rid, part, static_cast<int> (zlink::recv_flags_t::dontwait));
            if (recv_rc != 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                if (perf_debug_enabled ())
                    std::cerr << "dealer_router: recv failed errno=" << errno << std::endl;
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            if (!handle_part (part, &stop_received)) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            if (stop_received)
                break;

            for (;;) {
                zlink::message_t burst;
                const int burst_rc = router.sock ().recv (
                  source_rid, burst, static_cast<int> (zlink::recv_flags_t::dontwait));
                if (burst_rc != 0) {
                    if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
                        break;
                    if (perf_debug_enabled ())
                        std::cerr << "dealer_router: recv failed errno=" << errno << std::endl;
                    sender_ok.store (false, std::memory_order_release);
                    break;
                }
                bool burst_stop = false;
                if (!handle_part (burst, &burst_stop)) {
                    sender_ok.store (false, std::memory_order_release);
                    break;
                }
                if (burst_stop) {
                    stop_received = true;
                    break;
                }
            }
            if (!sender_ok.load (std::memory_order_acquire))
                break;
        }
    }

    co_await std::move (sender_task);

    if (!sender_ok.load (std::memory_order_acquire) || received_count == 0
        || latency_builder.count () == 0) {
        if (perf_debug_enabled ())
            std::cerr << "dealer_router: no active data sent="
                      << sent_count.load (std::memory_order_acquire)
                      << " received=" << received_count << std::endl;
        co_return false;
    }

    const perf::single::latency_stats_t latency = latency_builder.snapshot ();

    perf::single::emit_single_socket_hwm_detail (router.sock (), "DEALER_ROUTER", transport,
                                                 "receiver", "router", msg_size);
    perf::single::emit_single_socket_hwm_detail (dealer.sock (), "DEALER_ROUTER", transport,
                                                 "sender", "dealer", msg_size);

    const double throughput =
      static_cast<double> (received_count) / static_cast<double> (duration_s);
    perf::single::print_result (lib_name, "DEALER_ROUTER", transport, msg_size, throughput,
                                latency.mean_ns, latency.p95_ns, latency.p99_ns);
    co_return true;
}

bool run_pattern_dealer_router (const std::string &transport,
                                size_t msg_size,
                                const std::string &lib_name)
{
    return run_pattern_dealer_router_async (transport, msg_size, lib_name).get ();
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_dealer_router);
}
