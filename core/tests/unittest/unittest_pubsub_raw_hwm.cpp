/* SPDX-License-Identifier: MPL-2.0 */

// Raw empty frames charge only message storage. Public publish-part adds a
// topic frame, so this exact byte-HWM contract belongs to the Core unit layer.

#include "../testutil_unity.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/send_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
int send_raw_frame (void *socket_, const void *data_, size_t size_, int flags_)
{
    zlink_msg_t frame;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&frame, size_));
    if (size_)
        memcpy (zlink_msg_data (&frame), data_, size_);
    socket_handle_t handle = as_socket_handle (socket_);
    TEST_ASSERT_NOT_NULL (handle.socket);
    const int rc = zlink::send_msg_internal (handle.socket, &frame, flags_);
    const int saved_errno = errno;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&frame));
    errno = saved_errno;
    return rc;
}

int recv_raw_frame (void *socket_, void *data_, size_t size_, int flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    TEST_ASSERT_NOT_NULL (handle.socket);
    return zlink::recv_buffer_internal (handle.socket, data_, size_, flags_);
}

void receive_subscription (void *pub_)
{
    char command = 0;
    TEST_ASSERT_EQUAL_INT (1, recv_raw_frame (pub_, &command, 1, 0));
    TEST_ASSERT_EQUAL_UINT8 (1, static_cast<unsigned char> (command));
}

void test_nodrop_raw_empty_frame_hwm ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);

    const uint64_t hwm = 2000u * sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));


    //  set pub socket options
    int wait = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_NODROP, &wait, sizeof (wait)));

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    contract_socket_pair_t pair (pub, sub, 0, 0, true, hwm);

    //  Subscribe for all messages.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    //  we must wait for the subscription to be processed here, otherwise some
    //  or all published messages might be lost
    pair.pump ();
    receive_subscription (pub);

    int hwmlimit = 1999;
    int send_count = 0;

    //  Send an empty message
    for (int i = 0; i < hwmlimit; i++) {
        TEST_ASSERT_SUCCESS_ERRNO (send_raw_frame (pub, static_cast<const void *> (NULL), 0, 0));
        send_count++;
    }

    int recv_count = 0;
    do {
        //  Receive the message in the subscriber
        int rc = recv_raw_frame (sub, NULL, 0, 0);
        if (rc == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            break;
        }
        TEST_ASSERT_EQUAL_INT (0, rc);
        recv_count++;

        if (recv_count == 1) {
            const int sub_rcvtimeo = 250;
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &sub_rcvtimeo, sizeof (sub_rcvtimeo)));
        }

    } while (true);

    TEST_ASSERT_EQUAL_INT (send_count, recv_count);

    //  Now test real blocking behavior
    //  Set a timeout, default is infinite
    int timeout = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));

    send_count = 0;
    recv_count = 0;
    hwmlimit = 2000;

    //  Send an empty message until we get an error, which must be EAGAIN
    while (send_raw_frame (pub, "", 0, 0) == 0)
        send_count++;
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    if (send_count > 0) {
        //  Receive first message with blocking
        TEST_ASSERT_SUCCESS_ERRNO (recv_raw_frame (sub, NULL, 0, 0));
        recv_count++;

        while (recv_raw_frame (sub, NULL, 0, ZLINK_DONTWAIT) == 0)
            recv_count++;
    }

    TEST_ASSERT_EQUAL_INT (send_count, recv_count);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

void test_default_publish_drops_instead_of_backpressuring ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);

    const uint64_t hwm = 200u * sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));

    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    //  Bound both ends: an unbounded receive queue would absorb every message
    //  and the publisher pipe would never reach its HWM.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    contract_socket_pair_t pair (pub, sub, 0, 0, true, hwm);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    //  Wait for the subscription so the count below is not lost to the race.
    pair.pump ();
    receive_subscription (pub);

    //  Do not drain the subscriber. Every send must still succeed.
    const int send_target = 4000;
    for (int i = 0; i < send_target; i++)
        TEST_ASSERT_SUCCESS_ERRNO (send_raw_frame (pub, static_cast<const void *> (NULL), 0, 0));

    const int sub_rcvtimeo = 250;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &sub_rcvtimeo, sizeof (sub_rcvtimeo)));

    int recv_count = 0;
    while (recv_raw_frame (sub, NULL, 0, 0) == 0)
        recv_count++;
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    //  The HWM is far below the send count, so the socket must have dropped.
    TEST_ASSERT_TRUE (recv_count < send_target);

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

void test_a12_xsub_subscription_reports_success_but_is_lost_at_hwm ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_XSUB);
    // One subscription contains its command byte and a one-byte filter.
    const uint64_t hwm = sizeof (zlink_msg_t) + 2;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    contract_socket_pair_t pair (pub, sub, 0, 0, true, hwm);

    // Do not pump the publisher owner until both calls finish. This fills
    // the upstream pipe without a scheduling race or a transport buffer.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "a"));
    TEST_ASSERT_EQUAL_UINT64 (1, pair.application[1]->get_msgs_written ());
    TEST_ASSERT_FALSE (pair.application[1]->check_hwm ());

    // A12 diagnosis only: this records the current success-plus-loss behavior
    // in XSUB spec section 2; it does not define a future delivery guarantee.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "b"));
    TEST_ASSERT_EQUAL_UINT64 (1, pair.application[1]->get_msgs_written ());
    int topics = 0;
    size_t topics_size = sizeof (topics);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_sub_option (sub, ZLINK_SUB_OPT_TOPICS_COUNT, &topics, &topics_size));
    TEST_ASSERT_EQUAL_INT (2, topics);

    pair.pump ();
    char command[2] = {0, 0};
    TEST_ASSERT_EQUAL_INT (2, recv_raw_frame (pub, command, sizeof (command), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT8 (1, static_cast<unsigned char> (command[0]));
    TEST_ASSERT_EQUAL_UINT8 ('a', static_cast<unsigned char> (command[1]));
    TEST_ASSERT_EQUAL_INT (-1, recv_raw_frame (pub, command, sizeof (command), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    // Recovered credit allows a fresh subscription, but does not replay b.
    TEST_ASSERT_TRUE (pair.application[1]->check_hwm ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "c"));
    pair.pump ();
    TEST_ASSERT_EQUAL_INT (2, recv_raw_frame (pub, command, sizeof (command), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT8 (1, static_cast<unsigned char> (command[0]));
    TEST_ASSERT_EQUAL_UINT8 ('c', static_cast<unsigned char> (command[1]));
    TEST_ASSERT_EQUAL_INT (-1, recv_raw_frame (pub, command, sizeof (command), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

void test_subscription_snapshot_runs_with_sub_receive_turn ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (sub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://xsub-subscription-turn"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://xsub-subscription-turn"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "topic-"));

    std::atomic<bool> start (false);
    std::atomic<bool> stop (false);
    std::atomic<bool> subscriptions_done (false);
    std::atomic<unsigned int> snapshots (0);
    std::atomic<unsigned int> receive_attempts (0);
    std::atomic<unsigned int> received (0);
    std::atomic<int> errors (0);

    std::thread subscription_thread ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (unsigned int i = 0; i < 128; ++i) {
            char filter[32];
            snprintf (filter, sizeof (filter), "topic-%04u", i);
            if (zlink_set_subscription (sub, filter) != ZLINK_CONFIG_OK) {
                errors.fetch_add (1, std::memory_order_relaxed);
                break;
            }
            std::this_thread::yield ();
        }
        subscriptions_done.store (true, std::memory_order_release);
    });

    std::thread snapshot_thread ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        while (!stop.load (std::memory_order_acquire)) {
            char filter[64];
            size_t filter_len = sizeof (filter);
            int is_pattern = -1;
            if (zlink_subscription_at (sub, 0, filter, &filter_len, &is_pattern) != ZLINK_CONFIG_OK)
                errors.fetch_add (1, std::memory_order_relaxed);
            snapshots.fetch_add (1, std::memory_order_relaxed);
        }
    });

    std::thread receive_thread ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        while (!stop.load (std::memory_order_acquire)) {
            char topic[32];
            size_t topic_len = sizeof (topic);
            zlink_msg_t part;
            size_t part_count = 0;
            const zlink_recv_result_t rc =
              zlink_subscribe (sub, NULL, topic, sizeof (topic), &topic_len, &part, 1, &part_count,
                               ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_OK) {
                received.fetch_add (1, std::memory_order_relaxed);
                zlink_multipart_close (&part, part_count);
            } else if (rc != ZLINK_RECV_NO_DATA) {
                errors.fetch_add (1, std::memory_order_relaxed);
            }
            receive_attempts.fetch_add (1, std::memory_order_relaxed);
        }
    });

    std::thread publish_thread ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        while (!stop.load (std::memory_order_acquire)) {
            zlink_msg_t part;
            if (zlink_msg_init_size (&part, 1) != ZLINK_CONFIG_OK) {
                errors.fetch_add (1, std::memory_order_relaxed);
                break;
            }
            *static_cast<char *> (zlink_msg_data (&part)) = 'x';
            (void) zlink_publish (pub, "topic-turn", &part, 1, ZLINK_SEND_FLAGS_DONTWAIT);
            if (zlink_msg_close (&part) != ZLINK_CONFIG_OK)
                errors.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::sleep_for (std::chrono::microseconds (100));
        }
    });

    start.store (true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline
           && (!subscriptions_done.load (std::memory_order_acquire)
               || snapshots.load (std::memory_order_relaxed) < 128
               || receive_attempts.load (std::memory_order_relaxed) < 128
               || received.load (std::memory_order_relaxed) == 0))
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    stop.store (true, std::memory_order_release);

    subscription_thread.join ();
    snapshot_thread.join ();
    receive_thread.join ();
    publish_thread.join ();

    TEST_ASSERT_TRUE (subscriptions_done.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (snapshots.load (std::memory_order_relaxed) >= 128);
    TEST_ASSERT_TRUE (receive_attempts.load (std::memory_order_relaxed) >= 128);
    TEST_ASSERT_TRUE (received.load (std::memory_order_relaxed) > 0);
    TEST_ASSERT_EQUAL_INT (0, errors.load (std::memory_order_relaxed));

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_nodrop_raw_empty_frame_hwm);
    RUN_TEST (test_default_publish_drops_instead_of_backpressuring);
    RUN_TEST (test_a12_xsub_subscription_reports_success_but_is_lost_at_hwm);
    RUN_TEST (test_subscription_snapshot_runs_with_sub_receive_turn);
    return UNITY_END ();
}
