#include "../common/bench_common.hpp"
#include "../common/perf_single_latency.hpp"
#include "../common/perf_single_metric_header.hpp"
#include "../common/perf_single_monitor.hpp"
#include "../common/perf_single_phase.hpp"
#include <zlink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

struct dealer_router_recv_state_t
{
    dealer_router_recv_state_t () : run_id (0), msg_size (0), payload_size (0), active_received (0)
    {
    }

    uint32_t run_id;
    size_t msg_size;
    size_t payload_size;
    std::atomic<unsigned long long> active_received;
    latency_stats_builder_t latency;
};

bool setup_dealer_router_session (void *router_,
                                  void *dealer_,
                                  const std::string &transport_,
                                  const std::string &pair_id_)
{
    if (!router_ || !dealer_)
        return false;

    if (!setup_tls_server (router_, transport_) || !setup_tls_client (dealer_, transport_)) {
        return false;
    }

    apply_single_hwm (router_);
    apply_single_hwm (dealer_);

    const std::string endpoint = bind_and_resolve_endpoint (router_, transport_, pair_id_);
    if (endpoint.empty ())
        return false;

    void *router_monitor = open_configured_socket_monitor (router_, ZLINK_EVENT_CONNECTION_READY);
    if (!router_monitor)
        return false;
    void *dealer_monitor = open_configured_socket_monitor (dealer_, ZLINK_EVENT_CONNECTION_READY);
    if (!dealer_monitor) {
        zlink_monitor_close (&router_monitor);
        return false;
    }

    if (!connect_checked (dealer_, endpoint)) {
        zlink_monitor_close (&dealer_monitor);
        zlink_monitor_close (&router_monitor);
        return false;
    }

    apply_single_benchmark_socket_options (router_, transport_);
    apply_single_benchmark_socket_options (dealer_, transport_);

    const int ready_timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool router_ready = wait_for_socket_monitor_event_with_activity (
      router_monitor, router_, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    const bool dealer_ready = wait_for_socket_monitor_event (
      dealer_monitor, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    zlink_monitor_close (&dealer_monitor);
    zlink_monitor_close (&router_monitor);
    if (!router_ready || !dealer_ready)
        return false;

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (router_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (dealer_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    if (bench_debug_enabled ())
        std::cerr << "[perf-dealer-router] setup complete" << std::endl;
    return true;
}

int recv_router_header_flags (void *router_,
                              size_t payload_size_,
                              int flags_,
                              perf_single_metric::header_t *header_out_,
                              bool *header_ok_out_)
{
    if (!router_)
        return -1;

    if (header_ok_out_)
        *header_ok_out_ = false;

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return -1;
    const int rc = zlink_router_recv_part (router_, &source_rid, &request_seq, &part, &has_more,
                                           static_cast<zlink_recv_flags_t> (flags_));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    if (!source_rid || source_rid->size == 0 || request_seq != 0
        || has_more != ZLINK_PART_FINAL) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-dealer-router] invalid routed recv"
                      << " rid_size=" << static_cast<int> (source_rid ? source_rid->size : 0)
                      << " request_seq=" << request_seq
                      << " has_more=" << static_cast<int> (has_more) << std::endl;
        }
        zlink_msg_close (&part);
        return -1;
    }

    const size_t actual_size = zlink_msg_size (&part);
    if (is_stop_token (zlink_msg_data (&part), actual_size)) {
        zlink_msg_close (&part);
        return 2; // stop token
    }
    const bool size_ok = actual_size == payload_size_;
    bool header_ok = false;
    if (size_ok && header_out_) {
        header_ok = perf_single_metric::decode_payload_header (zlink_msg_data (&part), actual_size,
                                                               header_out_);
    }
    zlink_msg_close (&part);
    if (!size_ok) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-dealer-router] unexpected payload size=" << actual_size
                      << " expected=" << payload_size_ << std::endl;
        }
        return -1;
    }

    if (header_ok_out_)
        *header_ok_out_ = header_ok;
    return 1;
}

// PERF_SINGLE_TEST_POLICY § 1.4: send wire-level stop token from dealer
// to router so the receiver loop exits via is_stop_token. Bounded retry
// through transient backpressure.
bool send_dealer_stop_token (void *sender_)
{
    for (int retry = 0; retry < 100; ++retry) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, std::strlen (k_stop_token)) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, std::strlen (k_stop_token));
        if (perf_zlink_send_parts (sender_, &part, 1, ZLINK_SEND_FLAGS_NONE) == 0)
            return true;
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK && err != ETIMEDOUT)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

bool send_dealer_samples (void *sender_,
                          std::vector<char> *payload_,
                          dealer_router_recv_state_t *state_,
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

        zlink_msg_t part;
        if (zlink_msg_init_size (&part, payload_->size ()) != 0)
            return false;
        if (!payload_->empty ())
            std::memcpy (zlink_msg_data (&part), payload_->data (), payload_->size ());

        if (perf_zlink_send_parts (sender_, &part, 1, ZLINK_SEND_FLAGS_NONE) != 0) {
            const int err = zlink_errno ();
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-dealer-router] send failed err=" << err << std::endl;
            }
            if (err == EINTR || err == EAGAIN)
                continue;
            return false;
        }

        sent_count_->fetch_add (1, std::memory_order_release);
        ++seq;
    }

    return true;
}

int send_dealer_probe_once (void *sender_,
                            std::vector<char> *payload_,
                            dealer_router_recv_state_t *state_)
{
    if (!sender_ || !payload_ || !state_)
        return -1;

    if (!perf_single_metric::stamp_payload (payload_->data (), payload_->size (), state_->run_id,
                                            perf_single_metric::phase_active, state_->msg_size, 0,
                                            perf_single_metric::now_ns ())) {
        return -1;
    }

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_->size ()) != 0)
        return -1;
    if (!payload_->empty ()) {
        std::memcpy (zlink_msg_data (&part), payload_->data (), payload_->size ());
    }

    if (perf_zlink_send_parts (sender_, &part, 1, ZLINK_DONTWAIT) == 0)
        return 1;

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    if (err == EINTR || err == EAGAIN)
        return 0;
    return -1;
}

bool wait_for_dealer_router_ready (void *sender_,
                                   void *receiver_,
                                   std::vector<char> *payload_,
                                   dealer_router_recv_state_t *state_)
{
    if (!sender_ || !receiver_ || !payload_ || !state_)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000));
    while (std::chrono::steady_clock::now () < deadline) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-dealer-router] ready probe send" << std::endl;
        const int send_probe_rc = send_dealer_probe_once (sender_, payload_, state_);
        if (send_probe_rc < 0)
            return false;
        if (send_probe_rc == 0) {
            (void) perf_socket_poll (NULL, 0, 1);
            continue;
        }

        const auto probe_deadline =
          std::min (deadline, std::chrono::steady_clock::now () + std::chrono::milliseconds (50));
        while (wait_socket_event_until (receiver_, ZLINK_POLLIN, probe_deadline)) {
            for (;;) {
                perf_single_metric::header_t header;
                bool header_ok = false;
                const int recv_rc = recv_router_header_flags (receiver_, state_->payload_size,
                                                              ZLINK_DONTWAIT, &header, &header_ok);
                if (recv_rc < 0)
                    return false;
                if (recv_rc == 0)
                    break;
                if (bench_debug_enabled ()) {
                    std::cerr << "[perf-dealer-router] ready probe recv"
                              << " header_ok=" << (header_ok ? 1 : 0) << " run=" << header.run_id
                              << " phase=" << static_cast<unsigned int> (header.phase) << std::endl;
                }
                if (header_ok && single_header_matches_run (*state_, header))
                    return true;
            }
        }
    }

    return false;
}

bool run_active_phase (void *sender_,
                       void *receiver_,
                       std::vector<char> *payload_,
                       dealer_router_recv_state_t *state_,
                       bool use_nonblocking_recv_,
                       int duration_s_,
                       int recv_timeout_ms_,
                       unsigned long long *received_out_,
                       latency_stats_t *latency_out_)
{
    (void) use_nonblocking_recv_;
    (void) recv_timeout_ms_;
    if (!sender_ || !receiver_ || !payload_ || !state_ || !received_out_ || !latency_out_) {
        return false;
    }

    state_->active_received.store (0, std::memory_order_release);
    state_->latency = latency_stats_builder_t ();

    std::atomic<unsigned long long> sent_count (0);
    std::atomic<bool> sender_ok (true);
    unsigned long long received = 0;
    latency_stats_builder_t latency_builder;
    // PERF_SINGLE_TEST_POLICY § 1.4: sender thread emits active samples,
    // then sends a wire-level stop token so the receiver loop terminates
    // without consulting any atomic flag.
    std::thread sender_thread ([&] () {
        const bool active_ok =
          send_dealer_samples (sender_, payload_, state_, duration_s_, &sent_count);
        const bool stop_ok = send_dealer_stop_token (sender_);
        sender_ok.store (active_ok && stop_ok, std::memory_order_release);
    });

    // Receiver: blocking recv, exit on stop-token receipt. Socket
    // recv_timeout still bounds individual recv calls; phase end is
    // signaled purely by the stop token arriving on the wire.
    while (true) {
        perf_single_metric::header_t header;
        bool header_ok = false;
        const int recv_rc =
          recv_router_header_flags (receiver_, state_->payload_size, 0, &header, &header_ok);
        if (recv_rc == 1) {
            if (header_ok && single_header_matches_run (*state_, header)) {
                ++received;
                latency_builder.add (single_latency_ns (header));
            }
            continue;
        }
        if (recv_rc == 0)
            continue;
        if (recv_rc == 2)
            break;
        sender_ok.store (false, std::memory_order_release);
        break;
    }

    sender_thread.join ();

    if (!sender_ok.load (std::memory_order_acquire)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-dealer-router] active phase failed sent="
                      << sent_count.load (std::memory_order_relaxed) << " received=" << received
                      << std::endl;
        }
        return false;
    }

    *received_out_ = received;
    if (*received_out_ == 0)
        return false;
    *latency_out_ = latency_builder.snapshot ();
    if (latency_builder.count () == 0)
        return false;
    return true;
}

} // namespace

void run_dealer_router (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail = [&] () {
        print_fail_result (lib_name, "DEALER_ROUTER", transport, msg_size);
    };

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    dealer_router_recv_state_t state;
    state.run_id = next_single_metric_run_id ();
    state.msg_size = msg_size;
    state.payload_size = payload_size;

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }
    if (!apply_single_auto_hwm_msg_unit (ctx.get (), msg_size)) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    socket_guard_t receiver (ctx.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t sender (ctx.get (), ZLINK_SOCKET_DEALER);
    if (!receiver.valid () || !sender.valid ()) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    if (!setup_dealer_router_session (receiver.get (), sender.get (), transport,
                                      lib_name + "_dealer_router")) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }
    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    const int recv_timeout_ms = resolve_single_recv_timeout_ms ();
    unsigned long long received = 0;
    latency_stats_t latency;
    if (!run_active_phase (sender.get (), receiver.get (), &payload, &state, false, duration_s,
                           recv_timeout_ms, &received, &latency)) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }
    if (bench_debug_enabled ())
        std::cerr << "[perf-dealer-router] active complete" << std::endl;

    emit_single_socket_hwm_detail (receiver.get (), "DEALER_ROUTER", transport, "receiver",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    emit_single_socket_hwm_detail (sender.get (), "DEALER_ROUTER", transport, "sender",
                                   ZLINK_SOCKET_DEALER, msg_size);
    print_result (lib_name, "DEALER_ROUTER", transport, msg_size,
                  static_cast<double> (received) / static_cast<double> (duration_s),
                  latency.mean_ns, latency.p95_ns, latency.p99_ns);
    fflush (NULL);
    std::_Exit (0);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "DEALER_ROUTER", run_dealer_router);
}
