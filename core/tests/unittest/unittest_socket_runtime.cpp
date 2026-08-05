/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "core/mailbox.hpp"
#include "sockets/common/socket_runtime.hpp"
#include "utils/config.hpp"

namespace
{
void noop_send_ready_handler (void *subject_, void *userdata_)
{
    LIBZLINK_UNUSED (subject_);
    LIBZLINK_UNUSED (userdata_);
}

zlink::endpoint_uri_pair_t make_bind_endpoint (const char *local_, const char *remote_)
{
    return zlink::endpoint_uri_pair_t (local_, remote_, zlink::endpoint_type_bind);
}

void test_socket_monitor_runtime_tracks_ready_connections_once ()
{
    zlink::socket_monitor_runtime_t runtime;
    const zlink::endpoint_uri_pair_t endpoint =
      make_bind_endpoint ("inproc://ready-a", "tcp://peer-a");
    const unsigned char routing_id[] = {'r', 'i', 'd', '1'};

    uint32_t ready_count = 0;
    TEST_ASSERT_TRUE (
      runtime.mark_ready_connection (endpoint, routing_id, sizeof (routing_id), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (1u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (1u, runtime.ready_count ());

    ready_count = 99;
    TEST_ASSERT_FALSE (
      runtime.mark_ready_connection (endpoint, routing_id, sizeof (routing_id), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (99u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (1u, runtime.ready_count ());
}

void test_socket_monitor_runtime_erases_only_matching_ready_connection ()
{
    zlink::socket_monitor_runtime_t runtime;
    const zlink::endpoint_uri_pair_t endpoint_a =
      make_bind_endpoint ("inproc://ready-a", "tcp://peer-a");
    const zlink::endpoint_uri_pair_t endpoint_b =
      make_bind_endpoint ("inproc://ready-b", "tcp://peer-b");
    const unsigned char routing_id_a[] = {'r', 'i', 'd', 'a'};
    const unsigned char routing_id_b[] = {'r', 'i', 'd', 'b'};

    uint32_t ready_count = 0;
    TEST_ASSERT_TRUE (runtime.mark_ready_connection (endpoint_a, routing_id_a,
                                                     sizeof (routing_id_a), &ready_count));
    TEST_ASSERT_TRUE (runtime.mark_ready_connection (endpoint_b, routing_id_b,
                                                     sizeof (routing_id_b), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (2u, runtime.ready_count ());

    ready_count = 0;
    TEST_ASSERT_TRUE (runtime.erase_ready_connection (endpoint_a, routing_id_a,
                                                      sizeof (routing_id_a), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (1u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (1u, runtime.ready_count ());

    ready_count = 77;
    TEST_ASSERT_FALSE (runtime.erase_ready_connection (endpoint_a, routing_id_a,
                                                       sizeof (routing_id_a), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (77u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (1u, runtime.ready_count ());

    TEST_ASSERT_TRUE (runtime.erase_ready_connection (endpoint_b, routing_id_b,
                                                      sizeof (routing_id_b), &ready_count));
    TEST_ASSERT_EQUAL_UINT32 (0u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (0u, runtime.ready_count ());
}

void test_socket_monitor_runtime_dequeues_enqueued_worker_event_nowait ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t record;
    record.event = 41;
    runtime.enqueue_worker_event (record, 1);

    zlink::socket_monitor_event_record_t dequeued;
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (41u, dequeued.event);
}

void test_socket_monitor_runtime_hwm_drops_lossy_events ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t first;
    zlink::socket_monitor_event_record_t second;
    zlink::socket_monitor_event_record_t dequeued;
    first.event = 57;
    second.event = 58;

    runtime.enqueue_worker_event (first, 1);
    runtime.enqueue_worker_event (second, 1);
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (57u, dequeued.event);
    TEST_ASSERT_FALSE (runtime.dequeue_worker_event_nowait (&dequeued));
}

void test_socket_monitor_runtime_hwm_backpressures_reliable_events ()
{
    zlink::socket_monitor_runtime_t runtime;
    runtime.lossy = false;
    zlink::socket_monitor_event_record_t first;
    zlink::socket_monitor_event_record_t second;
    zlink::socket_monitor_event_record_t dequeued;
    first.event = 61;
    second.event = 62;
    runtime.enqueue_worker_event (first, 1);

    std::atomic<bool> producer_started (false);
    std::atomic<bool> producer_completed (false);
    std::thread producer ([&] {
        producer_started.store (true, std::memory_order_release);
        runtime.enqueue_worker_event (second, 1);
        producer_completed.store (true, std::memory_order_release);
    });
    while (!producer_started.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (20);
    const bool producer_was_blocked =
      !producer_completed.load (std::memory_order_acquire);

    const bool dequeued_first =
      runtime.dequeue_worker_event_nowait (&dequeued);
    const uint64_t first_event = dequeued.event;
    producer.join ();
    TEST_ASSERT_TRUE (producer_was_blocked);
    TEST_ASSERT_TRUE (dequeued_first);
    TEST_ASSERT_EQUAL_UINT64 (61u, first_event);
    TEST_ASSERT_TRUE (producer_completed.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (62u, dequeued.event);
}

void test_socket_monitor_runtime_reset_clears_stop_state ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t record;
    zlink::socket_monitor_event_record_t dequeued;
    record.event = 73;

    runtime.start_task (11);
    runtime.enqueue_worker_event (record, 1);
    runtime.stop_task ();
    TEST_ASSERT_FALSE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_FALSE (runtime.task_running);
    TEST_ASSERT_EQUAL_UINT64 (0u, runtime.task_id);

    runtime.reset_worker_state ();
    TEST_ASSERT_FALSE (runtime.queue_stop);
    TEST_ASSERT_FALSE (runtime.task_running);
    runtime.enqueue_worker_event (record, 1);
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (73u, dequeued.event);
}

void test_socket_endpoint_runtime_tracks_last_recv_source_rid ()
{
    zlink::socket_endpoint_runtime_t runtime;
    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    source_rid.size = 3;
    source_rid.data[0] = 'a';
    source_rid.data[1] = 'b';
    source_rid.data[2] = 'c';

    zlink_routing_id_t copied;
    memset (&copied, 0xAB, sizeof (copied));
    TEST_ASSERT_FALSE (runtime.copy_last_recv_source_rid (&copied));
    TEST_ASSERT_EQUAL_UINT8 (0u, copied.size);

    runtime.store_last_recv_source_rid (&source_rid);
    TEST_ASSERT_TRUE (runtime.copy_last_recv_source_rid (&copied));
    TEST_ASSERT_EQUAL_UINT8 (source_rid.size, copied.size);
    TEST_ASSERT_EQUAL_UINT8 ('a', copied.data[0]);
    TEST_ASSERT_EQUAL_UINT8 ('b', copied.data[1]);
    TEST_ASSERT_EQUAL_UINT8 ('c', copied.data[2]);

    runtime.clear_last_recv_source_rid ();
    TEST_ASSERT_FALSE (runtime.copy_last_recv_source_rid (&copied));
    TEST_ASSERT_EQUAL_UINT8 (0u, copied.size);
}

void test_socket_endpoint_runtime_tracks_last_endpoint ()
{
    zlink::socket_endpoint_runtime_t runtime;

    TEST_ASSERT_EQUAL_STRING ("", runtime.last_endpoint_uri ().c_str ());

    runtime.set_last_endpoint ("tcp://127.0.0.1:5555");
    TEST_ASSERT_EQUAL_STRING ("tcp://127.0.0.1:5555", runtime.last_endpoint_uri ().c_str ());

    runtime.set_last_endpoint ("inproc://socket-runtime-endpoint");
    TEST_ASSERT_EQUAL_STRING ("inproc://socket-runtime-endpoint",
                              runtime.last_endpoint_uri ().c_str ());
}

void test_socket_lifecycle_coordinator_tracks_destroy_state ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_FALSE (coordinator.is_destroy_pending ());
    TEST_ASSERT_FALSE (coordinator.is_destroyed ());
    TEST_ASSERT_NULL (coordinator.reaper_poller ());

    coordinator.mark_destroy_pending ();
    TEST_ASSERT_TRUE (coordinator.is_destroy_pending ());

    coordinator.clear_destroy_pending ();
    TEST_ASSERT_FALSE (coordinator.is_destroy_pending ());

    coordinator.mark_destroyed ();
    TEST_ASSERT_TRUE (coordinator.is_destroyed ());
}

void test_socket_lifecycle_coordinator_completes_deferred_close_without_async_mailbox ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;
    zlink::mailbox_t mailbox;

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.public_close_requested ());
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());

    coordinator.complete_deferred_close_handoff (&mailbox, 1);

    TEST_ASSERT_TRUE (coordinator.public_close_requested ());
    TEST_ASSERT_FALSE (coordinator.is_async_mailbox_active ());
    TEST_ASSERT_FALSE (coordinator.is_async_quiesce_pending ());
}

void test_socket_lifecycle_coordinator_handoff_waits_pending_async_quiesce ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;
    zlink::mailbox_t mailbox;

    coordinator.stop_async_mailbox_processing (&mailbox);
    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());

    coordinator.complete_deferred_close_handoff (&mailbox, 1);

    TEST_ASSERT_FALSE (coordinator.is_async_mailbox_active ());
    TEST_ASSERT_TRUE (coordinator.is_async_quiesce_pending ());
    coordinator.mark_async_processing_stopped (&mailbox);
    TEST_ASSERT_FALSE (coordinator.is_async_quiesce_pending ());
}

void test_socket_lifecycle_coordinator_claims_deferred_close_once ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());

    TEST_ASSERT_TRUE (coordinator.take_deferred_close ());
    TEST_ASSERT_FALSE (coordinator.take_deferred_close ());
}

void test_socket_public_api_scope_releases_inflight_admission ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    {
        zlink::socket_public_api_scope_t admission (coordinator);
        TEST_ASSERT_TRUE (admission.acquired ());
        errno = 0;
        TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (false));
        TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    }

    errno = 0;
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (false));
    TEST_ASSERT_TRUE (coordinator.public_close_requested ());
}

void test_socket_self_close_rejects_unrelated_inflight_api ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.enter_public_api ());

    errno = 0;
    TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_FALSE (coordinator.public_close_requested ());

    coordinator.leave_public_api ();
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());
}

void test_socket_self_close_rejects_another_inflight_callback ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());

    errno = 0;
    TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_FALSE (coordinator.public_close_requested ());

    TEST_ASSERT_FALSE (coordinator.leave_callback_api ());
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());
}

void test_socket_public_api_lock_scope_releases_sync_bit_on_exit ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_FALSE (coordinator.public_api_sync_held ());
    {
        zlink::socket_public_api_lock_scope_t lock (coordinator);
        TEST_ASSERT_TRUE (coordinator.public_api_sync_held ());
    }
    TEST_ASSERT_FALSE (coordinator.public_api_sync_held ());
}

void test_socket_callback_scope_tracks_callback_depth_and_inflight_state ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_EQUAL_UINT32 (0u, coordinator.callback_api_depth.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32 (0u, coordinator.public_api_state.load (std::memory_order_acquire));

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_EQUAL_UINT32 (1u,
                              coordinator.callback_api_depth.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32 (1u,
                              coordinator.public_api_state.load (std::memory_order_acquire));
    TEST_ASSERT_FALSE (coordinator.leave_callback_api ());

    TEST_ASSERT_EQUAL_UINT32 (0u, coordinator.callback_api_depth.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT32 (0u, coordinator.public_api_state.load (std::memory_order_acquire));
    TEST_ASSERT_FALSE (coordinator.public_close_requested ());
}

void test_socket_send_ready_dispatch_scope_restores_previous_socket ()
{
    zlink::socket_base_t *outer = reinterpret_cast<zlink::socket_base_t *> (0x1);
    zlink::socket_base_t *inner = reinterpret_cast<zlink::socket_base_t *> (0x2);

    TEST_ASSERT_NULL (zlink::socket_send_ready_dispatch_scope_t::current_socket ());
    TEST_ASSERT_FALSE (zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (outer));

    {
        zlink::socket_send_ready_dispatch_scope_t outer_scope (outer);
        TEST_ASSERT_EQUAL_PTR (outer, zlink::socket_send_ready_dispatch_scope_t::current_socket ());
        TEST_ASSERT_TRUE (zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (outer));

        {
            zlink::socket_send_ready_dispatch_scope_t inner_scope (inner);
            TEST_ASSERT_EQUAL_PTR (inner,
                                   zlink::socket_send_ready_dispatch_scope_t::current_socket ());
            TEST_ASSERT_TRUE (
              zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (inner));
            TEST_ASSERT_FALSE (
              zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (outer));
        }

        TEST_ASSERT_EQUAL_PTR (outer, zlink::socket_send_ready_dispatch_scope_t::current_socket ());
        TEST_ASSERT_TRUE (zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (outer));
    }

    TEST_ASSERT_NULL (zlink::socket_send_ready_dispatch_scope_t::current_socket ());
}

void test_socket_command_runtime_throttles_command_polls_by_tsc_window ()
{
    zlink::socket_command_runtime_t runtime;

    TEST_ASSERT_FALSE (runtime.should_skip_throttled_command_poll (0));
    TEST_ASSERT_FALSE (runtime.should_skip_throttled_command_poll (zlink::max_command_delay + 1));
    TEST_ASSERT_TRUE (runtime.should_skip_throttled_command_poll (zlink::max_command_delay + 2));
    TEST_ASSERT_FALSE (
      runtime.should_skip_throttled_command_poll ((2 * zlink::max_command_delay) + 2));
    TEST_ASSERT_FALSE (runtime.should_skip_throttled_command_poll (50));
}

void test_socket_command_runtime_tracks_recv_ticks_until_reset ()
{
    zlink::socket_command_runtime_t runtime;

    for (int i = 1; i < zlink::inbound_poll_rate; ++i)
        TEST_ASSERT_FALSE (runtime.should_poll_commands_after_recv (zlink::inbound_poll_rate));

    TEST_ASSERT_TRUE (runtime.should_poll_commands_after_recv (zlink::inbound_poll_rate));
    TEST_ASSERT_TRUE (runtime.should_block_on_recv ());

    runtime.reset_recv_ticks ();
    TEST_ASSERT_FALSE (runtime.should_block_on_recv ());
    TEST_ASSERT_FALSE (runtime.should_poll_commands_after_recv (zlink::inbound_poll_rate));
}

void test_socket_dispatch_bridge_reads_send_ready_triplet_consistently ()
{
    zlink::socket_dispatch_bridge_t bridge;
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;

    TEST_ASSERT_FALSE (bridge.load_send_ready_handler (&handler, &subject, &userdata));
    TEST_ASSERT_NULL (handler);
    TEST_ASSERT_NULL (subject);
    TEST_ASSERT_NULL (userdata);

    int subject_value = 7;
    int userdata_value = 11;
    bridge.store_send_ready_handler (&noop_send_ready_handler, &subject_value, &userdata_value);

    TEST_ASSERT_TRUE (bridge.load_send_ready_handler (&handler, &subject, &userdata));
    TEST_ASSERT_EQUAL_PTR (&noop_send_ready_handler, handler);
    TEST_ASSERT_EQUAL_PTR (&subject_value, subject);
    TEST_ASSERT_EQUAL_PTR (&userdata_value, userdata);
}

void test_socket_dispatch_bridge_arms_send_ready_only_with_handler ()
{
    zlink::socket_dispatch_bridge_t bridge;

    TEST_ASSERT_FALSE (bridge.arm_send_ready_notification ());
    TEST_ASSERT_FALSE (bridge.consume_send_ready_notification ());

    int subject_value = 13;
    bridge.store_send_ready_handler (&noop_send_ready_handler, &subject_value, NULL);

    TEST_ASSERT_TRUE (bridge.arm_send_ready_notification ());
    TEST_ASSERT_TRUE (bridge.consume_send_ready_notification ());
    TEST_ASSERT_FALSE (bridge.consume_send_ready_notification ());
}

void test_socket_public_send_scope_combines_initial_admission_and_sync ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    {
        zlink::socket_public_send_scope_t send_scope (coordinator, true);
        TEST_ASSERT_TRUE (send_scope.acquired ());
        TEST_ASSERT_TRUE (send_scope.sync_locked ());
        TEST_ASSERT_TRUE (coordinator.public_api_sync_held ());

        errno = 0;
        TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (false));
        TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    }

    TEST_ASSERT_FALSE (coordinator.public_api_sync_held ());
    errno = 0;
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (false));
}

void test_socket_public_send_scope_unlocks_sync_but_keeps_inflight_admission ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    {
        zlink::socket_public_send_scope_t send_scope (coordinator, true);
        TEST_ASSERT_TRUE (send_scope.acquired ());
        TEST_ASSERT_TRUE (send_scope.sync_locked ());

        send_scope.unlock_sync ();
        TEST_ASSERT_FALSE (send_scope.sync_locked ());
        TEST_ASSERT_FALSE (coordinator.public_api_sync_held ());

        errno = 0;
        TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (false));
        TEST_ASSERT_EQUAL_INT (EBUSY, errno);

        send_scope.relock_sync ();
        TEST_ASSERT_TRUE (send_scope.sync_locked ());
        TEST_ASSERT_TRUE (coordinator.public_api_sync_held ());
    }

    TEST_ASSERT_FALSE (coordinator.public_api_sync_held ());
    errno = 0;
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (false));
}

void test_socket_public_send_scope_without_sync_keeps_inflight_admission ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    {
        zlink::socket_public_send_scope_t send_scope (coordinator, false);
        TEST_ASSERT_TRUE (send_scope.acquired ());
        TEST_ASSERT_FALSE (send_scope.sync_locked ());
        TEST_ASSERT_EQUAL_UINT32 (1u,
                                  coordinator.public_api_state.load (std::memory_order_acquire));

        errno = 0;
        TEST_ASSERT_FALSE (coordinator.begin_close_or_fail_busy (false));
        TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    }

    TEST_ASSERT_EQUAL_UINT32 (0u, coordinator.public_api_state.load (std::memory_order_acquire));
    errno = 0;
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (false));
}
}

int main (int argc, char **argv)
{
    UNITY_BEGIN ();
    RUN_TEST (test_socket_monitor_runtime_tracks_ready_connections_once);
    RUN_TEST (test_socket_monitor_runtime_erases_only_matching_ready_connection);
    RUN_TEST (test_socket_monitor_runtime_dequeues_enqueued_worker_event_nowait);
    RUN_TEST (test_socket_monitor_runtime_hwm_drops_lossy_events);
    RUN_TEST (test_socket_monitor_runtime_hwm_backpressures_reliable_events);
    RUN_TEST (test_socket_monitor_runtime_reset_clears_stop_state);
    RUN_TEST (test_socket_endpoint_runtime_tracks_last_recv_source_rid);
    RUN_TEST (test_socket_endpoint_runtime_tracks_last_endpoint);
    RUN_TEST (test_socket_lifecycle_coordinator_tracks_destroy_state);
    RUN_TEST (test_socket_lifecycle_coordinator_completes_deferred_close_without_async_mailbox);
    RUN_TEST (test_socket_lifecycle_coordinator_handoff_waits_pending_async_quiesce);
    RUN_TEST (test_socket_lifecycle_coordinator_claims_deferred_close_once);
    RUN_TEST (test_socket_public_api_scope_releases_inflight_admission);
    RUN_TEST (test_socket_self_close_rejects_unrelated_inflight_api);
    RUN_TEST (test_socket_self_close_rejects_another_inflight_callback);
    RUN_TEST (test_socket_public_api_lock_scope_releases_sync_bit_on_exit);
    RUN_TEST (test_socket_callback_scope_tracks_callback_depth_and_inflight_state);
    RUN_TEST (test_socket_send_ready_dispatch_scope_restores_previous_socket);
    RUN_TEST (test_socket_command_runtime_throttles_command_polls_by_tsc_window);
    RUN_TEST (test_socket_command_runtime_tracks_recv_ticks_until_reset);
    RUN_TEST (test_socket_dispatch_bridge_reads_send_ready_triplet_consistently);
    RUN_TEST (test_socket_dispatch_bridge_arms_send_ready_only_with_handler);
    RUN_TEST (test_socket_public_send_scope_combines_initial_admission_and_sync);
    RUN_TEST (test_socket_public_send_scope_unlocks_sync_but_keeps_inflight_admission);
    RUN_TEST (test_socket_public_send_scope_without_sync_keeps_inflight_admission);
    return UNITY_END ();
}
