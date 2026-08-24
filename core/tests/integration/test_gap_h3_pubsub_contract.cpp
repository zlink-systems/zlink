/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

zlink_recv_result_t recv_xpub_event_eventually (
  void *xpub_, const zlink_routing_id_t **source_rid_out_,
  int *subscribed_out_, char *topic_, size_t topic_capacity_,
  size_t *topic_len_out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t result = zlink_xpub_recv_part (
          xpub_, source_rid_out_, subscribed_out_, topic_, topic_capacity_,
          topic_len_out_, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (result != ZLINK_RECV_NO_DATA)
            return result;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for XPUB subscription event");
    return ZLINK_RECV_INTERNAL_ERROR;
}

zlink_recv_result_t recv_xsub_part_eventually (
  void *xsub_, const zlink_routing_id_t **source_rid_out_, char *topic_,
  size_t topic_capacity_, size_t *topic_len_out_, zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t result = zlink_subscribe_part (
          xsub_, source_rid_out_, topic_, topic_capacity_, topic_len_out_,
          part_out_, has_more_out_,
          static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (result != ZLINK_RECV_NO_DATA)
            return result;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for XSUB topic part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

void expect_xpub_subscription (void *xpub_, const char *topic_,
                               const zlink_routing_id_t **rid_out_ = NULL)
{
    char topic[64];
    size_t topic_len = sizeof (topic);
    int subscribed = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_xpub_event_eventually (xpub_, rid_out_, &subscribed, topic,
                                  sizeof (topic), &topic_len));
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_), topic_len);
    if (topic_len != 0)
        TEST_ASSERT_EQUAL_MEMORY (topic_, topic, topic_len);
}

void configure_hwm (void *socket_, uint64_t hwm_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
}
} // namespace

void test_xsub_public_helpers_forward_upstream_and_retry_without_consuming ()
{
    const char *endpoint = "inproc://gap-h3-xsub-public";
    const char *topic_name = "contract-topic";
    void *xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *xsub = test_context_socket (ZLINK_SOCKET_XSUB);
    void *wrong_type = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (xpub, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (xsub, endpoint));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE,
                           zlink_set_subscription (NULL, topic_name));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT,
                           zlink_set_subscription (xsub, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT,
                           zlink_set_subscription (wrong_type, topic_name));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    char error_topic[8] = {'u', 'n', 'c', 'h', 'a', 'n', 'g', 'e'};
    size_t error_topic_len = sizeof (error_topic);
    zlink_msg_t error_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&error_part));
    zlink_part_flag_t error_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      zlink_subscribe_part (
        xsub, NULL, NULL, sizeof (error_topic), &error_topic_len, &error_part,
        &error_more, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NOT_SUPPORTED,
      zlink_subscribe_part (
        wrong_type, NULL, error_topic, sizeof (error_topic), &error_topic_len,
        &error_part, &error_more,
        static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_subscribe_part (
        xsub, NULL, error_topic, sizeof (error_topic), &error_topic_len,
        &error_part, &error_more,
        static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&error_part));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_subscription (xsub, topic_name));
    expect_xpub_subscription (xpub, topic_name);

    zlink_msg_t outbound;
    init_part (&outbound, "xsub-payload");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_publish_part (xpub, topic_name, &outbound,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&outbound));

    char small_topic[3] = {'k', 'e', 'p'};
    size_t topic_len = 0;
    zlink_msg_t inbound;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&inbound));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      recv_xsub_part_eventually (xsub, &source_rid, small_topic,
                                 sizeof (small_topic), &topic_len, &inbound,
                                 &has_more));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_name), topic_len);
    TEST_ASSERT_EQUAL_MEMORY ("kep", small_topic, sizeof (small_topic));
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&inbound));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);

    char full_topic[32];
    source_rid = reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_xsub_part_eventually (xsub, &source_rid, full_topic,
                                 sizeof (full_topic), &topic_len, &inbound,
                                 &has_more));
    TEST_ASSERT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_name), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (topic_name, full_topic, topic_len);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("xsub-payload"),
                              zlink_msg_size (&inbound));
    TEST_ASSERT_EQUAL_MEMORY ("xsub-payload", zlink_msg_data (&inbound),
                              strlen ("xsub-payload"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&inbound));

    test_context_socket_close_zero_linger (wrong_type);
    test_context_socket_close_zero_linger (xsub);
    test_context_socket_close_zero_linger (xpub);
}

void test_xpub_recv_tls_buffer_consumption_and_publish_part_contract ()
{
    const char *endpoint_a = "inproc://gap-h3-xpub-a";
    const char *endpoint_b = "inproc://gap-h3-xpub-b";
    void *xpub_a = test_context_socket (ZLINK_SOCKET_XPUB);
    void *xpub_b = test_context_socket (ZLINK_SOCKET_XPUB);
    void *xsub_a = test_context_socket (ZLINK_SOCKET_XSUB);
    void *xsub_b = test_context_socket (ZLINK_SOCKET_XSUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (xsub_a, "xsub-A", 6));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (xsub_b, "xsub-B", 6));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (xpub_a, endpoint_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (xpub_b, endpoint_b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (xsub_a, endpoint_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (xsub_b, endpoint_b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_subscription (xsub_a, "topic-a"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_subscription (xsub_b, "topic-b"));

    const zlink_routing_id_t *rid_a = NULL;
    expect_xpub_subscription (xpub_a, "topic-a", &rid_a);
    TEST_ASSERT_NOT_NULL (rid_a);
    TEST_ASSERT_EQUAL_UINT8 (6, rid_a->size);
    TEST_ASSERT_EQUAL_MEMORY ("xsub-A", rid_a->data, 6);
    const zlink_routing_id_t rid_a_copy = *rid_a;

    const zlink_routing_id_t *rid_b = NULL;
    expect_xpub_subscription (xpub_b, "topic-b", &rid_b);
    TEST_ASSERT_NOT_NULL (rid_b);
    TEST_ASSERT_EQUAL_PTR (rid_a, rid_b);
    TEST_ASSERT_EQUAL_MEMORY ("xsub-B", rid_b->data, 6);
    TEST_ASSERT_EQUAL_MEMORY ("xsub-B", rid_a->data, 6);
    TEST_ASSERT_EQUAL_MEMORY ("xsub-A", rid_a_copy.data, 6);

    //  Buffer failure reports the required length and consumes this event.
    const char *long_topic = "topic-too-long";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_subscription (xsub_a, long_topic));
    char small[2] = {'q', 'q'};
    size_t needed = 0;
    int subscribed = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      recv_xpub_event_eventually (xpub_a, NULL, &subscribed, small,
                                  sizeof (small), &needed));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (strlen (long_topic), needed);
    TEST_ASSERT_EQUAL_MEMORY ("qq", small, sizeof (small));
    char retry[32];
    needed = sizeof (retry);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_xpub_recv_part (xpub_a, NULL, &subscribed, retry,
                            sizeof (retry), &needed,
                            static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    errno = 0;
    needed = sizeof (retry);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_xpub_recv_part (xsub_a, NULL, &subscribed, retry,
                            sizeof (retry), &needed,
                            static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_xpub_recv_part (NULL, NULL, &subscribed, retry,
                            sizeof (retry), &needed,
                            static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());

    zlink_msg_t published;
    init_part (&published, "published-payload");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_publish_part (xpub_a, "topic-a", &published,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&published));

    char received_topic[32];
    size_t received_topic_len = 0;
    zlink_msg_t received;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&received));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_xsub_part_eventually (
        xsub_a, &source_rid, received_topic, sizeof (received_topic),
        &received_topic_len, &received, &has_more));
    TEST_ASSERT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("topic-a"), received_topic_len);
    TEST_ASSERT_EQUAL_MEMORY ("topic-a", received_topic, received_topic_len);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("published-payload", zlink_msg_data (&received),
                              strlen ("published-payload"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&received));

    void *wrong_type = test_context_socket (ZLINK_SOCKET_PAIR);
    zlink_msg_t rejected;
    init_part (&rejected, "rejected");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_publish_part (wrong_type, "topic", &rejected,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&rejected));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&rejected));

    test_context_socket_close_zero_linger (wrong_type);
    test_context_socket_close_zero_linger (xsub_b);
    test_context_socket_close_zero_linger (xsub_a);
    test_context_socket_close_zero_linger (xpub_b);
    test_context_socket_close_zero_linger (xpub_a);

    //  NODROP converts a full subscriber pipe into public backpressure.
    const char *nodrop_endpoint = "inproc://gap-h3-xpub-nodrop";
    const uint64_t hwm = 4096;
    void *nodrop_xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *nodrop_xsub = test_context_socket (ZLINK_SOCKET_XSUB);
    configure_hwm (nodrop_xpub, hwm);
    configure_hwm (nodrop_xsub, hwm);
    const int enabled = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_pub_option (nodrop_xpub, ZLINK_PUB_OPT_NODROP, &enabled,
                            sizeof (enabled)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_bind (nodrop_xpub, nodrop_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_connect (nodrop_xsub, nodrop_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_subscription (nodrop_xsub, ""));
    expect_xpub_subscription (nodrop_xpub, "");

    bool backpressured = false;
    for (size_t i = 0; i != 256 && !backpressured; ++i) {
        zlink_msg_t part;
        init_part (&part, std::string (1024, 'n'));
        const zlink_submit_result_t result = zlink_publish_part (
          nodrop_xpub, "topic", &part, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL);
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            backpressured = true;
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }
    TEST_ASSERT_TRUE (backpressured);

    test_context_socket_close_zero_linger (nodrop_xsub);
    test_context_socket_close_zero_linger (nodrop_xpub);
}

int main ()
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (
      test_xsub_public_helpers_forward_upstream_and_retry_without_consuming);
    RUN_TEST (
      test_xpub_recv_tls_buffer_consumption_and_publish_part_contract);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
