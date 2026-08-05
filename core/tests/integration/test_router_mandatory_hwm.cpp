/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

// DEBUG shouldn't be defined in sources as it will cause a redefined symbol
// error when it is defined in the build configuration. It appears that the
// intent here is to semi-permanently disable DEBUG tracing statements, so the
// implementation is changed to accommodate that intent.
//#define DEBUG 0
#define TRACE_ENABLED 0

namespace
{
int send_routed_payload_expect_maybe_eagain (
  void *router_, const zlink_routing_id_t *rid_, const void *buf_, size_t size_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, size_) != 0)
        return -1;
    if (size_ > 0 && buf_)
        memcpy (zlink_msg_data (&msg), buf_, size_);

    const int rc =
      zlink_send_rid (router_, rid_, &msg, 1, static_cast<zlink_send_flags_t> (flags_));
    if (rc != 0) {
        const int err = errno;
        zlink_msg_close (&msg);
        errno = err;
        return -1;
    }
    return 0;
}

int send_routed_multipart_expect_maybe_eagain (
  void *router_, const zlink_routing_id_t *rid_, const void *buf_, size_t size_)
{
    zlink_msg_t envelope;
    if (zlink_msg_init_size (&envelope, 32) != 0)
        return -1;
    memset (zlink_msg_data (&envelope), 0xA5, zlink_msg_size (&envelope));
    zlink_submit_result_t rc =
      zlink_send_part_rid (router_, rid_, &envelope, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_MORE);
    if (rc != ZLINK_SUBMIT_OK)
        return -1;

    zlink_msg_t payload;
    if (zlink_msg_init_size (&payload, size_) != 0)
        return -1;
    if (size_ > 0 && buf_)
        memcpy (zlink_msg_data (&payload), buf_, size_);
    rc = zlink_send_part_rid (router_, rid_, &payload, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_FINAL);
    return rc == ZLINK_SUBMIT_OK ? 0 : -1;
}
}

void test_router_mandatory_hwm ()
{
    if (TRACE_ENABLED)
        fprintf (stderr, "Staring router mandatory HWM test ...\n");
    char my_endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    // Configure router socket to mandatory routing and set HWM and linger
    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    int linger = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &linger, sizeof (linger)));

    bind_loopback_ipv4 (router, my_endpoint, sizeof my_endpoint);

    //  Create dealer called "X" and connect it to our router, configure HWM
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, my_endpoint));

    //  Get message from dealer to know when connection is ready
    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    int i;
    const int buf_size = 65536;
    const uint8_t buf[buf_size] = {0};
    // Send first batch of messages
    for (i = 0; i < 100000; ++i) {
        if (TRACE_ENABLED)
            fprintf (stderr, "Sending message %d ...\n", i);
        const int rc = zlink_send (router, "X", 1, ZLINK_DONTWAIT | ZLINK_SNDMORE);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (1, rc);
        send_array_expect_success (router, buf, ZLINK_DONTWAIT);
    }
    // This should fail after one message but kernel buffering could
    // skew results
    TEST_ASSERT_LESS_THAN_INT (10, i);
    msleep (1000);
    // Send second batch of messages
    for (; i < 100000; ++i) {
        if (TRACE_ENABLED)
            fprintf (stderr, "Sending message %d (part 2) ...\n", i);
        const int rc = zlink_send (router, "X", 1, ZLINK_DONTWAIT | ZLINK_SNDMORE);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (1, rc);
        send_array_expect_success (router, buf, ZLINK_DONTWAIT);
    }
    // This should fail after two messages but kernel buffering could
    // skew results
    TEST_ASSERT_LESS_THAN_INT (20, i);

    if (TRACE_ENABLED)
        fprintf (stderr, "Done sending messages.\n");

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

void test_router_send_rid_mandatory_hwm ()
{
    char my_endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    int linger = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &linger, sizeof (linger)));

    bind_loopback_ipv4 (router, my_endpoint, sizeof my_endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, my_endpoint));

    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 1;
    rid.data[0] = 'X';

    const int buf_size = 65536;
    const uint8_t buf[buf_size] = {0};
    int i = 0;
    for (; i < 100000; ++i) {
        const int rc =
          send_routed_payload_expect_maybe_eagain (router, &rid, buf, sizeof (buf), ZLINK_DONTWAIT);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (0, rc);
    }
    TEST_ASSERT_LESS_THAN_INT (10, i);

    msleep (1000);

    for (; i < 100000; ++i) {
        const int rc =
          send_routed_payload_expect_maybe_eagain (router, &rid, buf, sizeof (buf), ZLINK_DONTWAIT);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (0, rc);
    }
    TEST_ASSERT_LESS_THAN_INT (20, i);

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

void test_router_send_rid_multipart_hwm_is_backpressure ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                               sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + 2u * sizeof (zlink_msg_t) + 32u;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 1;
    rid.data[0] = 'X';

    const uint8_t payload[65536] = {0};
    bool backpressured = false;
    for (int i = 0; i < 1000; ++i) {
        const int rc = send_routed_multipart_expect_maybe_eagain (
          router, &rid, payload, sizeof (payload));
        if (rc == 0)
            continue;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        backpressured = true;
        break;
    }
    TEST_ASSERT_TRUE_MESSAGE (
      backpressured, "multipart routed send did not reach HWM");

    const int repeated_rc = send_routed_multipart_expect_maybe_eagain (
      router, &rid, payload, sizeof (payload));
    TEST_ASSERT_EQUAL_INT (-1, repeated_rc);
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      EAGAIN, zlink_errno (),
      "a route held inactive by HWM must remain backpressured");

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_mandatory_hwm);
    RUN_TEST (test_router_send_rid_mandatory_hwm);
    RUN_TEST (test_router_send_rid_multipart_hwm_is_backpressure);
    return UNITY_END ();
}
