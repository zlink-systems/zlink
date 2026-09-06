/* SPDX-License-Identifier: MPL-2.0 */

//  Internal flow-state monitor bookkeeping and injected protocol edges.
//  Public lifecycle and state-setting contracts live in integration/test_flow_state_c_api.cpp.

#include "unittest_flow_state_testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"

#include "../../src/runtime/core/flow_state_frame.hpp"
#include "../../src/runtime/core/msg.hpp"
#include "../../src/runtime/core/pipe.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"

#include <atomic>
#include <chrono>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
zlink::socket_base_t *as_socket (void *socket_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    return handle.socket;
}

bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
}

//  Reads the flow metrics without opening a monitor, so a test can hold a
//  monitor probe open for events and still sample the counters.
struct flow_metrics_t
{
    flow_metrics_t () :
        paused_connections (0),
        pause_applied (0),
        resume_applied (0),
        last_pause_duration_ms (0)
    {
    }

    uint64_t paused_connections;
    uint64_t pause_applied;
    uint64_t resume_applied;
    uint64_t last_pause_duration_ms;
};

flow_metrics_t read_flow_metrics (void *socket_)
{
    flow_metrics_t out;
    as_socket (socket_)->flow_state_metrics (&out.paused_connections,
                                             &out.pause_applied,
                                             &out.resume_applied, NULL,
                                             &out.last_pause_duration_ms);
    return out;
}

zlink_monitor_status_t read_monitor_status (void *socket_)
{
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = 0;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    TEST_ASSERT_NOT_NULL (monitor);
    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_monitor_status (monitor, &status));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    return status;
}

//  A locally admitted count-1 pair. The shared fixture owns pipe attachment
//  and synchronous command progression; this fixture supplies flow assertions.
struct paired_fixture_t
{
    paired_fixture_t () : pair (NULL), dealer (NULL), router (NULL) {}

    void setup (uint64_t hwm_ = 0)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        if (hwm_) {
            void *sockets[] = {router, dealer};
            for (size_t i = 0; i != 2; ++i) {
                TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
                  sockets[i], ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
                TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
                  sockets[i], ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
            }
        }
        pair = new contract_socket_pair_t (dealer, router, 1, 1, true, hwm_);
        flow_internal_send_string (dealer, "hello", 0);
        char rid[256];
        TEST_ASSERT_GREATER_THAN_INT (0, flow_internal_recv (router, rid, sizeof (rid), 0));
        flow_internal_recv_string (router, "hello", 0);
        pair->pump ();
        TEST_ASSERT_TRUE (resolve_dealer_target ());
        assert_single_lane ();
    }

    bool resolve_dealer_target ()
    {
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            if (as_socket (dealer)->select_routed_submit_target (NULL, &target) == 0
                && target.transport_pair_id != 0) {
                pair_id = target.transport_pair_id;
                pair_generation = target.transport_pair_generation;
                return true;
            }
            msleep (1);
        }
        return false;
    }

    void teardown ()
    {
        delete pair;
        pair = NULL;
        if (dealer)
            dealer = test_context_socket_close_zero_linger (dealer);
        if (router)
            router = test_context_socket_close_zero_linger (router);
    }

    void replace_pair ()
    {
        const uint64_t next_pair = pair_id + 1;
        const uint64_t next_generation = pair_generation + 1;
        pair->application[0]->terminate (false);
        pair->pump ();
        delete pair;
        pair = new contract_socket_pair_t (dealer, router, next_pair,
                                            next_generation);
        pair_id = pair->pair_id;
        pair_generation = pair->generation;
    }

    void assert_single_lane ()
    {
        zlink::pipe_t *const application =
          as_socket (dealer)->test_pair_pipe (pair_id, pair_generation, false);
        TEST_ASSERT_NOT_NULL (application);
        TEST_ASSERT_NULL (
          as_socket (dealer)->test_pair_pipe (pair_id, pair_generation, true));
        TEST_ASSERT_EQUAL_UINT (1u, application->get_transport_lane_count ());
        TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                               application->get_transport_lane ());
        TEST_ASSERT_NULL (
          as_socket (dealer)->completion_pipe_for_transport_pair (
            pair_id, pair_generation));
    }

    //  Delivers one hand-built frame to the DEALER through the topology-selected
    //  control source (Application for this count-1 pair), then drains the
    //  socket mailbox so the pipe applies it.
    bool inject (uint8_t state_, uint64_t epoch_)
    {
        return inject_frame (state_, epoch_, true);
    }

    bool inject_frame (uint8_t state_, uint64_t epoch_, bool drain_)
    {
        zlink::pipe_t *control =
          as_socket (dealer)->test_pair_pipe (pair_id, pair_generation, false);
        TEST_ASSERT_NOT_NULL (control);

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = state_;
        frame.epoch = epoch_;

        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        msg.set_transport_connection_id (
          control->get_transport_connection_id ());
        const bool consumed =
          as_socket (dealer)->consume_receive_flow_state_frame (control, msg);
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        if (drain_)
            (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
        return consumed;
    }

    bool wait_for_applied_pause (bool expected_)
    {
        const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
        do {
            pair->pump ();
            if (as_socket (dealer)->application_pipe_remote_flow_paused (
                  pair_id, pair_generation) == expected_)
                return true;
            msleep (1);
        } while (!deadline_expired (deadline));
        return false;
    }

    contract_socket_pair_t *pair;
    void *dealer;
    void *router;
    uint64_t pair_id;
    uint64_t pair_generation;
};

const uint32_t k_flow_events = ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED
                               | ZLINK_EVENT_FLOW_STATE_STALE;

zlink_submit_result_t try_send_flow_filler (void *socket_, size_t size_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size_));
    memset (zlink_msg_data (&part), 'h', size_);
    return zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE,
                            ZLINK_PART_FINAL, NULL, NULL);
}

size_t fill_flow_pipe_until_backpressured (void *socket_)
{
    for (size_t admitted = 0; admitted != 256; ++admitted) {
        const zlink_submit_result_t result =
          try_send_flow_filler (socket_, 1024);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            return admitted;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_FAIL_MESSAGE ("flow-state fixture did not reach configured HWM");
    return 0;
}

bool routing_id_equal (const zlink_routing_id_t &lhs_,
                       const zlink_routing_id_t &rhs_)
{
    return lhs_.size == rhs_.size
           && memcmp (lhs_.data, rhs_.data, lhs_.size) == 0;
}

zlink_routed_submit_target_t select_router_target_eventually (
  void *router_, const zlink_routing_id_t *rid_)
{
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (3000);
    while (!deadline_expired (deadline)) {
        if (as_socket (router_)->select_routed_submit_target (rid_, &target) == 0
            && target.transport_pair_id != 0)
            return target;
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("ROUTER transport pair did not become selectable");
    return target;
}

bool inject_flow_for_target (void *socket_,
                             const zlink_routed_submit_target_t &target_,
                             uint8_t state_, uint64_t epoch_)
{
    zlink::pipe_t *control = as_socket (socket_)->test_pair_pipe (
      target_.transport_pair_id, target_.transport_pair_generation, false);
    TEST_ASSERT_NOT_NULL (control);
    TEST_ASSERT_EQUAL_UINT (1u, control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           control->get_transport_lane ());
    TEST_ASSERT_NULL (as_socket (socket_)->test_pair_pipe (
      target_.transport_pair_id, target_.transport_pair_generation, true));

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = state_;
    frame.epoch = epoch_;
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
    msg.set_transport_connection_id (
      control->get_transport_connection_id ());
    const bool consumed =
      as_socket (socket_)->consume_receive_flow_state_frame (control, msg);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
    (void) contract_socket_pair_t::pump_owner (as_socket (socket_));
    return consumed;
}

//  A frame whose epoch does not advance (a duplicate, here delivered twice
//  with the same epoch) is ignored and reported as stale exactly once for
//  the repeat - never as a second PAUSED.
void test_duplicate_frame_emits_stale_event ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_paused, 5));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));

    //  Same epoch again: stale, not a second PAUSED.
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_paused, 5));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (&probe, 2, 200));

    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED), flow_unit_monitor_event_at (&probe, 0));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_FLOW_STATE_STALE), flow_unit_monitor_event_at (&probe, 1));

    //  The repeat is stale by epoch on the same public flow-event identity.
    const zlink_monitor_event_t paused = flow_unit_monitor_record_at (&probe, 0);
    const zlink_monitor_event_t stale = flow_unit_monitor_record_at (&probe, 1);
    TEST_ASSERT_TRUE (routing_id_equal (paused.routing_id, stale.routing_id));
    TEST_ASSERT_EQUAL_UINT64 (paused.connection_id, stale.connection_id);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            stale.transport_lane);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
      stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
    TEST_ASSERT_EQUAL_UINT64 (5, stale.value);

    close_flow_unit_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

//  A newer epoch that repeats the already applied state updates only the
//  pair's version record. Repeating the local public state setter is also a
//  no-op. Both are observed through the public monitor handler.
void test_same_state_forward_epoch_and_repeated_local_set_emit_no_event ()
{
    {
        paired_fixture_t fixture;
        fixture.setup ();
        flow_unit_monitor_probe_t probe;
        void *monitor =
          open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);

        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 10));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));

        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 11));
        TEST_ASSERT_TRUE (
          flow_unit_monitor_has_no_additional (&probe, 1, 200));
        TEST_ASSERT_EQUAL_INT (1, flow_unit_monitor_count (&probe));

        close_flow_unit_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup ();
        flow_unit_monitor_probe_t probe;
        void *monitor =
          open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);

        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (
          flow_unit_monitor_has_no_additional (&probe, 1, 200));

        close_flow_unit_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }
}

void test_resumed_routing_id_and_epoch_stale_match_prior_transition ()
{
    paired_fixture_t fixture;
    fixture.setup ();
    flow_unit_monitor_probe_t probe;
    void *monitor =
      open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_paused, 20));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
    const zlink_monitor_event_t paused =
      flow_unit_monitor_record_at (&probe, 0);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 21));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 2, 2000));
    const zlink_monitor_event_t resumed =
      flow_unit_monitor_record_at (&probe, 1);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_SEND_FLOW_RESUMED, resumed.event);
    TEST_ASSERT_TRUE (resumed.routing_id.size > 0);
    TEST_ASSERT_TRUE (routing_id_equal (paused.routing_id, resumed.routing_id));
    TEST_ASSERT_EQUAL_UINT64 (paused.connection_id, resumed.connection_id);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            resumed.transport_lane);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 21));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 3, 2000));
    const zlink_monitor_event_t stale =
      flow_unit_monitor_record_at (&probe, 2);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_FLOW_STATE_STALE, stale.event);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
      stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
    TEST_ASSERT_EQUAL_UINT64 (resumed.value, stale.value);
    TEST_ASSERT_TRUE (routing_id_equal (resumed.routing_id, stale.routing_id));
    TEST_ASSERT_EQUAL_UINT64 (resumed.connection_id, stale.connection_id);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            stale.transport_lane);

    close_flow_unit_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

void test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected ()
{
    paired_fixture_t fixture;
    fixture.setup (4096);
    // A zero-timeout blocking submit observes physical pipe admission, so a
    // receive-flow resume cannot make an HWM-blocked pipe writable.
    const int no_wait = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      fixture.dealer, ZLINK_OPT_SNDTIMEO, &no_wait, sizeof (no_wait)));
    const size_t filler_count =
      fill_flow_pipe_until_backpressured (fixture.dealer);
    TEST_ASSERT_TRUE (filler_count > 0);

    flow_unit_monitor_probe_t probe;
    void *monitor =
      open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);
    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_paused, 30));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 31));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 2, 2000));

    const zlink_monitor_event_t resumed =
      flow_unit_monitor_record_at (&probe, 1);
    const zlink_submit_result_t next_send =
      try_send_flow_filler (fixture.dealer, 1024);
    const int next_send_errno = zlink_errno ();
    const uint32_t writable_flag =
      resumed.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE;

    close_flow_unit_monitor_probe (&monitor, &probe);
    fixture.teardown ();

    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_SEND_FLOW_RESUMED, resumed.event);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, next_send);
    TEST_ASSERT_EQUAL_INT (EAGAIN, next_send_errno);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE (
      0u, writable_flag,
      "RESUMED reported writable while byte HWM still rejected send");
}

void test_flow_event_numeric_values_and_each_excluded_mask ()
{
    TEST_ASSERT_EQUAL_UINT32 (1u << 16,
                              ZLINK_EVENT_SEND_FLOW_PAUSED);
    TEST_ASSERT_EQUAL_UINT32 (1u << 17,
                              ZLINK_EVENT_SEND_FLOW_RESUMED);
    TEST_ASSERT_EQUAL_UINT32 (1u << 18, ZLINK_EVENT_FLOW_STATE_STALE);
    TEST_ASSERT_EQUAL_UINT32 (0x7FFFFu, ZLINK_EVENT_ALL);

    {
        paired_fixture_t fixture;
        fixture.setup ();
        flow_unit_monitor_probe_t probe;
        void *monitor = open_flow_unit_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_RESUMED | ZLINK_EVENT_FLOW_STATE_STALE,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 40));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (
          flow_unit_monitor_has_no_additional (&probe, 0, 200));
        close_flow_unit_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup ();
        flow_unit_monitor_probe_t probe;
        void *monitor = open_flow_unit_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_FLOW_STATE_STALE,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 41));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_running, 42));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
        TEST_ASSERT_TRUE (
          flow_unit_monitor_has_no_additional (&probe, 1, 200));
        close_flow_unit_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup ();
        flow_unit_monitor_probe_t probe;
        void *monitor = open_flow_unit_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 43));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 43));
        TEST_ASSERT_TRUE (
          flow_unit_monitor_has_no_additional (&probe, 1, 200));
        close_flow_unit_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }
}

//  The two events are produced for different transport pairs sharing one
//  ROUTER monitor. The test orders explicit Core commit points; it deliberately
//  does not infer or assert a wall-clock ordering between I/O threads.
void test_shared_monitor_preserves_explicit_commit_order_across_connections ()
{
    const int zero = 0;
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_a, "commit-a", 8));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_b, "commit-b", 8));
    contract_socket_pair_t pair_a (dealer_a, router, 11);
    contract_socket_pair_t pair_b (dealer_b, router, 12);

    flow_internal_send_string (dealer_a, "learn-a", 0);
    char rid_buf[256];
    TEST_ASSERT_GREATER_THAN_INT (
      0, flow_internal_recv (router, rid_buf, sizeof (rid_buf), 0));
    flow_internal_recv_string (router, "learn-a", 0);
    flow_internal_send_string (dealer_b, "learn-b", 0);
    TEST_ASSERT_GREATER_THAN_INT (
      0, flow_internal_recv (router, rid_buf, sizeof (rid_buf), 0));
    flow_internal_recv_string (router, "learn-b", 0);

    zlink_routing_id_t rid_a;
    zlink_routing_id_t rid_b;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (dealer_a, &rid_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (dealer_b, &rid_b));
    const zlink_routed_submit_target_t target_a =
      select_router_target_eventually (router, &rid_a);
    const zlink_routed_submit_target_t target_b =
      select_router_target_eventually (router, &rid_b);
    TEST_ASSERT_NOT_EQUAL (target_a.transport_pair_id,
                           target_b.transport_pair_id);

    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (
      router, ZLINK_EVENT_SEND_FLOW_PAUSED, &probe);
    TEST_ASSERT_TRUE (inject_flow_for_target (
      router, target_a, zlink::flow_state::receive_flow_paused, 50));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (inject_flow_for_target (
      router, target_b, zlink::flow_state::receive_flow_paused, 60));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (&probe, 2, 200));

    const zlink_monitor_event_t first =
      flow_unit_monitor_record_at (&probe, 0);
    const zlink_monitor_event_t second =
      flow_unit_monitor_record_at (&probe, 1);
    TEST_ASSERT_TRUE (routing_id_equal (rid_a, first.routing_id));
    TEST_ASSERT_TRUE (routing_id_equal (rid_b, second.routing_id));
    TEST_ASSERT_TRUE (first.connection_id != 0);
    TEST_ASSERT_TRUE (second.connection_id != 0);
    TEST_ASSERT_NOT_EQUAL (first.connection_id, second.connection_id);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            first.transport_lane);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            second.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (50, first.value);
    TEST_ASSERT_EQUAL_UINT64 (60, second.value);

    close_flow_unit_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer_b);
    test_context_socket_close_zero_linger (dealer_a);
    test_context_socket_close_zero_linger (router);
}

/* ------------------------------------------------------------------ */
/*  §6 metrics                                                        */
/* ------------------------------------------------------------------ */

//  M2(a). When the accepted state reaches the pair record before the pipe has
//  applied it, pair admission is what performs the PAUSED<->RUNNING flip. That
//  is the same transition the frame path performs and has to be booked the same
//  way, or the pause raises no event, moves no gauge and starts no duration -
//  and the RESUMED that follows looks unmatched.
//
//  The record is loaded without draining the socket, so the queued frame
//  command has not run yet; admission is then invoked directly through the
//  inproc validation entry, which is a real caller of the same attach path.
void test_pause_applied_by_pair_admission_is_booked ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);

    zlink::pipe_t *application = as_socket (fixture.dealer)
                                   ->test_pair_pipe (fixture.pair_id,
                                                     fixture.pair_generation,
                                                     false);
    TEST_ASSERT_NOT_NULL (application);

    //  Accepted into the record, not yet applied to the pipe.
    TEST_ASSERT_TRUE (fixture.inject_frame (
      zlink::flow_state::receive_flow_paused, 11, false));
    TEST_ASSERT_FALSE (as_socket (fixture.dealer)
                         ->application_pipe_remote_flow_paused (
                           fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_EQUAL_UINT64 (
      0, read_flow_metrics (fixture.dealer).pause_applied);

    //  Admission applies it.
    as_socket (fixture.dealer)->validate_inproc_connection (application);
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->application_pipe_remote_flow_paused (
                          fixture.pair_id, fixture.pair_generation));

    const flow_metrics_t metrics = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (1, metrics.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, metrics.pause_applied);
    TEST_ASSERT_EQUAL_UINT64 (0, metrics.resume_applied);

    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (&probe, 1, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED),
      flow_unit_monitor_event_at (&probe, 0));
    const zlink_monitor_event_t paused_event =
      flow_unit_monitor_record_at (&probe, 0);
    TEST_ASSERT_EQUAL_UINT64 (11, paused_event.value);
    TEST_ASSERT_TRUE (paused_event.routing_id.size > 0);
    TEST_ASSERT_TRUE (paused_event.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            paused_event.transport_lane);

    //  The queued frame command is a no-op now, so nothing is double counted.
    for (int i = 0; i < 50; ++i) {
        (void) contract_socket_pair_t::pump_owner (as_socket (fixture.dealer));
        msleep (1);
    }
    TEST_ASSERT_EQUAL_UINT64 (
      1, read_flow_metrics (fixture.dealer).pause_applied);
    TEST_ASSERT_EQUAL_INT (1, flow_unit_monitor_count (&probe));

    //  And the RESUMED that follows is matched, not orphaned.
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_running, 12));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    const flow_metrics_t resumed_metrics = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, resumed_metrics.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, resumed_metrics.resume_applied);

    close_flow_unit_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

//  Flow-state receipt retains the application pipe and queues the command after
//  it has left the table mutex, so a concurrent termination can settle the
//  pair's accounting first. The retain keeps the pipe alive for exactly that
//  long, so the command can still arrive - and must then do nothing at all.
//
//  A second pair is held genuinely paused throughout, so a late release would
//  have to take its count and be visible.
void test_late_flow_state_from_a_terminated_pair_changes_nothing ()
{
    const int zero = 0;

    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t pair_a (dealer, router_a, 11);
    contract_socket_pair_t pair_b (dealer, router_b, 12);

    char rid[256];
    bool seen_a = false;
    bool seen_b = false;
    const std::chrono::steady_clock::time_point learn_deadline =
      deadline_in_ms (6000);
    while ((!seen_a || !seen_b) && !deadline_expired (learn_deadline)) {
        (void) flow_internal_send (dealer, "hello", 5, ZLINK_DONTWAIT);
        if (flow_internal_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) flow_internal_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_a = true;
        }
        if (flow_internal_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) flow_internal_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_b = true;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (seen_a);
    TEST_ASSERT_TRUE (seen_b);

    //  Pair A: a real, accounted pause that must survive everything below.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router_a, ZLINK_RECEIVE_FLOW_PAUSED));
    bool paused = false;
    const std::chrono::steady_clock::time_point pause_deadline =
      deadline_in_ms (6000);
    while (!deadline_expired (pause_deadline)) {
        (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
        (void) contract_socket_pair_t::pump_owner (as_socket (router_a));
        if (read_flow_metrics (dealer).paused_connections == 1) {
            paused = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused);

    //  Find pair B and hold its application pipe, as receipt would.
    zlink_routed_submit_target_t target_b;
    memset (&target_b, 0, sizeof (target_b));
    zlink::pipe_t *held_b = NULL;
    for (int attempt = 0; attempt < 400 && !held_b; ++attempt) {
        if (as_socket (dealer)->select_routed_submit_target (NULL, &target_b) == 0
            && target_b.transport_pair_id != 0
            && !as_socket (dealer)->application_pipe_remote_flow_paused (
                 target_b.transport_pair_id, target_b.transport_pair_generation))
            held_b = as_socket (dealer)->test_retain_application_pipe (
              target_b.transport_pair_id, target_b.transport_pair_generation);
        if (!held_b)
            msleep (1);
    }
    TEST_ASSERT_NOT_NULL (held_b);

    //  Terminate pair B and let its accounting settle.
    pair_b.application[0]->terminate (false);
    pair_b.pump ();
    bool gone = false;
    const std::chrono::steady_clock::time_point gone_deadline =
      deadline_in_ms (6000);
    while (!deadline_expired (gone_deadline)) {
        (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
        (void) contract_socket_pair_t::pump_owner (as_socket (router_b));
        if (as_socket (dealer)->test_pair_pipe (target_b.transport_pair_id,
                                                target_b.transport_pair_generation,
                                                false)
            == NULL) {
            gone = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (gone);

    const flow_metrics_t before = read_flow_metrics (dealer);
    TEST_ASSERT_EQUAL_UINT64 (1, before.paused_connections);

    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (dealer, k_flow_events, &probe);

    //  A late RUNNING must not release a count this pair no longer holds.
    as_socket (dealer)->test_deliver_late_flow_state (held_b, false, 41);
    //  A late PAUSE must not add one that nothing will ever release.
    as_socket (dealer)->test_deliver_late_flow_state (held_b, true, 42);

    const flow_metrics_t after = read_flow_metrics (dealer);
    TEST_ASSERT_EQUAL_UINT64 (before.paused_connections,
                              after.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (before.pause_applied, after.pause_applied);
    TEST_ASSERT_EQUAL_UINT64 (before.resume_applied, after.resume_applied);
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (&probe, 0, 200));

    as_socket (dealer)->test_release_pipe (held_b);
    close_flow_unit_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router_b);
    test_context_socket_close_zero_linger (router_a);
}

//  The same guard, for a pair whose record is still there but whose reporting
//  pipe is not the registered application pipe.
void test_late_flow_state_from_a_foreign_pipe_changes_nothing ()
{
    const int zero = 0;
    const char first_rid_text[] = "flow-c-api-router-a";
    const char second_rid_text[] = "flow-c-api-router-b";
    void *first = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *second = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (first, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (second, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (
      first, first_rid_text, sizeof (first_rid_text) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (
      second, second_rid_text, sizeof (second_rid_text) - 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (second, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               first_rid_text, sizeof (first_rid_text) - 1));
    contract_socket_pair_t local_pair (first, second);

    flow_internal_send_string (second, first_rid_text, ZLINK_SNDMORE);
    flow_internal_send_string (second, "prime", 0);
    flow_internal_recv_string (first, second_rid_text, 0);
    flow_internal_recv_string (first, "prime", 0);

    zlink_routing_id_t first_rid;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (first, &first_rid));
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (second, &first_rid);
    zlink::pipe_t *const application =
      as_socket (second)->test_pair_pipe (target.transport_pair_id,
                                          target.transport_pair_generation,
                                          false);
    zlink::pipe_t *const completion =
      as_socket (second)->test_pair_pipe (target.transport_pair_id,
                                          target.transport_pair_generation,
                                          true);
    TEST_ASSERT_NOT_NULL (application);
    TEST_ASSERT_NOT_NULL (completion);
    TEST_ASSERT_EQUAL_UINT (2u, application->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_UINT (2u, completion->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_completion,
                           completion->get_transport_lane ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (first, ZLINK_RECEIVE_FLOW_PAUSED));
    bool paused = false;
    const std::chrono::steady_clock::time_point pause_deadline =
      deadline_in_ms (4000);
    while (!deadline_expired (pause_deadline)) {
        (void) contract_socket_pair_t::pump_owner (as_socket (first));
        (void) contract_socket_pair_t::pump_owner (as_socket (second));
        if (as_socket (second)->application_pipe_remote_flow_paused (
              target.transport_pair_id, target.transport_pair_generation)) {
            paused = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused);

    const flow_metrics_t before = read_flow_metrics (second);
    TEST_ASSERT_EQUAL_UINT64 (1, before.paused_connections);

    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (second, k_flow_events, &probe);

    //  The count-2 pair is alive and paused, but the Completion source is not
    //  the registered Application pipe whose blocker/accounting callback this
    //  test hook models.
    as_socket (second)->test_deliver_late_flow_state (completion, false, 51);
    as_socket (second)->test_deliver_late_flow_state (completion, true, 52);

    const flow_metrics_t after = read_flow_metrics (second);
    TEST_ASSERT_EQUAL_UINT64 (before.paused_connections,
                              after.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (before.pause_applied, after.pause_applied);
    TEST_ASSERT_EQUAL_UINT64 (before.resume_applied, after.resume_applied);
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (&probe, 0, 200));

    close_flow_unit_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (first);
}

//  The gauge is socket-wide, so releasing it on termination has to be decided
//  by whether this pair actually holds a counted pause - not by the last state
//  it received. A frame is recorded when it is accepted and only becomes
//  accounted when the pipe flips, and a pair can terminate inside that gap.

//  (a) Terminating a pair whose pause was received but never applied must not
//  touch the gauge. With a second pair genuinely paused, releasing from the
//  received state would take that pair's count instead.
void test_terminating_a_received_but_unapplied_pause_leaves_the_gauge_alone ()
{
    const int zero = 0;

    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t pair_a (dealer, router_a, 11);
    contract_socket_pair_t pair_b (dealer, router_b, 12);

    //  Learn both routes. The DEALER load-balances, so keep sending until each
    //  ROUTER has seen one rather than assuming a fixed number of sends lands
    //  on both.
    char rid[256];
    bool seen_a = false;
    bool seen_b = false;
    const std::chrono::steady_clock::time_point learn_deadline =
      deadline_in_ms (6000);
    while ((!seen_a || !seen_b) && !deadline_expired (learn_deadline)) {
        (void) flow_internal_send (dealer, "hello", 5, ZLINK_DONTWAIT);
        if (flow_internal_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) flow_internal_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_a = true;
        }
        if (flow_internal_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) flow_internal_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_b = true;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (seen_a);
    TEST_ASSERT_TRUE (seen_b);

    //  Pair A is genuinely paused: applied, accounted, on the gauge.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router_a, ZLINK_RECEIVE_FLOW_PAUSED));
    bool paused = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (6000);
    while (!deadline_expired (deadline)) {
        (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
        (void) contract_socket_pair_t::pump_owner (as_socket (router_a));
        if (read_flow_metrics (dealer).paused_connections == 1) {
            paused = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused);

    //  Pair B only received a pause: nothing applied, nothing accounted.
    zlink_routed_submit_target_t target_b;
    memset (&target_b, 0, sizeof (target_b));
    bool found_b = false;
    for (int attempt = 0; attempt < 200 && !found_b; ++attempt) {
        if (as_socket (dealer)->select_routed_submit_target (NULL, &target_b) == 0
            && target_b.transport_pair_id != 0
            && as_socket (dealer)->test_set_pair_received_flow_state (
                 target_b.transport_pair_id, target_b.transport_pair_generation,
                 true)
            && !as_socket (dealer)->application_pipe_remote_flow_paused (
                 target_b.transport_pair_id, target_b.transport_pair_generation))
            found_b = true;
        else
            msleep (1);
    }
    TEST_ASSERT_TRUE (found_b);
    TEST_ASSERT_EQUAL_UINT64 (1, read_flow_metrics (dealer).paused_connections);

    //  Tearing that pair down must not consume pair A's count.
    pair_b.application[0]->terminate (false);
    pair_b.pump ();
    for (int i = 0; i < 200; ++i) {
        (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
        (void) contract_socket_pair_t::pump_owner (as_socket (router_b));
        msleep (1);
    }
    TEST_ASSERT_EQUAL_UINT64 (1, read_flow_metrics (dealer).paused_connections);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router_b);
    test_context_socket_close_zero_linger (router_a);
}

//  (b) The mirror case: an accounted pause whose RUNNING was received but not
//  applied must still be released on termination, and the pause duration it
//  opened must be closed.
void test_terminating_an_accounted_pause_releases_it_and_closes_the_duration ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_EQUAL_UINT64 (
      1, read_flow_metrics (fixture.dealer).paused_connections);

    //  Let the pause last long enough for a closed measurement to be visible.
    msleep (25);

    //  A RUNNING arrives and is recorded, but never reaches the pipe: the
    //  accounted pause is still outstanding.
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)->test_set_pair_received_flow_state (
      fixture.pair_id, fixture.pair_generation, false));
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->application_pipe_remote_flow_paused (
                          fixture.pair_id, fixture.pair_generation));

    fixture.pair->application[0]->terminate (false);
    fixture.pair->pump ();
    bool released = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (6000);
    while (!deadline_expired (deadline)) {
        (void) contract_socket_pair_t::pump_owner (as_socket (fixture.dealer));
        (void) contract_socket_pair_t::pump_owner (as_socket (fixture.router));
        if (read_flow_metrics (fixture.dealer).paused_connections == 0) {
            released = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (released);

    const flow_metrics_t metrics = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, metrics.paused_connections);
    //  Releasing on termination closes the measurement the pause opened.
    TEST_ASSERT_TRUE (metrics.last_pause_duration_ms > 0);
    //  It is a lifecycle release, not a resume.
    TEST_ASSERT_EQUAL_UINT64 (0, metrics.resume_applied);

    fixture.teardown ();
}

//  M2. Two bookkeeping holes on the same lifecycle, covered together.
//
//  (b) A pair torn down while paused never reaches a RESUMED, so nothing else
//  would match its +1 on flow_paused_connections; repeating the cycle drifted
//  the gauge upwards for good.
//
//  (a) A PAUSE that pair admission applies - because the accepted state was
//  recorded before the application lane was attached - is the same
//  PAUSED<->RUNNING flip as one applied by the frame path, and has to be booked
//  the same way. Which of the two paths applies a given cycle depends on the
//  order between count-1 Application admission and its queued control, so the
//  invariant is unchanged: every pause raises exactly one PAUSED event and
//  moves the total exactly once.

void test_flow_state_metrics_snapshot_and_reset ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    zlink_monitor_status_t status = read_monitor_status (fixture.dealer);
    TEST_ASSERT_TRUE ((status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE) != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_state_stale_total);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    status = read_monitor_status (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));

    status = read_monitor_status (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_resume_applied_total);

    //  A duplicate frame bumps the stale counter without touching the
    //  pause/resume totals or the gauge.
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_running, 1));
    status = read_monitor_status (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_state_stale_total);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_resume_applied_total);

    //  Reset is internal-only until the monitoring spec fixes a public
    //  reset surface (plan §6); the C++ layer already owns the contract.
    as_socket (fixture.dealer)->reset_flow_state_metrics ();
    status = read_monitor_status (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_state_stale_total);

    fixture.teardown ();
}

// The wire duplicate/regression and reconnect observations remain in the
// single-lane integration test; only a retained retired pipe can drive this
// private late-callback boundary.
void test_retired_flow_frame_counts_stale_without_touching_replacement ()
{
    paired_fixture_t fixture;
    fixture.setup ();
    flow_unit_monitor_probe_t probe;
    void *monitor = open_flow_unit_monitor_probe (fixture.dealer, k_flow_events, &probe);
    zlink::pipe_t *const retired_application =
      as_socket (fixture.dealer)->test_retain_application_pipe (
        fixture.pair_id, fixture.pair_generation);
    TEST_ASSERT_NOT_NULL (retired_application);
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_paused, 10));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&probe, 1, 2000));

    const uint64_t old_pair_id = fixture.pair_id;
    const uint64_t old_generation = fixture.pair_generation;
    fixture.replace_pair ();
    TEST_ASSERT_NULL (as_socket (fixture.dealer)->test_pair_pipe (
      old_pair_id, old_generation, false));
    TEST_ASSERT_TRUE (fixture.pair_id != old_pair_id
                      || fixture.pair_generation != old_generation);
    zlink_monitor_status_t before_retired;
    memset (&before_retired, 0, sizeof (before_retired));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor, &before_retired));
    TEST_ASSERT_EQUAL_UINT64 (0, before_retired.flow_paused_connections);
    const int before_retired_events = flow_unit_monitor_count (&probe);
    as_socket (fixture.dealer)->test_consume_late_flow_state_frame (
      retired_application, true, 100);
    fixture.pair->pump ();
    zlink_monitor_status_t after_retired;
    memset (&after_retired, 0, sizeof (after_retired));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor, &after_retired));
    TEST_ASSERT_EQUAL_UINT64 (before_retired.flow_state_stale_total + 1,
                              after_retired.flow_state_stale_total);
    TEST_ASSERT_TRUE (flow_unit_monitor_has_no_additional (
      &probe, before_retired_events, 100));
    TEST_ASSERT_EQUAL_UINT64 (0, after_retired.flow_paused_connections);

    flow_internal_send_string (fixture.dealer, "new-generation-data", 0);
    char rid[256];
    TEST_ASSERT_GREATER_THAN_INT (0, flow_internal_recv (
      fixture.router, rid, sizeof (rid), 0));
    flow_internal_recv_string (fixture.router, "new-generation-data", 0);
    as_socket (fixture.dealer)->test_release_pipe (retired_application);
    close_flow_unit_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

}

int main ()
{
    setup_test_environment ();
    const char *selected_case = getenv ("ZLINK_TEST_CASE");
    if (selected_case && strcmp (selected_case, "same-state") == 0)
        selected_case = "test_same_state_forward_epoch_and_repeated_local_set_emit_no_event";
    if (selected_case && strcmp (selected_case, "resumed-stale") == 0)
        selected_case = "test_resumed_routing_id_and_epoch_stale_match_prior_transition";
    if (selected_case && strcmp (selected_case, "hwm-resumed") == 0)
        selected_case = "test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected";
    if (selected_case && strcmp (selected_case, "mask") == 0)
        selected_case = "test_flow_event_numeric_values_and_each_excluded_mask";
    if (selected_case && strcmp (selected_case, "ordering") == 0)
        selected_case = "test_shared_monitor_preserves_explicit_commit_order_across_connections";

    UNITY_BEGIN ();
    if (!selected_case || strcmp (selected_case, "test_duplicate_frame_emits_stale_event") == 0)
        RUN_TEST (test_duplicate_frame_emits_stale_event);
    if (!selected_case || strcmp (selected_case, "test_same_state_forward_epoch_and_repeated_local_set_emit_no_event") == 0)
        RUN_TEST (test_same_state_forward_epoch_and_repeated_local_set_emit_no_event);
    if (!selected_case || strcmp (selected_case, "test_resumed_routing_id_and_epoch_stale_match_prior_transition") == 0)
        RUN_TEST (test_resumed_routing_id_and_epoch_stale_match_prior_transition);
    if (!selected_case || strcmp (selected_case, "test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected") == 0)
        RUN_TEST (test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected);
    if (!selected_case || strcmp (selected_case, "test_flow_event_numeric_values_and_each_excluded_mask") == 0)
        RUN_TEST (test_flow_event_numeric_values_and_each_excluded_mask);
    if (!selected_case || strcmp (selected_case, "test_shared_monitor_preserves_explicit_commit_order_across_connections") == 0)
        RUN_TEST (test_shared_monitor_preserves_explicit_commit_order_across_connections);
    if (!selected_case || strcmp (selected_case, "test_pause_applied_by_pair_admission_is_booked") == 0)
        RUN_TEST (test_pause_applied_by_pair_admission_is_booked);
    if (!selected_case || strcmp (selected_case, "test_late_flow_state_from_a_terminated_pair_changes_nothing") == 0)
        RUN_TEST (test_late_flow_state_from_a_terminated_pair_changes_nothing);
    if (!selected_case || strcmp (selected_case, "test_late_flow_state_from_a_foreign_pipe_changes_nothing") == 0)
        RUN_TEST (test_late_flow_state_from_a_foreign_pipe_changes_nothing);
    if (!selected_case || strcmp (selected_case, "test_terminating_a_received_but_unapplied_pause_leaves_the_gauge_alone") == 0)
        RUN_TEST (test_terminating_a_received_but_unapplied_pause_leaves_the_gauge_alone);
    if (!selected_case || strcmp (selected_case, "test_terminating_an_accounted_pause_releases_it_and_closes_the_duration") == 0)
        RUN_TEST (test_terminating_an_accounted_pause_releases_it_and_closes_the_duration);
    if (!selected_case || strcmp (selected_case, "test_flow_state_metrics_snapshot_and_reset") == 0)
        RUN_TEST (test_flow_state_metrics_snapshot_and_reset);
    if (!selected_case || strcmp (selected_case, "test_retired_flow_frame_counts_stale_without_touching_replacement") == 0)
        RUN_TEST (test_retired_flow_frame_counts_stale_without_touching_replacement);
    return UNITY_END ();
}
