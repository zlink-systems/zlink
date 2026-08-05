#include "../common/bench_common_zlink.hpp"
#include "../common/perf_single_metric_header.hpp"
#include <zlink.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace
{

inline void debug_router_router (const char *message_)
{
    if (bench_debug_enabled ())
        std::cerr << "[bench-router-router] " << message_ << std::endl;
}

inline void assign_routing_id (zlink_routing_id_t *rid_out, const char *data, size_t size)
{
    if (!rid_out)
        return;

    std::memset (rid_out, 0, sizeof (*rid_out));
    rid_out->size = static_cast<uint8_t> (size);
    if (size > 0)
        std::memcpy (rid_out->data, data, size);
}

inline bool perform_router_router_handshake (void *router1, void *router2)
{
    bool connected = false;
    zlink_routing_id_t source_rid;
    source_rid.size = 0;
    zlink_routing_id_t target_rid;
    assign_routing_id (&target_rid, "ROUTER1", 7);

    const int handshake_timeout_ms = resolve_bench_count ("PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000);
    const auto deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (handshake_timeout_ms > 0 ? handshake_timeout_ms : 3000);

    while (!connected && std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t ping_msg;
        if (bench_msg_init_copy (&ping_msg, "PING", 4) != 0)
            return false;
        const int send_rc = bench_send_single_part_routed (
          router2, &target_rid, &ping_msg, static_cast<zlink_send_flags_t> (ZLINK_DONTWAIT));
        if (send_rc < 0) {
            ::zlink_msg_close (&ping_msg);
            const int err = zlink_errno ();
            if (err != EAGAIN && err != EINTR)
                return false;
        } else {
            zlink_msg_t recv_msg;
            if (::zlink_msg_init (&recv_msg) != 0)
                return false;
            const int recv_rc = bench_recv_single_part_routed (
              router1, &recv_msg, &source_rid, static_cast<zlink_send_flags_t> (ZLINK_DONTWAIT));
            if (recv_rc >= 0) {
                const bool payload_ok =
                  ::zlink_msg_size (&recv_msg) == 4
                  && std::memcmp (::zlink_msg_data (&recv_msg), "PING", 4) == 0;
                ::zlink_msg_close (&recv_msg);
                if (payload_ok)
                    connected = true;
            } else {
                const int err = zlink_errno ();
                ::zlink_msg_close (&recv_msg);
                if (err != EAGAIN && err != EINTR)
                    return false;
            }
        }

        if (!connected)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    if (!connected)
        return false;

    assign_routing_id (&target_rid, "ROUTER2", 7);
    zlink_msg_t pong_msg;
    if (bench_msg_init_copy (&pong_msg, "PONG", 4) != 0)
        return false;
    if (bench_send_single_part_routed (router1, &target_rid, &pong_msg,
                                       static_cast<zlink_send_flags_t> (0))
        < 0) {
        ::zlink_msg_close (&pong_msg);
        return false;
    }

    zlink_msg_t recv_msg;
    if (::zlink_msg_init (&recv_msg) != 0)
        return false;
    const int recv_rc = bench_recv_single_part_routed (router2, &recv_msg, &source_rid,
                                                       static_cast<zlink_send_flags_t> (0));
    if (recv_rc < 0) {
        ::zlink_msg_close (&recv_msg);
        return false;
    }

    const bool ok = ::zlink_msg_size (&recv_msg) == 4
                    && std::memcmp (::zlink_msg_data (&recv_msg), "PONG", 4) == 0;
    ::zlink_msg_close (&recv_msg);
    return ok;
}

inline void drain_router_socket (void *socket_)
{
    if (!socket_)
        return;

    while (true) {
        zlink_routing_id_t source_rid;
        source_rid.size = 0;
        zlink_msg_t msg;
        if (::zlink_msg_init (&msg) != 0)
            return;

        const int rc = bench_recv_single_part_routed (
          socket_, &msg, &source_rid, static_cast<zlink_send_flags_t> (ZLINK_DONTWAIT));
        if (rc < 0) {
            ::zlink_msg_close (&msg);
            const int err = zlink_errno ();
            if (err == EAGAIN || err == EINTR)
                return;
            return;
        }

        ::zlink_msg_close (&msg);
    }
}

inline bool send_router_payload_flags (
  void *socket, const char *target_rid, size_t target_rid_size, zlink_msg_t *msg, int flags)
{
    if (!socket || !target_rid || !msg)
        return false;

    zlink_routing_id_t routing_id;
    assign_routing_id (&routing_id, target_rid, target_rid_size);

    const int rc = bench_send_single_part_routed (socket, &routing_id, msg,
                                                  static_cast<zlink_send_flags_t> (flags));
    if (rc < 0) {
        zlink_msg_close (msg);
        return false;
    }
    return true;
}

inline bool send_router_payload_until_ready (void *socket,
                                             const char *target_rid,
                                             size_t target_rid_size,
                                             const void *payload_data,
                                             size_t payload_size,
                                             const std::chrono::steady_clock::time_point &deadline)
{
    LIBZLINK_UNUSED (deadline);

    zlink_msg_t msg;
    if (bench_msg_init_copy (&msg, payload_data, payload_size) != 0)
        return false;

    return send_router_payload_flags (socket, target_rid, target_rid_size, &msg, 0);
}

inline bool
send_router_payload_nonblocking_until_ready (void *socket,
                                             const char *target_rid,
                                             size_t target_rid_size,
                                             const void *payload_data,
                                             size_t payload_size,
                                             const std::chrono::steady_clock::time_point &deadline)
{
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t msg;
        if (bench_msg_init_copy (&msg, payload_data, payload_size) != 0)
            return false;
        if (send_router_payload_flags (socket, target_rid, target_rid_size, &msg, ZLINK_DONTWAIT)) {
            return true;
        }

        const int err = zlink_errno ();
        if (err != EAGAIN && err != EINTR)
            return false;

        zlink_pollitem_t item = {socket, 0, ZLINK_POLLOUT, 0};
        const int poll_rc = zlink_poll (&item, 1, 1, NULL);
        if (poll_rc < 0 && zlink_errno () != EINTR)
            return false;
    }

    errno = EAGAIN;
    return false;
}

inline int recv_router_header_flags (void *socket,
                                     size_t payload_size,
                                     int flags,
                                     perf_single_metric::header_t *header_out,
                                     bool *header_ok_out)
{
    if (!socket)
        return -1;

    if (header_ok_out)
        *header_ok_out = false;

    zlink_routing_id_t source_rid;
    source_rid.size = 0;
    zlink_msg_t payload;
    if (::zlink_msg_init (&payload) != 0)
        return -1;

    const int id_rc = bench_recv_single_part_routed (socket, &payload, &source_rid,
                                                     static_cast<zlink_send_flags_t> (flags));
    if (id_rc < 0) {
        const int err = zlink_errno ();
        ::zlink_msg_close (&payload);
        if (err == EAGAIN || err == EINTR)
            return 0;
        if (bench_debug_enabled ()) {
            std::cerr << "[bench-router-router] recv error" << " errno=" << err << " ("
                      << zlink_strerror (err) << ")" << std::endl;
        }
        return -1;
    }

    const size_t actual_size = zlink_msg_size (&payload);
    const bool size_ok = actual_size == payload_size;
    const bool has_more = bench_msg_has_more (payload);
    bool header_ok = false;
    if (source_rid.size > 0 && size_ok && !has_more) {
        if (header_out) {
            header_ok = perf_single_metric::decode_payload_header (zlink_msg_data (&payload),
                                                                   actual_size, header_out);
        } else {
            header_ok = true;
        }
    } else if (bench_debug_enabled ()) {
        std::cerr << "[bench-router-router] recv ignored shape"
                  << " rid_size=" << static_cast<int> (source_rid.size)
                  << " payload_size=" << actual_size << " expected=" << payload_size << std::endl;
    }

    ::zlink_msg_close (&payload);
    if (!size_ok || has_more)
        return -1;
    if (header_ok_out)
        *header_ok_out = header_ok;
    return 1;
}

inline bool setup_router_router_session (void *router1,
                                         void *router2,
                                         const std::string &transport,
                                         const std::string &pair_id)
{
    if (!router1 || !router2)
        return false;

    zlink_set_routing_id (router1, "ROUTER1", 7);
    zlink_set_routing_id (router2, "ROUTER2", 7);
    int mandatory = 1;
    zlink_set_router_option (router1, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory));
    zlink_set_router_option (router2, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory));

    if (!setup_connected_pair (router1, router2, transport, pair_id)
        || !perform_router_router_handshake (router1, router2)) {
        return false;
    }

    drain_router_socket (router1);
    drain_router_socket (router2);

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (router1, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (router2, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    return true;
}

inline bool run_oneway_phase (void *router1,
                              void *router2,
                              std::vector<char> *payload,
                              size_t payload_size,
                              size_t msg_size,
                              uint32_t run_id,
                              uint64_t *seq,
                              perf_single_metric::phase_t phase,
                              int warmup_s,
                              int duration_s,
                              int recv_timeout_ms,
                              queue_probe_t *queue_probe,
                              unsigned long long *out_received,
                              latency_stats_t *out_latency)
{
    if (!router1 || !router2 || !payload || !seq || !out_received)
        return false;

    const bool active_phase = phase == perf_single_metric::phase_active;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (active_phase ? (duration_s > 0 ? duration_s : 1)
                                                               : (warmup_s > 0 ? warmup_s : 1));
    const auto drain_idle_limit =
      std::chrono::milliseconds (recv_timeout_ms > 0 ? recv_timeout_ms : 200);

    std::atomic<bool> sender_done (false);
    std::atomic<bool> recv_failed (false);

    std::atomic<unsigned long long> received (0);
    latency_stats_builder_t latency_builder;

    std::thread receiver_thread ([&] () {
        auto last_recv_at = std::chrono::steady_clock::now ();

        auto account_header = [&] (const perf_single_metric::header_t &header, bool header_ok) {
            if (active_phase && queue_probe)
                queue_probe->sample_recv_if_due ();

            if (!header_ok || header.magic != perf_single_metric::k_magic
                || header.phase != static_cast<uint32_t> (phase)) {
                return;
            }

            if (active_phase) {
                if (std::chrono::steady_clock::now () < deadline) {
                    received.fetch_add (1, std::memory_order_relaxed);
                    const uint64_t now = perf_single_metric::now_us ();
                    const double latency_us = now >= header.sent_ts_us
                                                ? static_cast<double> (now - header.sent_ts_us)
                                                : 0.0;
                    latency_builder.add (latency_us);
                }
            } else {
                received.fetch_add (1, std::memory_order_relaxed);
            }
        };

        if (active_phase && queue_probe)
            queue_probe->force_sample_recv ();

        while (true) {
            const bool done = sender_done.load (std::memory_order_acquire);
            const int flags = ZLINK_DONTWAIT;

            perf_single_metric::header_t header;
            bool header_ok = false;
            const int recv_rc =
              recv_router_header_flags (router1, payload_size, flags, &header, &header_ok);
            if (recv_rc > 0) {
                last_recv_at = std::chrono::steady_clock::now ();
                account_header (header, header_ok);

                for (;;) {
                    perf_single_metric::header_t burst_header;
                    bool burst_header_ok = false;
                    const int burst_rc = recv_router_header_flags (
                      router1, payload_size, ZLINK_DONTWAIT, &burst_header, &burst_header_ok);
                    if (burst_rc > 0) {
                        last_recv_at = std::chrono::steady_clock::now ();
                        account_header (burst_header, burst_header_ok);
                        continue;
                    }
                    if (burst_rc == 0)
                        break;

                    recv_failed.store (true, std::memory_order_release);
                    break;
                }

                if (recv_failed.load (std::memory_order_acquire))
                    break;
                continue;
            }

            if (recv_rc == 0) {
                if (done && std::chrono::steady_clock::now () - last_recv_at >= drain_idle_limit) {
                    break;
                }
                zlink_pollitem_t item = {router1, 0, ZLINK_POLLIN, 0};
                const int poll_rc = zlink_poll (&item, 1, 1, NULL);
                if (poll_rc < 0 && zlink_errno () != EINTR && zlink_errno () != EAGAIN) {
                    if (bench_debug_enabled ()) {
                        const int err = zlink_errno ();
                        std::cerr << "[bench-router-router] poll error" << " errno=" << err << " ("
                                  << zlink_strerror (err) << ")" << std::endl;
                    }
                    recv_failed.store (true, std::memory_order_release);
                    break;
                }
                continue;
            }

            recv_failed.store (true, std::memory_order_release);
            break;
        }

        if (active_phase && queue_probe)
            queue_probe->force_sample_recv ();
    });

    bool send_failed = false;
    if (active_phase && queue_probe)
        queue_probe->force_sample_send ();

    if (active_phase) {
        while (std::chrono::steady_clock::now () < deadline) {
            const uint64_t sent_ts = perf_single_metric::now_us ();
            bool send_ok = false;
            if (perf_single_metric::stamp_payload (payload->data (), payload_size, run_id, phase,
                                                   msg_size, (*seq)++, sent_ts)) {
                send_ok = send_router_payload_until_ready (router2, "ROUTER1", 7, payload->data (),
                                                           payload_size, deadline);
            }
            if (!send_ok) {
                send_failed = true;
                break;
            }
            if (queue_probe)
                queue_probe->sample_send_if_due ();
        }
    } else {
        while (std::chrono::steady_clock::now () < deadline) {
            bool send_ok = false;
            if (perf_single_metric::stamp_payload (payload->data (), payload_size, run_id, phase,
                                                   msg_size, (*seq)++,
                                                   perf_single_metric::now_us ())) {
                send_ok = send_router_payload_until_ready (router2, "ROUTER1", 7, payload->data (),
                                                           payload_size, deadline);
            }
            if (!send_ok) {
                send_failed = true;
                break;
            }
        }
    }

    if (active_phase && queue_probe)
        queue_probe->force_sample_send ();

    sender_done.store (true, std::memory_order_release);
    receiver_thread.join ();

    if (send_failed || recv_failed.load (std::memory_order_acquire)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[bench-router-router] phase failed"
                      << " active=" << (active_phase ? 1 : 0)
                      << " received=" << received.load (std::memory_order_relaxed)
                      << " send_failed=" << (send_failed ? 1 : 0)
                      << " recv_failed=" << (recv_failed.load (std::memory_order_acquire) ? 1 : 0)
                      << " errno=" << zlink_errno () << " (" << zlink_strerror (zlink_errno ())
                      << ")" << std::endl;
        }
        return false;
    }

    *out_received = received.load (std::memory_order_relaxed);

    if (active_phase) {
        if (received.load (std::memory_order_relaxed) == 0 || latency_builder.count () == 0
            || !out_latency) {
            debug_router_router ("active phase produced no samples");
            return false;
        }
        *out_latency = latency_builder.snapshot ();
    } else if (received.load (std::memory_order_relaxed) == 0) {
        debug_router_router ("warmup phase produced no samples");
        return false;
    }

    return true;
}

} // namespace

void run_router_router (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail_no_queue = [&] () {
        print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size);
    };

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail_no_queue ();
        return;
    }

    socket_guard_t router1 (ctx.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t router2 (ctx.get (), ZLINK_SOCKET_ROUTER);
    if (!router1.valid () || !router2.valid ()) {
        print_fail_no_queue ();
        return;
    }

    if (!setup_router_router_session (router1.get (), router2.get (), transport,
                                      lib_name + "_router_router")) {
        debug_router_router ("session setup failed");
        print_fail_no_queue ();
        return;
    }

    const int recv_timeout_ms = resolve_single_recv_timeout_ms ();
    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    queue_probe_t queue_probe (router2.get (), router1.get ());

    auto print_fail_with_queue = [&] () {
        print_fail_result (lib_name, "ROUTER_ROUTER", transport, msg_size, &queue_probe);
    };

    const uint32_t run_id = static_cast<uint32_t> (perf_single_metric::now_us ());
    uint64_t seq = 1;

    unsigned long long warmup_received = 0;
    const int warmup_s = resolve_single_warmup_seconds ();
    if (!run_oneway_phase (router1.get (), router2.get (), &payload, payload_size, msg_size, run_id,
                           &seq, perf_single_metric::phase_warmup, warmup_s, 0, recv_timeout_ms,
                           NULL, &warmup_received, NULL)) {
        debug_router_router ("warmup failed");
        print_fail_with_queue ();
        return;
    }

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    unsigned long long received = 0;
    latency_stats_t latency_stats;
    if (!run_oneway_phase (router1.get (), router2.get (), &payload, payload_size, msg_size, run_id,
                           &seq, perf_single_metric::phase_active, 0, duration_s, recv_timeout_ms,
                           &queue_probe, &received, &latency_stats)) {
        debug_router_router ("active failed");
        print_fail_with_queue ();
        return;
    }

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    const queue_stats_t queue_stats = queue_probe.snapshot ();

    print_result (lib_name, "ROUTER_ROUTER", transport, msg_size, throughput, latency_stats.mean_us,
                  latency_stats.p95_us, latency_stats.p99_us, queue_stats);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, run_router_router);
}
