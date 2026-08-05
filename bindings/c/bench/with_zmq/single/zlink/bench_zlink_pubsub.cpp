#include "../common/bench_common_zlink.hpp"
#include "../common/perf_single_metric_header.hpp"
#include <zlink.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

static const char *k_pubsub_topic = "bench";

inline void configure_pubsub_monitor_socket (void *monitor_)
{
    if (!monitor_)
        return;

    set_sockopt_int (monitor_, ZLINK_OPT_LINGER, 0, "ZLINK_OPT_LINGER");
    set_sockopt_int (monitor_, ZLINK_OPT_SNDHWM, 1000, "ZLINK_OPT_SNDHWM");
    set_sockopt_int (monitor_, ZLINK_OPT_RCVHWM, 1000, "ZLINK_OPT_RCVHWM");
}

inline void *open_pubsub_monitor (void *socket_, uint64_t events_)
{
    if (!socket_ || events_ == 0)
        return NULL;

    zlink_socket_monitor_open_options_t opts;
    std::memset (&opts, 0, sizeof (opts));
    opts.events = events_;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor)
        return NULL;
    configure_pubsub_monitor_socket (monitor);
    return monitor;
}

inline bool is_pubsub_monitor_error_event (uint64_t event_)
{
    switch (event_) {
        case ZLINK_EVENT_BIND_FAILED:
        case ZLINK_EVENT_ACCEPT_FAILED:
        case ZLINK_EVENT_CLOSE_FAILED:
        case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
            return true;

        default:
            return false;
    }
}

inline bool pubsub_monitor_ready (const zlink_socket_monitor_event_t &event_,
                                  uint64_t success_event_)
{
    if (event_.event != success_event_)
        return false;
    if (success_event_ == ZLINK_EVENT_CONNECTION_READY)
        return true;
    return event_.value > 0;
}

inline bool wait_for_pubsub_monitor_event (void *monitor_, uint64_t success_event_, int timeout_ms_)
{
    if (!monitor_ || success_event_ == 0)
        return false;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1);

    while (std::chrono::steady_clock::now () < deadline) {
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        const long timeout_ms =
          static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                               deadline - std::chrono::steady_clock::now ())
                               .count ());
        const int poll_rc = zlink_poll (&item, 1, timeout_ms > 0 ? timeout_ms : 1, NULL);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (poll_rc == 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            zlink_socket_monitor_event_t event;
            if (zlink_socket_monitor_recv (monitor_, &event, ZLINK_DONTWAIT) != 0) {
                if (errno == EAGAIN || errno == EINTR)
                    break;
                return false;
            }
            if (pubsub_monitor_ready (event, success_event_))
                return true;
            if (is_pubsub_monitor_error_event (event.event)) {
                errno = event.value > 0 ? static_cast<int> (event.value) : EIO;
                return false;
            }
        }
    }

    return false;
}

inline bool setup_connected_pubsub_pair (void *pub_socket_,
                                         void *sub_socket_,
                                         const std::string &transport_,
                                         const std::string &id_)
{
    if (!pub_socket_ || !sub_socket_)
        return false;

    if (!setup_tls_server (pub_socket_, transport_)
        || !setup_tls_client (sub_socket_, transport_)) {
        return false;
    }

    apply_single_hwm (pub_socket_);
    apply_single_hwm (sub_socket_);

    if (!zlink_set_subscription (sub_socket_, ""))
        return false;

    std::string endpoint = bind_and_resolve_endpoint (pub_socket_, transport_, id_);
    if (endpoint.empty ())
        return false;

    void *sub_monitor = open_pubsub_monitor (sub_socket_, ZLINK_EVENT_CONNECTION_READY);
    if (!sub_monitor)
        return false;
    void *pub_monitor = open_pubsub_monitor (pub_socket_, ZLINK_EVENT_CONNECTION_READY);
    if (!pub_monitor) {
        zlink_monitor_close (&sub_monitor);
        return false;
    }

    if (!connect_checked (sub_socket_, endpoint)) {
        zlink_monitor_close (&pub_monitor);
        zlink_monitor_close (&sub_monitor);
        return false;
    }

    apply_single_send_timeout (pub_socket_, transport_);
    apply_single_send_timeout (sub_socket_, transport_);

    const int timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 3000);
    const bool sub_ready =
      wait_for_pubsub_monitor_event (sub_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    const bool pub_ready =
      wait_for_pubsub_monitor_event (pub_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    zlink_monitor_close (&pub_monitor);
    zlink_monitor_close (&sub_monitor);
    if (sub_ready && pub_ready)
        std::this_thread::sleep_for (std::chrono::seconds (1));
    return sub_ready && pub_ready;
}

inline int resolve_pubsub_xpub_nodrop_opt ()
{
    const char *env = std::getenv ("PERF_SINGLE_PUBSUB_XPUB_NODROP");
    if (!env || !*env)
        return 1;

    if (std::strcmp (env, "1") == 0 || std::strcmp (env, "true") == 0
        || std::strcmp (env, "TRUE") == 0 || std::strcmp (env, "on") == 0
        || std::strcmp (env, "ON") == 0) {
        return 1;
    }
    if (std::strcmp (env, "0") == 0 || std::strcmp (env, "false") == 0
        || std::strcmp (env, "FALSE") == 0 || std::strcmp (env, "off") == 0
        || std::strcmp (env, "OFF") == 0) {
        return 0;
    }
    return 1;
}

inline int recv_single_part_header_flags (void *socket,
                                          size_t expected_size,
                                          int flags,
                                          perf_single_metric::header_t *header_out,
                                          bool *header_ok_out)
{
    if (!socket)
        return -1;

    if (header_ok_out)
        *header_ok_out = false;

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    char topic_id[256];
    size_t topic_len = sizeof (topic_id);
    const int rc =
      ::zlink_std_compat_subscribe (socket, NULL, &parts, &part_count, topic_id, &topic_len,
                                    static_cast<zlink_recv_flags_t> (flags));
    if (rc < 0) {
        const int err = zlink_errno ();
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }
    if (!parts || part_count != 1) {
        if (parts)
            ::zlink_multipart_close (parts, part_count);
        return -1;
    }

    const bool topic_ok = topic_len == std::strlen (k_pubsub_topic)
                          && std::memcmp (topic_id, k_pubsub_topic, topic_len) == 0;
    const size_t actual_size = zlink_msg_size (&parts[0]);
    const bool size_ok = actual_size == expected_size;
    const bool has_more = bench_msg_has_more (parts[0]);
    bool header_ok = false;

    if (topic_ok && size_ok && !has_more) {
        if (header_out) {
            header_ok = perf_single_metric::decode_payload_header (zlink_msg_data (&parts[0]),
                                                                   actual_size, header_out);
        } else {
            header_ok = true;
        }
    }

    ::zlink_multipart_close (parts, part_count);

    if (!topic_ok || !size_ok || has_more)
        return -1;

    if (header_ok_out)
        *header_ok_out = header_ok;

    return 1;
}

inline bool run_oneway_phase (void *pub_socket,
                              void *sub_socket,
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
    if (!pub_socket || !sub_socket || !payload || !seq || !out_received)
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
              recv_single_part_header_flags (sub_socket, payload_size, flags, &header, &header_ok);
            if (recv_rc > 0) {
                last_recv_at = std::chrono::steady_clock::now ();
                account_header (header, header_ok);

                for (;;) {
                    perf_single_metric::header_t burst_header;
                    bool burst_header_ok = false;
                    const int burst_rc = recv_single_part_header_flags (
                      sub_socket, payload_size, ZLINK_DONTWAIT, &burst_header, &burst_header_ok);
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
            if (::zlink_std_compat_publish (pub_socket, k_pubsub_topic, &part, 1,
                                            static_cast<zlink_send_flags_t> (0))
                != ZLINK_SUBMIT_OK) {
                ::zlink_msg_close (&part);
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
            if (::zlink_std_compat_publish (pub_socket, k_pubsub_topic, &part, 1,
                                            static_cast<zlink_send_flags_t> (0))
                != ZLINK_SUBMIT_OK) {
                ::zlink_msg_close (&part);
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

void run_pubsub (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail_no_queue = [&] () {
        print_fail_result (lib_name, "PUBSUB", transport, msg_size);
    };

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail_no_queue ();
        return;
    }

    socket_guard_t pub (ctx.get (), ZLINK_SOCKET_PUB);
    socket_guard_t sub (ctx.get (), ZLINK_SOCKET_SUB);
    if (!pub.valid () || !sub.valid ()) {
        print_fail_no_queue ();
        return;
    }

    const int xpub_nodrop_opt = resolve_pubsub_xpub_nodrop_opt ();
    set_pub_opt_int (pub.get (), ZLINK_PUB_OPT_NODROP, xpub_nodrop_opt, "ZLINK_PUB_OPT_NODROP");

    if (!setup_connected_pubsub_pair (pub.get (), sub.get (), transport, lib_name + "_pubsub")) {
        print_fail_no_queue ();
        return;
    }

    const int recv_timeout_ms = resolve_single_pubsub_recv_timeout_ms ();
    set_sockopt_int (sub.get (), ZLINK_OPT_RCVTIMEO, recv_timeout_ms, "ZLINK_OPT_RCVTIMEO");

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    queue_probe_t queue_probe (pub.get (), sub.get ());

    auto print_fail_with_queue = [&] () {
        print_fail_result (lib_name, "PUBSUB", transport, msg_size, &queue_probe);
    };

    const uint32_t run_id = static_cast<uint32_t> (perf_single_metric::now_us ());
    uint64_t seq = 1;

    unsigned long long warmup_received = 0;
    const int warmup_s = resolve_single_warmup_seconds ();
    if (!run_oneway_phase (pub.get (), sub.get (), &payload, payload_size, msg_size, run_id, &seq,
                           perf_single_metric::phase_warmup, warmup_s, 0, recv_timeout_ms, NULL,
                           &warmup_received, NULL)) {
        print_fail_with_queue ();
        return;
    }

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    unsigned long long received = 0;
    latency_stats_t latency_stats;
    if (!run_oneway_phase (pub.get (), sub.get (), &payload, payload_size, msg_size, run_id, &seq,
                           perf_single_metric::phase_active, 0, duration_s, recv_timeout_ms,
                           &queue_probe, &received, &latency_stats)) {
        print_fail_with_queue ();
        return;
    }

    const double throughput = static_cast<double> (received) / static_cast<double> (duration_s);
    const queue_stats_t queue_stats = queue_probe.snapshot ();

    print_result (lib_name, "PUBSUB", transport, msg_size, throughput, latency_stats.mean_us,
                  latency_stats.p95_us, latency_stats.p99_us, queue_stats);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, run_pubsub);
}
