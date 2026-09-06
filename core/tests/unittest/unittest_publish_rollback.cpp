/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include <cstring>
#include <string>
#include <sstream>

SETUP_TEARDOWN_TESTCONTEXT

static const char k_pubsub_topic[] = "bench";

void set_timeout_opts (void *socket_)
{
    const int timeout_ms = 2000;
    const int linger_ms = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger_ms, sizeof (linger_ms)));
}

std::string make_fixed_size_payload (char phase_, size_t seq_, size_t size_)
{
    std::ostringstream stream;
    stream << phase_ << ":" << seq_ << ":payload";
    std::string payload = stream.str ();
    if (payload.size () > size_)
        payload.resize (size_);
    if (payload.size () < size_)
        payload.append (size_ - payload.size (), '#');
    return payload;
}

void publish_payload (void *pub_, const std::string &payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, payload_.size ()));
    memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub_, k_pubsub_topic, &part, 1, 0));
}

void recv_subscribe_expect_topic_and_payload (void *sub_, const std::string &payload_)
{
    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub_, NULL, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (k_pubsub_topic, topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_NOT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (payload_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_.data (), zlink_msg_data (&parts[0]), payload_.size ());

    zlink_multipart_close (parts, part_count);
}

void test_pubsub_publish_rollback_preserves_next_topic_boundary ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    set_timeout_opts (pub);
    set_timeout_opts (sub);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));
    contract_socket_pair_t pair (pub, sub);
    int subscribed = 0;
    char subscription_topic[16];
    size_t subscription_topic_size = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_xpub_recv_part (
      pub, NULL, &subscribed, subscription_topic, sizeof (subscription_topic),
      &subscription_topic_size, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_EQUAL_UINT64 (5, subscription_topic_size);
    TEST_ASSERT_EQUAL_MEMORY ("bench", subscription_topic, 5);

    zlink_msg_t topic_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&topic_part, std::strlen (k_pubsub_topic)));
    memcpy (zlink_msg_data (&topic_part), k_pubsub_topic, std::strlen (k_pubsub_topic));
    socket_handle_t pub_handle = as_socket_handle (pub);
    zlink::socket_base_t *pub_socket = pub_handle.socket;
    TEST_ASSERT_SUCCESS_ERRNO (pub_socket->send (
      reinterpret_cast<zlink::msg_t *> (&topic_part), ZLINK_SNDMORE));
    TEST_ASSERT_SUCCESS_ERRNO (pub_socket->rollback ());
    pub_handle = socket_handle_t ();

    char topic[32];
    memset (topic, 0, sizeof (topic));
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (sub, NULL, &parts, &part_count,
                                                                topic, &topic_len, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    const std::string recovered_payload = make_fixed_size_payload ('W', 1, 64);
    publish_payload (pub, recovered_payload);
    recv_subscribe_expect_topic_and_payload (sub, recovered_payload);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pubsub_publish_rollback_preserves_next_topic_boundary);
    return UNITY_END ();
}
