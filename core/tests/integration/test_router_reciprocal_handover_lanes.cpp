/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wait_ms = 5000;
const uint32_t losing_request_timeout_ms = 600;

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
    int losing_timeout_ms;
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
      {router_a_, 0, ZLINK_POLLIN | ZLINK_POLLCOMPLETION, 0},
      {router_z_, 0, ZLINK_POLLIN | ZLINK_POLLCOMPLETION, 0},
      {monitor_a_, 0, ZLINK_POLLIN, 0},
      {monitor_z_, 0, ZLINK_POLLIN, 0}};
    (void) zlink_poll (items, 4, slice_ms_, NULL);
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
        msleep (1);
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

zlink_completion_t receive_completion (void *router_,
                                       zlink_completion_id_t expected_id_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    while (clock_type::now () < deadline) {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t result = zlink_completion_recv (
          router_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
            return completion;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for REQUEST completion");
    zlink_completion_t unreachable;
    memset (&unreachable, 0, sizeof (unreachable));
    return unreachable;
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
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (duration_ms_);
    do {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_completion_recv (router_, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        msleep (1);
    } while (clock_type::now () < deadline);
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

    const clock_type::time_point losing_submitted_at = clock_type::now ();
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

    (void) losing_request;

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

    zlink_completion_t losing_completion =
      receive_completion (router_z, losing_id);
    const int losing_timeout_elapsed_ms = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - losing_submitted_at)
        .count ());
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, losing_completion.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           losing_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (0, losing_completion.reply_part_count);
    zlink_completion_close (&losing_completion);
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

    // Remove only the losing Z -> A connector. The surviving A -> Z pair must
    // serve both the retried Application request and its Completion reply.
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

    const zlink_completion_id_t retry_id =
      submit_request (router_z, "A", "retry-on-winner", 3000);
    const received_request_t retry_request =
      receive_request (router_a, "Z", "retry-on-winner");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      reply_request (router_a, retry_request, "retry-on-winner-reply"));
    assert_request_ok (router_z, retry_id, "retry-on-winner-reply");
    assert_no_completion (router_z, 50);

    scenario_result_t result;
    result.losing_timeout_ms = losing_timeout_elapsed_ms;
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
            "losing_timeout_ms=%d "
            "explicit_disconnected_a=%d explicit_disconnected_z=%d "
            "winner_direction=A->Z winner_first=ok retry=ok\n",
            tcp_ ? "tcp" : "inproc", reconnect_ivl_ms_,
            result.standby_observation_ms,
            result.arbitration_disconnects_a,
            result.arbitration_disconnects_z, result.arbitration_closed_a,
            result.arbitration_closed_z, result.losing_timeout_ms,
            result.explicit_disconnects_a, result.explicit_disconnects_z);
    fflush (stdout);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor_z));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor_a));
    test_context_socket_close_zero_linger (router_z);
    test_context_socket_close_zero_linger (router_a);
    return result;
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
#undef RUN_SELECTED
    return UNITY_END ();
}
