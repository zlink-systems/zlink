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

    void setup_inproc (const char *endpoint_, uint64_t hwm_ = 0)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        if (hwm_ != 0) {
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
        }
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint_));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_));

        send_string_expect_success (dealer, "hello", 0);
        char rid[256];
        TEST_ASSERT_GREATER_THAN_INT (
          0, zlink_recv (router, rid, sizeof (rid), 0));
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
                            uint64_t frame_generation_, bool drain_ = true)
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
        if (drain_)
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

zlink_submit_result_t try_send_flow_filler (void *socket_, size_t size_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size_));
    memset (zlink_msg_data (&part), 'h', size_);
    return zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                            ZLINK_PART_FINAL);
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
    zlink::pipe_t *completion =
      as_socket (socket_)->completion_pipe_for_transport_pair (
        target_.transport_pair_id, target_.transport_pair_generation);
    TEST_ASSERT_NOT_NULL (completion);

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = state_;
    frame.pair_id = target_.transport_pair_id;
    frame.generation = target_.transport_pair_generation;
    frame.epoch = epoch_;
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
    const bool consumed =
      as_socket (socket_)->consume_receive_flow_state_frame (completion, msg);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
    (void) as_socket (socket_)->process_submit_commands ();
    return consumed;
}

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
        zlink_close_result_t close_result = ZLINK_CLOSE_INTERNAL_ERROR;
        int close_errno = 0;
        //  Both threads spin-wait on the same release so scheduling skew from
        //  thread creation cannot decide the race by itself.
        std::thread closer ([dealer, &go, &close_result, &close_errno] {
            while (go.load (std::memory_order_acquire) == 0)
                std::this_thread::yield ();
            close_result = zlink_close (dealer);
            close_errno = errno;
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

        TEST_ASSERT_TRUE (close_result == ZLINK_CLOSE_OK
                          || close_result == ZLINK_CLOSE_BUSY);
        if (close_result == ZLINK_CLOSE_BUSY) {
            TEST_ASSERT_EQUAL_INT (EBUSY, close_errno);
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        }
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

//  A newer epoch that repeats the already applied state updates only the
//  pair's version record. Repeating the local public state setter is also a
//  no-op. Both are observed through the public monitor handler.
void test_same_state_forward_epoch_and_repeated_local_set_emit_no_event ()
{
    {
        paired_fixture_t fixture;
        fixture.setup_inproc ("inproc://gap-h3-flow-same-epoch");
        test_monitor_probe_t probe;
        void *monitor =
          open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 10));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));

        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 11));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 1, 200));
        TEST_ASSERT_EQUAL_INT (1, test_monitor_probe_count (&probe));

        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup_inproc ("inproc://gap-h3-flow-local-repeat");
        test_monitor_probe_t probe;
        void *monitor =
          open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (
            fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 1, 200));

        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }
}

void test_resumed_routing_id_and_epoch_stale_match_prior_transition ()
{
    paired_fixture_t fixture;
    fixture.setup_inproc ("inproc://gap-h3-flow-resume-stale");
    test_monitor_probe_t probe;
    void *monitor =
      open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_paused, 20));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    const zlink_monitor_event_t paused =
      test_monitor_probe_record_at (&probe, 0);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 21));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));
    const zlink_monitor_event_t resumed =
      test_monitor_probe_record_at (&probe, 1);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_SEND_FLOW_RESUMED, resumed.event);
    TEST_ASSERT_TRUE (resumed.routing_id.size > 0);
    TEST_ASSERT_TRUE (routing_id_equal (paused.routing_id, resumed.routing_id));
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, resumed.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              resumed.transport_pair_generation);

    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 21));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 3, 2000));
    const zlink_monitor_event_t stale =
      test_monitor_probe_record_at (&probe, 2);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_FLOW_STATE_STALE, stale.event);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
      stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
    TEST_ASSERT_EQUAL_UINT32 (
      0u, stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION);
    TEST_ASSERT_EQUAL_UINT64 (resumed.value, stale.value);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, stale.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_generation,
                              stale.transport_pair_generation);

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

void test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected ()
{
    paired_fixture_t fixture;
    fixture.setup_inproc ("inproc://gap-h3-flow-hwm-resume", 4096);
    const size_t filler_count =
      fill_flow_pipe_until_backpressured (fixture.dealer);
    TEST_ASSERT_TRUE (filler_count > 0);

    test_monitor_probe_t probe;
    void *monitor =
      open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);
    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_paused, 30));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (
      fixture.inject (zlink::flow_state::receive_flow_running, 31));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));

    const zlink_monitor_event_t resumed =
      test_monitor_probe_record_at (&probe, 1);
    const zlink_submit_result_t next_send =
      try_send_flow_filler (fixture.dealer, 1024);
    const int next_send_errno = zlink_errno ();
    const uint32_t writable_flag =
      resumed.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE;

    close_test_monitor_probe (&monitor, &probe);
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
        fixture.setup_inproc ("inproc://gap-h3-flow-mask-pause");
        test_monitor_probe_t probe;
        void *monitor = open_test_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_RESUMED | ZLINK_EVENT_FLOW_STATE_STALE,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 40));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 0, 200));
        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup_inproc ("inproc://gap-h3-flow-mask-resume");
        test_monitor_probe_t probe;
        void *monitor = open_test_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_FLOW_STATE_STALE,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 41));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_running, 42));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 1, 200));
        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

    {
        paired_fixture_t fixture;
        fixture.setup_inproc ("inproc://gap-h3-flow-mask-stale");
        test_monitor_probe_t probe;
        void *monitor = open_test_monitor_probe (
          fixture.dealer,
          ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED,
          &probe);
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 43));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
        TEST_ASSERT_TRUE (
          fixture.inject (zlink::flow_state::receive_flow_paused, 43));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 1, 200));
        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }
}

//  The two events are produced for different transport pairs sharing one
//  ROUTER monitor. The test orders explicit Core commit points; it deliberately
//  does not infer or assert a wall-clock ordering between I/O threads.
void test_shared_monitor_preserves_explicit_commit_order_across_connections ()
{
    const int zero = 0;
    const char *endpoint = "inproc://gap-h3-flow-commit-order";
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
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_a, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_b, endpoint));

    send_string_expect_success (dealer_a, "learn-a", 0);
    char rid_buf[256];
    TEST_ASSERT_GREATER_THAN_INT (
      0, zlink_recv (router, rid_buf, sizeof (rid_buf), 0));
    recv_string_expect_success (router, "learn-a", 0);
    send_string_expect_success (dealer_b, "learn-b", 0);
    TEST_ASSERT_GREATER_THAN_INT (
      0, zlink_recv (router, rid_buf, sizeof (rid_buf), 0));
    recv_string_expect_success (router, "learn-b", 0);

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

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_SEND_FLOW_PAUSED, &probe);
    TEST_ASSERT_TRUE (inject_flow_for_target (
      router, target_a, zlink::flow_state::receive_flow_paused, 50));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (inject_flow_for_target (
      router, target_b, zlink::flow_state::receive_flow_paused, 60));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 2, 200));

    const zlink_monitor_event_t first =
      test_monitor_probe_record_at (&probe, 0);
    const zlink_monitor_event_t second =
      test_monitor_probe_record_at (&probe, 1);
    TEST_ASSERT_EQUAL_UINT64 (target_a.transport_pair_id,
                              first.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (target_b.transport_pair_id,
                              second.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (50, first.value);
    TEST_ASSERT_EQUAL_UINT64 (60, second.value);

    close_test_monitor_probe (&monitor, &probe);
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

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    zlink::pipe_t *application = as_socket (fixture.dealer)
                                   ->test_pair_pipe (fixture.pair_id,
                                                     fixture.pair_generation,
                                                     false);
    TEST_ASSERT_NOT_NULL (application);

    //  Accepted into the record, not yet applied to the pipe.
    TEST_ASSERT_TRUE (fixture.inject_generation (
      zlink::flow_state::receive_flow_paused, 11, fixture.pair_generation,
      false));
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

    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 1, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED),
      test_monitor_probe_event_at (&probe, 0));
    const zlink_monitor_event_t paused_event =
      test_monitor_probe_record_at (&probe, 0);
    TEST_ASSERT_EQUAL_UINT64 (11, paused_event.value);
    TEST_ASSERT_EQUAL_UINT64 (fixture.pair_id, paused_event.transport_pair_id);

    //  The queued frame command is a no-op now, so nothing is double counted.
    for (int i = 0; i < 50; ++i) {
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_EQUAL_UINT64 (
      1, read_flow_metrics (fixture.dealer).pause_applied);
    TEST_ASSERT_EQUAL_INT (1, test_monitor_probe_count (&probe));

    //  And the RESUMED that follows is matched, not orphaned.
    TEST_ASSERT_TRUE (fixture.inject (zlink::flow_state::receive_flow_running, 12));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    const flow_metrics_t resumed_metrics = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, resumed_metrics.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, resumed_metrics.resume_applied);

    close_test_monitor_probe (&monitor, &probe);
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
    char endpoint_a[MAX_SOCKET_STRING];
    char endpoint_b[MAX_SOCKET_STRING];

    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router_a, endpoint_a, sizeof endpoint_a);

    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router_b, endpoint_b, sizeof endpoint_b);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_a));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_b));

    char rid[256];
    bool seen_a = false;
    bool seen_b = false;
    const std::chrono::steady_clock::time_point learn_deadline =
      deadline_in_ms (6000);
    while ((!seen_a || !seen_b) && !deadline_expired (learn_deadline)) {
        (void) zlink_send (dealer, "hello", 5, ZLINK_DONTWAIT);
        if (zlink_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) zlink_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_a = true;
        }
        if (zlink_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) zlink_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT);
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
        (void) as_socket (dealer)->process_submit_commands ();
        (void) as_socket (router_a)->process_submit_commands ();
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
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (dealer, endpoint_b));
    bool gone = false;
    const std::chrono::steady_clock::time_point gone_deadline =
      deadline_in_ms (6000);
    while (!deadline_expired (gone_deadline)) {
        (void) as_socket (dealer)->process_submit_commands ();
        (void) as_socket (router_b)->process_submit_commands ();
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

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (dealer, k_flow_events, &probe);

    //  A late RUNNING must not release a count this pair no longer holds.
    as_socket (dealer)->test_deliver_late_flow_state (held_b, false, 41);
    //  A late PAUSE must not add one that nothing will ever release.
    as_socket (dealer)->test_deliver_late_flow_state (held_b, true, 42);

    const flow_metrics_t after = read_flow_metrics (dealer);
    TEST_ASSERT_EQUAL_UINT64 (before.paused_connections,
                              after.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (before.pause_applied, after.pause_applied);
    TEST_ASSERT_EQUAL_UINT64 (before.resume_applied, after.resume_applied);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 0, 200));

    as_socket (dealer)->test_release_pipe (held_b);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router_b);
    test_context_socket_close_zero_linger (router_a);
}

//  The same guard, for a pair whose record is still there but whose reporting
//  pipe is not the registered application pipe.
void test_late_flow_state_from_a_foreign_pipe_changes_nothing ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    const flow_metrics_t before = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (1, before.paused_connections);

    zlink::pipe_t *completion = as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, true);
    TEST_ASSERT_NOT_NULL (completion);

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    //  The pair is alive and paused, but this is not its application pipe.
    as_socket (fixture.dealer)->test_deliver_late_flow_state (completion, false, 51);
    as_socket (fixture.dealer)->test_deliver_late_flow_state (completion, true, 52);

    const flow_metrics_t after = read_flow_metrics (fixture.dealer);
    TEST_ASSERT_EQUAL_UINT64 (before.paused_connections,
                              after.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (before.pause_applied, after.pause_applied);
    TEST_ASSERT_EQUAL_UINT64 (before.resume_applied, after.resume_applied);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 0, 200));

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
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
    char endpoint_a[MAX_SOCKET_STRING];
    char endpoint_b[MAX_SOCKET_STRING];

    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router_a, endpoint_a, sizeof endpoint_a);

    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router_b, endpoint_b, sizeof endpoint_b);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_a));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_b));

    //  Learn both routes. The DEALER load-balances, so keep sending until each
    //  ROUTER has seen one rather than assuming a fixed number of sends lands
    //  on both.
    char rid[256];
    bool seen_a = false;
    bool seen_b = false;
    const std::chrono::steady_clock::time_point learn_deadline =
      deadline_in_ms (6000);
    while ((!seen_a || !seen_b) && !deadline_expired (learn_deadline)) {
        (void) zlink_send (dealer, "hello", 5, ZLINK_DONTWAIT);
        if (zlink_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) zlink_recv (router_a, rid, sizeof (rid), ZLINK_DONTWAIT);
            seen_a = true;
        }
        if (zlink_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT) > 0) {
            (void) zlink_recv (router_b, rid, sizeof (rid), ZLINK_DONTWAIT);
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
        (void) as_socket (dealer)->process_submit_commands ();
        (void) as_socket (router_a)->process_submit_commands ();
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
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (dealer, endpoint_b));
    for (int i = 0; i < 200; ++i) {
        (void) as_socket (dealer)->process_submit_commands ();
        (void) as_socket (router_b)->process_submit_commands ();
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

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_disconnect (fixture.dealer, fixture.endpoint));
    bool released = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (6000);
    while (!deadline_expired (deadline)) {
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        (void) as_socket (fixture.router)->process_submit_commands ();
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
//  order the pair's two lanes are admitted in, so the assertion here is the
//  invariant that holds for both: every pause raises exactly one PAUSED event
//  and moves the total exactly once.
void test_paused_pair_lifecycle_keeps_gauge_and_events_matched ()
{
    const int zero = 0;
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (dealer, k_flow_events, &probe);

    const int cycles = 4;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        //  A fresh receiver each cycle, so a cycle never inherits the previous
        //  route or the previous stored state.
        char endpoint[MAX_SOCKET_STRING];
        void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        const int recv_timeout = 200;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          router, ZLINK_OPT_RCVTIMEO, &recv_timeout, sizeof (recv_timeout)));
        bind_loopback_ipv4 (router, endpoint, sizeof endpoint);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

        //  The previous cycle's pipe may still be draining, so a first "hello"
        //  can be lost with the connection it was written to. Retry until the
        //  fresh route carries one rather than blocking forever on recv.
        char rid[256];
        int rid_size = -1;
        const std::chrono::steady_clock::time_point hello_deadline =
          deadline_in_ms (6000);
        while (rid_size <= 0 && !deadline_expired (hello_deadline)) {
            (void) zlink_send (dealer, "hello", 5, ZLINK_DONTWAIT);
            rid_size = zlink_recv (router, rid, sizeof (rid), 0);
        }
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        recv_string_expect_success (router, "hello", 0);
        //  Drain any duplicate "hello" the retry loop produced.
        char spare[256];
        while (zlink_recv (router, spare, sizeof (spare), ZLINK_DONTWAIT) > 0) {
            (void) zlink_recv (router, spare, sizeof (spare), ZLINK_DONTWAIT);
        }

        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (router, ZLINK_RECEIVE_FLOW_PAUSED));

        bool paused = false;
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (6000);
        while (!deadline_expired (deadline)) {
            (void) as_socket (dealer)->process_submit_commands ();
            (void) as_socket (router)->process_submit_commands ();
            if (read_flow_metrics (dealer).paused_connections == 1) {
                paused = true;
                break;
            }
            msleep (1);
        }
        TEST_ASSERT_TRUE (paused);

        //  Booked, whichever path applied it.
        TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (cycle + 1),
                                  read_flow_metrics (dealer).pause_applied);
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, cycle + 1, 2000));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, cycle + 1, 100));
        TEST_ASSERT_EQUAL_UINT64 (
          static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED),
          test_monitor_probe_event_at (&probe, cycle));

        //  Tear the pair down while it is still paused.
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (dealer, endpoint));
        bool released = false;
        const std::chrono::steady_clock::time_point release_deadline =
          deadline_in_ms (6000);
        while (!deadline_expired (release_deadline)) {
            (void) as_socket (dealer)->process_submit_commands ();
            (void) as_socket (router)->process_submit_commands ();
            if (read_flow_metrics (dealer).paused_connections == 0) {
                released = true;
                break;
            }
            msleep (1);
        }
        TEST_ASSERT_TRUE (released);

        //  A lifecycle release is not a resume: the peer never resumed.
        TEST_ASSERT_EQUAL_UINT64 (0, read_flow_metrics (dealer).resume_applied);

        test_context_socket_close_zero_linger (router);
    }

    const flow_metrics_t final_metrics = read_flow_metrics (dealer);
    TEST_ASSERT_EQUAL_UINT64 (0, final_metrics.paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (cycles),
                              final_metrics.pause_applied);
    TEST_ASSERT_EQUAL_INT (cycles, test_monitor_probe_count (&probe));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
}

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
    const char *h3_case = getenv ("ZLINK_TEST_CASE");
    const bool h3_only = getenv ("ZLINK_H3_FLOW_ONLY") != NULL || h3_case;
#define RUN_H3_CASE(NAME, TEST_FN)                                                                \
    do {                                                                                          \
        if (!h3_case || strcmp (h3_case, NAME) == 0)                                              \
            RUN_TEST (TEST_FN);                                                                   \
    } while (false)

    UNITY_BEGIN ();
    if (!h3_only) {
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
    }
    RUN_H3_CASE (
      "same-state",
      test_same_state_forward_epoch_and_repeated_local_set_emit_no_event);
    RUN_H3_CASE (
      "resumed-stale",
      test_resumed_routing_id_and_epoch_stale_match_prior_transition);
    RUN_H3_CASE (
      "hwm-resumed",
      test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected);
    RUN_H3_CASE ("mask", test_flow_event_numeric_values_and_each_excluded_mask);
    RUN_H3_CASE (
      "ordering",
      test_shared_monitor_preserves_explicit_commit_order_across_connections);
    if (!h3_only) {
        RUN_TEST (test_pause_applied_by_pair_admission_is_booked);
        RUN_TEST (test_late_flow_state_from_a_terminated_pair_changes_nothing);
        RUN_TEST (test_late_flow_state_from_a_foreign_pipe_changes_nothing);
        RUN_TEST (test_terminating_a_received_but_unapplied_pause_leaves_the_gauge_alone);
        RUN_TEST (test_terminating_an_accounted_pause_releases_it_and_closes_the_duration);
        RUN_TEST (test_paused_pair_lifecycle_keeps_gauge_and_events_matched);
        RUN_TEST (test_flow_state_metrics_snapshot_and_reset);
    }
    const int rc = UNITY_END ();
#undef RUN_H3_CASE
    return rc;
}
