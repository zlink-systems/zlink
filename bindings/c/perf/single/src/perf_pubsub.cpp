#include "../common/bench_common.hpp"
#include "../common/perf_single_latency.hpp"
#include "../common/perf_single_metric_header.hpp"
#include "../common/perf_single_monitor.hpp"
#include "../common/perf_single_one_way.hpp"
#include "../common/perf_single_phase.hpp"
#include <zlink.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

static const char *k_pubsub_topic = "bench";

inline int resolve_pubsub_xpub_nodrop_opt ()
{
    const char *env = std::getenv ("PERF_SINGLE_PUBSUB_XPUB_NODROP");
    return (env && std::strcmp (env, "0") == 0) ? 0 : 1;
}

typedef perf_single_one_way::state_t pubsub_recv_state_t;

bool setup_connected_pubsub_pair (void *pub_socket_,
                                  void *sub_socket_,
                                  const std::string &transport_,
                                  const std::string &id_)
{
    if (!setup_tls_server (pub_socket_, transport_)
        || !setup_tls_client (sub_socket_, transport_)) {
        return false;
    }
    apply_single_hwm (pub_socket_);
    apply_single_hwm (sub_socket_);
    if (zlink_set_subscription (sub_socket_, "") != ZLINK_CONFIG_OK)
        return false;
    const std::string endpoint = bind_and_resolve_endpoint (pub_socket_, transport_, id_);
    if (endpoint.empty ())
        return false;
    void *sub_monitor = open_configured_socket_monitor (sub_socket_, ZLINK_EVENT_CONNECTION_READY);
    void *pub_monitor = open_configured_socket_monitor (pub_socket_, ZLINK_EVENT_CONNECTION_READY);
    if (!sub_monitor || !pub_monitor || !connect_checked (sub_socket_, endpoint)) {
        zlink_monitor_close (&sub_monitor);
        zlink_monitor_close (&pub_monitor);
        return false;
    }
    apply_single_benchmark_socket_options (pub_socket_, transport_);
    apply_single_benchmark_socket_options (sub_socket_, transport_);
    const int timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool sub_ready =
      wait_for_socket_monitor_event (sub_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    const bool pub_ready =
      wait_for_socket_monitor_event (pub_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    zlink_monitor_close (&sub_monitor);
    zlink_monitor_close (&pub_monitor);
    if (sub_ready && pub_ready) {
        const int settle_ms = resolve_single_pubsub_ready_settle_ms ();
        if (settle_ms > 0 && perf_socket_poll (NULL, 0, settle_ms) < 0)
            return false;
    }
    return sub_ready && pub_ready;
}

int recv_pubsub_header_flags (void *subscriber_,
                              size_t payload_size_,
                              int flags_,
                              perf_single_metric::header_t *header_out_,
                              bool *header_ok_out_)
{
    if (!subscriber_)
        return perf_single_one_way::recv_result_error;

    if (header_ok_out_)
        *header_ok_out_ = false;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    char topic[256];
    size_t topic_len = sizeof (topic);
    if (zlink_msg_init (&part) != 0)
        return perf_single_one_way::recv_result_error;
    const int rc =
      zlink_subscribe_part (subscriber_, &source_rid, topic, sizeof (topic), &topic_len, &part,
                            &has_more, static_cast<zlink_recv_flags_t> (flags_));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return perf_single_one_way::recv_result_again;
        return perf_single_one_way::recv_result_error;
    }

    const bool topic_ok = topic_len == std::strlen (k_pubsub_topic)
                          && std::memcmp (topic, k_pubsub_topic, topic_len) == 0;
    if (!topic_ok || source_rid || has_more != ZLINK_PART_FINAL) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-pubsub] invalid recv topic_ok=" << (topic_ok ? 1 : 0)
                      << " has_more=" << static_cast<int> (has_more) << std::endl;
        }
        zlink_msg_close (&part);
        return perf_single_one_way::recv_result_error;
    }

    const size_t actual_size = zlink_msg_size (&part);
    if (is_stop_token (zlink_msg_data (&part), actual_size)) {
        zlink_msg_close (&part);
        return perf_single_one_way::recv_result_stop;
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
            std::cerr << "[perf-pubsub] unexpected payload size=" << actual_size
                      << " expected=" << payload_size_ << std::endl;
        }
        return perf_single_one_way::recv_result_error;
    }

    if (header_ok_out_)
        *header_ok_out_ = header_ok;
    return perf_single_one_way::recv_result_payload;
}

// PERF_SINGLE_TEST_POLICY § 1.4: publish wire-level stop token on the
// PUBSUB topic so the subscriber recv loop exits via is_stop_token.
int send_pubsub_stop_token (void *publisher_)
{
    if (!publisher_)
        return -1;
    zlink_msg_t part;
    if (zlink_msg_init_size (&part, stop_token_size ()) != 0)
        return -1;
    std::memcpy (zlink_msg_data (&part), k_stop_token, stop_token_size ());
    if (perf_zlink_publish_parts (publisher_, k_pubsub_topic, &part, 1, ZLINK_DONTWAIT) == 0)
        return 0;
    const int err = zlink_errno ();
    zlink_msg_close (&part);
    if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
        return 1;
    return -1;
}


} // namespace

void run_pubsub (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail = [&] () { print_fail_result (lib_name, "PUBSUB", transport, msg_size); };

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    pubsub_recv_state_t state;
    state.run_id = next_single_metric_run_id ();
    state.msg_size = msg_size;
    state.payload_size = payload_size;

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail ();
        return;
    }
    if (!apply_single_auto_hwm_msg_unit (ctx.get (), msg_size)) {
        print_fail ();
        return;
    }

    socket_guard_t publisher (ctx.get (), ZLINK_SOCKET_PUB);
    socket_guard_t subscriber (ctx.get (), ZLINK_SOCKET_SUB);
    if (!publisher.valid () || !subscriber.valid ()) {
        print_fail ();
        return;
    }

    set_pub_opt_int (publisher.get (), ZLINK_PUB_OPT_NODROP, resolve_pubsub_xpub_nodrop_opt (),
                     "ZLINK_PUB_OPT_NODROP");
    if (!setup_connected_pubsub_pair (publisher.get (), subscriber.get (), transport,
                                      lib_name + "_pubsub")) {
        print_fail ();
        return;
    }

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    const int recv_timeout_ms = resolve_single_pubsub_recv_timeout_ms ();
    unsigned long long received = 0;
    latency_stats_t latency;
    if (!perf_single_one_way::run_active_phase (
          publisher.get (), subscriber.get (), &payload, &state, duration_s, recv_timeout_ms,
          "perf-pubsub",
          [] (void *publisher, std::vector<char> *active_payload,
              const pubsub_recv_state_t &active_state, uint64_t seq) {
              zlink_msg_t part;
              if (!perf_single_one_way::init_active_payload_part (&part, active_payload,
                                                                  active_state, seq)) {
                  return perf_single_one_way::send_step_fatal;
              }
              if (perf_zlink_publish_parts (publisher, k_pubsub_topic, &part, 1,
                                            ZLINK_SEND_FLAGS_NONE)
                  == 0) {
                  return perf_single_one_way::send_step_sent;
              }

              const int err = zlink_errno ();
              zlink_msg_close (&part);
              if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
                  return perf_single_one_way::send_step_retry;
              if (bench_debug_enabled ()) {
                  std::cerr << "[perf-pubsub] publish failed err=" << err << std::endl;
              }
              return perf_single_one_way::send_step_fatal;
          },
          recv_pubsub_header_flags, send_pubsub_stop_token, &received, &latency)) {
        print_fail ();
        return;
    }
    emit_single_socket_hwm_detail (publisher.get (), "PUBSUB", transport, "publisher",
                                   ZLINK_SOCKET_PUB, msg_size);
    emit_single_socket_hwm_detail (subscriber.get (), "PUBSUB", transport, "subscriber",
                                   ZLINK_SOCKET_SUB, msg_size);
    print_result (lib_name, "PUBSUB", transport, msg_size,
                  static_cast<double> (received) / static_cast<double> (duration_s),
                  latency.mean_ns, latency.p95_ns, latency.p99_ns);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "PUBSUB", run_pubsub);
}
