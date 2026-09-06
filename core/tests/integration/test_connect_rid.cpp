/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

const char *rconn1routing_id = "conn1";
const char *x_routing_id = "X";
const char *y_routing_id = "Y";
const char *z_routing_id = "Z";
const char *client_routing_id = "C";
const char *dup_routing_id = "dup";


void test_router_2_router (bool named_)
{
    char buff[256];
    const char msg[] = "hi 1";
    const int zero = 0;
    char my_endpoint[MAX_SOCKET_STRING];

    //  Create bind socket.
    void *rbind = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (rbind, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (rbind, my_endpoint, sizeof my_endpoint);

    //  Create connection socket.
    void *rconn1 = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (rconn1, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    //  If we're in named mode, set some identities.
    if (named_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (rbind, x_routing_id, 1));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (rconn1, y_routing_id, 1));
    }

    //  Make call to connect using a connect_routing_id.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      rconn1, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, rconn1routing_id, strlen (rconn1routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (rconn1, my_endpoint));
    /*  Uncomment to test assert on duplicate routing id
    //  Test duplicate connect attempt.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (rconn1, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, rconn1routing_id, strlen (rconn1routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (rconn1, bindip));
*/
    //  Send some data.

    send_routed_string_expect_success (rconn1, rconn1routing_id, msg);

    const zlink_routing_id_t source = recv_routed_string_expect_success (rbind, msg);
    const int routing_id_len = source.size;
    memcpy (buff, source.data, source.size);
    if (named_) {
        TEST_ASSERT_EQUAL_INT (strlen (y_routing_id), routing_id_len);
        TEST_ASSERT_EQUAL_STRING_LEN (y_routing_id, buff, routing_id_len);
    } else {
        TEST_ASSERT_EQUAL_INT (16, routing_id_len);
    }

    TEST_ASSERT_EQUAL_INT (2, test_stream_send_bytes (rbind, &source, "ok", 2, 0));

    //  If bound socket identity naming a problem, we'll likely see something funky here.
    recv_routed_string_expect_success (rconn1, "ok", rconn1routing_id);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_unbind (rbind, my_endpoint));
    test_context_socket_close (rbind);
    test_context_socket_close (rconn1);
}

void test_router_2_router_while_receiving ()
{
    char buff[256];
    const char msg[] = "hi 1";
    const int zero = 0;
    char x_endpoint[MAX_SOCKET_STRING];
    char z_endpoint[MAX_SOCKET_STRING];

    //  Create xbind socket.
    void *xbind = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (xbind, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (xbind, x_endpoint, sizeof x_endpoint);

    //  Create zbind socket.
    void *zbind = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (zbind, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (zbind, z_endpoint, sizeof z_endpoint);

    //  Create connection socket.
    void *yconn = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (yconn, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    // set identities for each socket
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (xbind, x_routing_id, strlen (x_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (yconn, y_routing_id, 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (zbind, z_routing_id, strlen (z_routing_id)));

    //  Connect Y to X using a routing id
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (yconn, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        x_routing_id, strlen (x_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (yconn, x_endpoint));

    //  Send some data from Y to X.
    send_routed_string_expect_success (yconn, x_routing_id, msg);

    // wait for the Y->X message to be received
    msleep (SETTLE_TIME);

    // Now X tries to connect to Z and send a message
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (xbind, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        z_routing_id, strlen (z_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (xbind, z_endpoint));

    //  Try to send some data from X to Z.
    send_routed_string_expect_success (xbind, z_routing_id, msg);

    // wait for the X->Z message to be received (so that our non-blocking check will actually
    // fail if the message is routed to Y)
    msleep (SETTLE_TIME);

    // nothing should have been received on the Y socket
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, test_recv_router (yconn, buff, 256, ZLINK_DONTWAIT));

    // the message should have been received on the Z socket
    recv_routed_string_expect_success (zbind, msg, x_routing_id);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_unbind (xbind, x_endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unbind (zbind, z_endpoint));

    test_context_socket_close (yconn);
    test_context_socket_close (xbind);
    test_context_socket_close (zbind);
}

void test_router_2_router_unnamed ()
{
    test_router_2_router (false);
}

void test_router_2_router_named ()
{
    test_router_2_router (true);
}

void test_duplicate_connect_rid_without_handover ()
{
    const int zero = 0;
    const int timeout = SETTLE_TIME;
    char endpoint_one[MAX_SOCKET_STRING];
    char endpoint_two[MAX_SOCKET_STRING];
    char buffer[256];

    void *server_one = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_two = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    bind_loopback_ipv4 (server_one, endpoint_one, sizeof endpoint_one);
    bind_loopback_ipv4 (server_two, endpoint_two, sizeof endpoint_two);

    int duplicate_policy = ZLINK_RID_DUPLICATE_REJECT;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY,
                                                 &duplicate_policy, sizeof (duplicate_policy)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, client_routing_id, strlen (client_routing_id)));

    //  First connect using CONNECT_ROUTING_ID.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        dup_routing_id, strlen (dup_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_one));
    msleep (SETTLE_TIME);

    send_routed_string_expect_success (client, dup_routing_id, "first");
    recv_routed_string_expect_success (server_one, "first", client_routing_id);

    //  Duplicate connect with same CONNECT_ROUTING_ID must not abort.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        dup_routing_id, strlen (dup_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_two));
    msleep (SETTLE_TIME);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));

    send_routed_string_expect_success (client, dup_routing_id, "second");

    //  Without handover, duplicate RID stays on first connection.
    recv_routed_string_expect_success (server_one, "second", client_routing_id);
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, test_recv_router (server_two, buffer, sizeof (buffer), 0));

    test_context_socket_close (client);
    test_context_socket_close (server_two);
    test_context_socket_close (server_one);
}

void test_duplicate_connect_rid_with_handover ()
{
    const int zero = 0;
    const int timeout = SETTLE_TIME;
    const int handover = 1;
    char endpoint_one[MAX_SOCKET_STRING];
    char endpoint_two[MAX_SOCKET_STRING];
    char buffer[256];

    void *server_one = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_two = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    bind_loopback_ipv4 (server_one, endpoint_one, sizeof endpoint_one);
    bind_loopback_ipv4 (server_two, endpoint_two, sizeof endpoint_two);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, client_routing_id, strlen (client_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof (handover)));

    //  First connect using CONNECT_ROUTING_ID.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        dup_routing_id, strlen (dup_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_one));
    msleep (SETTLE_TIME);

    send_routed_string_expect_success (client, dup_routing_id, "first");
    recv_routed_string_expect_success (server_one, "first", client_routing_id);

    //  Duplicate connect with same CONNECT_ROUTING_ID must hand over to second
    //  connection when handover is enabled.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        dup_routing_id, strlen (dup_routing_id)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_two));
    msleep (SETTLE_TIME);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));

    send_routed_string_expect_success (client, dup_routing_id, "second");

    recv_routed_string_expect_success (server_two, "second", client_routing_id);
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, test_recv_router (server_one, buffer, sizeof (buffer), 0));

    test_context_socket_close (client);
    test_context_socket_close (server_two);
    test_context_socket_close (server_one);
}

static void copy_rid (zlink_routing_id_t *rid_, const char *value_)
{
    memset (rid_, 0, sizeof (*rid_));
    rid_->size = static_cast<uint8_t> (strlen (value_));
    memcpy (rid_->data, value_, rid_->size);
}

void test_disconnect_rid_rejects_invalid_and_missing_peer ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    zlink_routing_id_t rid;
    copy_rid (&rid, "missing");

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_ARGUMENT, zlink_disconnect_rid (router, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_NOT_FOUND, zlink_disconnect_rid (router, &rid));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    test_context_socket_close (router);
}

void test_router_disconnect_rid_terminates_matching_peer ()
{
    const int zero = 0;
    char endpoint[MAX_SOCKET_STRING];
    char buffer[256];
    zlink_routing_id_t rid;
    copy_rid (&rid, client_routing_id);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, client_routing_id, strlen (client_routing_id)));

    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    send_string_expect_success (dealer, "hello", 0);
    recv_routed_string_expect_success (router, "hello", client_routing_id);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect_rid (router, &rid));
    msleep (SETTLE_TIME);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_recv_router (router, buffer, sizeof (buffer), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_2_router_unnamed);
    RUN_TEST (test_router_2_router_named);
    RUN_TEST (test_router_2_router_while_receiving);
    RUN_TEST (test_duplicate_connect_rid_without_handover);
    RUN_TEST (test_duplicate_connect_rid_with_handover);
    RUN_TEST (test_disconnect_rid_rejects_invalid_and_missing_peer);
    RUN_TEST (test_router_disconnect_rid_terminates_matching_peer);
    return UNITY_END ();
}
