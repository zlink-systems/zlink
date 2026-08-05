/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void expect_no_additional_probe (void *server_)
{
    unsigned char buffer[1];
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (server_, buffer, sizeof (buffer), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}
}

void test_probe_router_router ()
{
    //  Create server and bind to endpoint
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);

    char my_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, my_endpoint, sizeof (my_endpoint));

    //  Create client and connect to server, doing a probe
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "X", 1));
    int probe = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_PROBE, &probe, sizeof (probe)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, my_endpoint));

    //  We expect a routing id=X + empty message from client
    recv_string_expect_success (server, "X", 0);
    unsigned char buffer[255];
    TEST_ASSERT_EQUAL_INT (0, TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, buffer, 255, 0)));
    expect_no_additional_probe (server);

    //  Send a message to client now
    send_string_expect_success (server, "X", ZLINK_SNDMORE);
    send_string_expect_success (server, "Hello", 0);

    // receive the routing ID, which is auto-generated in this case, since the
    // peer did not set one explicitly
    TEST_ASSERT_EQUAL_INT (16, TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (client, buffer, 255, 0)));

    recv_string_expect_success (client, "Hello", 0);

    test_context_socket_close (server);
    test_context_socket_close (client);
}

void test_probe_router_dealer ()
{
    //  Create server and bind to endpoint
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);

    char my_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, my_endpoint, sizeof (my_endpoint));

    //  Create client and connect to server, doing a probe
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "X", 1));
    int probe = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_PROBE, &probe, sizeof (probe)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, my_endpoint));

    //  We expect a routing id=X + empty message from client
    recv_string_expect_success (server, "X", 0);
    unsigned char buffer[255];
    TEST_ASSERT_EQUAL_INT (0, TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, buffer, 255, 0)));
    expect_no_additional_probe (server);

    //  Send a message to client now
    send_string_expect_success (server, "X", ZLINK_SNDMORE);
    send_string_expect_success (server, "Hello", 0);

    recv_string_expect_success (client, "Hello", 0);

    test_context_socket_close (server);
    test_context_socket_close (client);
}

void test_probe_router_router_inproc ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int timeout_ms = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "inproc://probe-router-router"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "X", 1));
    int probe = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_PROBE, &probe, sizeof (probe)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "inproc://probe-router-router"));

    recv_string_expect_success (server, "X", 0);
    unsigned char buffer[1];
    TEST_ASSERT_EQUAL_INT (
      0, TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, buffer, sizeof (buffer), 0)));
    expect_no_additional_probe (server);

    test_context_socket_close (server);
    test_context_socket_close (client);
}


void test_probe_dealer_inproc_connect_before_bind_preserves_application_gate ()
{
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "X", 1));
    int probe = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_PROBE, &probe, sizeof (probe)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client, "inproc://probe-dealer-deferred-bind"));

    // The transport probe may be queued internally, but ordinary Application
    // messages remain blocked until both transport lanes pass admission.
    TEST_ASSERT_FAILURE_ERRNO (
      EAGAIN, zlink_send (client, "blocked", 7, ZLINK_DONTWAIT));

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int timeout_ms = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server, "inproc://probe-dealer-deferred-bind"));

    recv_string_expect_success (server, "X", 0);
    unsigned char buffer[1];
    TEST_ASSERT_EQUAL_INT (
      0, TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (server, buffer, sizeof (buffer), 0)));
    expect_no_additional_probe (server);

    // Once admission is Ready, the same Application lane accepts and delivers
    // ordinary payload without replaying the earlier rejected send.
    send_string_expect_success (client, "ready", 0);
    recv_string_expect_success (server, "X", 0);
    recv_string_expect_success (server, "ready", 0);
    expect_no_additional_probe (server);

    test_context_socket_close (server);
    test_context_socket_close (client);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

    RUN_TEST (test_probe_router_router);
    RUN_TEST (test_probe_router_dealer);
    RUN_TEST (test_probe_router_router_inproc);
    RUN_TEST (test_probe_dealer_inproc_connect_before_bind_preserves_application_gate);

    return UNITY_END ();
}
