/* SPDX-License-Identifier: MPL-2.0 */

//  Regression: a SUB socket closed with linger 0 while its transport engine is
//  still receiving the payload of a frame.
//
//  The ZMP decoder takes a frame reservation when the frame header arrives and
//  releases it through a session back-pointer once the whole payload has been
//  pushed. The engine is destroyed on a posted io_context handler that can run
//  after the session that owned it has already been deleted, so a reservation
//  still held at close time used to be released through freed memory
//  (ASan: heap-use-after-free in session_base_t::release_decoder_frame from
//  ~zmp_decoder_t, session freed by own_t::process_term). The fix severs the
//  decoder's session references in asio_engine_t::unplug ().
//
//  Public synchronisation used to land inside that window without sleeps: a
//  small message A is published immediately before a very large message B.
//  The publisher's engine writes A and B's frame header in one batch, and the
//  subscriber's decoder consumes B's header (taking the reservation) in the
//  same decode pass that pushes A to the socket. Once the application has
//  received A, the decoder therefore holds B's reservation while B's payload
//  is still in transit, and the subscriber is closed at that point. The
//  failure mode is a crash / sanitizer report, never a wrong value.

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kTimeoutMs = 10000;
const char *const kTopic = "frame";
const size_t kMarkerBytes = 64;
const size_t kFramePayloadBytes = static_cast<size_t> (32) << 20; // 32 MiB
const int kIterations = 8;

void configure_common (void *socket_)
{
    const int zero = 0;
    const uint64_t unlimited = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_SNDTIMEO, &kTimeoutMs, sizeof (kTimeoutMs)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_RCVTIMEO, &kTimeoutMs, sizeof (kTimeoutMs)));
    //  Unlimited byte HWM so the whole frame is admitted (reservation taken)
    //  as soon as its header arrives.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_SNDHWM, &unlimited, sizeof (unlimited)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_RCVHWM, &unlimited, sizeof (unlimited)));
}

void wait_subscription (void *xpub_)
{
    const zlink_routing_id_t *source_rid = NULL;
    int subscribed = 0;
    char topic[64];
    size_t topic_len = sizeof (topic);
    //  Blocking (RCVTIMEO-bounded) read: the SUB's subscription reaching the
    //  XPUB proves the connection is ready in both directions.
    const zlink_recv_result_t rc =
      zlink_xpub_recv_part (xpub_, &source_rid, &subscribed, topic,
                            sizeof (topic), &topic_len, ZLINK_RECV_FLAGS_NONE);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_EQUAL_UINT (strlen (kTopic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (kTopic, topic, topic_len);
}

void receive_marker (void *sub_)
{
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    char topic[64];
    size_t topic_len = sizeof (topic);
    //  Blocking (RCVTIMEO-bounded).
    TEST_ASSERT_SUCCESS_ERRNO (zlink_subscribe (sub_, NULL, &parts, &part_count,
                                                topic, &topic_len,
                                                ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT (1, part_count);
    TEST_ASSERT_EQUAL_UINT (kMarkerBytes, zlink_msg_size (&parts[0]));
    zlink_multipart_close (parts, part_count);
}

void run_close_during_inbound_frame_once (bool close_subscriber_first_)
{
    void *xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    configure_common (xpub);
    configure_common (sub);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (xpub, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, kTopic));
    wait_subscription (xpub);

    //  A: small marker message. B: a frame whose payload takes far longer to
    //  cross loopback than the close handshake below.
    zlink_msg_t marker;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&marker, kMarkerBytes));
    memset (zlink_msg_data (&marker), 'a', kMarkerBytes);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (xpub, kTopic, &marker, 1, 0));

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, kFramePayloadBytes));
    memset (zlink_msg_data (&part), 'f', kFramePayloadBytes);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (xpub, kTopic, &part, 1, 0));

    //  Receiving A proves the subscriber's engine has decoded A and, with it,
    //  B's frame header that followed A in the same transport batch: the
    //  decoder now holds B's frame reservation while B's payload is in flight.
    receive_marker (sub);

    if (close_subscriber_first_) {
        test_context_socket_close_zero_linger (sub);
        test_context_socket_close_zero_linger (xpub);
    } else {
        test_context_socket_close_zero_linger (xpub);
        test_context_socket_close_zero_linger (sub);
    }
}
} // namespace

void test_sub_close_during_inbound_frame ()
{
    for (int i = 0; i < kIterations; ++i)
        run_close_during_inbound_frame_once (true);
}

void test_xpub_close_during_outbound_frame ()
{
    for (int i = 0; i < kIterations; ++i)
        run_close_during_inbound_frame_once (false);
}

int main ()
{
    setup_test_environment (180);
    UNITY_BEGIN ();
    RUN_TEST (test_sub_close_during_inbound_frame);
    RUN_TEST (test_xpub_close_during_outbound_frame);
    return UNITY_END ();
}
