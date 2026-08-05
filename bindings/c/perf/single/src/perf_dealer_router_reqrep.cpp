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

bool setup_session (void *router_, void *dealer_, const std::string &transport_, const std::string &id_)
{
    if (!router_ || !dealer_)
        return false;
    if (zlink_set_routing_id (dealer_, "DEALER-REQ", 10) != ZLINK_CONFIG_OK)
        return false;
    if (!setup_tls_server (router_, transport_) || !setup_tls_client (dealer_, transport_))
        return false;

    apply_single_hwm (router_);
    apply_single_hwm (dealer_);
    const std::string endpoint = bind_and_resolve_endpoint (router_, transport_, id_);
    if (endpoint.empty ())
        return false;

    void *router_monitor = open_configured_socket_monitor (router_, ZLINK_EVENT_CONNECTION_READY);
    void *dealer_monitor = open_configured_socket_monitor (dealer_, ZLINK_EVENT_CONNECTION_READY);
    if (!router_monitor || !dealer_monitor) {
        zlink_monitor_close (&router_monitor);
        zlink_monitor_close (&dealer_monitor);
        return false;
    }
    if (!connect_checked (dealer_, endpoint)) {
        zlink_monitor_close (&router_monitor);
        zlink_monitor_close (&dealer_monitor);
        return false;
    }

    apply_single_benchmark_socket_options (router_, transport_);
    apply_single_benchmark_socket_options (dealer_, transport_);
    const int ready_timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool router_ready = wait_for_socket_monitor_event_with_activity (
      router_monitor, router_, ZLINK_EVENT_CONNECTION_READY, ready_timeout_ms);
    const bool dealer_ready =
      wait_for_socket_monitor_event (dealer_monitor, ZLINK_EVENT_CONNECTION_READY,
                                     ready_timeout_ms);
    zlink_monitor_close (&router_monitor);
    zlink_monitor_close (&dealer_monitor);
    if (!router_ready || !dealer_ready) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] connection ready timeout router="
                      << router_ready << " dealer=" << dealer_ready << std::endl;
        return false;
    }

    const int timeout_ms = resolve_single_recv_timeout_ms ();
    set_sockopt_int (router_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    set_sockopt_int (dealer_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    return true;
}

void run_dealer_router_reqrep (const std::string &transport,
                               size_t msg_size,
                               const std::string &lib_name)
{
    const char *pattern = "DEALER_ROUTER_REQREP";
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
    socket_guard_t requester (ctx.get (), ZLINK_SOCKET_DEALER);
    if (!replier.valid () || !requester.valid ()
        || !setup_session (replier.get (), requester.get (), transport,
                           lib_name + "_dealer_router_reqrep")) {
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
          return zlink_dealer_request_part (requester.get (), part_, ZLINK_DONTWAIT,
                                            ZLINK_PART_FINAL, timeout_ms_, handler_, userdata_);
      },
      &completion_poller, &completed, &latency);

    const bool stop_ok = perf_single_reqrep::send_stop_to_router (requester.get ());
    if (!stop_ok)
        reply_state.stop.store (true, std::memory_order_release);
    replier_thread.join ();
    const bool poller_ok = completion_poller
                             && zlink_poller_destroy (&completion_poller) == ZLINK_CLOSE_OK;
    if (!ok || !stop_ok || !poller_ok
        || reply_state.fatal.load (std::memory_order_acquire)) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] shutdown failed requester=" << ok
                      << " stop=" << stop_ok << " poller=" << poller_ok
                      << " replier_fatal="
                      << reply_state.fatal.load (std::memory_order_acquire)
                      << " received="
                      << reply_state.received.load (std::memory_order_acquire)
                      << " replied="
                      << reply_state.replied.load (std::memory_order_acquire)
                      << " completed=" << completed << std::endl;
        print_fail ();
        fflush (NULL);
        std::_Exit (1);
    }

    emit_single_socket_hwm_detail (replier.get (), pattern, transport, "replier",
                                   ZLINK_SOCKET_ROUTER, msg_size);
    emit_single_socket_hwm_detail (requester.get (), pattern, transport, "requester",
                                   ZLINK_SOCKET_DEALER, msg_size);
    perf_single_reqrep::print_reqrep_result (
      lib_name, pattern, transport, msg_size,
      static_cast<double> (completed) / static_cast<double> (duration_s), latency);
    fflush (NULL);
    std::_Exit (0);
}

} // namespace

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "DEALER_ROUTER_REQREP",
                                    run_dealer_router_reqrep);
}
