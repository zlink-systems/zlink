/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"
#include "api/monitoring/monitor_api_internal.hpp"

SETUP_TEARDOWN_TESTCONTEXT

zlink_auto_hwm_budget_snapshot_t read_auto_hwm_budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}

void test_monitor_context_snapshot_tracks_one_pending_event_exactly ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);

    const uint64_t event_charge = socket_monitor_event_accounted_bytes ();
    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_LISTENING;
    monitor_opts.monitor_hwm_bytes = event_charge * 4;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);

    socket_handle_t server_handle = as_socket_handle (server);
    TEST_ASSERT_NOT_NULL (server_handle.socket);
    server_handle.socket->event_listening (
      zlink::make_unconnected_bind_endpoint_pair ("inproc://unit-monitor"),
      zlink::retired_fd);

    // Only the local monitor worker progresses asynchronously; no transport is
    // opened. Preserve the original observation bound for queue publication.
    zlink_auto_hwm_budget_snapshot_t pending;
    TEST_ASSERT_TRUE (zlink_test_wait_until (3000, [&] {
        pending = read_auto_hwm_budget_snapshot ();
        return pending.monitor_queue_accounted_bytes == event_charge;
    }));
    TEST_ASSERT_EQUAL_UINT64 (monitor_opts.monitor_hwm_bytes * 2,
                              pending.monitor_queue_applied_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      pending.total_messaging_accounted_bytes + event_charge,
      pending.total_instance_accounted_bytes);

    zlink_monitor_event_t event;
    TEST_ASSERT_TRUE (zlink_test_wait_until (3000, [&] {
        return recv_monitor_event_from_socket (monitor, &event, ZLINK_DONTWAIT)
               == 0;
    }));
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_LISTENING, event.event);
    TEST_ASSERT_TRUE (zlink_test_wait_until (3000, [&] {
        return read_auto_hwm_budget_snapshot ().monitor_queue_accounted_bytes
               == 0;
    }));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    server_handle = socket_handle_t ();
    test_context_socket_close_zero_linger (server);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_monitor_context_snapshot_tracks_one_pending_event_exactly);
    return UNITY_END ();
}
