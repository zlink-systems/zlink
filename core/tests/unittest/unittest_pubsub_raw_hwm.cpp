/* SPDX-License-Identifier: MPL-2.0 */

// Raw empty frames charge only message storage. Public publish-part adds a
// topic frame, so this exact byte-HWM contract belongs to the Core unit layer.

#include "../testutil_unity.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/send_internal.hpp"

#include <cstring>

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

}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_nodrop_raw_empty_frame_hwm);
    RUN_TEST (test_default_publish_drops_instead_of_backpressuring);
    return UNITY_END ();
}
