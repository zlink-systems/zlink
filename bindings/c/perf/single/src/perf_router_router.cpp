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

struct router_router_recv_state_t
{
    router_router_recv_state_t () : run_id (0), msg_size (0), payload_size (0), active_received (0)
    {
        std::memset (&target_rid, 0, sizeof (target_rid));
    }

    uint32_t run_id;
    size_t msg_size;
    size_t payload_size;
    zlink_routing_id_t target_rid;
    std::atomic<unsigned long long> active_received;
    latency_stats_builder_t latency;
};

inline void assign_routing_id (zlink_routing_id_t *rid_out_, const char *data_, size_t size_)
{
    if (!rid_out_)
        return;

    std::memset (rid_out_, 0, sizeof (*rid_out_));
    rid_out_->size = static_cast<uint8_t> (size_);
    if (size_ > 0)
        std::memcpy (rid_out_->data, data_, size_);
}

bool perform_router_router_handshake (void *receiver_,
                                      void *sender_,
                                      zlink_routing_id_t *target_rid_out_)
{
    if (!receiver_ || !sender_)
        return false;

    zlink_routing_id_t target_rid;
    assign_routing_id (&target_rid, "ROUTER1", 7);

    const auto deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (parse_positive_env ("PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000));
    bool connected = false;
    zlink_routing_id_t sender_actual_rid;
    std::memset (&sender_actual_rid, 0, sizeof (sender_actual_rid));
    while (!connected && std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t ping;
        if (zlink_msg_init_size (&ping, 4) != 0)
            return false;
        std::memcpy (zlink_msg_data (&ping), "PING", 4);

        if (perf_zlink_send_rid_parts (sender_, &target_rid, &ping, 1, ZLINK_DONTWAIT) != 0) {
            zlink_msg_close (&ping);
            const int err = zlink_errno ();
            if (err != EAGAIN && err != EINTR)
                return false;
        } else {
            const zlink_routing_id_t *source_rid = NULL;
            uint64_t request_seq = 0;
            zlink_msg_t part;
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            if (zlink_msg_init (&part) != 0)
                return false;
            const int recv_rc =
              zlink_router_recv_part (receiver_, &source_rid, &request_seq, &part,
                                      &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_rc == 0) {
                connected = source_rid && source_rid->size > 0 && request_seq == 0
                            && has_more == ZLINK_PART_FINAL && zlink_msg_size (&part) == 4
                            && std::memcmp (zlink_msg_data (&part), "PING", 4) == 0;
                if (connected)
                    assign_routing_id (&sender_actual_rid,
                                       reinterpret_cast<const char *> (source_rid->data),
                                       source_rid->size);
                zlink_msg_close (&part);
            } else if (zlink_errno () != EAGAIN && zlink_errno () != EINTR) {
                zlink_msg_close (&part);
                return false;
            } else {
                zlink_msg_close (&part);
            }
        }

        if (!connected) {
            zlink_pollitem_t item = {receiver_, 0, ZLINK_POLLIN, 0};
            const long timeout_ms = remaining_timeout_ms (deadline, 1);
            const int poll_rc = perf_socket_poll (&item, 1, timeout_ms);
            if (poll_rc < 0 && zlink_errno () != EINTR)
                return false;
        }
    }

    if (!connected)
        return false;

    if (sender_actual_rid.size == 0)
        assign_routing_id (&sender_actual_rid, "ROUTER2", 7);
    zlink_msg_t pong;
    if (zlink_msg_init_size (&pong, 4) != 0)
        return false;
    std::memcpy (zlink_msg_data (&pong), "PONG", 4);
    if (perf_zlink_send_rid_parts (receiver_, &sender_actual_rid, &pong, 1, ZLINK_SEND_FLAGS_NONE)
        != 0) {
        zlink_msg_close (&pong);
        return false;
    }

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return false;
    if (zlink_router_recv_part (sender_, &source_rid, &request_seq, &part,
                                &has_more, ZLINK_RECV_FLAGS_NONE)
        != 0) {
        zlink_msg_close (&part);
        return false;
    }

    const bool ok = source_rid && source_rid->size > 0 && request_seq == 0
                    && has_more == ZLINK_PART_FINAL && zlink_msg_size (&part) == 4
                    && std::memcmp (zlink_msg_data (&part), "PONG", 4) == 0;
    if (ok && target_rid_out_) {
        assign_routing_id (target_rid_out_, reinterpret_cast<const char *> (source_rid->data),
                           source_rid->size);
    }
    zlink_msg_close (&part);
    return ok;
}

void drain_router_socket (void *socket_)
{
    if (!socket_)
        return;

    for (;;) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        if (zlink_msg_init (&part) != 0)
            return;
        if (zlink_router_recv_part (socket_, &source_rid, &request_seq, &part,
                                    &has_more, ZLINK_RECV_FLAGS_DONTWAIT)
            != 0) {
            zlink_msg_close (&part);
            if (zlink_errno () == EAGAIN || zlink_errno () == EINTR)
                return;
            return;
        }
        zlink_msg_close (&part);
    }
}

bool setup_router_router_session (void *receiver_,
                                  void *sender_,
                                  zlink_routing_id_t *target_rid_out_,
                                  const std::string &transport_,
                                  const std::string &pair_id_)
{
    if (!receiver_ || !sender_)
        return false;

    zlink_set_routing_id (receiver_, "ROUTER1", 7);
    zlink_set_routing_id (sender_, "ROUTER2", 7);
    const int mandatory = 1;
    (void) zlink_set_router_option (receiver_, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                                    sizeof (mandatory));
    (void) zlink_set_router_option (sender_, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                                    sizeof (mandatory));

    if (!setup_tls_server (receiver_, transport_) || !setup_tls_client (sender_, transport_)) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] tls setup failed" << std::endl;
        return false;
    }

    apply_single_hwm (receiver_);
    apply_single_hwm (sender_);

    const std::string endpoint = bind_and_resolve_endpoint (receiver_, transport_, pair_id_);
    if (endpoint.empty ()) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] bind endpoint empty" << std::endl;
        return false;
    }

    void *receiver_monitor =
      open_configured_socket_monitor (receiver_, ZLINK_EVENT_CONNECTION_READY);
    if (!receiver_monitor) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] receiver monitor open failed" << std::endl;
        return false;
    }
    void *sender_monitor = open_configured_socket_monitor (sender_, ZLINK_EVENT_CONNECTION_READY);
    if (!sender_monitor) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] sender monitor open failed" << std::endl;
        zlink_monitor_close (&receiver_monitor);
        return false;
    }

    if (!connect_checked (sender_, endpoint)) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] connect failed" << std::endl;
        zlink_monitor_close (&sender_monitor);
        zlink_monitor_close (&receiver_monitor);
        return false;
    }

    apply_single_benchmark_socket_options (receiver_, transport_);
    apply_single_benchmark_socket_options (sender_, transport_);

    const int ready_timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool receiver_ready = wait_for_socket_monitor_event_with_activity (
      receiver_monitor, receiver_, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    const bool sender_ready = wait_for_socket_monitor_event (
      sender_monitor, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    zlink_monitor_close (&sender_monitor);
    zlink_monitor_close (&receiver_monitor);
    if (!receiver_ready || !sender_ready) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-router-router] monitor ready failed"
                      << " receiver_ready=" << (receiver_ready ? 1 : 0)
                      << " sender_ready=" << (sender_ready ? 1 : 0) << std::endl;
        }
        return false;
    }

    if (!perform_router_router_handshake (receiver_, sender_, target_rid_out_)) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] handshake failed" << std::endl;
        return false;
    }

    drain_router_socket (receiver_);
    drain_router_socket (sender_);

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (receiver_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (sender_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    if (bench_debug_enabled ())
        std::cerr << "[perf-router-router] setup complete" << std::endl;
    return true;
}

int recv_router_router_header_flags (void *receiver_,
                                     size_t payload_size_,
                                     int flags_,
                                     perf_single_metric::header_t *header_out_,
                                     bool *header_ok_out_)
{
    if (!receiver_)
        return -1;

    if (header_ok_out_)
        *header_ok_out_ = false;

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return -1;
    const int rc =
      zlink_router_recv_part (receiver_, &source_rid, &request_seq, &part,
                              &has_more, static_cast<zlink_recv_flags_t> (flags_));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    const bool rid_ok = source_rid && source_rid->size > 0;
    const bool shape_ok = rid_ok && request_seq == 0 && has_more == ZLINK_PART_FINAL;
    if (!shape_ok) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-router-router] invalid routed recv"
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
            std::cerr << "[perf-router-router] unexpected payload size=" << actual_size
                      << " expected=" << payload_size_ << std::endl;
        }
        return -1;
    }

    if (header_ok_out_)
        *header_ok_out_ = header_ok;
    return 1;
}

// PERF_SINGLE_TEST_POLICY § 1.4: send wire-level stop token between two
// ROUTER sockets so the receiver loop exits via is_stop_token. Bounded
// retry through transient backpressure.
bool send_router_stop_token (void *sender_, const zlink_routing_id_t *target_rid_)
{
    if (!sender_ || !target_rid_)
        return false;
    for (int retry = 0; retry < 100; ++retry) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, std::strlen (k_stop_token)) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, std::strlen (k_stop_token));
        if (perf_zlink_send_rid_parts (sender_, target_rid_, &part, 1, ZLINK_SEND_FLAGS_NONE) == 0)
            return true;
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK && err != ETIMEDOUT
            && err != EHOSTUNREACH && err != ENOTCONN)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

bool send_router_samples (void *sender_,
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

        zlink_msg_t part;
        if (zlink_msg_init_size (&part, payload_->size ()) != 0)
            return false;
        if (!payload_->empty ())
            std::memcpy (zlink_msg_data (&part), payload_->data (), payload_->size ());

        if (perf_zlink_send_rid_parts (sender_, &state_->target_rid, &part, 1,
                                       ZLINK_SEND_FLAGS_NONE)
            != 0) {
            const int err = zlink_errno ();
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-router-router] send failed err=" << err << std::endl;
            }
            if (err == EINTR || err == EAGAIN || err == EHOSTUNREACH || err == ENOTCONN)
                continue;
            return false;
        }

        sent_count_->fetch_add (1, std::memory_order_release);
        ++seq;
    }

    return true;
}

int send_router_probe_once (void *sender_,
                            std::vector<char> *payload_,
                            router_router_recv_state_t *state_)
{
    if (!sender_ || !payload_ || !state_)
        return -1;

    zlink_routing_id_t target_rid;
    std::memset (&target_rid, 0, sizeof (target_rid));
    target_rid.size = 7;
    std::memcpy (target_rid.data, "ROUTER1", target_rid.size);

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

    if (perf_zlink_send_rid_parts (sender_, &target_rid, &part, 1, ZLINK_DONTWAIT) == 0)
        return 1;

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    if (err == EINTR || err == EAGAIN)
        return 0;
    return -1;
}

bool wait_for_router_router_ready (void *sender_,
                                   void *receiver_,
                                   std::vector<char> *payload_,
                                   router_router_recv_state_t *state_)
{
    if (!sender_ || !receiver_ || !payload_ || !state_)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000));
    while (std::chrono::steady_clock::now () < deadline) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-router-router] ready probe send" << std::endl;
        const int send_probe_rc = send_router_probe_once (sender_, payload_, state_);
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
                const int recv_rc = recv_router_router_header_flags (
                  receiver_, state_->payload_size, ZLINK_DONTWAIT, &header, &header_ok);
                if (recv_rc < 0)
                    return false;
                if (recv_rc == 0)
                    break;
                if (bench_debug_enabled ()) {
                    std::cerr << "[perf-router-router] ready probe recv"
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
                       router_router_recv_state_t *state_,
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
          send_router_samples (sender_, payload_, state_, duration_s_, &sent_count);
        const bool stop_ok = send_router_stop_token (sender_, &state_->target_rid);
        sender_ok.store (active_ok && stop_ok, std::memory_order_release);
    });

    while (true) {
        perf_single_metric::header_t header;
        bool header_ok = false;
        const int recv_rc =
          recv_router_router_header_flags (receiver_, state_->payload_size, 0, &header, &header_ok);
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
            std::cerr << "[perf-router-router] active phase failed sent="
                      << sent_count.load (std::memory_order_relaxed) << " received=" << received
                      << std::endl;
        }
        return false;
    }

    *received_out_ = received;
    if (*received_out_ == 0 || latency_builder.count () == 0) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-router-router] no active samples sent="
                      << sent_count.load (std::memory_order_relaxed) << " received=" << received
                      << " latency_count=" << latency_builder.count () << std::endl;
        }
        return false;
    }
    *latency_out_ = latency_builder.snapshot ();
    return true;
}

} // namespace

void run_router_router (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail = [&] () {
        print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
    };

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    router_router_recv_state_t state;
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
    socket_guard_t sender (ctx.get (), ZLINK_SOCKET_ROUTER);
    if (!receiver.valid () || !sender.valid ()) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    if (!setup_router_router_session (receiver.get (), sender.get (), &state.target_rid, transport,
                                      lib_name + "_router_router")) {
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
        std::cerr << "[perf-router-router] active complete" << std::endl;

    emit_single_socket_hwm_detail (receiver.get (), "ROUTER_ROUTER", transport, "receiver",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    emit_single_socket_hwm_detail (sender.get (), "ROUTER_ROUTER", transport, "sender",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    print_result (lib_name, "ROUTER_ROUTER", transport, msg_size,
                  static_cast<double> (received) / static_cast<double> (duration_s),
                  latency.mean_ns, latency.p95_ns, latency.p99_ns);
    fflush (NULL);
    std::_Exit (0);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "ROUTER_ROUTER", run_router_router);
}
