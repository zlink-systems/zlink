#include "../common/bench_common.hpp"
#include "../common/perf_single_metric_header.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace
{

inline int recv_router_header_flags (void *router,
                                     size_t payload_size,
                                     int flags,
                                     perf_single_metric::header_t *header_out,
                                     bool *header_ok_out)
{
    if (!router)
        return -1;

    if (header_ok_out)
        *header_ok_out = false;

    zlink_msg_t payload;
    if (zlink_msg_init (&payload) != 0)
        return -1;

    zlink_routing_id_t source_rid;
    source_rid.size = 0;
    const int id_rc = bench_recv_single_part_routed (router, &payload, &source_rid, flags);
    if (id_rc < 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&payload);
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    if (source_rid.size == 0) {
        zlink_msg_close (&payload);
        return -1;
    }

    const size_t actual_size = zlink_msg_size (&payload);
    const bool size_ok = actual_size == payload_size;
    const bool has_more = bench_msg_has_more (payload);
    bool header_ok = false;

    if (size_ok && !has_more) {
        if (header_out) {
            header_ok = perf_single_metric::decode_payload_header (zlink_msg_data (&payload),
                                                                   actual_size, header_out);
        } else {
            header_ok = true;
        }
    }

    zlink_msg_close (&payload);

    if (!size_ok || has_more)
        return -1;

    if (header_ok_out)
        *header_ok_out = header_ok;

    return 1;
}

inline bool setup_dealer_router_session (void *router,
                                         void *dealer,
                                         const std::string &transport,
                                         const std::string &pair_id)
{
    if (!router || !dealer)
        return false;

    zlink_set_routing_id (dealer, "CLIENT", 6);
    if (!setup_connected_pair (router, dealer, transport, pair_id))
        return false;

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (router, ZLINK_RCVTIMEO, timeout_ms, "ZLINK_RCVTIMEO");
    set_sockopt_int (dealer, ZLINK_RCVTIMEO, timeout_ms, "ZLINK_RCVTIMEO");
    return true;
}

inline bool run_oneway_phase (void *dealer,
                              void *router,
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
    if (!dealer || !router || !payload || !seq || !out_received)
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
            const int flags = 0;

            perf_single_metric::header_t header;
            bool header_ok = false;
            const int recv_rc =
              recv_router_header_flags (router, payload_size, flags, &header, &header_ok);
            if (recv_rc > 0) {
                last_recv_at = std::chrono::steady_clock::now ();
                account_header (header, header_ok);

                for (;;) {
                    perf_single_metric::header_t burst_header;
                    bool burst_header_ok = false;
                    const int burst_rc = recv_router_header_flags (
                      router, payload_size, ZLINK_DONTWAIT, &burst_header, &burst_header_ok);
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
            if (!perf_single_metric::stamp_payload (payload->data (), payload_size, run_id, phase,
                                                    msg_size, (*seq)++, sent_ts)) {
                send_failed = true;
                break;
            }
            zlink_msg_t part;
            if (bench_msg_init_copy (&part, payload->data (), payload_size) != 0) {
                send_failed = true;
                break;
            }
            if (bench_send_single_part (dealer, &part, 0) < 0) {
                zlink_msg_close (&part);
                send_failed = true;
                break;
            }
            if (queue_probe)
                queue_probe->sample_send_if_due ();
        }
    } else {
        while (std::chrono::steady_clock::now () < deadline) {
            if (!perf_single_metric::stamp_payload (payload->data (), payload_size, run_id, phase,
                                                    msg_size, (*seq)++,
                                                    perf_single_metric::now_us ())) {
                send_failed = true;
                break;
            }
            zlink_msg_t part;
            if (bench_msg_init_copy (&part, payload->data (), payload_size) != 0) {
                send_failed = true;
                break;
            }
            if (bench_send_single_part (dealer, &part, 0) < 0) {
                zlink_msg_close (&part);
                send_failed = true;
                break;
            }
        }
    }

    if (active_phase && queue_probe)
        queue_probe->force_sample_send ();

    sender_done.store (true, std::memory_order_release);
    receiver_thread.join ();

    if (send_failed || recv_failed.load (std::memory_order_acquire))
        return false;

    *out_received = received.load (std::memory_order_relaxed);

    if (active_phase) {
        if (received.load (std::memory_order_relaxed) == 0 || latency_builder.count () == 0
            || !out_latency)
            return false;
        *out_latency = latency_builder.snapshot ();
    } else if (received.load (std::memory_order_relaxed) == 0) {
        return false;
    }

    return true;
}

} // namespace

void run_dealer_router (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail_no_queue = [&] () {
        print_fail_result (lib_name, "DEALER_ROUTER", transport, msg_size);
    };

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail_no_queue ();
        return;
    }

    socket_guard_t router (ctx.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t dealer (ctx.get (), ZLINK_SOCKET_DEALER);
    if (!router.valid () || !dealer.valid ()) {
        print_fail_no_queue ();
        return;
    }

    if (!setup_dealer_router_session (router.get (), dealer.get (), transport,
                                      lib_name + "_dealer_router")) {
        print_fail_no_queue ();
        return;
    }

    const int recv_timeout_ms = resolve_single_recv_timeout_ms ();
    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    queue_probe_t queue_probe (dealer.get (), router.get ());

    auto print_fail_with_queue = [&] () {
        print_fail_result (lib_name, "DEALER_ROUTER", transport, msg_size, &queue_probe);
    };

    const uint32_t run_id = static_cast<uint32_t> (perf_single_metric::now_us ());
    uint64_t seq = 1;

    unsigned long long warmup_received = 0;
    const int warmup_s = resolve_single_warmup_seconds ();
    if (!run_oneway_phase (dealer.get (), router.get (), &payload, payload_size, msg_size, run_id,
                           &seq, perf_single_metric::phase_warmup, warmup_s, 0, recv_timeout_ms,
                           NULL, &warmup_received, NULL)) {
        print_fail_with_queue ();
        return;
    }

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    unsigned long long received = 0;
    latency_stats_t latency_stats;
    if (!run_oneway_phase (dealer.get (), router.get (), &payload, payload_size, msg_size, run_id,
                           &seq, perf_single_metric::phase_active, 0, duration_s, recv_timeout_ms,
                           &queue_probe, &received, &latency_stats)) {
        print_fail_with_queue ();
        return;
    }

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    const queue_stats_t queue_stats = queue_probe.snapshot ();

    print_result (lib_name, "DEALER_ROUTER", transport, msg_size, throughput, latency_stats.mean_us,
                  latency_stats.p95_us, latency_stats.p99_us, queue_stats);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, run_dealer_router);
}
