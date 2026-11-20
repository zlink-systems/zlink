/* SPDX-License-Identifier: MPL-2.0 */

#include "contract_socket_pair_fixture.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static socket_lifecycle_coordinator_t &lifecycle (socket_base_t *socket_)
    {
        return socket_->lifecycle_coordinator ();
    }
};
}

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct delivery_barrier_t
{
    zlink::socket_lifecycle_coordinator_t *coordinator;
    zlink::pipe_t *destination;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool sync_owned = false;
};

void stop_before_mailbox_delivery (const zlink::command_t &command_, void *arg_)
{
    delivery_barrier_t &barrier = *static_cast<delivery_barrier_t *> (arg_);
    if (command_.type != zlink::command_t::activate_read
        || command_.destination != barrier.destination)
        return;
    std::unique_lock<std::mutex> lock (barrier.mutex);
    barrier.sync_owned = barrier.coordinator->public_api_sync_owned_by_current_thread ();
    barrier.entered = true;
    barrier.changed.notify_all ();
    barrier.changed.wait (lock, [&] { return barrier.released; });
}

void test_delivery_probe_accepts_unlocked_writer ()
{
    void *source = test_context_socket (ZLINK_SOCKET_PAIR);
    void *target = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (source, target);
    TEST_ASSERT_FALSE (pair.application[1]->check_read ());
    delivery_barrier_t barrier;
    barrier.coordinator = &zlink::session_termination_test_access_t::lifecycle (pair.cores[0]);
    barrier.destination = pair.application[1];
    barrier.released = true;
    zlink::msg_t message;
    TEST_ASSERT_EQUAL_INT (0, message.init_size (1));
    zlink::object_t::test_set_command_delivery_hook (stop_before_mailbox_delivery, &barrier);
    const bool written = pair.application[0]->write (&message);
    pair.application[0]->flush ();
    zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    if (!written)
        message.close ();
    pair.pump ();
    const bool observed = barrier.entered;
    const bool sync_owned = barrier.sync_owned;
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
    TEST_ASSERT_TRUE (written);
    TEST_ASSERT_TRUE (observed);
    TEST_ASSERT_FALSE (sync_owned);
}

void test_request_wake_delivery_releases_public_sync ()
{
    void *source = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *target = test_context_socket (ZLINK_SOCKET_ROUTER);
    contract_socket_pair_t pair (source, target);
    // Fix the sleeping-reader order before starting the producer.
    TEST_ASSERT_FALSE (pair.application[1]->check_read ());
    delivery_barrier_t barrier;
    barrier.coordinator = &zlink::session_termination_test_access_t::lifecycle (pair.cores[0]);
    barrier.destination = pair.application[1];
    zlink_submit_result_t result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_completion_id_t completion_id = 0;
    std::thread submitter ([&] {
        zlink::object_t::test_set_command_delivery_hook (stop_before_mailbox_delivery, &barrier);
        zlink_msg_t parts[2];
        zlink_msg_init_size (&parts[0], 76);
        zlink_msg_init_size (&parts[1], 1024);
        memset (zlink_msg_data (&parts[0]), 'h', 76);
        memset (zlink_msg_data (&parts[1]), 'p', 1024);
        result = zlink_request (source, &pair.rids[1], parts, 2, ZLINK_SEND_FLAGS_NONE, 10000, NULL,
                                &completion_id);
        zlink_multipart_close (parts, 2);
        zlink::object_t::test_set_command_delivery_hook (NULL, NULL);
    });
    bool entered;
    bool sync_owned;
    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        entered = barrier.changed.wait_for (lock, std::chrono::seconds (5),
                                            [&] { return barrier.entered; });
        sync_owned = barrier.sync_owned;
    }
    // The bound is only hang cleanup. Lock ownership decides the result.
    bool same_socket_entered = false;
    if (entered && !sync_owned) {
        zlink::socket_public_send_scope_t scope (*barrier.coordinator, true,
                                                 zlink::socket_send_admission_complete);
        same_socket_entered = scope.acquired ();
    }
    {
        std::lock_guard<std::mutex> lock (barrier.mutex);
        barrier.released = true;
        barrier.changed.notify_all ();
    }
    submitter.join ();
    pair.pump ();
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t received[2];
    size_t count = 0;
    const zlink_recv_result_t received_result =
      zlink_router_recv (target, &rid, &token, received, 2, &count, ZLINK_RECV_FLAGS_DONTWAIT);
    if (received_result == ZLINK_RECV_OK)
        zlink_multipart_close (received, count);
    test_context_socket_close_zero_linger (source);
    test_context_socket_close_zero_linger (target);
    TEST_ASSERT_TRUE (entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, received_result);
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_FALSE_MESSAGE (sync_owned,
                               "wake delivery must execute after releasing public API sync");
    TEST_ASSERT_TRUE (same_socket_entered);
}
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_delivery_probe_accepts_unlocked_writer);
    RUN_TEST (test_request_wake_delivery_releases_public_sync);
    return UNITY_END ();
}
