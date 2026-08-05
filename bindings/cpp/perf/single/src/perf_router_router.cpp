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

zlink::routing_id_t routing_id_from_ascii (const char *value_)
{
    return zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> (value_),
                                      std::strlen (value_));
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

bool complete_handshake (::perf::socket_t &receiver,
                         ::perf::socket_t &sender,
                         zlink::routing_id_t *target_rid_out_)
{
    zlink::routing_id_t receiver_rid = routing_id_from_ascii (k_receiver_id);

    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (perf::single::parse_positive_env (
                            "PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000));
    bool connected = false;
    std::optional<zlink::routing_id_t> sender_actual_rid;
    zlink::poller_t poller;
    receiver.poller_add (poller, zlink::poll_event_flag_t::pollin);
    while (!connected && std::chrono::steady_clock::now () < deadline) {
        zlink::message_t outbound = zlink::message_t::from ("PING");
        if (!outbound.valid ())
            return false;

        if (perf::send_socket (sender, receiver_rid, outbound,
                               static_cast<int> (zlink::send_flags_t::dontwait))
            != 0) {
            const int err = errno;
            if (err != EAGAIN && err != EINTR && err != EHOSTUNREACH && err != ENOTCONN) {
                if (perf_debug_enabled ())
                    std::cerr << "router_router: handshake request failed errno=" << err
                              << std::endl;
                return false;
            }
        } else {
            for (;;) {
                zlink::received_t inbound;
                if (receiver.receive (inbound, static_cast<int> (zlink::send_flags_t::dontwait))
                    != 0) {
                    if (errno == EAGAIN || errno == EINTR)
                        break;
                    if (perf_debug_enabled ())
                        std::cerr << "router_router: handshake receive failed errno=" << errno
                                  << std::endl;
                    return false;
                }
                connected = inbound.routing_id ().has_value () && inbound.parts ().size () == 1
                            && inbound.parts ()[0].to_string () == "PING";
                if (connected) {
                    sender_actual_rid = *inbound.routing_id ();
                    break;
                }
            }
        }

        if (!connected) {
            zlink::poll_event_t event;
            (void) poller.wait (&event, 1, std::chrono::milliseconds (-1));
        }
    }

    if (!connected || !sender_actual_rid.has_value ())
        return false;

    zlink::message_t reply = zlink::message_t::from ("PONG");
    if (!reply.valid () || perf::send_socket (receiver, *sender_actual_rid, reply) != 0) {
        if (perf_debug_enabled ())
            std::cerr << "router_router: handshake reply send failed errno=" << errno << std::endl;
        return false;
    }

    zlink::received_t response;
    if (sender.receive (response, 0) != 0) {
        if (perf_debug_enabled ())
            std::cerr << "router_router: handshake response recv failed errno=" << errno
                      << std::endl;
        return false;
    }
    const bool ok = response.routing_id ().has_value () && response.parts ().size () == 1
                    && response.parts ()[0].to_string () == "PONG";
    if (ok && target_rid_out_)
        *target_rid_out_ = *response.routing_id ();
    if (!ok && perf_debug_enabled ())
        std::cerr << "router_router: handshake response failed errno=" << errno << std::endl;
    return ok;
}

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

bool send_router_samples (::perf::socket_t *sender_,
                          std::vector<char> *payload_,
                          router_router_recv_state_t *state_,
                          int duration_s_,
                          std::atomic<unsigned long long> *sent_count_)
{
    if (!sender_ || !payload_ || !state_ || !sent_count_)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (std::max (1, duration_s_));
    uint64_t seq = 1;
    while (std::chrono::steady_clock::now () < deadline) {
        if (!perf_single_metric::stamp_payload (payload_->data (), payload_->size (),
                                                state_->run_id, perf_single_metric::phase_active,
                                                state_->msg_size, seq,
                                                perf_single_metric::now_ns ())) {
            return false;
        }

        if (!state_->target_rid.has_value ())
            return false;

        if (!perf::single::send_payload_blocking (*sender_, *state_->target_rid, payload_->data (),
                                                  payload_->size ())) {
            const int err = errno;
            if (perf::single::is_transient_routed_send_errno (err)
                && std::chrono::steady_clock::now () < deadline) {
                continue;
            }
            if (perf::single::is_transient_routed_send_errno (err))
                break;
            if (perf_debug_enabled ())
                std::cerr << "router_router: send failed errno=" << err << std::endl;
            return false;
        }

        sent_count_->fetch_add (1, std::memory_order_release);
        ++seq;
    }

    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with one
    // wire-level blocking stop token.
    return perf::single::send_stop_token_blocking (*sender_, *state_->target_rid);
}

} // namespace

bool run_pattern_router_router (const std::string &transport,
                                size_t msg_size,
                                const std::string &lib_name)
{
    if (!perf::single::transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",ROUTER_ROUTER," << transport << std::endl;
        return true;
    }

    perf::single::ctx_guard_t ctx;
    if (!ctx.valid ()) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
    }

    perf::single::socket_guard_t receiver (ctx, zlink::socket_type::router);
    perf::single::socket_guard_t sender (ctx, zlink::socket_type::router);
    if (!receiver.valid () || !sender.valid ()) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
    }

    (void) receiver.sock ().set_routing_id (std::string (k_receiver_id));
    (void) sender.sock ().set_routing_id (std::string (k_sender_id));
    (void) receiver.sock ().set (perf::options::router_options::mandatory, 1);
    (void) sender.sock ().set (perf::options::router_options::mandatory, 1);
    if (!perf::single::apply_single_auto_hwm_msg_unit (ctx, msg_size)
        || !perf::single::recalculate_single_auto_hwm (ctx)) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
    }

    router_router_recv_state_t state;
    state.target_rid = routing_id_from_ascii (k_receiver_id);
    if (!perf::single::setup_connected_pair (receiver.sock (), sender.sock (), transport,
                                             lib_name + "_router_router")
        || !complete_handshake (receiver.sock (), sender.sock (), &(*state.target_rid))) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
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
    std::thread sender_thread ([&] () {
        sender_ok.store (
          send_router_samples (&sender.sock (), &payload, &state, duration_s, &sent_count),
          std::memory_order_release);
    });
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

    sender_thread.join ();
    if (!sender_ok.load (std::memory_order_acquire)) {
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
    }
    // Stop token is the last in-flight message, so any earlier payloads
    // have already been recorded above. No bounded drain loop needed.

    if (received == 0 || state.latency.count () == 0) {
        if (perf_debug_enabled ())
            std::cerr << "router_router: no active data sent="
                      << sent_count.load (std::memory_order_acquire) << " received=" << received
                      << std::endl;
        perf::single::print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
        return false;
    }
    latency = state.latency.snapshot ();

    perf::single::emit_single_socket_hwm_detail (receiver.sock (), "ROUTER_ROUTER", transport,
                                                 "receiver", "router", msg_size);
    perf::single::emit_single_socket_hwm_detail (sender.sock (), "ROUTER_ROUTER", transport,
                                                 "sender", "router", msg_size);

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    perf::single::print_result (lib_name, "ROUTER_ROUTER", transport, msg_size, throughput,
                                latency.mean_ns, latency.p95_ns, latency.p99_ns);
    return true;
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern_router_router);
}
