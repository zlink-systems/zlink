/* SPDX-License-Identifier: MPL-2.0 */

// Public receive-flow configuration, monitor events and lifecycle metrics.
// Injected protocol frames and internal accounting live under tests/unittest.

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
}

void process_socket_commands (void *socket_)
{
    int events = 0;
    size_t size = sizeof (events);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_option (socket_, ZLINK_OPT_EVENTS,
                                             &events, &size));
}

zlink_monitor_status_t read_monitor_status (void *monitor_)
{
    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor_, &status));
    return status;
}

void *open_status_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

// Routing identity is public receive metadata, never an application part.
void receive_router_payload (void *router_, const char *payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t more = ZLINK_PART_MORE;
    const zlink_routing_id_t *rid = NULL;
    uint64_t request_seq = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv_part (router_, &rid, &request_seq, &part, &more,
                                             ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (rid);
    TEST_ASSERT_GREATER_THAN_INT (0, rid->size);
    TEST_ASSERT_EQUAL_UINT (strlen (payload_), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&part), strlen (payload_));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

struct paired_fixture_t
{
    paired_fixture_t () : dealer (NULL), router (NULL) {}

    void setup ()
    {
        create (0);
        char endpoint[MAX_SOCKET_STRING];
        bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
        connect_and_prime (endpoint);
    }

    void setup_inproc (const char *endpoint_, uint64_t hwm_ = 0)
    {
        create (hwm_);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint_));
        connect_and_prime (endpoint_);
    }

    void create (uint64_t hwm_)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        void *sockets[] = {router, dealer};
        for (size_t i = 0; i < 2; ++i) {
            TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
              sockets[i], ZLINK_OPT_LINGER, &zero, sizeof (zero)));
            if (hwm_ != 0) {
                TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
                  sockets[i], ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
                TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
                  sockets[i], ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
            }
        }
    }

    void connect_and_prime (const char *endpoint_)
    {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_));
        send_string_expect_success (dealer, "hello", 0);
        receive_router_payload (router, "hello");
    }

    bool wait_for_applied_pause (bool expected_, void *monitor_)
    {
        const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            process_socket_commands (router);
            process_socket_commands (dealer);
            if (read_monitor_status (monitor_).flow_paused_connections
                == static_cast<uint64_t> (expected_))
                return true;
            msleep (1);
        }
        return false;
    }

    void teardown ()
    {
        dealer = test_context_socket_close_zero_linger (dealer);
        router = test_context_socket_close_zero_linger (router);
    }

    void *dealer;
    void *router;
};

const uint32_t k_flow_events = ZLINK_EVENT_SEND_FLOW_PAUSED
                               | ZLINK_EVENT_SEND_FLOW_RESUMED
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

void test_unsupported_socket_types_report_not_supported ()
{
    const int types[] = {ZLINK_SOCKET_PAIR, ZLINK_SOCKET_PUB, ZLINK_SOCKET_SUB,
                         ZLINK_SOCKET_XPUB, ZLINK_SOCKET_XSUB, ZLINK_SOCKET_STREAM};
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

void test_pause_and_resume_each_emit_exactly_one_event ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    //  No extra event trails the transition.
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 1, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_PAUSED), test_monitor_probe_event_at (&probe, 0));

    //  The public flow-event identity is routing ID, connection ID and lane;
    //  `value` is the applied flow epoch.
    const zlink_monitor_event_t paused = test_monitor_probe_record_at (&probe, 0);
    TEST_ASSERT_TRUE (paused.value != 0);
    TEST_ASSERT_TRUE (paused.routing_id.size > 0);
    TEST_ASSERT_TRUE (paused.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            paused.transport_lane);
    //  A pause is never writable, so the RESUMED-only flag must be clear.
    TEST_ASSERT_EQUAL_UINT32 (
      0u, paused.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE);
    const uint64_t paused_epoch = paused.value;

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false, monitor));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 2000));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 2, 200));
    TEST_ASSERT_EQUAL_UINT64 (
      static_cast<uint64_t> (ZLINK_EVENT_SEND_FLOW_RESUMED), test_monitor_probe_event_at (&probe, 1));

    //  §6 adds "actually writable" to RESUMED. Nothing else blocks this pipe,
    //  so the resume really did make it writable and the flag must say so.
    const zlink_monitor_event_t resumed = test_monitor_probe_record_at (&probe, 1);
    TEST_ASSERT_TRUE (resumed.value > paused_epoch);
    TEST_ASSERT_TRUE (routing_id_equal (paused.routing_id, resumed.routing_id));
    TEST_ASSERT_EQUAL_UINT64 (paused.connection_id, resumed.connection_id);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            resumed.transport_lane);
    TEST_ASSERT_EQUAL_UINT32 (
      ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE,
      resumed.flags & ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE);

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

void test_data_traffic_emits_no_flow_events ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);

    for (int i = 0; i < 200; ++i) {
        send_string_expect_success (fixture.dealer, "payload", 0);
        receive_router_payload (fixture.router, "payload");
    }
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 0, 200));
    TEST_ASSERT_EQUAL_INT (0, test_monitor_probe_count (&probe));

    close_test_monitor_probe (&monitor, &probe);
    fixture.teardown ();
}

void test_repeated_local_set_emits_no_event ()
{
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
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));
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

void test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected ()
{
    paired_fixture_t fixture;
    fixture.setup_inproc ("inproc://gap-h3-flow-hwm-resume", 4096);
    // A zero-timeout blocking submit observes physical pipe admission, so a
    // receive-flow resume cannot make an HWM-blocked pipe writable.
    const int no_wait = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      fixture.dealer, ZLINK_OPT_SNDTIMEO, &no_wait, sizeof (no_wait)));
    const size_t filler_count =
      fill_flow_pipe_until_backpressured (fixture.dealer);
    TEST_ASSERT_TRUE (filler_count > 0);

    test_monitor_probe_t probe;
    void *monitor =
      open_test_monitor_probe (fixture.dealer, k_flow_events, &probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false, monitor));
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

void test_flow_event_numeric_values_and_state_excluded_masks ()
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
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));
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
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 1, 2000));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false, monitor));
        TEST_ASSERT_TRUE (
          test_monitor_probe_wait_no_additional (&probe, 1, 200));
        close_test_monitor_probe (&monitor, &probe);
        fixture.teardown ();
    }

}

void test_flow_state_metrics_snapshot ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    void *monitor = open_status_monitor (fixture.dealer);
    zlink_monitor_status_t status = read_monitor_status (monitor);
    TEST_ASSERT_TRUE ((status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE) != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_state_stale_total);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true, monitor));

    status = read_monitor_status (monitor);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router, ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false, monitor));

    status = read_monitor_status (monitor);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (1, status.flow_resume_applied_total);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    fixture.teardown ();
}

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
        bool received = false;
        const std::chrono::steady_clock::time_point hello_deadline =
          deadline_in_ms (6000);
        while (!received && !deadline_expired (hello_deadline)) {
            (void) zlink_send (dealer, "hello", 5, ZLINK_DONTWAIT);
            zlink_msg_t part;
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
            zlink_part_flag_t more = ZLINK_PART_FINAL;
            const zlink_routing_id_t *rid = NULL;
            uint64_t request_seq = 0;
            const zlink_recv_result_t result = zlink_router_recv_part (
              router, &rid, &request_seq, &part, &more, ZLINK_RECV_FLAGS_NONE);
            if (result == ZLINK_RECV_OK) {
                TEST_ASSERT_NOT_NULL (rid);
                TEST_ASSERT_GREATER_THAN_INT (0, rid->size);
                TEST_ASSERT_EQUAL_UINT (5, zlink_msg_size (&part));
                TEST_ASSERT_EQUAL_MEMORY ("hello", zlink_msg_data (&part), 5);
                TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
                received = true;
            } else {
                TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
                TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            }
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        }
        TEST_ASSERT_TRUE (received);
        char spare[256];
        while (test_recv_router (router, spare, sizeof (spare), ZLINK_DONTWAIT) > 0) {}

        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_socket_set_receive_flow_state (router, ZLINK_RECEIVE_FLOW_PAUSED));

        bool paused = false;
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (6000);
        while (!deadline_expired (deadline)) {
            process_socket_commands (dealer);
            process_socket_commands (router);
            if (read_monitor_status (monitor).flow_paused_connections == 1) {
                paused = true;
                break;
            }
            msleep (1);
        }
        TEST_ASSERT_TRUE (paused);

        //  Booked, whichever path applied it.
        TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (cycle + 1),
                                  read_monitor_status (monitor).flow_pause_applied_total);
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
            process_socket_commands (dealer);
            process_socket_commands (router);
            if (read_monitor_status (monitor).flow_paused_connections == 0) {
                released = true;
                break;
            }
            msleep (1);
        }
        TEST_ASSERT_TRUE (released);

        //  A lifecycle release is not a resume: the peer never resumed.
        TEST_ASSERT_EQUAL_UINT64 (0, read_monitor_status (monitor).flow_resume_applied_total);

        test_context_socket_close_zero_linger (router);
    }

    const zlink_monitor_status_t final_metrics = read_monitor_status (monitor);
    TEST_ASSERT_EQUAL_UINT64 (0, final_metrics.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (cycles),
                              final_metrics.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_INT (cycles, test_monitor_probe_count (&probe));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
}

}

int main ()
{
    setup_test_environment ();
    const char *selected_case = getenv ("ZLINK_TEST_CASE");
    if (selected_case && strcmp (selected_case, "same-state") == 0)
        selected_case = "test_repeated_local_set_emits_no_event";
    if (selected_case && strcmp (selected_case, "hwm-resumed") == 0)
        selected_case = "test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected";
    if (selected_case && strcmp (selected_case, "mask") == 0)
        selected_case = "test_flow_event_numeric_values_and_state_excluded_masks";

    UNITY_BEGIN ();
    if (!selected_case || strcmp (selected_case, "test_valid_state_on_dealer_and_router_succeeds_and_is_idempotent") == 0)
        RUN_TEST (test_valid_state_on_dealer_and_router_succeeds_and_is_idempotent);
    if (!selected_case || strcmp (selected_case, "test_null_or_invalid_handle_is_invalid_handle") == 0)
        RUN_TEST (test_null_or_invalid_handle_is_invalid_handle);
    if (!selected_case || strcmp (selected_case, "test_out_of_range_state_is_invalid_argument") == 0)
        RUN_TEST (test_out_of_range_state_is_invalid_argument);
    if (!selected_case || strcmp (selected_case, "test_unsupported_socket_types_report_not_supported") == 0)
        RUN_TEST (test_unsupported_socket_types_report_not_supported);
    if (!selected_case || strcmp (selected_case, "test_close_admitted_first_reports_invalid_state_or_handle") == 0)
        RUN_TEST (test_close_admitted_first_reports_invalid_state_or_handle);
    if (!selected_case || strcmp (selected_case, "test_close_races_with_set_receive_flow_state") == 0)
        RUN_TEST (test_close_races_with_set_receive_flow_state);
    if (!selected_case || strcmp (selected_case, "test_pause_and_resume_each_emit_exactly_one_event") == 0)
        RUN_TEST (test_pause_and_resume_each_emit_exactly_one_event);
    if (!selected_case || strcmp (selected_case, "test_data_traffic_emits_no_flow_events") == 0)
        RUN_TEST (test_data_traffic_emits_no_flow_events);
    if (!selected_case || strcmp (selected_case, "test_repeated_local_set_emits_no_event") == 0)
        RUN_TEST (test_repeated_local_set_emits_no_event);
    if (!selected_case || strcmp (selected_case, "test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected") == 0)
        RUN_TEST (test_resumed_while_hwm_blocked_is_not_writable_and_send_stays_rejected);
    if (!selected_case || strcmp (selected_case, "test_flow_event_numeric_values_and_state_excluded_masks") == 0)
        RUN_TEST (test_flow_event_numeric_values_and_state_excluded_masks);
    if (!selected_case || strcmp (selected_case, "test_flow_state_metrics_snapshot") == 0)
        RUN_TEST (test_flow_state_metrics_snapshot);
    if (!selected_case || strcmp (selected_case, "test_paused_pair_lifecycle_keeps_gauge_and_events_matched") == 0)
        RUN_TEST (test_paused_pair_lifecycle_keeps_gauge_and_events_matched);
    return UNITY_END ();
}
