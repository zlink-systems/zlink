// ROUTER-ROUTER benchmark: one-way router->router with explicit handshake.

#include "../common/perf_single_common.hpp"
#include "../common/perf_single_runner.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{

const char *const k_receiver_id = "ROUTER1";
const char *const k_sender_id = "ROUTER2";

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

struct router_router_recv_state_t
{
    router_router_recv_state_t () : run_id (0), msg_size (0), payload_size (0), latency () {}

    uint32_t run_id;
    size_t msg_size;
    size_t payload_size;
    std::optional<zlink::routing_id_t> target_rid;
    perf::single::latency_stats_builder_t latency;
};

bool record_router_router_sample (uint32_t run_id_,
                                  size_t msg_size_,
                                  size_t payload_size_,
                                  zlink::message_t &part_,
                                  perf::single::latency_stats_builder_t *latency_,
                                  unsigned long long *received_)
{
    if (!latency_ || !received_)
        return false;

    if (part_.size () != payload_size_)
        return true;

    perf_single_metric::header_t header;
    if (!perf_single_metric::decode_payload_header (part_.data (), part_.size (), &header)) {
        return true;
    }

    if (!perf_single_metric::is_expected (header, run_id_, perf_single_metric::phase_active,
                                          msg_size_)) {
        return true;
    }

    ++(*received_);
    const uint64_t now = perf_single_metric::now_ns ();
    const double latency_ns = perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns);
    latency_->add (latency_ns);
    return true;
}

perf::async_task_t<bool> send_router_samples (::perf::socket_t *sender_,
                                              std::vector<char> *payload_,
                                              router_router_recv_state_t *state_,
                                              int duration_s_,
                                              std::atomic<unsigned long long> *sent_count_)
{
    if (!sender_ || !payload_ || !state_ || !sent_count_)
        co_return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (std::max (1, duration_s_));
    uint64_t seq = 1;
    while (std::chrono::steady_clock::now () < deadline) {
        if (!perf_single_metric::stamp_payload (payload_->data (), payload_->size (),
                                                state_->run_id, perf_single_metric::phase_active,
                                                state_->msg_size, seq,
                                                perf_single_metric::now_ns ())) {
            co_return false;
        }

        if (!state_->target_rid.has_value ())
            co_return false;

        const int send_rc = co_await perf::single::send_payload_active (
          *sender_, *state_->target_rid, payload_->data (), payload_->size ());
        if (send_rc <= 0) {
            const int err = errno;
            if (perf::single::is_transient_routed_send_errno (err)
                && std::chrono::steady_clock::now () < deadline) {
                continue;
            }
            if (perf::single::is_transient_routed_send_errno (err))
                break;
            if (perf_debug_enabled ())
                std::cerr << "router_router: send failed errno=" << err << std::endl;
            co_return false;
        }

        sent_count_->fetch_add (1, std::memory_order_release);
        ++seq;
    }

    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with one
    // wire-level blocking stop token.
    const bool stop_ok =
      co_await perf::single::send_stop_token_async (*sender_, *state_->target_rid);
    co_return stop_ok;
}

} // namespace

perf::async_task_t<bool> run_pattern_router_router_async (const std::string &transport,
                                                          size_t msg_size,
                                                          const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",ROUTER_ROUTER," << transport << std::endl;
        co_return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }

    perf::single::socket_guard_t receiver (ctx, zlink::socket_type::router);
    perf::single::socket_guard_t sender (ctx, zlink::socket_type::router);
    if (!receiver.valid () || !sender.valid ()) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }

    (void) receiver.sock ().set_routing_id (std::string (k_receiver_id));
    (void) sender.sock ().set_routing_id (std::string (k_sender_id));
    (void) receiver.sock ().set (perf::options::router_options::mandatory, 1);
    (void) sender.sock ().set (perf::options::router_options::mandatory, 1);
    (void) sender.sock ().set (perf::options::router_options::connect_routing_id,
                               std::string (k_receiver_id));
    if (!perf::single::recalculate_single_auto_hwm (ctx)) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }

    router_router_recv_state_t state;
    state.target_rid = zlink::routing_id_t::from (std::string (k_receiver_id));
    if (!perf::single::setup_connected_pair (receiver.sock (), sender.sock (), transport,
                                             lib_name + "_router_router")) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }
    const bool handshake_ok = co_await perf::single::complete_router_router_handshake (
      receiver.sock (), sender.sock (), *state.target_rid, &(*state.target_rid));
    if (!handshake_ok) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }

    const int recv_timeout = perf::single::resolve_single_recv_timeout_ms ();
    (void) receiver.sock ().set_option (perf::options::socket_options::rcvtimeo, recv_timeout);
    (void) sender.sock ().set_option (perf::options::socket_options::sndtimeo,
                                      perf::single::resolve_single_send_timeout_ms ());

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');

    const uint32_t run_id = 1U;

    const int duration_s = std::max (1, perf::single::resolve_single_duration_seconds ());
    std::atomic<unsigned long long> sent_count (0);
    std::atomic<bool> sender_ok (true);
    state.run_id = run_id;
    state.msg_size = msg_size;
    state.payload_size = payload_size;
    perf::async_task_t<bool> sender_task =
      send_router_samples (&sender.sock (), &payload, &state, duration_s, &sent_count);
    unsigned long long received = 0;
    perf::single::latency_stats_t latency;
    // C-faithful receiver (bindings/c/perf single perf_router_router.cpp
    // run_active_phase): blocking recv (flags=0, bounded by rcvtimeo) into
    // a reused routing id and a single message_t, exiting on the wire-level
    // stop token. The previous poller.wait()+received_t drain allocated a
    // fresh std::vector<message_t> per message (received_t::parts ()
    // materialize), capping ROUTER_ROUTER throughput at ~76-80% of C; C
    // uses one reused zlink_msg_t recv buffer with no per-message heap
    // churn.
    {
        zlink::routing_id_t source_rid = zlink::routing_id_t::from (std::string ("placeholder"));
        bool stop_received = false;
        while (!stop_received) {
            zlink::message_t part;
            const int recv_rc = receiver.sock ().recv (source_rid, part, 0);
            if (recv_rc != 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                if (perf_debug_enabled ())
                    std::cerr << "router_router: recv failed errno=" << errno << std::endl;
                sender_ok.store (false, std::memory_order_release);
                break;
            }
            if (perf::single::is_stop_token_message (part)) {
                stop_received = true;
                break;
            }
            if (!record_router_router_sample (run_id, msg_size, payload_size, part, &state.latency,
                                              &received)) {
                sender_ok.store (false, std::memory_order_release);
                break;
            }
        }
    }

    sender_ok.store (co_await std::move (sender_task), std::memory_order_release);
    if (!sender_ok.load (std::memory_order_acquire)) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }
    // Stop token is the last in-flight message, so any earlier payloads
    // have already been recorded above. No bounded drain loop needed.

    if (received == 0 || state.latency.count () == 0) {
        if (perf_debug_enabled ())
            std::cerr << "router_router: no active data sent="
                      << sent_count.load (std::memory_order_acquire) << " received=" << received
                      << std::endl;
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        co_return false;
    }
    latency = state.latency.snapshot ();

    perf::single::emit_single_socket_hwm_detail (receiver.sock (), "ROUTER_ROUTER", transport,
                                                 "receiver", "router", msg_size);
    perf::single::emit_single_socket_hwm_detail (sender.sock (), "ROUTER_ROUTER", transport,
                                                 "sender", "router", msg_size);

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    perf::single::print_result (lib_name, "ROUTER_ROUTER", transport, msg_size, throughput,
                                latency.mean_ns, latency.p95_ns, latency.p99_ns);
    co_return true;
}

bool run_pattern_router_router (const std::string &transport,
                                size_t msg_size,
                                const std::string &lib_name)
{
    return run_pattern_router_router_async (transport, msg_size, lib_name).get ();
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_router_router);
}
