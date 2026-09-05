/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wait_ms = 5000;
const uint32_t losing_request_timeout_ms = 2000;
const int handover_completion_limit_ms = 200;

typedef std::chrono::steady_clock clock_type;

struct observed_event_t
{
    zlink_monitor_event_t event;
    clock_type::time_point observed_at;
};

struct received_request_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
};

struct scenario_result_t
{
    int losing_completion_ms;
    int standby_observation_ms;
    int arbitration_disconnects_a;
    int arbitration_disconnects_z;
    int arbitration_closed_a;
    int arbitration_closed_z;
    int explicit_disconnects_a;
    int explicit_disconnects_z;
};

bool should_run_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

void configure_router (void *router_, const char *routing_id_,
                       int reconnect_ivl_ms_)
{
    const int zero = 0;
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (router_, routing_id_, strlen (routing_id_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_, ZLINK_OPT_RECONNECT_IVL,
                        &reconnect_ivl_ms_, sizeof (reconnect_ivl_ms_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
                        sizeof (handover)));
}

void set_connect_routing_id (void *router_, const char *routing_id_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router_,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               routing_id_, strlen (routing_id_)));
}

void *open_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECT_DELAYED
                     | ZLINK_EVENT_CONNECT_RETRIED | ZLINK_EVENT_LISTENING
                     | ZLINK_EVENT_ACCEPTED | ZLINK_EVENT_CLOSED
                     | ZLINK_EVENT_DISCONNECTED
                     | ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

void drain_monitor (void *monitor_, std::vector<observed_event_t> *events_)
{
    for (;;) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t result = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
        observed_event_t observed;
        observed.event = event;
        observed.observed_at = clock_type::now ();
        events_->push_back (observed);
    }
}

void drive_and_drain (void *router_a_, void *router_z_, void *monitor_a_,
                      void *monitor_z_, std::vector<observed_event_t> *events_a_,
                      std::vector<observed_event_t> *events_z_, int slice_ms_)
{
    zlink_pollitem_t items[] = {
      {router_a_, 0, ZLINK_POLLIN, 0},
      {router_z_, 0, ZLINK_POLLIN, 0}};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_TRUE (zlink_poll (items, 2, slice_ms_, &error) >= 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    drain_monitor (monitor_a_, events_a_);
    drain_monitor (monitor_z_, events_z_);
}

int count_events_after (const std::vector<observed_event_t> &events_,
                        uint64_t event_, clock_type::time_point start_)
{
    int count = 0;
    for (size_t i = 0; i != events_.size (); ++i) {
        if (events_[i].observed_at >= start_
            && events_[i].event.event == event_)
            ++count;
    }
    return count;
}

bool has_ready_edge (const std::vector<observed_event_t> &events_)
{
    for (size_t i = 0; i != events_.size (); ++i) {
        if (events_[i].event.event == ZLINK_EVENT_CONNECTION_READY
            && (events_[i].event.flags
                & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                 != 0)
            return true;
    }
    return false;
}

zlink_auto_hwm_budget_snapshot_t budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_auto_hwm_recalculate (get_test_context ()));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}

void wait_for_initial_ready (void *router_a_, void *router_z_, void *monitor_a_,
                             void *monitor_z_,
                             std::vector<observed_event_t> *events_a_,
                             std::vector<observed_event_t> *events_z_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    while (clock_type::now () < deadline
           && (!has_ready_edge (*events_a_) || !has_ready_edge (*events_z_)))
        drive_and_drain (router_a_, router_z_, monitor_a_, monitor_z_, events_a_,
                         events_z_, 10);
    TEST_ASSERT_TRUE_MESSAGE (has_ready_edge (*events_a_),
                              "A did not observe reciprocal pair readiness");
    TEST_ASSERT_TRUE_MESSAGE (has_ready_edge (*events_z_),
                              "Z did not observe reciprocal pair readiness");
}

void wait_for_second_lane_set (
  bool tcp_, void *router_a_, void *router_z_, void *monitor_a_,
  void *monitor_z_, std::vector<observed_event_t> *events_a_,
  std::vector<observed_event_t> *events_z_, clock_type::time_point start_,
  const zlink_auto_hwm_budget_snapshot_t &one_pair_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    bool ready = false;
    while (clock_type::now () < deadline && !ready) {
        drive_and_drain (router_a_, router_z_, monitor_a_, monitor_z_, events_a_,
                         events_z_, 10);
        if (tcp_) {
            std::set<uint32_t> connected_lanes;
            int accepted = 0;
            for (size_t i = 0; i != events_a_->size (); ++i) {
                const observed_event_t &event = (*events_a_)[i];
                if (event.observed_at >= start_
                    && event.event.event == ZLINK_EVENT_CONNECTED)
                    connected_lanes.insert (event.event.transport_lane);
            }
            for (size_t i = 0; i != events_z_->size (); ++i) {
                const observed_event_t &event = (*events_z_)[i];
                if (event.observed_at >= start_
                    && event.event.event == ZLINK_EVENT_ACCEPTED)
                    ++accepted;
            }
            ready = connected_lanes.count (
                      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)
                      != 0
                    && connected_lanes.count (
                         ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION)
                         != 0
                    && accepted >= 2;
        } else {
            const zlink_auto_hwm_budget_snapshot_t two_pairs = budget_snapshot ();
            ready = two_pairs.active_directional_queue_count
                        >= one_pair_.active_directional_queue_count + 2
                    && two_pairs.active_completion_directional_queue_count
                         >= one_pair_.active_completion_directional_queue_count
                              + 2;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (
      ready, "reciprocal Application+Completion lane set did not attach");
}

zlink_completion_id_t submit_request (void *router_, const char *target_rid_,
                                      const char *payload_, uint32_t timeout_ms_)
{
    zlink_routing_id_t target;
    memset (&target, 0, sizeof (target));
    target.size = static_cast<uint8_t> (strlen (target_rid_));
    memcpy (target.data, target_rid_, target.size);

    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, strlen (payload_)));
    memcpy (zlink_msg_data (&part), payload_, strlen (payload_));
    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t result = zlink_request_part (
      router_, &target, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      timeout_ms_, NULL, &completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    return completion_id;
}

received_request_t receive_request (void *router_, const char *source_rid_,
                                    const char *payload_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    while (clock_type::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t flag = ZLINK_PART_MORE;
        const zlink_recv_result_t result = zlink_router_recv_part (
          router_, &source, &token, &part, &flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source);
            TEST_ASSERT_NOT_EQUAL (0, token);
            TEST_ASSERT_EQUAL_UINT8 (strlen (source_rid_), source->size);
            TEST_ASSERT_EQUAL_MEMORY (source_rid_, source->data, source->size);
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
            TEST_ASSERT_EQUAL_UINT64 (strlen (payload_), zlink_msg_size (&part));
            TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&part),
                                      strlen (payload_));
            received_request_t request;
            request.source_rid = *source;
            request.reply_token = token;
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return request;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        zlink_pollitem_t item = {router_, 0, ZLINK_POLLIN, 0};
        (void) zlink_poll (&item, 1, wait_ms, NULL);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ROUTER request");
    return received_request_t ();
}

zlink_submit_result_t reply_request (void *router_,
                                     const received_request_t &request_,
                                     const char *payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, strlen (payload_)));
    memcpy (zlink_msg_data (&part), payload_, strlen (payload_));
    const zlink_submit_result_t result =
      zlink_reply_part (router_, &request_.source_rid, request_.reply_token,
                        &part, ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return result;
}

void *open_completion_poller (void *router_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, router_, router_, ZLINK_POLLCOMPLETION));
    return poller;
}

zlink_completion_t receive_completion (void *router_,
                                       zlink_completion_id_t expected_id_)
{
    void *poller = open_completion_poller (router_);
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, wait_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_PTR (router_, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLCOMPLETION) != 0);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (router_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    if (expected_id_ != 0)
        TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    return completion;
}

void assert_request_ok (void *router_, zlink_completion_id_t completion_id_,
                        const char *reply_)
{
    zlink_completion_t completion =
      receive_completion (router_, completion_id_);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (reply_),
                              zlink_msg_size (&completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (reply_, zlink_msg_data (&completion.reply_parts[0]),
                              strlen (reply_));
    zlink_completion_close (&completion);
}

void assert_no_completion (void *router_, int duration_ms_)
{
    void *poller = open_completion_poller (router_);
    zlink_poller_event_t event;
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &event, 1, duration_ms_, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (router_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

int assert_handover_completion (void *router_, zlink_completion_id_t id_,
                                clock_type::time_point started_at_,
                                std::set<zlink_completion_id_t> *pending_ids_ = NULL)
{
    zlink_completion_t completion = receive_completion (router_, id_);
    if (pending_ids_)
        TEST_ASSERT_EQUAL_UINT64 (1, pending_ids_->erase (completion.completion_id));
    const int elapsed_ms = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - started_at_).count ());
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED,
                           completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    zlink_completion_close (&completion);
    TEST_ASSERT_LESS_THAN_INT (handover_completion_limit_ms, elapsed_ms);
    return elapsed_ms;
}

scenario_result_t run_scenario (bool tcp_, int reconnect_ivl_ms_)
{
    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *router_z = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_router (router_a, "A", reconnect_ivl_ms_);
    configure_router (router_z, "Z", reconnect_ivl_ms_);
    void *monitor_a = open_monitor (router_a);
    void *monitor_z = open_monitor (router_z);
    std::vector<observed_event_t> events_a;
    std::vector<observed_event_t> events_z;

    char endpoint_a[MAX_SOCKET_STRING];
    char endpoint_z[MAX_SOCKET_STRING];
    if (tcp_) {
        bind_loopback_ipv4 (router_a, endpoint_a, sizeof (endpoint_a));
        bind_loopback_ipv4 (router_z, endpoint_z, sizeof (endpoint_z));
    } else {
        snprintf (endpoint_a, sizeof (endpoint_a),
                  "inproc://reciprocal-handover-a-%d", reconnect_ivl_ms_);
        snprintf (endpoint_z, sizeof (endpoint_z),
                  "inproc://reciprocal-handover-z-%d", reconnect_ivl_ms_);
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (router_a, endpoint_a));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (router_z, endpoint_z));
    }

    // Z -> A sorts as the losing reciprocal direction. Make it the sole route
    // first so the request is physically admitted there before handover.
    set_connect_routing_id (router_z, "A");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router_z, endpoint_a));
    wait_for_initial_ready (router_a, router_z, monitor_a, monitor_z, &events_a,
                            &events_z);
    const zlink_auto_hwm_budget_snapshot_t one_pair = budget_snapshot ();

    const zlink_completion_id_t losing_id = submit_request (
      router_z, "A", "losing-request", losing_request_timeout_ms);
    const received_request_t losing_request =
      receive_request (router_a, "Z", "losing-request");

    // A sorts before Z, so A -> Z is the direction both ROUTERs must select.
    set_connect_routing_id (router_a, "Z");
    const clock_type::time_point arbitration_started_at = clock_type::now ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router_a, endpoint_z));
    wait_for_second_lane_set (tcp_, router_a, router_z, monitor_a, monitor_z,
                              &events_a, &events_z, arbitration_started_at,
                              one_pair);

    const int losing_completion_elapsed_ms = assert_handover_completion (
      router_z, losing_id, arbitration_started_at);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      reply_request (router_a, losing_request, "late-losing-reply"));

    // This is the first request after A -> Z has attached, while Z -> A is
    // still physically present. It must complete on the selected direction.
    const zlink_completion_id_t winner_id =
      submit_request (router_a, "Z", "winner-first", 3000);
    const received_request_t winner_request =
      receive_request (router_z, "A", "winner-first");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           reply_request (router_z, winner_request,
                                          "winner-first-reply"));
    assert_request_ok (router_a, winner_id, "winner-first-reply");

    const zlink_completion_id_t retry_id =
      submit_request (router_z, "A", "retry-on-winner", 3000);
    const received_request_t retry_request =
      receive_request (router_a, "Z", "retry-on-winner");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      reply_request (router_a, retry_request, "retry-on-winner-reply"));
    assert_request_ok (router_z, retry_id, "retry-on-winner-reply");
    assert_no_completion (router_z, 50);

    const int observation_ms = std::max (250, reconnect_ivl_ms_ * 2);
    const clock_type::time_point observation_deadline =
      arbitration_started_at + std::chrono::milliseconds (observation_ms);
    while (clock_type::now () < observation_deadline)
        drive_and_drain (router_a, router_z, monitor_a, monitor_z, &events_a,
                         &events_z, 10);
    const int arbitration_disconnects_a = count_events_after (
      events_a, ZLINK_EVENT_DISCONNECTED, arbitration_started_at);
    const int arbitration_disconnects_z = count_events_after (
      events_z, ZLINK_EVENT_DISCONNECTED, arbitration_started_at);
    const int arbitration_closed_a = count_events_after (
      events_a, ZLINK_EVENT_CLOSED, arbitration_started_at);
    const int arbitration_closed_z = count_events_after (
      events_z, ZLINK_EVENT_CLOSED, arbitration_started_at);
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      0, arbitration_disconnects_a,
      "A unexpectedly disconnected reciprocal standby during handover");
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      0, arbitration_disconnects_z,
      "Z unexpectedly disconnected reciprocal standby during handover");
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      0, arbitration_closed_a,
      "A unexpectedly closed reciprocal standby during handover");
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      0, arbitration_closed_z,
      "Z unexpectedly closed reciprocal standby during handover");

    // Remove only the losing Z -> A connector after the successful retry.
    const clock_type::time_point explicit_disconnect_at = clock_type::now ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (router_z, endpoint_a));
    const clock_type::time_point detach_deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    bool detached = false;
    while (clock_type::now () < detach_deadline && !detached) {
        drive_and_drain (router_a, router_z, monitor_a, monitor_z, &events_a,
                         &events_z, 10);
        const zlink_auto_hwm_budget_snapshot_t after = budget_snapshot ();
        detached = after.active_directional_queue_count
                       <= one_pair.active_directional_queue_count
                   && after.active_completion_directional_queue_count
                        <= one_pair.active_completion_directional_queue_count;
        if (tcp_)
            detached = count_events_after (events_a,
                                           ZLINK_EVENT_DISCONNECTED,
                                           explicit_disconnect_at)
                         >= 2
                       && count_events_after (events_z,
                                              ZLINK_EVENT_DISCONNECTED,
                                              explicit_disconnect_at)
                            >= 2;
    }
    TEST_ASSERT_TRUE_MESSAGE (detached,
                              "explicit loser disconnect did not retire two lanes");

    // The original request deadline must not publish a second terminal,
    // including after the retired standby physically detaches.
    assert_no_completion (router_z, losing_request_timeout_ms);

    scenario_result_t result;
    result.losing_completion_ms = losing_completion_elapsed_ms;
    result.standby_observation_ms = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        explicit_disconnect_at - arbitration_started_at)
        .count ());
    result.arbitration_disconnects_a = arbitration_disconnects_a;
    result.arbitration_disconnects_z = arbitration_disconnects_z;
    result.arbitration_closed_a = arbitration_closed_a;
    result.arbitration_closed_z = arbitration_closed_z;
    result.explicit_disconnects_a = count_events_after (
      events_a, ZLINK_EVENT_DISCONNECTED, explicit_disconnect_at);
    result.explicit_disconnects_z = count_events_after (
      events_z, ZLINK_EVENT_DISCONNECTED, explicit_disconnect_at);

    printf ("RESULT transport=%s reconnect_ivl_ms=%d standby_observed_ms=%d "
            "arbitration_disconnected_a=%d arbitration_disconnected_z=%d "
            "arbitration_closed_a=%d arbitration_closed_z=%d "
            "losing_completion_ms=%d "
            "explicit_disconnected_a=%d explicit_disconnected_z=%d "
            "winner_direction=A->Z winner_first=ok retry=ok\n",
            tcp_ ? "tcp" : "inproc", reconnect_ivl_ms_,
            result.standby_observation_ms,
            result.arbitration_disconnects_a,
            result.arbitration_disconnects_z, result.arbitration_closed_a,
            result.arbitration_closed_z, result.losing_completion_ms,
            result.explicit_disconnects_a, result.explicit_disconnects_z);
    fflush (stdout);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor_z));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor_a));
    test_context_socket_close_zero_linger (router_z);
    test_context_socket_close_zero_linger (router_a);
    return result;
}

void run_simultaneous_reciprocal (bool tcp_, bool connect_alias_,
                                  int delayed_bind_ = -1)
{
    for (int iteration = 0; iteration < 20; ++iteration) {
        void *routers[] = {test_context_socket (ZLINK_SOCKET_ROUTER),
                           test_context_socket (ZLINK_SOCKET_ROUTER)};
        const char *rids[] = {"A", "Z"};
        const char *payloads[] = {"request-from-A", "request-from-Z"};
        const char *replies[] = {"reply-to-A", "reply-to-Z"};
        void *monitors[2];
        std::vector<observed_event_t> events[2];
        char endpoints[2][MAX_SOCKET_STRING];
        for (int i = 0; i < 2; ++i) {
            configure_router (routers[i], rids[i], 100);
            monitors[i] = open_monitor (routers[i]);
            if (tcp_)
                bind_loopback_ipv4 (routers[i], endpoints[i],
                                    sizeof (endpoints[i]));
            else {
                snprintf (endpoints[i], sizeof (endpoints[i]),
                          "inproc://simultaneous-%s-%d", rids[i], iteration);
                if (i != delayed_bind_)
                    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                                           zlink_bind (routers[i], endpoints[i]));
            }
            if (connect_alias_)
                set_connect_routing_id (routers[i], rids[1 - i]);
        }

        std::mutex sync;
        std::condition_variable start;
        int waiting = 0;
        bool released = false;
        zlink_connect_result_t connected[2];
        clock_type::time_point started[2];
        const auto connect_peer = [&](int i) {
            {
                std::unique_lock<std::mutex> lock (sync);
                ++waiting;
                start.notify_all ();
                start.wait (lock, [&] { return released; });
            }
            started[i] = clock_type::now ();
            connected[i] = zlink_connect (routers[i], endpoints[1 - i]);
        };
        if (delayed_bind_ >= 0) {
            // Mesh nodes may start one at a time: the first node's outbound
            // intent exists before the second node binds and connects back.
            const int first = 1 - delayed_bind_;
            started[first] = clock_type::now ();
            connected[first] = zlink_connect (routers[first],
                                               endpoints[delayed_bind_]);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_BIND_OK,
              zlink_bind (routers[delayed_bind_], endpoints[delayed_bind_]));
            started[delayed_bind_] = clock_type::now ();
            connected[delayed_bind_] = zlink_connect (routers[delayed_bind_],
                                                       endpoints[first]);
        } else {
            std::thread a (connect_peer, 0);
            std::thread z (connect_peer, 1);
            {
                std::unique_lock<std::mutex> lock (sync);
                start.wait (lock, [&] { return waiting == 2; });
                released = true;
                start.notify_all ();
            }
            a.join ();
            z.join ();
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, connected[0]);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, connected[1]);
        wait_for_initial_ready (routers[0], routers[1], monitors[0], monitors[1],
                                &events[0], &events[1]);

        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        for (int i = 0; i < 2; ++i)
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_poller_add (poller, routers[i], NULL,
                                ZLINK_POLLIN | ZLINK_POLLCOMPLETION));

        zlink_completion_id_t ids[2] = {0, 0};
        int disconnected[2] = {0, 0};
        bool complete[2] = {false, false};
        const auto submit = [&](int i) {
            zlink_routing_id_t target = {};
            target.size = 1;
            target.data[0] = rids[1 - i][0];
            zlink_msg_t part;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_msg_init_size (&part, strlen (payloads[i])));
            memcpy (zlink_msg_data (&part), payloads[i], strlen (payloads[i]));
            const zlink_submit_result_t result = zlink_request_part (
              routers[i], &target, &part, ZLINK_SEND_FLAGS_DONTWAIT,
              ZLINK_PART_FINAL, 3000, NULL, &ids[i]);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            if (result == ZLINK_SUBMIT_NOT_CONNECTED) {
                TEST_ASSERT_EQUAL_INT (0, disconnected[i]++);
                ids[i] = 0;
            } else {
                TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
                TEST_ASSERT_NOT_EQUAL (0, ids[i]);
            }
        };
        submit (0);
        submit (1);
        const clock_type::time_point deadline =
          clock_type::now () + std::chrono::milliseconds (wait_ms);
        while (clock_type::now () < deadline && (!complete[0] || !complete[1])) {
            for (int i = 0; i < 2; ++i) {
                drain_monitor (monitors[i], &events[i]);
                // A replacement can retire an admitted request before its
                // peer receives it. Drain requests and terminals together.
                zlink_msg_t part;
                TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
                const zlink_routing_id_t *source = NULL;
                zlink_reply_token_t token = 0;
                zlink_part_flag_t flag = ZLINK_PART_MORE;
                const zlink_recv_result_t received = zlink_router_recv_part (
                  routers[i], &source, &token, &part, &flag,
                  ZLINK_RECV_FLAGS_DONTWAIT);
                if (received == ZLINK_RECV_OK) {
                    TEST_ASSERT_NOT_NULL (source);
                    TEST_ASSERT_EQUAL_UINT8 (1, source->size);
                    TEST_ASSERT_EQUAL_MEMORY (rids[1 - i], source->data, 1);
                    TEST_ASSERT_NOT_EQUAL (0, token);
                    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
                    TEST_ASSERT_EQUAL_UINT64 (strlen (payloads[1 - i]),
                                              zlink_msg_size (&part));
                    TEST_ASSERT_EQUAL_MEMORY (payloads[1 - i],
                                               zlink_msg_data (&part),
                                               strlen (payloads[1 - i]));
                    received_request_t request = {*source, token};
                    TEST_ASSERT_EQUAL_INT (
                      ZLINK_SUBMIT_OK,
                      reply_request (routers[i], request, replies[1 - i]));
                } else
                    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, received);
                TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

                zlink_completion_t completion = {};
                completion.struct_size = sizeof (completion);
                const zlink_recv_result_t result = zlink_completion_recv (
                  routers[i], &completion, ZLINK_RECV_FLAGS_DONTWAIT);
                if (result == ZLINK_RECV_OK) {
                    TEST_ASSERT_FALSE (complete[i]);
                    TEST_ASSERT_EQUAL_UINT64 (ids[i], completion.completion_id);
                    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                                           completion.kind);
                    if (completion.request_result == ZLINK_REQUEST_NOT_CONNECTED) {
                        TEST_ASSERT_EQUAL_INT (0, disconnected[i]++);
                        ids[i] = 0;
                    } else {
                        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                                               completion.request_result);
                        TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
                        TEST_ASSERT_EQUAL_UINT64 (
                          strlen (replies[i]),
                          zlink_msg_size (&completion.reply_parts[0]));
                        TEST_ASSERT_EQUAL_MEMORY (
                          replies[i], zlink_msg_data (&completion.reply_parts[0]),
                          strlen (replies[i]));
                        complete[i] = true;
                    }
                    zlink_completion_close (&completion);
                } else
                    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
                if (!complete[i] && ids[i] == 0)
                    submit (i);
            }
            if (!complete[0] || !complete[1]) {
                zlink_poller_event_t event;
                zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
                TEST_ASSERT_TRUE (zlink_poller_wait (poller, &event, 1, 10,
                                                     &error) >= 0);
                TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
            }
        }
        TEST_ASSERT_TRUE_MESSAGE (complete[0] && complete[1],
                                   "both reciprocal requests must complete");
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));

        // Initial readiness may precede the reciprocal admission. Observe
        // both physical Application connections before checking the survivor.
        std::set<uint64_t> ready_connections[2];
        const clock_type::time_point ready_deadline =
          clock_type::now () + std::chrono::milliseconds (wait_ms);
        while (clock_type::now () < ready_deadline
               && (ready_connections[0].size () < 2
                   || ready_connections[1].size () < 2)) {
            drive_and_drain (routers[0], routers[1], monitors[0], monitors[1],
                             &events[0], &events[1], 10);
            for (int i = 0; i < 2; ++i) {
                for (size_t j = 0; j < events[i].size (); ++j) {
                    const zlink_monitor_event_t &event = events[i][j].event;
                    if (event.event == ZLINK_EVENT_CONNECTION_READY
                        && event.transport_lane
                             == ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION
                        && event.connection_id != 0)
                        ready_connections[i].insert (event.connection_id);
                }
            }
        }
        for (int i = 0; i < 2; ++i)
            TEST_ASSERT_EQUAL_UINT64 (2, ready_connections[i].size ());
        for (int i = 0; i < 2; ++i) {
            const zlink_completion_id_t id =
              submit_request (routers[i], rids[1 - i], "survivor", 3000);
            const received_request_t request =
              receive_request (routers[1 - i], rids[i], "survivor");
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK,
              reply_request (routers[1 - i], request, "survivor-reply"));
            assert_request_ok (routers[i], id, "survivor-reply");
        }

        for (int i = 0; i < 2; ++i) {
            drain_monitor (monitors[i], &events[i]);
            TEST_ASSERT_EQUAL_INT (
              0, count_events_after (events[i], ZLINK_EVENT_DISCONNECTED,
                                      clock_type::time_point ()));
            TEST_ASSERT_EQUAL_INT (
              0, count_events_after (events[i], ZLINK_EVENT_CLOSED,
                                      clock_type::time_point ()));
        }
        printf ("RESULT simultaneous transport=%s alias=%d delayed_bind=%d "
                "iteration=%d "
                "connect_delta_us=%lld not_connected_a=%d not_connected_z=%d "
                "reply_a=ok reply_z=ok survivor_a=ok survivor_z=ok\n",
                tcp_ ? "tcp" : "inproc", connect_alias_ ? 1 : 0,
                delayed_bind_, iteration,
                static_cast<long long> (
                  std::chrono::duration_cast<std::chrono::microseconds> (
                    started[0] < started[1] ? started[1] - started[0]
                                             : started[0] - started[1]).count ()),
                disconnected[0], disconnected[1]);
        fflush (stdout);
        for (int i = 0; i < 2; ++i)
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                   zlink_monitor_close (&monitors[i]));
        for (int i = 0; i < 2; ++i)
            test_context_socket_close_zero_linger (routers[i]);
    }
}

void run_same_direction_takeover (bool tcp_)
{
    void *requester = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *old_peer = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *new_peer = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *other_peer = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_router (requester, "R", 100);
    configure_router (old_peer, "S", 100);
    configure_router (new_peer, "S", 100);
    configure_router (other_peer, "T", 100);
    void *monitor = open_monitor (requester);
    void *old_monitor = open_monitor (old_peer);
    void *new_monitor = open_monitor (new_peer);
    void *other_monitor = open_monitor (other_peer);
    std::vector<observed_event_t> events;
    std::vector<observed_event_t> old_events;
    std::vector<observed_event_t> new_events;
    std::vector<observed_event_t> other_events;
    char old_endpoint[MAX_SOCKET_STRING];
    char new_endpoint[MAX_SOCKET_STRING];
    char other_endpoint[MAX_SOCKET_STRING];
    if (tcp_) {
        bind_loopback_ipv4 (old_peer, old_endpoint, sizeof (old_endpoint));
        bind_loopback_ipv4 (new_peer, new_endpoint, sizeof (new_endpoint));
        bind_loopback_ipv4 (other_peer, other_endpoint, sizeof (other_endpoint));
    } else {
        strcpy (old_endpoint, "inproc://same-direction-old");
        strcpy (new_endpoint, "inproc://same-direction-new");
        strcpy (other_endpoint, "inproc://same-direction-other");
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (old_peer, old_endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (new_peer, new_endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (other_peer, other_endpoint));
    }
    set_connect_routing_id (requester, "S");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (requester, old_endpoint));
    wait_for_initial_ready (requester, old_peer, monitor, old_monitor,
                            &events, &old_events);
    events.clear ();
    set_connect_routing_id (requester, "T");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (requester, other_endpoint));
    wait_for_initial_ready (requester, other_peer, monitor, other_monitor,
                            &events, &other_events);
    const zlink_completion_id_t other_id =
      submit_request (requester, "T", "unaffected", losing_request_timeout_ms);
    const received_request_t other_request =
      receive_request (other_peer, "R", "unaffected");

    const size_t pending_count = 3;
    std::set<zlink_completion_id_t> losing_ids;
    received_request_t losing_requests[pending_count];
    for (size_t i = 0; i < pending_count; ++i) {
        losing_ids.insert (submit_request (requester, "S", "superseded",
                                           losing_request_timeout_ms));
        losing_requests[i] = receive_request (old_peer, "R", "superseded");
    }

    events.clear ();
    set_connect_routing_id (requester, "S");
    const clock_type::time_point started_at = clock_type::now ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (requester, new_endpoint));
    wait_for_initial_ready (requester, new_peer, monitor, new_monitor,
                            &events, &new_events);
    int elapsed_ms = 0;
    for (size_t i = 0; i < pending_count; ++i)
        elapsed_ms = assert_handover_completion (requester, 0, started_at,
                                                 &losing_ids);
    TEST_ASSERT_TRUE (losing_ids.empty ());

    // A reply submitted after handover still uses the old request's pair.
    // It cannot consume a replacement request or create another terminal.
    for (size_t i = 0; i < pending_count; ++i)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          reply_request (old_peer, losing_requests[i], "late-reply"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      reply_request (other_peer, other_request, "unaffected-reply"));
    assert_request_ok (requester, other_id, "unaffected-reply");
    const zlink_completion_id_t retry_id =
      submit_request (requester, "S", "retry", losing_request_timeout_ms);
    const received_request_t retry = receive_request (new_peer, "R", "retry");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           reply_request (new_peer, retry, "retry-reply"));
    assert_request_ok (requester, retry_id, "retry-reply");
    assert_no_completion (requester, losing_request_timeout_ms);
    printf ("RESULT same_direction transport=%s pending=%zu "
            "handover_completion_ms=%d unaffected=ok retry=ok duplicate=0\n",
            tcp_ ? "tcp" : "inproc", pending_count, elapsed_ms);
    fflush (stdout);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&other_monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&new_monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&old_monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (requester);
    test_context_socket_close_zero_linger (other_peer);
    test_context_socket_close_zero_linger (new_peer);
    test_context_socket_close_zero_linger (old_peer);
}

}

#define DEFINE_SCENARIO_TEST(transport_, tcp_, interval_)                         \
    void test_reciprocal_handover_##transport_##_##interval_##ms ()               \
    {                                                                              \
        (void) run_scenario (tcp_, interval_);                                      \
    }

DEFINE_SCENARIO_TEST (tcp, true, 10)
DEFINE_SCENARIO_TEST (tcp, true, 100)
DEFINE_SCENARIO_TEST (tcp, true, 1000)
DEFINE_SCENARIO_TEST (inproc, false, 10)
DEFINE_SCENARIO_TEST (inproc, false, 100)
DEFINE_SCENARIO_TEST (inproc, false, 1000)

void test_same_direction_takeover_tcp ()
{
    run_same_direction_takeover (true);
}

void test_same_direction_takeover_inproc ()
{
    run_same_direction_takeover (false);
}

void test_simultaneous_reciprocal_tcp ()
{
    run_simultaneous_reciprocal (true, false);
}

void test_simultaneous_reciprocal_tcp_alias ()
{
    run_simultaneous_reciprocal (true, true);
}

void test_simultaneous_reciprocal_inproc ()
{
    run_simultaneous_reciprocal (false, false);
}

void test_simultaneous_reciprocal_inproc_alias ()
{
    run_simultaneous_reciprocal (false, true);
}

void test_reciprocal_inproc_connect_before_bind_a_first ()
{
    run_simultaneous_reciprocal (false, false, 1);
}

void test_reciprocal_inproc_connect_before_bind_a_first_alias ()
{
    run_simultaneous_reciprocal (false, true, 1);
}

void test_reciprocal_inproc_connect_before_bind_z_first ()
{
    run_simultaneous_reciprocal (false, false, 0);
}

void test_reciprocal_inproc_connect_before_bind_z_first_alias ()
{
    run_simultaneous_reciprocal (false, true, 0);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define RUN_SELECTED(test_)                                                        \
    do {                                                                           \
        if (should_run_test (#test_))                                               \
            RUN_TEST (test_);                                                       \
    } while (false)
    RUN_SELECTED (test_reciprocal_handover_tcp_10ms);
    RUN_SELECTED (test_reciprocal_handover_tcp_100ms);
    RUN_SELECTED (test_reciprocal_handover_tcp_1000ms);
    RUN_SELECTED (test_reciprocal_handover_inproc_10ms);
    RUN_SELECTED (test_reciprocal_handover_inproc_100ms);
    RUN_SELECTED (test_reciprocal_handover_inproc_1000ms);
    RUN_SELECTED (test_same_direction_takeover_tcp);
    RUN_SELECTED (test_same_direction_takeover_inproc);
    RUN_SELECTED (test_simultaneous_reciprocal_tcp);
    RUN_SELECTED (test_simultaneous_reciprocal_tcp_alias);
    RUN_SELECTED (test_simultaneous_reciprocal_inproc);
    RUN_SELECTED (test_simultaneous_reciprocal_inproc_alias);
    RUN_SELECTED (test_reciprocal_inproc_connect_before_bind_a_first);
    RUN_SELECTED (test_reciprocal_inproc_connect_before_bind_a_first_alias);
    RUN_SELECTED (test_reciprocal_inproc_connect_before_bind_z_first);
    RUN_SELECTED (test_reciprocal_inproc_connect_before_bind_z_first_alias);
#undef RUN_SELECTED
    return UNITY_END ();
}
