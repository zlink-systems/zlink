/* SPDX-License-Identifier: MPL-2.0 */

//  Contract tests for CONNECT_ROUTING_ID (next-connect alias) binding.
//
//  Spec: core/doc/spec/core/socket/07-router.ko.md:118-121 - the
//  CONNECT_ROUTING_ID is the local alias for the pipe created by the *next*
//  zlink_connect(); the alias must bind to that connect's pipe. These tests
//  pin two properties that a socket-global one-shot slot cannot guarantee:
//
//    1. The alias survives a forced reconnect and keeps driving the directed
//       route, even when the alias differs from the peer's own identity.
//    2. Two back-to-back connects (each with its own alias, both issued before
//       the first is admitted) bind each alias to its own endpoint/pipe.

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void make_rid (const char *value_, zlink_routing_id_t *out_)
{
    memset (out_, 0, sizeof (*out_));
    out_->size = static_cast<uint8_t> (strlen (value_));
    memcpy (out_->data, value_, out_->size);
}

void recv_router_payload_expect_success (void *router_, const char *payload_)
{
    recv_routed_string_expect_success (router_, payload_);
}

//  Send a directed payload by RID until the route is admitted, then confirm the
//  receiver observes it. Fails the test if the route never becomes ready.
void directed_send_when_ready (void *router_,
                              const zlink_routing_id_t *rid_,
                              void *receiver_,
                              const char *payload_)
{
    for (int i = 0; i < 200; ++i) {
        if (test_stream_send_bytes (router_, rid_, payload_, strlen (payload_),
                                    ZLINK_DONTWAIT)
            == static_cast<int> (strlen (payload_))) {
            recv_router_payload_expect_success (receiver_, payload_);
            return;
        }
        msleep (10);
    }
    TEST_FAIL_MESSAGE ("router never became ready for the directed alias route");
}
}

//  Blast radius item 1: alias must survive a forced disconnect + reconnect and
//  keep the same logical RID driving the directed route. The alias ("ALIAS1")
//  deliberately differs from the peer identity ("SRVID") so the test cannot
//  pass merely because the alias happens to equal the peer's advertised id.
void alias_survives_reconnect_directed_route ()
{
    const int zero = 0;
    const int reconnect_ivl = 20;
    const int sndtimeo = 1000;

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_rebind = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_rebind, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "SRVID", 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_rebind, "SRVID", 5));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (reconnect_ivl)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_SNDTIMEO, &sndtimeo, sizeof (sndtimeo)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "ALIAS1", 6));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_1));

    zlink_routing_id_t rid;
    make_rid ("ALIAS1", &rid);

    //  Directed route via the alias works on the first connection.
    directed_send_when_ready (client, &rid, server, "first");

    //  Force the transport down and stand a fresh peer up on the same endpoint.
    test_context_socket_close_zero_linger (server);
    msleep (SETTLE_TIME * 2);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_rebind, ENDPOINT_1));

    //  After reconnect the same alias must still drive the directed route.
    directed_send_when_ready (client, &rid, server_rebind, "after-reconnect");

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_rebind);
}

//  Blast radius item 2: two aliases set back-to-back (each before the previous
//  connection is admitted) must each bind to their own endpoint/pipe. Verified
//  two ways: (a) the client's CONNECTION_READY monitor reports both distinct
//  aliases, and (b) each directed RID reaches only its own server.
void back_to_back_aliases_bind_distinct_routes ()
{
    const int zero = 0;
    const int sndtimeo = 1000;
    const int rcvtimeo = SETTLE_TIME;
    char buffer[256];

    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_b = test_context_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server_a, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server_b, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_SNDTIMEO, &sndtimeo, sizeof (sndtimeo)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_a, ZLINK_OPT_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_b, ZLINK_OPT_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_a, "SRV-A", 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_b, "SRV-B", 5));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_a, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_b, ENDPOINT_2));

    void *monitor = NULL;
    test_monitor_probe_t probe;
    monitor = open_test_monitor_probe (
      client, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY, &probe);

    //  Back-to-back: alias A -> connect(A), then alias B -> connect(B) before
    //  the first connection is admitted. A socket-global one-shot slot loses A.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "ALIASA", 6));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "ALIASB", 6));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_2));

    //  Both CONNECTION_READY events must arrive, each carrying its own alias.
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (&probe, 2, 3000));
    bool saw_alias_a = false;
    bool saw_alias_b = false;
    const int record_count = test_monitor_probe_count (&probe);
    for (int i = 0; i < record_count; ++i) {
        const zlink_monitor_event_t record =
          test_monitor_probe_record_at (&probe, i);
        if (record.routing_id.size == 6
            && memcmp (record.routing_id.data, "ALIASA", 6) == 0)
            saw_alias_a = true;
        else if (record.routing_id.size == 6
                 && memcmp (record.routing_id.data, "ALIASB", 6) == 0)
            saw_alias_b = true;
    }
    TEST_ASSERT_TRUE_MESSAGE (saw_alias_a,
                              "CONNECTION_READY missing alias ALIASA");
    TEST_ASSERT_TRUE_MESSAGE (saw_alias_b,
                              "CONNECTION_READY missing alias ALIASB");

    close_test_monitor_probe (&monitor, &probe);

    //  Each logical RID drives its own endpoint's directed route.
    zlink_routing_id_t rid_a;
    zlink_routing_id_t rid_b;
    make_rid ("ALIASA", &rid_a);
    make_rid ("ALIASB", &rid_b);

    directed_send_when_ready (client, &rid_a, server_a, "to-a");
    //  The A payload must not have leaked to server B.
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN,
                               test_recv_router (server_b, buffer, sizeof (buffer), 0));

    directed_send_when_ready (client, &rid_b, server_b, "to-b");
    //  And the B payload must not have leaked to server A.
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN,
                               test_recv_router (server_a, buffer, sizeof (buffer), 0));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_a);
    test_context_socket_close_zero_linger (server_b);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (alias_survives_reconnect_directed_route);
    RUN_TEST (back_to_back_aliases_bind_distinct_routes);
    return UNITY_END ();
}
