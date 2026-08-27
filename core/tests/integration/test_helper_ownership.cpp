/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

zlink_routing_id_t missing_routing_id ()
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 4;
    memcpy (rid.data, "none", rid.size);
    return rid;
}

void ignore_reply (zlink_request_result_t, zlink_msg_t *, size_t, void *)
{
}

void set_errno_on_free (void *, void *hint_)
{
    ++*static_cast<int *> (hint_);
    errno = E2BIG;
}
}

void test_send_part_consumes_input_and_requires_reinit_before_reuse ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-send"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-send"));

    zlink_msg_t part;
    init_part (&part, "first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 6));
    memcpy (zlink_msg_data (&part), "second", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&received[0]), 5);
    zlink_multipart_close (received, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("second", zlink_msg_data (&received[0]), 6);
    zlink_multipart_close (received, part_count);
}

void test_rejected_send_part_consumes_current_part ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-rejected"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-rejected"));

    zlink_msg_t first;
    init_part (&first, "first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_submit_result_t rejected_rc = ZLINK_SUBMIT_OK;
    int rejected_errno = 0;
    size_t remaining_size = 0;
    bool contender_failed = false;
    std::thread contender ([&] {
        zlink_msg_t rejected;
        if (zlink_msg_init_size (&rejected, 8) != ZLINK_CONFIG_OK) {
            contender_failed = true;
            return;
        }
        memcpy (zlink_msg_data (&rejected), "rejected", 8);
        rejected_rc = zlink_send_part (sender, &rejected,
                                       static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL);
        rejected_errno = zlink_errno ();
        remaining_size = zlink_msg_size (&rejected);
        if (zlink_msg_close (&rejected) != ZLINK_CONFIG_OK)
            contender_failed = true;
    });
    contender.join ();

    zlink_msg_t final_part;
    init_part (&final_part, "final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &final_part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    std::printf ("rejected_part_ownership rc=%d errno=%d remaining_size=%zu\n",
                 static_cast<int> (rejected_rc), rejected_errno, remaining_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, rejected_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, rejected_errno);
    TEST_ASSERT_FALSE (contender_failed);
    TEST_ASSERT_EQUAL_UINT64 (0, remaining_size);

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&received[0]), 5);
    TEST_ASSERT_EQUAL_MEMORY ("final", zlink_msg_data (&received[1]), 5);
    zlink_multipart_close (received, part_count);
}

void test_send_failures_consume_current_part ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const zlink_routing_id_t missing = missing_routing_id ();

    zlink_msg_t invalid_flags;
    init_part (&invalid_flags, "invalid-flags");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (pair, &invalid_flags,
                       static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&invalid_flags));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_flags));

    zlink_msg_t missing_route;
    init_part (&missing_route, "missing-route");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_send_part_rid (router, &missing, &missing_route,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&missing_route));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&missing_route));

    zlink_msg_t wrong_publish_type;
    init_part (&wrong_publish_type, "wrong-publish-type");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_publish_part (pair, "topic", &wrong_publish_type,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_publish_type));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_publish_type));
}

void test_unrouted_send_part_rejects_routed_only_socket_families ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);

    zlink_msg_t router_part;
    init_part (&router_part, "router-needs-rid");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (router, &router_part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&router_part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&router_part));

    zlink_msg_t stream_part;
    init_part (&stream_part, "stream-needs-rid");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (stream, &stream_part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&stream_part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&stream_part));
}

void test_same_thread_failure_aborts_non_publish_sequence ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://helper-ownership-abort"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://helper-ownership-abort"));

    zlink_msg_t head;
    init_part (&head, "aborted-head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE));

    zlink_msg_t rejected;
    init_part (&rejected, "rejected-tail");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (sender, &rejected,
                       static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&rejected));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&rejected));

    zlink_msg_t fresh;
    init_part (&fresh, "fresh-record");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &fresh, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &received, &part_count,
                  ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (12, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY ("fresh-record", zlink_msg_data (&received[0]),
                              12);
    zlink_multipart_close (received, part_count);
}

void test_publish_prevalidation_failures_preserve_open_sequence ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    const char *const endpoint = "inproc://helper-ownership-publish-preserve";
    const char *const topic = "topic-a";

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME * 2);

    zlink_msg_t head;
    init_part (&head, "head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_publish_part (pub, topic, &head, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_MORE));

    zlink_msg_t invalid_flags;
    init_part (&invalid_flags, "invalid-flags");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_publish_part (pub, topic, &invalid_flags,
                          static_cast<zlink_send_flags_t> (0x40),
                          ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&invalid_flags));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_flags));

    zlink_msg_t changed_topic;
    init_part (&changed_topic, "changed-topic");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_publish_part (pub, "topic-b", &changed_topic,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&changed_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&changed_topic));

    zlink_msg_t different_helper;
    init_part (&different_helper, "different-helper");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (pub, &different_helper, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&different_helper));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&different_helper));

    zlink_msg_t different_thread;
    init_part (&different_thread, "different-thread");
    zlink_submit_result_t thread_result = ZLINK_SUBMIT_OK;
    int thread_errno = 0;
    size_t thread_part_size = 1;
    std::thread contender ([&] {
        thread_result = zlink_publish_part (pub, topic, &different_thread,
                                            ZLINK_SEND_FLAGS_NONE,
                                            ZLINK_PART_FINAL);
        thread_errno = zlink_errno ();
        thread_part_size = zlink_msg_size (&different_thread);
    });
    contender.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, thread_result);
    TEST_ASSERT_EQUAL_INT (EINVAL, thread_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, thread_part_size);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&different_thread));

    zlink_msg_t tail;
    init_part (&tail, "tail");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_publish_part (pub, topic, &tail, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL));

    char received_topic[sizeof ("topic-a")];
    size_t received_topic_len = sizeof (received_topic);
    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_subscribe (sub, NULL, &received, &part_count, received_topic,
                       &received_topic_len, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (sizeof ("topic-a") - 1, received_topic_len);
    TEST_ASSERT_EQUAL_MEMORY (topic, received_topic, received_topic_len);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("head", zlink_msg_data (&received[0]), 4);
    TEST_ASSERT_EQUAL_MEMORY ("tail", zlink_msg_data (&received[1]), 4);
    zlink_multipart_close (received, part_count);
}

void test_consuming_failure_preserves_result_errno_from_free_callback ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    char payload[] = "errno-payload";
    int free_count = 0;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_data (&part, payload, sizeof (payload) - 1,
                           &set_errno_on_free, &free_count));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (pair, &part,
                       static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_INT (1, free_count);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void test_request_reply_failures_consume_final_part ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const zlink_routing_id_t missing = missing_routing_id ();

    zlink_msg_t request;
    init_part (&request, "request-payload");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_router_request_part (
        router, &missing, &request, ZLINK_SEND_FLAGS_DONTWAIT,
        ZLINK_PART_FINAL, 0, &ignore_reply, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&request));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));

    zlink_msg_t router_reply;
    init_part (&router_reply, "router-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_router_reply_part (router, &missing, 1, &router_reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&router_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&router_reply));

    zlink_msg_t dealer_reply;
    init_part (&dealer_reply, "dealer-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_dealer_reply_part (dealer, 1, &dealer_reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&dealer_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&dealer_reply));
}

void test_recv_part_returns_caller_owned_message_handle ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-recv"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-recv"));

    zlink_msg_t send_a;
    zlink_msg_t send_b;
    init_part (&send_a, "owned-a");
    init_part (&send_b, "owned-b");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (sender, &send_a, 1, static_cast<zlink_send_flags_t> (0)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (sender, &send_b, 1, static_cast<zlink_send_flags_t> (0)));

    zlink_msg_t recv_a;
    zlink_msg_t recv_b;
    zlink_msg_init (&recv_a);
    zlink_msg_init (&recv_b);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv_part (receiver, NULL, &recv_a, &has_more, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("owned-a", zlink_msg_data (&recv_a), 7);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&recv_a));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv_part (receiver, NULL, &recv_b, &has_more, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("owned-b", zlink_msg_data (&recv_b), 7);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&recv_b));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_send_part_consumes_input_and_requires_reinit_before_reuse);
    RUN_TEST (test_rejected_send_part_consumes_current_part);
    RUN_TEST (test_send_failures_consume_current_part);
    RUN_TEST (test_unrouted_send_part_rejects_routed_only_socket_families);
    RUN_TEST (test_same_thread_failure_aborts_non_publish_sequence);
    RUN_TEST (test_publish_prevalidation_failures_preserve_open_sequence);
    RUN_TEST (test_consuming_failure_preserves_result_errno_from_free_callback);
    RUN_TEST (test_request_reply_failures_consume_final_part);
    RUN_TEST (test_recv_part_returns_caller_owned_message_handle);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
