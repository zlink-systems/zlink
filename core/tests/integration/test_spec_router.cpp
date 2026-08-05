/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <stdlib.h>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

// SHALL receive incoming messages from its peers using a fair-queuing
// strategy.
void test_fair_queue_in (const char *bind_address_)
{
    char connect_address[MAX_SOCKET_STRING];
    void *receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, bind_address_));
    size_t len = MAX_SOCKET_STRING;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (receiver, ZLINK_OPT_LAST_ENDPOINT, connect_address, &len));

    const unsigned char services = 5;
    void *senders[services];
    for (unsigned char peer = 0; peer < services; ++peer) {
        senders[peer] = test_context_socket (ZLINK_SOCKET_DEALER);

        char *str = strdup ("A");
        str[0] += peer;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (senders[peer], str, 2));
        free (str);

        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (senders[peer], connect_address));
    }

    msleep (SETTLE_TIME);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));

    s_send_seq (senders[0], "M", SEQ_END);
    s_recv_seq (receiver, "A", "M", SEQ_END);

    s_send_seq (senders[0], "M", SEQ_END);
    s_recv_seq (receiver, "A", "M", SEQ_END);

    int sum = 0;

    // send N requests
    for (unsigned char peer = 0; peer < services; ++peer) {
        s_send_seq (senders[peer], "M", SEQ_END);
        sum += 'A' + peer;
    }

    TEST_ASSERT_EQUAL_INT (services * 'A' + services * (services - 1) / 2, sum);

    // handle N requests
    for (unsigned char peer = 0; peer < services; ++peer) {
        TEST_ASSERT_EQUAL_INT (
          2, TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, receiver, 0)));
        const char *id = static_cast<const char *> (zlink_msg_data (&msg));
        sum -= id[0];

        s_recv_seq (receiver, "M", SEQ_END);
    }

    TEST_ASSERT_EQUAL_INT (0, sum);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    test_context_socket_close_zero_linger (receiver);

    for (size_t peer = 0; peer < services; ++peer)
        test_context_socket_close_zero_linger (senders[peer]);

    // Wait for disconnects.
    msleep (SETTLE_TIME);
}

#define TEST_SUITE(name, bind_address)                                                             \
    void test_fair_queue_in_##name ()                                                              \
    {                                                                                              \
        test_fair_queue_in (bind_address);                                                         \
    }

TEST_SUITE (inproc, "inproc://a")
TEST_SUITE (tcp, "tcp://127.0.0.1:*")

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_fair_queue_in_tcp);
    RUN_TEST (test_fair_queue_in_inproc);
    return UNITY_END ();
}
