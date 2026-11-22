/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"
#include "api/socket/socket_request_reply_internal.hpp"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>


SETUP_TEARDOWN_TESTCONTEXT

namespace
{
namespace reqrep = zlink::socket_reqrep_internal;

size_t consume_notifications (zlink::socket_base_t *socket_)
{
    size_t count = 0;
    zlink::command_t command;
    while (socket_->get_mailbox ()->recv (&command, 0) == 0) {
        TEST_ASSERT_EQUAL_INT (zlink::command_t::request_completion, command.type);
        socket_->acknowledge_request_completion_notification ();
        ++count;
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    return count;
}

void publish (const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
              zlink_completion_id_t *id_)
{
    reqrep::pending_request_t pending;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state_->socket->completion_runtime (), ZLINK_COMPLETION_REQUEST, NULL, NULL,
      &pending.pull_completion, id_));
    TEST_ASSERT_SUCCESS_ERRNO (reqrep::publish_pending_request_completion (
      state_, &pending, ZLINK_REQUEST_TIMED_OUT, NULL, 0));
}

void receive (zlink::socket_base_t *socket_, zlink_completion_id_t id_)
{
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
      &socket_->completion_runtime (), &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, completion.request_result);
    zlink_completion_close (&completion);
}
}

void test_completion_arriving_during_drain_only_notifies_empty_transition ()
{
    void *handle = test_context_socket (ZLINK_SOCKET_DEALER);
    zlink::socket_base_t *socket = as_socket_handle (handle).socket;
    const std::shared_ptr<reqrep::socket_request_reply_state_t> state (
      new reqrep::socket_request_reply_state_t (socket, ZLINK_SOCKET_DEALER));
    const size_t request_count = 64;
    zlink_completion_id_t ids[request_count] = {};
    publish (state, &ids[0]);
    size_t notifications = consume_notifications (socket);
    TEST_ASSERT_EQUAL_UINT64 (1, notifications);
    // The first wake has been consumed, but its completion is still ready.
    publish (state, &ids[1]);
    notifications += consume_notifications (socket);

    std::mutex mutex;
    std::condition_variable changed;
    size_t requested = 1;
    size_t published = 1;
    std::thread producer ([&] {
        for (size_t i = 2; i < request_count; ++i) {
            std::unique_lock<std::mutex> lock (mutex);
            changed.wait (lock, [&] { return requested == i; });
            lock.unlock ();
            publish (state, &ids[i]);
            lock.lock ();
            published = i;
            changed.notify_all ();
        }
    });
    // Retain one ready record while another thread appends after each dequeue.
    // The command owner is inactive between turns; the completion owner is awake.
    for (size_t i = 2; i < request_count; ++i) {
        receive (socket, ids[i - 2]);
        {
            std::unique_lock<std::mutex> lock (mutex);
            requested = i;
            changed.notify_all ();
            changed.wait (lock, [&] { return published == i; });
        }
        notifications += consume_notifications (socket);
    }
    producer.join ();
    receive (socket, ids[request_count - 2]);
    receive (socket, ids[request_count - 1]);
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&socket->completion_runtime ()));
    printf ("requests=%zu request_completion_commands=%zu commands_per_request=%.6f\n",
            request_count, notifications, double (notifications) / request_count);
    TEST_ASSERT_EQUAL_UINT64 (1, notifications);

    // After the last dequeue a new completion must create a fresh wake.
    zlink_completion_id_t next_id = 0;
    publish (state, &next_id);
    TEST_ASSERT_EQUAL_UINT64 (1, consume_notifications (socket));
    receive (socket, next_id);
    test_context_socket_close_zero_linger (handle);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_completion_arriving_during_drain_only_notifies_empty_transition);
    return UNITY_END ();
}
