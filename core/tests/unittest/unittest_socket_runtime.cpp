/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "core/mailbox.hpp"
#include "sockets/common/socket_runtime.hpp"
#include "utils/config.hpp"

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
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

void test_socket_monitor_runtime_erases_transport_pair_by_endpoint_when_rid_differs ()
{
    zlink::socket_monitor_runtime_t runtime;
    const zlink::endpoint_uri_pair_t endpoint =
      make_bind_endpoint ("inproc://ready-pair", "tcp://peer-pair");
    const unsigned char routing_id_a[] = {'r', 'i', 'd', 'a'};
    const unsigned char routing_id_b[] = {'r', 'i', 'd', 'b'};
    const unsigned char stale_routing_id[] = {'s', 't', 'a', 'l', 'e'};

    uint32_t ready_count = 0;
    TEST_ASSERT_TRUE (runtime.mark_ready_connection (
      endpoint, routing_id_a, sizeof (routing_id_a), &ready_count, 11, 1));
    TEST_ASSERT_TRUE (runtime.mark_ready_connection (
      endpoint, routing_id_b, sizeof (routing_id_b), &ready_count, 12, 1));
    TEST_ASSERT_EQUAL_UINT32 (2u, runtime.ready_count ());

    TEST_ASSERT_TRUE (runtime.erase_ready_connection (
      endpoint, stale_routing_id, sizeof (stale_routing_id), &ready_count, 11, 1));
    TEST_ASSERT_FALSE (runtime.erase_ready_connection_for_endpoint (endpoint, &ready_count, 11, 1));
    TEST_ASSERT_EQUAL_UINT32 (1u, ready_count);
    TEST_ASSERT_EQUAL_UINT32 (1u, runtime.ready_count ());

    TEST_ASSERT_TRUE (runtime.erase_ready_connection (
      endpoint, routing_id_b, sizeof (routing_id_b), &ready_count, 12, 1));
    TEST_ASSERT_EQUAL_UINT32 (0u, runtime.ready_count ());
}

void test_socket_monitor_runtime_dequeues_enqueued_worker_event_nowait ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t record;
    record.event = 41;
    runtime.reset_worker_state (1, 1);
    runtime.enqueue_worker_event (record);

    zlink::socket_monitor_event_record_t dequeued;
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (41u, dequeued.event);
    runtime.complete_worker_event ();
}

void test_socket_monitor_runtime_hwm_drops_lossy_events ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t first;
    zlink::socket_monitor_event_record_t second;
    zlink::socket_monitor_event_record_t dequeued;
    first.event = 57;
    second.event = 58;

    runtime.reset_worker_state (1, 1);
    runtime.enqueue_worker_event (first);
    runtime.enqueue_worker_event (second);
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (57u, dequeued.event);
    TEST_ASSERT_FALSE (runtime.dequeue_worker_event_nowait (&dequeued));
    runtime.complete_worker_event ();
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
    runtime.reset_worker_state (1, 1);
    runtime.enqueue_worker_event (first);

    std::atomic<bool> producer_started (false);
    std::atomic<bool> producer_completed (false);
    std::thread producer ([&] {
        producer_started.store (true, std::memory_order_release);
        runtime.enqueue_worker_event (second);
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
    runtime.complete_worker_event ();
    producer.join ();
    TEST_ASSERT_TRUE (producer_was_blocked);
    TEST_ASSERT_TRUE (dequeued_first);
    TEST_ASSERT_EQUAL_UINT64 (61u, first_event);
    TEST_ASSERT_TRUE (producer_completed.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (62u, dequeued.event);
    runtime.complete_worker_event ();
}

void test_socket_monitor_runtime_reset_clears_stop_state ()
{
    zlink::socket_monitor_runtime_t runtime;
    zlink::socket_monitor_event_record_t record;
    zlink::socket_monitor_event_record_t dequeued;
    record.event = 73;

    runtime.reset_worker_state (1, 1);
    runtime.start_task (11);
    runtime.enqueue_worker_event (record);
    runtime.stop_task ();
    TEST_ASSERT_FALSE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_FALSE (runtime.task_running);
    TEST_ASSERT_EQUAL_UINT64 (0u, runtime.task_id);

    runtime.reset_worker_state (1, 1);
    TEST_ASSERT_FALSE (runtime.queue_stop);
    TEST_ASSERT_FALSE (runtime.task_running);
    runtime.enqueue_worker_event (record);
    TEST_ASSERT_TRUE (runtime.dequeue_worker_event_nowait (&dequeued));
    TEST_ASSERT_EQUAL_UINT64 (73u, dequeued.event);
    runtime.complete_worker_event ();
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

void test_socket_lifecycle_coordinator_seals_mailbox_refs_at_zero ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;

    TEST_ASSERT_TRUE (coordinator.try_inc_mailbox_ref ());
    TEST_ASSERT_EQUAL_INT (1, coordinator.mailbox_refcount ());
    TEST_ASSERT_FALSE (coordinator.seal_mailbox_refs_if_zero ());
    TEST_ASSERT_FALSE (coordinator.mailbox_refs_sealed ());

    TEST_ASSERT_FALSE (coordinator.dec_mailbox_ref ());
    TEST_ASSERT_TRUE (coordinator.seal_mailbox_refs_if_zero ());
    TEST_ASSERT_TRUE (coordinator.mailbox_refs_sealed ());
    TEST_ASSERT_FALSE (coordinator.try_inc_mailbox_ref ());
    TEST_ASSERT_EQUAL_INT (0, coordinator.mailbox_refcount ());
}

void test_socket_lifecycle_coordinator_atomically_seals_or_acquires_mailbox_ref ()
{
    for (size_t attempt = 0; attempt < 200; ++attempt) {
        zlink::socket_lifecycle_coordinator_t coordinator;
        std::atomic<bool> start (false);
        bool acquired = false;
        bool sealed = false;

        std::thread acquire_thread ([&] {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            acquired = coordinator.try_inc_mailbox_ref ();
        });
        std::thread seal_thread ([&] {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            sealed = coordinator.seal_mailbox_refs_if_zero ();
        });

        start.store (true, std::memory_order_release);
        acquire_thread.join ();
        seal_thread.join ();

        TEST_ASSERT_TRUE (acquired != sealed);
        if (acquired) {
            TEST_ASSERT_FALSE (coordinator.dec_mailbox_ref ());
            TEST_ASSERT_TRUE (coordinator.seal_mailbox_refs_if_zero ());
        }
        TEST_ASSERT_TRUE (coordinator.mailbox_refs_sealed ());
        TEST_ASSERT_FALSE (coordinator.try_inc_mailbox_ref ());
    }
}

void test_socket_lifecycle_coordinator_completes_deferred_close_without_async_mailbox ()
{
    zlink::socket_lifecycle_coordinator_t coordinator;
    zlink::mailbox_t mailbox;

    TEST_ASSERT_TRUE (coordinator.enter_callback_api ());
    TEST_ASSERT_TRUE (coordinator.begin_close_or_fail_busy (true));
    TEST_ASSERT_TRUE (coordinator.public_close_requested ());
    TEST_ASSERT_TRUE (coordinator.leave_callback_api ());

    coordinator.complete_deferred_close_handoff (&mailbox, NULL, 1);

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

    coordinator.complete_deferred_close_handoff (&mailbox, NULL, 1);

    TEST_ASSERT_FALSE (coordinator.is_async_mailbox_active ());
    TEST_ASSERT_TRUE (coordinator.is_async_quiesce_pending ());
    coordinator.mark_async_processing_stopped (&mailbox);
    TEST_ASSERT_FALSE (coordinator.is_async_quiesce_pending ());

    // A later monitor detach must not reopen a handoff that has already
    // published its quiesce acknowledgement.
    coordinator.stop_async_mailbox_processing (&mailbox);
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

void test_socket_send_complete_dispatch_scope_restores_previous_socket ()
{
    zlink::socket_base_t *outer = reinterpret_cast<zlink::socket_base_t *> (0x1);
    zlink::socket_base_t *inner = reinterpret_cast<zlink::socket_base_t *> (0x2);

    TEST_ASSERT_NULL (zlink::socket_send_complete_dispatch_scope_t::current_socket ());
    TEST_ASSERT_FALSE (zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (outer));

    {
        zlink::socket_send_complete_dispatch_scope_t outer_scope (outer);
        TEST_ASSERT_TRUE (
          zlink::socket_send_complete_dispatch_scope_t::dispatching_any ());
        TEST_ASSERT_EQUAL_PTR (outer, zlink::socket_send_complete_dispatch_scope_t::current_socket ());
        TEST_ASSERT_TRUE (zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (outer));

        {
            zlink::socket_send_complete_dispatch_scope_t inner_scope (inner);
            TEST_ASSERT_EQUAL_PTR (inner,
                                   zlink::socket_send_complete_dispatch_scope_t::current_socket ());
            TEST_ASSERT_TRUE (
              zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (inner));
            TEST_ASSERT_FALSE (
              zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (outer));
        }

        TEST_ASSERT_EQUAL_PTR (outer, zlink::socket_send_complete_dispatch_scope_t::current_socket ());
        TEST_ASSERT_TRUE (zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (outer));
    }

    TEST_ASSERT_NULL (zlink::socket_send_complete_dispatch_scope_t::current_socket ());
    TEST_ASSERT_FALSE (
      zlink::socket_send_complete_dispatch_scope_t::dispatching_any ());
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

void test_socket_dispatch_bridge_tracks_send_recovery_edges ()
{
    zlink::socket_dispatch_bridge_t bridge;

    //  Ready is meaningless until something is actually waiting on recovery,
    //  which is what keeps has_out() from reporting a stale edge.
    TEST_ASSERT_FALSE (bridge.send_recovery_pending ());
    TEST_ASSERT_FALSE (bridge.send_recovery_ready ());
    bridge.mark_send_recovery_ready ();
    TEST_ASSERT_FALSE (bridge.send_recovery_ready ());

    bridge.mark_send_recovery_pending ();
    TEST_ASSERT_TRUE (bridge.send_recovery_pending ());
    TEST_ASSERT_FALSE (bridge.send_recovery_ready ());

    bridge.mark_send_recovery_ready ();
    TEST_ASSERT_TRUE (bridge.send_recovery_ready ());

    bridge.clear_send_recovery_ready ();
    TEST_ASSERT_TRUE (bridge.send_recovery_pending ());
    TEST_ASSERT_FALSE (bridge.send_recovery_ready ());

    bridge.mark_send_recovery_ready ();
    bridge.clear_send_recovery_pending ();
    TEST_ASSERT_FALSE (bridge.send_recovery_pending ());
    TEST_ASSERT_FALSE (bridge.send_recovery_ready ());
}

void test_socket_send_pending_runtime_starts_empty ()
{
    zlink::socket_send_pending_runtime_t pending;

    //  op id 0 is reserved as "no operation", so the first assigned id is 1.
    TEST_ASSERT_EQUAL_UINT64 (1, pending.next_op_id);
    TEST_ASSERT_NULL (pending.handler);
    TEST_ASSERT_FALSE (pending.handler_installed.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (pending.queues.empty ());
    TEST_ASSERT_TRUE (pending.by_op.empty ());
    TEST_ASSERT_NULL (pending.completion_head);
    TEST_ASSERT_NULL (pending.completion_tail);
    TEST_ASSERT_EQUAL_UINT64 (0, pending.pending_msgs);
    TEST_ASSERT_EQUAL_UINT64 (0, pending.pending_bytes);
    TEST_ASSERT_FALSE (pending.failing);
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
    RUN_TEST (test_socket_monitor_runtime_erases_transport_pair_by_endpoint_when_rid_differs);
    RUN_TEST (test_socket_monitor_runtime_dequeues_enqueued_worker_event_nowait);
    RUN_TEST (test_socket_monitor_runtime_hwm_drops_lossy_events);
    RUN_TEST (test_socket_monitor_runtime_hwm_backpressures_reliable_events);
    RUN_TEST (test_socket_monitor_runtime_reset_clears_stop_state);
    RUN_TEST (test_socket_endpoint_runtime_tracks_last_recv_source_rid);
    RUN_TEST (test_socket_endpoint_runtime_tracks_last_endpoint);
    RUN_TEST (test_socket_lifecycle_coordinator_tracks_destroy_state);
    RUN_TEST (test_socket_lifecycle_coordinator_seals_mailbox_refs_at_zero);
    RUN_TEST (
      test_socket_lifecycle_coordinator_atomically_seals_or_acquires_mailbox_ref);
    RUN_TEST (test_socket_lifecycle_coordinator_completes_deferred_close_without_async_mailbox);
    RUN_TEST (test_socket_lifecycle_coordinator_handoff_waits_pending_async_quiesce);
    RUN_TEST (test_socket_lifecycle_coordinator_claims_deferred_close_once);
    RUN_TEST (test_socket_public_api_scope_releases_inflight_admission);
    RUN_TEST (test_socket_self_close_rejects_unrelated_inflight_api);
    RUN_TEST (test_socket_self_close_rejects_another_inflight_callback);
    RUN_TEST (test_socket_public_api_lock_scope_releases_sync_bit_on_exit);
    RUN_TEST (test_socket_callback_scope_tracks_callback_depth_and_inflight_state);
    RUN_TEST (test_socket_send_complete_dispatch_scope_restores_previous_socket);
    RUN_TEST (test_socket_command_runtime_throttles_command_polls_by_tsc_window);
    RUN_TEST (test_socket_command_runtime_tracks_recv_ticks_until_reset);
    RUN_TEST (test_socket_dispatch_bridge_tracks_send_recovery_edges);
    RUN_TEST (test_socket_send_pending_runtime_starts_empty);
    RUN_TEST (test_socket_public_send_scope_combines_initial_admission_and_sync);
    RUN_TEST (test_socket_public_send_scope_unlocks_sync_but_keeps_inflight_admission);
    RUN_TEST (test_socket_public_send_scope_without_sync_keeps_inflight_admission);
    return UNITY_END ();
}
