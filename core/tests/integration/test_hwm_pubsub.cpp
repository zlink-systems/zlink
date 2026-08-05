/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

// NOTE: on OSX the endpoint returned by ZLINK_INTERNAL_OPT_LAST_ENDPOINT may be quite long,
//       ensure we have extra space for that:
#define SOCKET_STRING_LEN (MAX_SOCKET_STRING * 4)

SETUP_TEARDOWN_TESTCONTEXT

void test_default_socket_hwm_is_byte_budget ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    uint64_t sndhwm = 0;
    size_t sndhwm_size = sizeof (sndhwm);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (socket, ZLINK_OPT_SNDHWM, &sndhwm, &sndhwm_size));
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull, sndhwm);

    uint64_t rcvhwm = 0;
    size_t rcvhwm_size = sizeof (rcvhwm);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (socket, ZLINK_OPT_RCVHWM, &rcvhwm, &rcvhwm_size));
    TEST_ASSERT_EQUAL_UINT64 (256ull * 4096ull, rcvhwm);

    test_context_socket_close (socket);
}

int test_defaults (int send_hwm_, int msg_cnt_, const char *endpoint_)
{
    char pub_endpoint[SOCKET_STRING_LEN];
    const uint64_t hwm_bytes =
      static_cast<uint64_t> (send_hwm_) * (13u + sizeof (zlink_msg_t));

    // Set up and bind XPUB socket
    void *pub_socket = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub_socket, ZLINK_OPT_SNDHWM, &hwm_bytes, sizeof (hwm_bytes)));
    test_bind (pub_socket, endpoint_, pub_endpoint, sizeof pub_endpoint);

    // Set up and connect SUB socket
    void *sub_socket = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub_socket, ZLINK_OPT_RCVHWM, &hwm_bytes, sizeof (hwm_bytes)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_socket, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_socket, pub_endpoint));

    // Wait before starting TX operations till 1 subscriber has subscribed
    // (in this test there's 1 subscriber only)
    const char subscription_to_all_topics[] = {1, 0};
    recv_string_expect_success (pub_socket, subscription_to_all_topics, 0);

    // Send until we reach "mute" state
    int send_count = 0;
    while (send_count < msg_cnt_
           && zlink_send (pub_socket, "test message", 13, ZLINK_DONTWAIT) == 13)
        ++send_count;

    TEST_ASSERT_EQUAL_INT (send_hwm_, send_count);
    msleep (SETTLE_TIME);

    // Now receive all sent messages
    int recv_count = 0;
    char dummybuff[64];
    while (13 == zlink_recv (sub_socket, &dummybuff, 64, ZLINK_DONTWAIT)) {
        ++recv_count;
    }

    TEST_ASSERT_EQUAL_INT (send_hwm_, recv_count);

    // Clean up
    test_context_socket_close (sub_socket);
    test_context_socket_close (pub_socket);

    return recv_count;
}

int receive (void *socket_, int *is_termination_)
{
    int recv_count = 0;
    *is_termination_ = 0;

    // Now receive all sent messages
    char buffer[255];
    int len;
    while ((len = zlink_recv (socket_, buffer, sizeof (buffer), 0)) >= 0) {
        ++recv_count;

        if (len == 3 && strncmp (buffer, "end", len) == 0) {
            *is_termination_ = 1;
            return recv_count;
        }
    }

    return recv_count;
}

int test_blocking (int send_hwm_, int msg_cnt_, const char *endpoint_)
{
    char pub_endpoint[SOCKET_STRING_LEN];
    const uint64_t hwm_bytes =
      static_cast<uint64_t> (send_hwm_) * sizeof (zlink_msg_t);

    // Set up bind socket
    void *pub_socket = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub_socket, ZLINK_OPT_SNDHWM, &hwm_bytes, sizeof (hwm_bytes)));
    test_bind (pub_socket, endpoint_, pub_endpoint, sizeof pub_endpoint);

    // Set up connect socket
    void *sub_socket = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub_socket, ZLINK_OPT_RCVHWM, &hwm_bytes, sizeof (hwm_bytes)));
    int wait = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub_socket, ZLINK_PUB_OPT_NODROP, &wait, sizeof (wait)));
    int timeout_ms = 10;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub_socket, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_socket, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_socket, pub_endpoint));

    // Wait before starting TX operations till 1 subscriber has subscribed
    // (in this test there's 1 subscriber only)
    const uint8_t subscription_to_all_topics[] = {1};
    recv_array_expect_success (pub_socket, subscription_to_all_topics, 0);

    // Send until we block
    int send_count = 0;
    int recv_count = 0;
    int blocked_count = 0;
    int is_termination = 0;
    while (send_count < msg_cnt_) {
        const int rc = zlink_send (pub_socket, static_cast<const void *> (NULL), 0, ZLINK_DONTWAIT);
        if (rc == 0) {
            ++send_count;
        } else if (-1 == rc) {
            // if the PUB socket blocks due to HWM, errno should be EAGAIN:
            blocked_count++;
            TEST_ASSERT_FAILURE_ERRNO (EAGAIN, -1);
            recv_count += receive (sub_socket, &is_termination);
        }
    }

    // if send_hwm_ < msg_cnt_, we should block at least once:
    char counts_string[128];
    snprintf (counts_string, sizeof counts_string - 1, "sent = %i, received = %i", send_count,
              recv_count);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE (0, blocked_count, counts_string);

    // dequeue SUB socket again, to make sure XPUB has space to send the termination message
    recv_count += receive (sub_socket, &is_termination);

    // send termination message
    send_string_expect_success (pub_socket, "end", 0);

    // now block on the SUB side till we get the termination message
    while (is_termination == 0)
        recv_count += receive (sub_socket, &is_termination);

    // remove termination message from the count:
    recv_count--;

    TEST_ASSERT_EQUAL_INT (send_count, recv_count);

    // Clean up
    test_context_socket_close (sub_socket);
    test_context_socket_close (pub_socket);

    return recv_count;
}

// hwm should apply to the messages that have already been received
// with hwm 11024: send 9999 msg, receive 9999, send 1100, receive 1100
void test_reset_hwm ()
{
    const int first_count = 9999;
    const int second_count = 1100;
    const uint64_t hwm = 11024u * sizeof (zlink_msg_t);
    char my_endpoint[SOCKET_STRING_LEN];

    // Set up bind socket
    void *pub_socket = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub_socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    bind_loopback_ipv4 (pub_socket, my_endpoint, MAX_SOCKET_STRING);

    // Set up connect socket
    void *sub_socket = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub_socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_socket, my_endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_socket, ""));

    msleep (SETTLE_TIME);

    // Send messages
    int send_count = 0;
    while (send_count < first_count
           && zlink_send (pub_socket, static_cast<const void *> (NULL), 0, ZLINK_DONTWAIT) == 0)
        ++send_count;
    TEST_ASSERT_EQUAL_INT (first_count, send_count);

    msleep (SETTLE_TIME);

    // Now receive all sent messages
    int recv_count = 0;
    while (0 == zlink_recv (sub_socket, NULL, 0, ZLINK_DONTWAIT)) {
        ++recv_count;
    }
    TEST_ASSERT_EQUAL_INT (first_count, recv_count);

    msleep (SETTLE_TIME);

    // Send messages
    send_count = 0;
    while (send_count < second_count
           && zlink_send (pub_socket, static_cast<const void *> (NULL), 0, ZLINK_DONTWAIT) == 0)
        ++send_count;
    TEST_ASSERT_EQUAL_INT (second_count, send_count);

    msleep (SETTLE_TIME);

    // Now receive all sent messages
    recv_count = 0;
    while (0 == zlink_recv (sub_socket, NULL, 0, ZLINK_DONTWAIT)) {
        ++recv_count;
    }
    TEST_ASSERT_EQUAL_INT (second_count, recv_count);

    // Clean up
    test_context_socket_close (sub_socket);
    test_context_socket_close (pub_socket);
}

void test_defaults_large (const char *bind_endpoint_)
{
    // send 1000 msg on hwm 1000, receive 1000
    TEST_ASSERT_EQUAL_INT (1000, test_defaults (1000, 1000, bind_endpoint_));
}

void test_defaults_small (const char *bind_endpoint_)
{
    // send 1000 msg on hwm 100, receive 100
    TEST_ASSERT_EQUAL_INT (100, test_defaults (100, 100, bind_endpoint_));
}

void test_blocking (const char *bind_endpoint_)
{
    // send 6000 msg on hwm 2000, drops above hwm, only receive hwm:
    TEST_ASSERT_EQUAL_INT (6000, test_blocking (2000, 6000, bind_endpoint_));
}

#define DEFINE_REGULAR_TEST_CASES(name, bind_endpoint)                                             \
    void test_defaults_large_##name ()                                                             \
    {                                                                                              \
        test_defaults_large (bind_endpoint);                                                       \
    }                                                                                              \
                                                                                                   \
    void test_defaults_small_##name ()                                                             \
    {                                                                                              \
        test_defaults_small (bind_endpoint);                                                       \
    }                                                                                              \
                                                                                                   \
    void test_blocking_##name ()                                                                   \
    {                                                                                              \
        test_blocking (bind_endpoint);                                                             \
    }

#define RUN_REGULAR_TEST_CASES(name)                                                               \
    RUN_TEST (test_defaults_large_##name);                                                         \
    RUN_TEST (test_defaults_small_##name);                                                         \
    RUN_TEST (test_blocking_##name)

DEFINE_REGULAR_TEST_CASES (tcp, "tcp://127.0.0.1:*")
DEFINE_REGULAR_TEST_CASES (inproc, "inproc://a")

#if !defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_HAVE_GNU)
DEFINE_REGULAR_TEST_CASES (ipc, "ipc://*")
#endif

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

    RUN_TEST (test_default_socket_hwm_is_byte_budget);
    RUN_REGULAR_TEST_CASES (tcp);
    RUN_REGULAR_TEST_CASES (inproc);

#if !defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_HAVE_GNU)
    RUN_REGULAR_TEST_CASES (ipc);
#endif
    RUN_TEST (test_reset_hwm);
    return UNITY_END ();
}
