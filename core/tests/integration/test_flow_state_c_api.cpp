/* SPDX-License-Identifier: MPL-2.0 */

//  Stage 7 (core-byte-hwm-flow-control-plan.ko.md §5/§6/§7, checklist §12.3
//  row 6): the public C API result mapping, the flow observation events and
//  the flow observation metrics. Stage 3 already owns the internal frame,
//  socket-wide state and send-blocker contract tests
//  (test_flow_state_paired.cpp); this file only exercises the public surface
//  layered on top of it.

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
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
    return static_cast<zlink::socket_base_t *> (socket_);
}

bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
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

//  One connected DEALER (sender) and ROUTER (receiver) over TCP, with the
//  route already learned by the ROUTER and both transport-pair lanes ready.
//  Mirrors test_flow_state_paired.cpp's fixture; kept local to this file so
//  each integration test binary stays self-contained.
struct paired_fixture_t
{
    paired_fixture_t () : dealer (NULL), router (NULL)
    {
        memset (endpoint, 0, sizeof (endpoint));
    }

    void setup ()
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

        //  One round trip so the ROUTER learns the route and both pairs are
        //  ready on both ends.
        send_string_expect_success (dealer, "hello", 0);
        char rid[256];
        const int rid_size = zlink_recv (router, rid, sizeof (rid), 0);
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        recv_string_expect_success (router, "hello", 0);

        TEST_ASSERT_TRUE (resolve_dealer_target ());
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
        if (dealer)
            dealer = test_context_socket_close_zero_linger (dealer);
        if (router)
            router = test_context_socket_close_zero_linger (router);
    }

    //  Delivers one hand-built frame to the DEALER exactly as the completion
    //  lane would, then drains the socket mailbox so the pipe applies it.
    bool inject (uint8_t state_, uint64_t epoch_)
    {
        return inject_generation (state_, epoch_, pair_generation);
    }

    bool inject_generation (uint8_t state_, uint64_t epoch_,
                            uint64_t frame_generation_)
    {
        zlink::pipe_t *completion = as_socket (dealer)->completion_pipe_for_transport_pair (
          pair_id, pair_generation);
        TEST_ASSERT_NOT_NULL (completion);

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = state_;
        frame.pair_id = pair_id;
        frame.generation = frame_generation_;
        frame.epoch = epoch_;

        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        const bool consumed =
          as_socket (dealer)->consume_receive_flow_state_frame (completion, msg);
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        (void) as_socket (dealer)->process_submit_commands ();
        return consumed;
    }

    bool wait_for_applied_pause (bool expected_)
    {
        const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            (void) as_socket (dealer)->process_submit_commands ();
            if (as_socket (dealer)->application_pipe_remote_flow_paused (pair_id, pair_generation)
                == expected_)
                return true;
            msleep (1);
        }
        return false;
    }

    void *dealer;
    void *router;
    char endpoint[MAX_SOCKET_STRING];
    uint64_t pair_id;
    uint64_t pair_generation;
};

const uint32_t k_flow_events = ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED
                               | ZLINK_EVENT_FLOW_STATE_STALE;

/* ------------------------------------------------------------------ */
/*  §5 result-mapping matrix                                          */
/* ------------------------------------------------------------------ */

void test_valid_state_on_dealer_and_router_succeeds_and_is_idempotent ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_PAUSED));
    //  Repeating the current state is a successful no-op.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_RUNNING));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router, ZLINK_RECEIVE_FLOW_PAUSED));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_null_or_invalid_handle_is_invalid_handle ()
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_HANDLE,
      zlink_socket_set_receive_flow_state (NULL, ZLINK_RECEIVE_FLOW_PAUSED));
}

void test_out_of_range_state_is_invalid_argument ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_socket_set_receive_flow_state (
        dealer, static_cast<zlink_receive_flow_state_t> (2)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_socket_set_receive_flow_state (
        dealer, static_cast<zlink_receive_flow_state_t> (-1)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_socket_set_receive_flow_state (
        router, static_cast<zlink_receive_flow_state_t> (999)));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  PAIR, the PUB/SUB family and STREAM have no completion lane.
void test_unsupported_socket_types_report_not_supported ()
{
    const int types[] = {ZLINK_SOCKET_PAIR, ZLINK_SOCKET_PUB, ZLINK_SOCKET_SUB,
                         ZLINK_SOCKET_STREAM};
    for (size_t i = 0; i < sizeof (types) / sizeof (types[0]); ++i) {
        void *socket = test_context_socket (types[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_NOT_SUPPORTED,
          zlink_socket_set_receive_flow_state (socket, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_NOT_SUPPORTED,
          zlink_socket_set_receive_flow_state (socket, ZLINK_RECEIVE_FLOW_RUNNING));
        test_context_socket_close_zero_linger (socket);
    }
}

//  Close accepted before the config call is admitted: the plan's "close
//  raced and won" row. A freshly created, never-connected DEALER has nothing
//  to linger on, so zlink_close() runs its admission check
//  (socket_lifecycle_coordinator_t::begin_close_or_fail_busy(), the same
//  gate socket_public_api_scope_t consults) and its full teardown inside one
//  synchronous call - so by the time it returns, the handle a losing caller
//  would have raced against is already gone rather than merely closing.
//  set_local_receive_flow_state()'s own admission
//  (socket_public_api_scope_t::acquired()) reports that same "already
//  closing" outcome as errno=ESHUTDOWN, which config_result_internal maps to
//  ZLINK_CONFIG_INVALID_STATE (see config_result_internal.hpp); a call that
//  instead lands after teardown has finished sees an unknown handle and
//  reports ZLINK_CONFIG_INVALID_HANDLE. Both are the plan's contract: close
//  and the config call race for the same socket state, and only whichever
//  is admitted first is observable - the config call is never left
//  half-applied and the process never corrupts state.
void test_close_admitted_first_reports_invalid_state_or_handle ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    test_context_socket_mark_closed (dealer);

    const zlink_config_result_t result =
      zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_PAUSED);
    TEST_ASSERT_TRUE (result == ZLINK_CONFIG_INVALID_STATE
                      || result == ZLINK_CONFIG_INVALID_HANDLE);
}

//  A genuine concurrent close and config call never produce anything other
//  than a successful store or one of close's two "already gone" outcomes,
//  and never crash, whichever side the scheduler admits first.
void test_close_races_with_set_receive_flow_state ()
{
    const int attempts = 20;
    for (int i = 0; i < attempts; ++i) {
        void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        std::atomic<int> go (0);
        zlink_config_result_t result = static_cast<zlink_config_result_t> (-1);
        //  Both threads spin-wait on the same release so scheduling skew from
        //  thread creation cannot decide the race by itself.
        std::thread closer ([dealer, &go] {
            while (go.load (std::memory_order_acquire) == 0)
                std::this_thread::yield ();
            zlink_close (dealer);
        });
        std::thread setter ([dealer, &go, &result] {
            while (go.load (std::memory_order_acquire) == 0)
                std::this_thread::yield ();
            result =
              zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_PAUSED);
        });
        go.store (1, std::memory_order_release);
        closer.join ();
        setter.join ();
        test_context_socket_mark_closed (dealer);

        TEST_ASSERT_TRUE (result == ZLINK_CONFIG_OK
                          || result == ZLINK_CONFIG_INVALID_STATE
                          || result == ZLINK_CONFIG_INVALID_HANDLE);
    }
}

/* ------------------------------------------------------------------ */
/*  §6 event emission                                                 */
/* ------------------------------------------------------------------ */

//  The receiver's socket-wide PAUSED reaches the sender's application pipe
//  exactly once, and RESUMED reaches it exactly once, with no other flow
//  event emitted for the same transition.
void test_pause_and_resume_each_emit_exactly_one_event ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    //  No extra event trails the transition.
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 1, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED), test_monitor_probe_event_at (&probe, 0));

    //  §6 field list for PAUSED: routing ID, pair ID, generation, epoch.
    const zlink_monitor_event_t paused = test_monitor_probe_record_at (&probe, 0);
    TEST_ASSERT_TRUE (paused.value != 0);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, paused.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              paused.transport_pair_generation);
    TEST_ASSERT_TRUE (paused.routing_id.size > 0);
    //  A pause is never writable, so the RESUMED-only flag must be clear.
    TEST_ASSERT_EQUAL_UINT32 (
      0u, paused.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE);
    const uint64_t paused_epoch = paused.value;

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 2, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_RESUMED), test_monitor_probe_event_at (&probe, 1));

    //  §6 adds "actually writable" to RESUMED. Nothing else blocks this pipe,
    //  so the resume really did make it writable and the flag must say so.
    const zlink_monitor_event_t resumed = test_monitor_probe_record_at (&probe, 1);
    TEST_ASSERT_TRUE (resumed.value > paused_epoch);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, resumed.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              resumed.transport_pair_generation);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE,
      resumed.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE);

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

//  A frame whose epoch does not advance (a duplicate, here delivered twice
//  with the same epoch) is ignored and reported as stale exactly once for
//  the repeat - never as a second PAUSED.
void test_duplicate_frame_emits_stale_event ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_paused, 5));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));

    //  Same epoch again: stale, not a second PAUSED.
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_paused, 5));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 2, 200));

    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED), test_monitor_probe_event_at (&probe, 0));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_FLOW_STATE_STALE), test_monitor_probe_event_at (&probe, 1));

    //  §6 field list for STALE: pair ID plus the generation/epoch context. The
    //  repeat carried the current generation, so the reason is the epoch and
    //  `value` is the received epoch that did not advance.
    const zlink_monitor_event_t stale = test_monitor_probe_record_at (&probe, 1);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, stale.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              stale.transport_pair_generation);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
      stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
    TEST_ASSERT_EQUAL_UINT32 (
      0u, stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION);
    TEST_ASSERT_EQUAL_UINT64 (5, stale.value);

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

//  A frame from a previous connection generation is stale for a different
//  reason, and the event has to say which: the reason flag flips and `value`
//  becomes the received generation, while the event's own
//  transport_pair_generation stays the current one.
void test_stale_generation_event_reports_the_received_generation ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    const uint64_t foreign_generation = fixture.pair_generation + 7;
    TEST_ASSERT_TRUE (fixture.inject_generation (
      zlink::flow_state::receive_flow_paused, 9, foreign_generation));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 1, 200));

    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_FLOW_STATE_STALE),
      test_monitor_probe_event_at (&probe, 0));
    const zlink_monitor_event_t stale = test_monitor_probe_record_at (&probe, 0);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION,
      stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION);
    TEST_ASSERT_EQUAL_UINT32 (
      0u, stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
    TEST_ASSERT_EQUAL_UINT64 (foreign_generation, stale.value);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, stale.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              stale.transport_pair_generation);

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

//  Ordinary data traffic while RUNNING never produces a flow event.
void test_data_traffic_emits_no_flow_events ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    for (int i = 0; i < 200; ++i) {
        send_string_expect_success (fixture.dealer, "payload", 0);
        char rid[256];
        const int rid_size = zlink_recv (fixture.router, rid, sizeof (rid), 0);
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        recv_string_expect_success (fixture.router, "payload", 0);
    }
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 0, 200));
    TEST_ASSERT_EQUAL_INT (0, test_monitor_probe_count (&probe));

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

/* ------------------------------------------------------------------ */
/*  §6 metrics                                                        */
/* ------------------------------------------------------------------ */

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
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_valid_state_on_dealer_and_router_succeeds_and_is_idempotent);
    RUN_TEST (test_null_or_invalid_handle_is_invalid_handle);
    RUN_TEST (test_out_of_range_state_is_invalid_argument);
    RUN_TEST (test_unsupported_socket_types_report_not_supported);
    RUN_TEST (test_close_admitted_first_reports_invalid_state_or_handle);
    RUN_TEST (test_close_races_with_set_receive_flow_state);
    RUN_TEST (test_pause_and_resume_each_emit_exactly_one_event);
    RUN_TEST (test_duplicate_frame_emits_stale_event);
    RUN_TEST (test_stale_generation_event_reports_the_received_generation);
    RUN_TEST (test_data_traffic_emits_no_flow_events);
    RUN_TEST (test_flow_state_metrics_snapshot_and_reset);
    return UNITY_END ();
}
