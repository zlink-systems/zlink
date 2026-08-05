#include "../common/bench_common.hpp"
#include "../common/perf_single_monitor.hpp"
#include "../common/perf_single_reqrep.hpp"
#include <zlink.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

bool recv_ping (void *server_, zlink_routing_id_t *client_rid_out_)
{
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return false;
    const int rc = zlink_router_recv_part (server_, &source_rid, &request_seq,
                                           &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != 0) {
        zlink_msg_close (&part);
        return false;
    }
    const bool ok = source_rid && source_rid->size > 0 && request_seq == 0
                    && has_more == ZLINK_PART_FINAL && zlink_msg_size (&part) == 4
                    && std::memcmp (zlink_msg_data (&part), "PING", 4) == 0;
    if (ok && client_rid_out_) {
        std::memset (client_rid_out_, 0, sizeof (*client_rid_out_));
        client_rid_out_->size = source_rid->size;
        std::memcpy (client_rid_out_->data, source_rid->data, source_rid->size);
    }
    zlink_msg_close (&part);
    return ok;
}

bool perform_handshake (void *server_,
                        void *client_,
                        const zlink_routing_id_t *server_rid_)
{
    const auto deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (parse_positive_env ("PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000));
    zlink_routing_id_t observed_client_rid;
    bool connected = false;
    while (!connected && std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t ping;
        if (zlink_msg_init_size (&ping, 4) != 0)
            return false;
        std::memcpy (zlink_msg_data (&ping), "PING", 4);
        if (perf_zlink_send_rid_parts (client_, server_rid_, &ping, 1, ZLINK_DONTWAIT) != 0) {
            zlink_msg_close (&ping);
            const int err = zlink_errno ();
            if (err != EAGAIN && err != EINTR && err != EHOSTUNREACH && err != ENOTCONN)
                return false;
        }

        const auto wait_deadline =
          std::min (deadline, std::chrono::steady_clock::now () + std::chrono::milliseconds (50));
        while (!connected && wait_socket_event_until (server_, ZLINK_POLLIN, wait_deadline)) {
            connected = recv_ping (server_, &observed_client_rid);
        }
        if (!connected)
            (void) perf_socket_poll (NULL, 0, 1);
    }
    if (!connected)
        return false;

    zlink_msg_t pong;
    if (zlink_msg_init_size (&pong, 4) != 0)
        return false;
    std::memcpy (zlink_msg_data (&pong), "PONG", 4);
    if (perf_zlink_send_rid_parts (server_, &observed_client_rid, &pong, 1,
                                   ZLINK_SEND_FLAGS_NONE)
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
    if (zlink_router_recv_part (client_, &source_rid, &request_seq, &part,
                                &has_more, ZLINK_RECV_FLAGS_NONE)
        != 0) {
        zlink_msg_close (&part);
        return false;
    }
    const bool ok = source_rid && source_rid->size == server_rid_->size
                    && std::memcmp (source_rid->data, server_rid_->data, source_rid->size) == 0
                    && request_seq == 0
                    && has_more == ZLINK_PART_FINAL && zlink_msg_size (&part) == 4
                    && std::memcmp (zlink_msg_data (&part), "PONG", 4) == 0;
    zlink_msg_close (&part);
    return ok;
}

bool setup_session (void *server_,
                    void *client_,
                    zlink_routing_id_t *server_rid_out_,
                    zlink_routing_id_t *client_rid_out_,
                    const std::string &transport_,
                    const std::string &id_)
{
    if (!server_ || !client_ || !server_rid_out_ || !client_rid_out_)
        return false;
    if (!perf_single_reqrep::init_routing_id_text ("SERVER", server_rid_out_)
        || !perf_single_reqrep::init_routing_id_text ("CLIENT", client_rid_out_))
        return false;
    if (zlink_set_routing_id (server_, "SERVER", 6) != ZLINK_CONFIG_OK
        || zlink_set_routing_id (client_, "CLIENT", 6) != ZLINK_CONFIG_OK)
        return false;
    const int mandatory = 1;
    (void) zlink_set_router_option (server_, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                                    sizeof (mandatory));
    (void) zlink_set_router_option (client_, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                                    sizeof (mandatory));
    if (!setup_tls_server (server_, transport_) || !setup_tls_client (client_, transport_))
        return false;

    apply_single_hwm (server_);
    apply_single_hwm (client_);
    const std::string endpoint = bind_and_resolve_endpoint (server_, transport_, id_);
    if (endpoint.empty ())
        return false;

    void *server_monitor = open_configured_socket_monitor (server_, ZLINK_EVENT_CONNECTION_READY);
    void *client_monitor = open_configured_socket_monitor (client_, ZLINK_EVENT_CONNECTION_READY);
    if (!server_monitor || !client_monitor) {
        zlink_monitor_close (&server_monitor);
        zlink_monitor_close (&client_monitor);
        return false;
    }
    if (!connect_checked (client_, endpoint)) {
        zlink_monitor_close (&server_monitor);
        zlink_monitor_close (&client_monitor);
        return false;
    }

    apply_single_benchmark_socket_options (server_, transport_);
    apply_single_benchmark_socket_options (client_, transport_);
    const int ready_timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool server_ready = wait_for_socket_monitor_event_with_activity (
      server_monitor, server_, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    const bool client_ready =
      wait_for_socket_monitor_event (client_monitor, ZLINK_EVENT_CONNECTION_READY,
                                     ready_timeout_ms);
    zlink_monitor_close (&server_monitor);
    zlink_monitor_close (&client_monitor);
    if (!server_ready || !client_ready)
        return false;
    if (!perform_handshake (server_, client_, server_rid_out_))
        return false;

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (server_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (client_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    return true;
}

void run_router_router_reqrep (const std::string &transport,
                               size_t msg_size,
                               const std::string &lib_name)
{
    const char *pattern = "ROUTER_ROUTER_REQREP";
    if (!transport_available (transport))
        return;

    auto print_fail = [&] () { print_fail_result (lib_name, pattern, transport, msg_size); };
    const size_t payload_size =
      std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'r');

    ctx_guard_t ctx;
    if (!ctx.valid () || !apply_single_auto_hwm_msg_unit (ctx.get (), msg_size)) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    socket_guard_t replier (ctx.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t requester (ctx.get (), ZLINK_SOCKET_ROUTER);
    zlink_routing_id_t server_rid;
    zlink_routing_id_t client_rid;
    if (!replier.valid () || !requester.valid ()
        || !setup_session (replier.get (), requester.get (), &server_rid, &client_rid, transport,
                           lib_name + "_router_router_reqrep")) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    perf_single_reqrep::reply_state_t reply_state;
    std::thread replier_thread (
      [&] () { perf_single_reqrep::run_router_replier (replier.get (), &reply_state); });

    perf_single_reqrep::request_state_t request_state;
    request_state.run_id = next_single_metric_run_id ();
    request_state.msg_size = msg_size;
    request_state.latency_sample_cap = perf_single_reqrep::resolve_latency_sample_cap ();

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    void *completion_poller = NULL;
    unsigned long long completed = 0;
    latency_stats_t latency;
    const bool ok = perf_single_reqrep::run_requester (
      requester.get (), &request_state, &payload, duration_s,
      [&] (zlink_msg_t *part_, uint32_t timeout_ms_, zlink_reply_handler_fn handler_,
           void *userdata_) {
          return zlink_router_request_part (requester.get (), &server_rid, part_, ZLINK_DONTWAIT,
                                            ZLINK_PART_FINAL, timeout_ms_, handler_, userdata_);
      },
      &completion_poller, &completed, &latency);

    const bool stop_ok =
      perf_single_reqrep::send_routed_stop_to_router (requester.get (), &server_rid);
    if (!stop_ok)
        reply_state.stop.store (true, std::memory_order_release);
    replier_thread.join ();
    const bool poller_ok = completion_poller
                             && zlink_poller_destroy (&completion_poller) == ZLINK_CLOSE_OK;
    if (!ok || !stop_ok || !poller_ok
        || reply_state.fatal.load (std::memory_order_acquire)) {
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    emit_single_socket_hwm_detail (replier.get (), pattern, transport, "replier",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    emit_single_socket_hwm_detail (requester.get (), pattern, transport, "requester",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    perf_single_reqrep::print_reqrep_result (
      lib_name, pattern, transport, msg_size,
      static_cast<double> (completed) / static_cast<double> (duration_s), latency);
    fflush (NULL);
    std::_Exit (0);
}

} // namespace

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "ROUTER_ROUTER_REQREP",
                                    run_router_router_reqrep);
}
