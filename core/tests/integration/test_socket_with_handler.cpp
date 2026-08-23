/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
void discard_stream_message (const zlink_routing_id_t *,
                             zlink_msg_t *parts_,
                             size_t part_count_,
                             void *)
{
    zlink_multipart_close (parts_, part_count_);
}

void discard_stream_packet_message (
  void *, const zlink_routing_id_t *, zlink_msg_t *header_, zlink_msg_t *body_, void *)
{
    if (header_)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (header_));
    if (body_)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (body_));
}

void noop_send_complete_handler (void *, const zlink_send_complete_event_t *,
                                 void *)
{
}

void test_raw_socket_receive_callback_contracts ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    void *pair = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    void *xsub = zlink_socket (ctx, ZLINK_SOCKET_XSUB);
    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    void *xpub = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    void *stream_recv = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (pair);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (sub);
    TEST_ASSERT_NOT_NULL (xsub);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (xpub);
    TEST_ASSERT_NOT_NULL (stream_recv);

    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (pair, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (dealer, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (router, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (EOPNOTSUPP, zlink_errno ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (stream_recv, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (sub, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (xsub, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (pub, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           zlink_recv_handler (xpub, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, pair, pair, ZLINK_POLLIN));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, dealer, dealer, ZLINK_POLLIN));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, router, router, ZLINK_POLLIN));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, sub, sub, ZLINK_POLLIN));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, xsub, xsub, ZLINK_POLLIN));
    TEST_ASSERT_TRUE (zlink_poller_add (poller, stream_recv, stream_recv, ZLINK_POLLIN)
                      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_BUSY,
      zlink_stream_packet_handler (stream_recv, &discard_stream_packet_message, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY,
                           zlink_recv_handler (stream_recv, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_TRUE (zlink_poller_add (poller, stream_recv, stream_recv, ZLINK_POLLIN)
                      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    close_zero_linger (stream_recv);

    void *poller_packet = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller_packet);
    void *stream_packet = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream_packet);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_stream_packet_handler (stream_packet, &discard_stream_packet_message, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_BUSY,
      zlink_stream_packet_handler (stream_packet, &discard_stream_packet_message, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY,
                           zlink_recv_handler (stream_packet, &discard_stream_message, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_TRUE (zlink_poller_add (poller_packet, stream_packet, stream_packet, ZLINK_POLLIN)
                      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller_packet));
    close_zero_linger (stream_packet);
    close_zero_linger (xpub);
    close_zero_linger (pub);
    close_zero_linger (xsub);
    close_zero_linger (sub);
    close_zero_linger (router);
    close_zero_linger (dealer);
    close_zero_linger (pair);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_raw_socket_send_complete_contracts ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);

    void *pair = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    void *xpub = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *stream = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
    void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (stream);
    TEST_ASSERT_NOT_NULL (pair);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (xpub);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (sub);

    //  The completion channel exists exactly where zlink_send_async does.
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_complete_handler (pair, &noop_send_complete_handler, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_complete_handler (dealer, &noop_send_complete_handler, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_complete_handler (router, &noop_send_complete_handler, NULL));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_complete_handler (stream, &noop_send_complete_handler, NULL));

    //  PUB/XPUB publish and SUB have no asynchronous send admission surface,
    //  so they have no completion channel either.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_NOT_SUPPORTED,
      zlink_send_complete_handler (pub, &noop_send_complete_handler, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_NOT_SUPPORTED,
      zlink_send_complete_handler (xpub, &noop_send_complete_handler, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_NOT_SUPPORTED,
      zlink_send_complete_handler (sub, &noop_send_complete_handler, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, pair, pair, ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, pub, pub, ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, xpub, xpub, ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, dealer, dealer, ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, router, router, ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, stream, stream, ZLINK_POLLOUT));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_TRUE (zlink_recv (pair, NULL, &parts, &part_count, ZLINK_DONTWAIT)
                      != ZLINK_RECV_OK);
    TEST_ASSERT_TRUE (zlink_errno () != EBUSY);
    TEST_ASSERT_TRUE (zlink_recv (dealer, NULL, &parts, &part_count, ZLINK_DONTWAIT)
                      != ZLINK_RECV_OK);
    TEST_ASSERT_TRUE (zlink_errno () != EBUSY);
    TEST_ASSERT_TRUE (zlink_recv (router, NULL, &parts, &part_count, ZLINK_DONTWAIT)
                      != ZLINK_RECV_OK);
    TEST_ASSERT_TRUE (zlink_errno () != EBUSY);
    TEST_ASSERT_TRUE (zlink_recv (stream, NULL, &parts, &part_count, ZLINK_DONTWAIT)
                      != ZLINK_RECV_OK);
    TEST_ASSERT_TRUE (zlink_errno () != EBUSY);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    close_zero_linger (sub);
    close_zero_linger (stream);
    close_zero_linger (router);
    close_zero_linger (dealer);
    close_zero_linger (xpub);
    close_zero_linger (pub);
    close_zero_linger (pair);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_raw_subscribe_recv_returns_topic_and_payload ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (sub);
    TEST_ASSERT_NOT_NULL (pub);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topic"));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME);

    s_send_seq (pub, "topic", "payload", SEQ_END);

    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub, &source_rid, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT (0, source_rid.size);
    TEST_ASSERT_TRUE (topic_len >= 5);
    TEST_ASSERT_EQUAL_MEMORY ("topic", topic, 5);
    TEST_ASSERT_EQUAL_UINT (1, part_count);
    TEST_ASSERT_EQUAL_STRING ("payload", static_cast<const char *> (zlink_msg_data (&parts[0])));
    zlink_multipart_close (parts, part_count);

    close_zero_linger (sub);
    close_zero_linger (pub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pubsub_generic_surface_accepts_raw_prefix_filters_and_raw_publish ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (sub);
    TEST_ASSERT_NOT_NULL (pub);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "*"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "bad*mid"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME);

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 7));
    memcpy (zlink_msg_data (&part), "payload", 7);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub, NULL, &part, 1, 0));

    char buffer[32];
    memset (buffer, 0, sizeof (buffer));
    TEST_ASSERT_EQUAL_INT (7, zlink_recv (sub, buffer, sizeof (buffer), 0));
    TEST_ASSERT_EQUAL_STRING ("payload", buffer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 4));
    memcpy (zlink_msg_data (&part), "drop", 4);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub, "topic", &part, 1, 0));

    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub, NULL, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (5, topic_len);
    TEST_ASSERT_EQUAL_MEMORY ("topic", topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("drop", zlink_msg_data (&parts[0]), 4);
    zlink_multipart_close (parts, part_count);

    close_zero_linger (sub);
    close_zero_linger (pub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_xpub_direct_recv_returns_source_rid_and_topic ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *xpub = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (xpub);
    TEST_ASSERT_NOT_NULL (sub);

    int verbose = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (xpub, ZLINK_PUB_OPT_VERBOSE, &verbose, sizeof (verbose)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (xpub, endpoint, sizeof endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topic"));

    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    int subscribed = 0;
    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscription_event (xpub, &source_rid, &subscribed, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_TRUE (source_rid.size > 0);
    TEST_ASSERT_EQUAL_UINT (5, topic_len);
    TEST_ASSERT_EQUAL_STRING ("topic", topic);

    close_zero_linger (sub);
    close_zero_linger (xpub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_raw_socket_receive_callback_contracts);
    RUN_TEST (test_raw_socket_send_complete_contracts);
    RUN_TEST (test_raw_subscribe_recv_returns_topic_and_payload);
    RUN_TEST (test_pubsub_generic_surface_accepts_raw_prefix_filters_and_raw_publish);
    RUN_TEST (test_xpub_direct_recv_returns_source_rid_and_topic);
    return UNITY_END ();
}
