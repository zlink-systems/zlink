/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
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

void set_errno_on_free (void *, void *hint_)
{
    ++*static_cast<int *> (hint_);
    errno = E2BIG;
}

void count_free (void *, void *hint_)
{
    static_cast<std::atomic<int> *> (hint_)->fetch_add (
      1, std::memory_order_relaxed);
}

struct reentrant_free_probe_t
{
    void *socket;
    std::atomic<int> calls;
    std::atomic<int> option_result;

    explicit reentrant_free_probe_t (void *socket_) :
        socket (socket_), calls (0), option_result (ZLINK_CONFIG_INTERNAL_ERROR)
    {
    }
};

void set_socket_option_on_free (void *, void *hint_)
{
    reentrant_free_probe_t *const probe =
      static_cast<reentrant_free_probe_t *> (hint_);
    const uint64_t hwm = 8192;
    probe->option_result.store (
      zlink_set_option (probe->socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)),
      std::memory_order_release);
    probe->calls.fetch_add (1, std::memory_order_release);
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
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 6));
    memcpy (zlink_msg_data (&part), "second", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));
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

void test_other_caller_final_consumes_input_and_keeps_staged_sequence ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-rejected"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-rejected"));

    zlink_msg_t first;
    init_part (&first, "first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE, NULL, NULL));

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
                                       static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL);
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
      zlink_send_part (sender, &final_part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));

    std::printf ("rejected_part_ownership rc=%d errno=%d remaining_size=%zu\n",
                 static_cast<int> (rejected_rc), rejected_errno, remaining_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, rejected_rc);
    TEST_ASSERT_EQUAL_INT (0, rejected_errno);
    TEST_ASSERT_FALSE (contender_failed);
    TEST_ASSERT_EQUAL_UINT64 (0, remaining_size);

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("rejected", zlink_msg_data (&received[0]), 8);
    zlink_multipart_close (received, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&received[0]), 5);
    TEST_ASSERT_EQUAL_MEMORY ("final", zlink_msg_data (&received[1]), 5);
    zlink_multipart_close (received, part_count);
}

void test_other_caller_different_family_is_independent ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://helper-ownership-family-independent"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://helper-ownership-family-independent"));

    std::mutex mutex;
    std::condition_variable changed;
    bool more_done = false;
    bool finish_owner = false;
    zlink_submit_result_t more_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_submit_result_t final_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread owner ([&] {
        zlink_msg_t more;
        init_part (&more, "send-more");
        more_result = zlink_send_part (
          dealer, &more, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, NULL, NULL);
        zlink_msg_close (&more);
        {
            std::unique_lock<std::mutex> lock (mutex);
            more_done = true;
            changed.notify_all ();
            changed.wait (lock, [&] { return finish_owner; });
        }
        zlink_msg_t final_part;
        init_part (&final_part, "send-final");
        final_result = zlink_send_part (
          dealer, &final_part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
          NULL);
        zlink_msg_close (&final_part);
    });

    bool owner_ready = false;
    {
        std::unique_lock<std::mutex> lock (mutex);
        owner_ready = changed.wait_for (
          lock, std::chrono::seconds (5), [&] { return more_done; });
    }

    zlink_msg_t request;
    init_part (&request, "independent-request");
    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t request_result = zlink_request_part (
      dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 5000,
      NULL, &completion_id);
    const int request_close_rc = zlink_msg_close (&request);

    {
        std::lock_guard<std::mutex> lock (mutex);
        finish_owner = true;
    }
    changed.notify_all ();
    owner.join ();

    TEST_ASSERT_TRUE (owner_ready);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, more_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request_result);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, request_close_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, final_result);

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &received,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_EQUAL (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("independent-request", zlink_msg_data (&received),
                              strlen ("independent-request"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &received,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("send-more", zlink_msg_data (&received),
                              strlen ("send-more"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &received,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("send-final", zlink_msg_data (&received),
                              strlen ("send-final"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
}

void test_send_failures_consume_current_part ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const zlink_routing_id_t missing = missing_routing_id ();

    zlink_msg_t invalid_flags;
    init_part (&invalid_flags, "invalid-flags");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (pair, &invalid_flags,
                       static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&invalid_flags));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_flags));

    zlink_msg_t missing_route;
    init_part (&missing_route, "missing-route");
    zlink_completion_id_t writable_token = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_send_part_rid (router, &missing, &missing_route,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &writable_token));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, writable_token);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&missing_route));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&missing_route));

    // A routed target that has no route can never become writable, so it
    // fails synchronously instead of holding a wait token (STREAM too).
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    zlink_msg_t missing_stream_route;
    init_part (&missing_stream_route, "missing-stream-route");
    writable_token = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_send_part_rid (stream, &missing, &missing_stream_route,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &writable_token));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, writable_token);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&missing_stream_route));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&missing_stream_route));
    test_context_socket_close (stream);

    zlink_msg_t wrong_publish_type;
    init_part (&wrong_publish_type, "wrong-publish-type");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_publish_part (pair, "topic", &wrong_publish_type,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_publish_type));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_publish_type));
}

void test_invalid_routed_targets_consume_parts_before_blocking_admission ()
{
    const zlink_socket_type_t types[] = {ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_STREAM};
    const zlink_send_flags_t flags[] = {ZLINK_SEND_FLAGS_NONE,
                                       ZLINK_SEND_FLAGS_DONTWAIT};
    zlink_routing_id_t empty = {};
    const zlink_routing_id_t *targets[] = {NULL, &empty};

    for (size_t type = 0; type != 2; ++type) {
        void *socket = test_context_socket (types[type]);
        for (size_t flag = 0; flag != 2; ++flag) {
            for (size_t target = 0; target != 2; ++target) {
                zlink_msg_t part;
                init_part (&part, "invalid-target");
                zlink_completion_id_t token = 99;
                errno = 0;
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_SUBMIT_INVALID_ARGUMENT,
                  zlink_send_part_rid (socket, targets[target], &part,
                                       flags[flag], ZLINK_PART_FINAL, NULL,
                                       &token));
                TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
                TEST_ASSERT_EQUAL_UINT64 (0, token);
                TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
            }
        }
        test_context_socket_close (socket);
    }
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
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&router_part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&router_part));

    zlink_msg_t stream_part;
    init_part (&stream_part, "stream-needs-rid");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (stream, &stream_part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
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
                       ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t rejected;
    init_part (&rejected, "rejected-tail");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (sender, &rejected,
                       ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&rejected));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&rejected));

    zlink_msg_t fresh;
    init_part (&fresh, "fresh-record");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &fresh, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

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
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_publish_part (pub, topic, &invalid_flags,
                          static_cast<zlink_send_flags_t> (0x40),
                          ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
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
                       ZLINK_PART_FINAL, NULL, NULL));
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
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (pair, &part,
                       static_cast<zlink_send_flags_t> (0x40),
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (1, free_count);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void test_expired_zero_copy_prefix_is_released_by_other_caller_final_outside_scope ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://expired-prefix-reentrant-free"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://expired-prefix-reentrant-free"));
    reentrant_free_probe_t probe (sender);
    char payload[] = "expired-zero-copy-prefix";
    zlink_submit_result_t more_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::mutex transition_mutex;
    std::condition_variable transition_changed;
    bool abandoned_more_open = false;
    bool allow_abandoned_exit = false;
    bool fresh_more_open = false;
    bool allow_fresh_final = false;
    std::thread abandoned ([&] {
        zlink_msg_t more;
        zlink_msg_init_data (&more, payload, sizeof (payload) - 1,
                             &set_socket_option_on_free, &probe);
        more_result = zlink_send_part (
          sender, &more, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, NULL,
          NULL);
        zlink_msg_close (&more);
        std::unique_lock<std::mutex> lock (transition_mutex);
        abandoned_more_open = true;
        transition_changed.notify_all ();
        transition_changed.wait (
          lock, [&] { return allow_abandoned_exit; });
    });

    zlink_submit_result_t fresh_more_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_submit_result_t fresh_final_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread fresh_caller ([&] {
        {
            std::unique_lock<std::mutex> lock (transition_mutex);
            transition_changed.wait (
              lock, [&] { return abandoned_more_open; });
        }
        zlink_msg_t fresh_more;
        init_part (&fresh_more, "independent-more");
        fresh_more_result = zlink_send_part (
          sender, &fresh_more, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE,
          NULL, NULL);
        zlink_msg_close (&fresh_more);
        {
            std::unique_lock<std::mutex> lock (transition_mutex);
            fresh_more_open = true;
            transition_changed.notify_all ();
            transition_changed.wait (lock, [&] { return allow_fresh_final; });
        }
        zlink_msg_t fresh_final;
        init_part (&fresh_final, "independent-final");
        fresh_final_result = zlink_send_part (
          sender, &fresh_final, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, NULL, NULL);
        zlink_msg_close (&fresh_final);
    });

    bool observed_both_open = false;
    {
        std::unique_lock<std::mutex> lock (transition_mutex);
        observed_both_open = transition_changed.wait_for (
          lock, std::chrono::milliseconds (3000),
          [&] { return abandoned_more_open && fresh_more_open; });
        allow_abandoned_exit = true;
        transition_changed.notify_all ();
    }
    abandoned.join ();
    // B opened MORE while A's caller identity was still live. A's exit alone
    // does not scan helper state; B FINAL is the boundary that must detach A.
    const int calls_before_final =
      probe.calls.load (std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock (transition_mutex);
        allow_fresh_final = true;
        transition_changed.notify_all ();
    }
    fresh_caller.join ();
    TEST_ASSERT_TRUE (observed_both_open);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, more_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, fresh_more_result);
    TEST_ASSERT_EQUAL_INT (0, calls_before_final);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, fresh_final_result);
    TEST_ASSERT_EQUAL_INT (1, probe.calls.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, probe.option_result.load (std::memory_order_acquire));
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &parts, &part_count,
                  ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_STRING (
      "independent-more",
      std::string (static_cast<const char *> (zlink_msg_data (&parts[0])),
                   zlink_msg_size (&parts[0])).c_str ());
    TEST_ASSERT_EQUAL_STRING (
      "independent-final",
      std::string (static_cast<const char *> (zlink_msg_data (&parts[1])),
                   zlink_msg_size (&parts[1])).c_str ());
    zlink_multipart_close (parts, part_count);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_same_thread_keeps_sequences_on_two_sockets ()
{
    void *receivers[2] = {test_context_socket (ZLINK_SOCKET_PAIR),
                          test_context_socket (ZLINK_SOCKET_PAIR)};
    void *senders[2] = {test_context_socket (ZLINK_SOCKET_PAIR),
                        test_context_socket (ZLINK_SOCKET_PAIR)};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receivers[0], "inproc://same-thread-two-sockets-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receivers[1], "inproc://same-thread-two-sockets-b"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (senders[0], "inproc://same-thread-two-sockets-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (senders[1], "inproc://same-thread-two-sockets-b"));

    for (int i = 0; i != 2; ++i) {
        zlink_msg_t more;
        init_part (&more, i == 0 ? "a-more" : "b-more");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (senders[i], &more, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE, NULL, NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&more));
    }
    for (int i = 1; i >= 0; --i) {
        zlink_msg_t final_part;
        init_part (&final_part, i == 0 ? "a-final" : "b-final");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (senders[i], &final_part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&final_part));
    }

    for (int i = 0; i != 2; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_recv (receivers[i], NULL, &parts, &part_count,
                      ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_UINT64 (2, part_count);
        TEST_ASSERT_EQUAL_MEMORY (i == 0 ? "a-more" : "b-more",
                                  zlink_msg_data (&parts[0]), 6);
        TEST_ASSERT_EQUAL_MEMORY (i == 0 ? "a-final" : "b-final",
                                  zlink_msg_data (&parts[1]), 7);
        zlink_multipart_close (parts, part_count);
        test_context_socket_close_zero_linger (senders[i]);
        test_context_socket_close_zero_linger (receivers[i]);
    }
}

void test_five_part_sequence_spills_and_preserves_order ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://five-part-helper-spill"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://five-part-helper-spill"));

    const char *const payloads[5] = {"part-0", "part-1", "part-2", "part-3",
                                     "part-4"};
    for (int i = 0; i != 5; ++i) {
        zlink_msg_t part;
        init_part (&part, payloads[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_NONE,
                           i == 4 ? ZLINK_PART_FINAL : ZLINK_PART_MORE, NULL,
                           NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &parts, &part_count,
                  ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (5, part_count);
    for (int i = 0; i != 5; ++i)
        TEST_ASSERT_EQUAL_MEMORY (payloads[i], zlink_msg_data (&parts[i]), 6);
    zlink_multipart_close (parts, part_count);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_close_discards_all_staged_callers_without_owner_final ()
{
    const int caller_count = 4;
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://close-all-staged-callers"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://close-all-staged-callers"));

    std::atomic<int> frees (0);
    char payloads[caller_count][16] = {};
    std::atomic<int> failures (0);
    std::mutex mutex;
    std::condition_variable changed;
    int staged = 0;
    bool release_callers = false;
    std::thread callers[caller_count];
    for (int i = 0; i != caller_count; ++i) {
        callers[i] = std::thread ([&, i] {
            zlink_msg_t more;
            if (zlink_msg_init_data (&more, payloads[i], sizeof (payloads[i]),
                                     &count_free, &frees)
                != ZLINK_CONFIG_OK) {
                failures.fetch_add (1, std::memory_order_relaxed);
            } else {
                if (zlink_send_part (sender, &more, ZLINK_SEND_FLAGS_NONE,
                                     ZLINK_PART_MORE, NULL, NULL)
                    != ZLINK_SUBMIT_OK)
                    failures.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&more);
            }
            std::unique_lock<std::mutex> lock (mutex);
            ++staged;
            changed.notify_all ();
            changed.wait (lock, [&] { return release_callers; });
        });
    }
    {
        std::unique_lock<std::mutex> lock (mutex);
        changed.wait (lock, [&] { return staged == caller_count; });
    }
    const int failures_before_close =
      failures.load (std::memory_order_relaxed);
    const int frees_before_close = frees.load (std::memory_order_relaxed);
    const int close_result = zlink_close (sender);
    if (close_result == ZLINK_CLOSE_OK)
        test_context_socket_mark_closed (sender);
    {
        std::lock_guard<std::mutex> lock (mutex);
        release_callers = true;
    }
    changed.notify_all ();
    for (int i = 0; i != caller_count; ++i)
        callers[i].join ();
    TEST_ASSERT_EQUAL_INT (0, failures_before_close);
    TEST_ASSERT_EQUAL_INT (0, frees_before_close);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_result);
    TEST_ASSERT_EQUAL_INT (caller_count,
                           frees.load (std::memory_order_relaxed));
    test_context_socket_close_zero_linger (receiver);
}

void test_staged_request_blocked_final_is_released_by_context_shutdown ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    std::mutex mutex;
    std::condition_variable changed;
    bool final_started = false;
    bool final_done = false;
    zlink_submit_result_t more_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_submit_result_t final_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    int final_errno = 0;
    size_t final_remaining = 1;
    zlink_completion_id_t completion_id = UINT64_MAX;
    std::thread requester ([&] {
        zlink_msg_t more;
        init_part (&more, "blocked-request-prefix");
        more_result = zlink_request_part (
          dealer, NULL, &more, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, 0,
          NULL, NULL);
        zlink_msg_close (&more);

        zlink_msg_t final_part;
        init_part (&final_part, "blocked-request-final");
        {
            std::lock_guard<std::mutex> lock (mutex);
            final_started = true;
        }
        changed.notify_all ();
        errno = 0;
        final_result = zlink_request_part (
          dealer, NULL, &final_part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
          120000, NULL, &completion_id);
        final_errno = zlink_errno ();
        final_remaining = zlink_msg_size (&final_part);
        zlink_msg_close (&final_part);
        {
            std::lock_guard<std::mutex> lock (mutex);
            final_done = true;
        }
        changed.notify_all ();
    });

    bool reached_final = false;
    bool final_was_blocked = false;
    {
        std::unique_lock<std::mutex> lock (mutex);
        reached_final = changed.wait_for (lock, std::chrono::seconds (3),
                                          [&] { return final_started; });
        final_was_blocked = reached_final
                            && !changed.wait_for (
                              lock, std::chrono::milliseconds (100),
                              [&] { return final_done; });
    }
    const zlink_close_result_t shutdown_result = zlink_ctx_shutdown (context);
    requester.join ();

    TEST_ASSERT_TRUE (reached_final);
    TEST_ASSERT_TRUE (final_was_blocked);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, shutdown_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, more_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_TERMINATED, final_result);
    TEST_ASSERT_EQUAL_INT (ETERM, final_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, final_remaining);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_request_reply_failures_consume_final_part ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const zlink_routing_id_t missing = missing_routing_id ();

    zlink_msg_t request;
    init_part (&request, "request-payload");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_request_part (router, &missing, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 0,
                          NULL, &completion_id));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&request));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));

    zlink_msg_t router_reply;
    init_part (&router_reply, "router-reply");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply_part (router, &missing, 1, &router_reply,
                        ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&router_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&router_reply));

    zlink_msg_t dealer_reply;
    init_part (&dealer_reply, "dealer-reply");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_reply_part (dealer, &missing, 1, &dealer_reply,
                        ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&dealer_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&dealer_reply));
}

void test_blocking_multipart_request_releases_caller_parts_once ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const char *const endpoint = "inproc://helper-ownership-blocking-request";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    std::atomic<int> first_frees (0);
    std::atomic<int> final_frees (0);
    char first_data[] = "request-first";
    char final_data[] = "request-final";
    zlink_msg_t first;
    zlink_msg_t final;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_data (&first, first_data, sizeof (first_data) - 1,
                           &count_free, &first_frees));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_data (&final, final_data, sizeof (final_data) - 1,
                           &count_free, &final_frees));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &first, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_MORE, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&first));

    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &final, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 5000, NULL, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&final));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &received,
                              &part_flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, part_flag);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &received,
                              &part_flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    TEST_ASSERT_EQUAL_INT (1, first_frees.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (1, final_frees.load (std::memory_order_relaxed));

    test_context_socket_close (router);
    test_context_socket_close (dealer);
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
    RUN_TEST (test_other_caller_final_consumes_input_and_keeps_staged_sequence);
    RUN_TEST (test_other_caller_different_family_is_independent);
    RUN_TEST (test_send_failures_consume_current_part);
    RUN_TEST (test_invalid_routed_targets_consume_parts_before_blocking_admission);
    RUN_TEST (test_unrouted_send_part_rejects_routed_only_socket_families);
    RUN_TEST (test_same_thread_failure_aborts_non_publish_sequence);
    RUN_TEST (test_publish_prevalidation_failures_preserve_open_sequence);
    RUN_TEST (test_consuming_failure_preserves_result_errno_from_free_callback);
    RUN_TEST (
      test_expired_zero_copy_prefix_is_released_by_other_caller_final_outside_scope);
    RUN_TEST (test_same_thread_keeps_sequences_on_two_sockets);
    RUN_TEST (test_five_part_sequence_spills_and_preserves_order);
    RUN_TEST (test_close_discards_all_staged_callers_without_owner_final);
    RUN_TEST (
      test_staged_request_blocked_final_is_released_by_context_shutdown);
    RUN_TEST (test_request_reply_failures_consume_final_part);
    RUN_TEST (test_blocking_multipart_request_releases_caller_parts_once);
    RUN_TEST (test_recv_part_returns_caller_owned_message_handle);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
