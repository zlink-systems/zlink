/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "../src/api/socket/socket_request_reply_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct reply_probe_t
{
    reply_probe_t () : done (false), callback_count (0), result (ZLINK_REQUEST_PROTOCOL_ERROR) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    int callback_count;
    zlink_request_result_t result;
};

void capture_reply (zlink_request_result_t result_, zlink_msg_t *, size_t, void *userdata_)
{
    reply_probe_t *probe = static_cast<reply_probe_t *> (userdata_);
    TEST_ASSERT_NOT_NULL (probe);

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
        ++probe->callback_count;
        probe->result = result_;
    }
    probe->cv.notify_all ();
}

bool wait_for_reply (reply_probe_t *probe_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (1000),
                                [probe_] () { return probe_->done; });
}

bool wait_for_reply_with_socket_progress (void *socket_, reply_probe_t *probe_, int timeout_ms_)
{
    const socket_handle_t handle = as_socket_handle (socket_);
    const std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      zlink::socket_reqrep_internal::find_request_reply_state (handle);
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        if (wait_for_reply (probe_))
            return true;
        if (state)
            (void) zlink::socket_reqrep_internal::drain_reply_completions (state, socket_);
    }

    return false;
}

void set_recv_timeout_ms (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

zlink_recv_result_t recv_router_part_with_retry (void *router_,
                                                 const zlink_routing_id_t **source_node_rid_out_,
                                                 uint64_t *request_seq_out_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc = zlink_router_recv_part (
          router_, source_node_rid_out_, request_seq_out_, part_out_,
          has_more_out_, static_cast<zlink_recv_flags_t> (0));
        if (rc == ZLINK_RECV_OK)
            return rc;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_router_recv_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

zlink_recv_result_t recv_subscribe_part_with_retry (void *sub_,
                                                    char *topic_id_buf_,
                                                    size_t topic_id_capacity_,
                                                    size_t *topic_id_len_out_,
                                                    zlink_msg_t *part_out_,
                                                    zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc =
          zlink_subscribe_part (sub_, NULL, topic_id_buf_, topic_id_capacity_, topic_id_len_out_,
                                part_out_, has_more_out_, static_cast<zlink_recv_flags_t> (0));
        if (rc == ZLINK_RECV_OK || zlink_errno () == EMSGSIZE)
            return rc;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_subscribe_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void send_dealer_request (void *dealer_,
                          const char *part0_,
                          const char *part1_,
                          reply_probe_t *probe_)
{
    zlink_msg_t parts[2];
    init_part (&parts[0], part0_);
    init_part (&parts[1], part1_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_dealer_request (dealer_, parts, 2, &capture_reply, probe_,
                                                     static_cast<zlink_send_flags_t> (0), 3000));
}

void send_dealer_request_single (void *dealer_, const char *part0_, reply_probe_t *probe_)
{
    zlink_msg_t part;
    init_part (&part, part0_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_dealer_request (dealer_, &part, 1, &capture_reply, probe_,
                                                     static_cast<zlink_send_flags_t> (0), 3000));
}

void reply_from_router (void *router_,
                        const zlink_routing_id_t *peer_rid_,
                        uint64_t request_seq_,
                        const char *payload_)
{
    zlink_msg_t part;
    init_part (&part, payload_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_reply (router_, peer_rid_, request_seq_, &part, 1));
}

void publish_and_recv_subscribe_part_eventually (void *pub_,
                                                 void *sub_,
                                                 const char *topic_,
                                                 char *topic_id_buf_,
                                                 size_t topic_id_capacity_,
                                                 size_t *topic_id_len_out_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_,
                                                 zlink_recv_result_t expected_rc_,
                                                 int expected_errno_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        init_part (&part, expected_errno_ == EMSGSIZE ? "payload-1" : "payload-2");
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_publish (pub_, topic_, &part, 1, static_cast<zlink_send_flags_t> (0)));

        const zlink_recv_result_t rc = zlink_subscribe_part (
          sub_, NULL, topic_id_buf_, topic_id_capacity_, topic_id_len_out_, part_out_,
          has_more_out_, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == expected_rc_ && zlink_errno () == expected_errno_)
            return;
        if (rc == ZLINK_RECV_OK && expected_rc_ == ZLINK_RECV_OK)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for publish/subscribe helper exchange");
}

}

void test_recv_part_reads_each_part_and_tracks_has_more ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (receiver, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (receiver, "inproc://helper-recv-part-basic"));
    set_recv_timeout_ms (receiver, 50);
    msleep (SETTLE_TIME);

    zlink_msg_t ping;
    init_part (&ping, "prime");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (receiver, &ping, 1, static_cast<zlink_send_flags_t> (0)));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *primed = NULL;
    size_t primed_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router, &source_rid,
                                                  &request_seq, &primed, &primed_count,
                                                  static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    zlink_multipart_close (primed, primed_count);

    zlink_msg_t parts[2];
    init_part (&parts[0], "one");
    init_part (&parts[1], "two");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_rid (router, source_rid, parts, 2, static_cast<zlink_send_flags_t> (0)));
    msleep (SETTLE_TIME);

    const char *expected[2] = {"one", "two"};
    for (int i = 0; i < 2; ++i) {
        zlink_msg_t part;
        zlink_msg_init (&part);
        const zlink_routing_id_t *part_source_rid =
          reinterpret_cast<const zlink_routing_id_t *> (0x1);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_part (receiver, &part_source_rid, &part, &has_more,
                                                    static_cast<zlink_recv_flags_t> (0)));
        TEST_ASSERT_NULL (part_source_rid);
        TEST_ASSERT_EQUAL_UINT64 (strlen (expected[i]), zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY (expected[i], zlink_msg_data (&part), strlen (expected[i]));
        TEST_ASSERT_EQUAL_INT (i == 1 ? 0 : 1, has_more);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    test_context_socket_close_zero_linger (receiver);
    test_context_socket_close_zero_linger (router);
}

void test_router_recv_part_metadata_view_invalidates_on_next_recv_like_call ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-router-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://helper-router-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://helper-router-recv-part-basic"));
    set_recv_timeout_ms (router, 50);
    msleep (SETTLE_TIME);

    const zlink_routing_id_t *primed_source_rid = NULL;
    uint64_t primed_request_seq = 0;
    zlink_msg_t *primed_parts = NULL;
    size_t primed_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA, zlink_router_recv (router, &primed_source_rid,
                                             &primed_request_seq, &primed_parts, &primed_part_count,
                                             static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    reply_probe_t probe1;
    send_dealer_request_single (dealer1, "first", &probe1);

    const zlink_routing_id_t *first_source_rid = NULL;
    uint64_t first_request_seq = 0;
    zlink_msg_t part;
    zlink_msg_init (&part);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (recv_router_part_with_retry (
      router, &first_source_rid, &first_request_seq, &part, &has_more));
    TEST_ASSERT_NOT_NULL (first_source_rid);
    TEST_ASSERT_TRUE (first_request_seq != 0);
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("D1", first_source_rid->data, 2);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&part), 5);
    zlink_routing_id_t first_source_copy = *first_source_rid;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

    reply_from_router (router, first_source_rid, first_request_seq, "reply-1");
    TEST_ASSERT_TRUE (wait_for_reply_with_socket_progress (dealer1, &probe1, 3000));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe1.result);

    reply_probe_t probe2;
    send_dealer_request_single (dealer2, "second", &probe2);

    const zlink_routing_id_t *second_source_rid = NULL;
    const zlink_routing_id_t *second_spot_rid = NULL;
    uint64_t second_request_seq = 0;
    zlink_msg_init (&part);
    has_more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (recv_router_part_with_retry (
      router, &second_source_rid, &second_request_seq, &part, &has_more));
    TEST_ASSERT_NOT_NULL (second_source_rid);
    TEST_ASSERT_EQUAL_MEMORY ("D2", second_source_rid->data, 2);
    TEST_ASSERT_EQUAL_INT (0, has_more);

    TEST_ASSERT_EQUAL_MEMORY (second_source_rid->data, first_source_rid->data,
                              second_source_rid->size);
    TEST_ASSERT_FALSE (
      first_source_copy.size == first_source_rid->size
      && memcmp (first_source_copy.data, first_source_rid->data, first_source_copy.size) == 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    reply_from_router (router, second_source_rid, second_request_seq, "reply-2");
    TEST_ASSERT_TRUE (wait_for_reply_with_socket_progress (dealer2, &probe2, 3000));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe2.result);

    test_context_socket_close_zero_linger (dealer2);
    test_context_socket_close_zero_linger (dealer1);
    test_context_socket_close_zero_linger (router);
}

void test_subscribe_part_reports_needed_topic_size_and_recovers_after_emsgsize ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://helper-subscribe-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://helper-subscribe-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    set_recv_timeout_ms (sub, 50);
    msleep (SETTLE_TIME * 6);

    char small_topic[2] = {'x', 'y'};
    size_t topic_len = 0;
    zlink_msg_t payload;
    zlink_msg_init (&payload);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    publish_and_recv_subscribe_part_eventually (pub, sub, "topic-1", small_topic,
                                                sizeof (small_topic), &topic_len, &payload,
                                                &has_more, ZLINK_RECV_INTERNAL_ERROR, EMSGSIZE);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (7, topic_len);
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("payload-1", zlink_msg_data (&payload), 9);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));
    TEST_ASSERT_EQUAL_MEMORY ("xy", small_topic, sizeof (small_topic));

    char sentinel[4] = {'k', 'e', 'e', 'p'};
    topic_len = 0;
    zlink_msg_init (&payload);
    has_more = ZLINK_PART_FINAL;
    publish_and_recv_subscribe_part_eventually (pub, sub, "topic-2", sentinel, 0, &topic_len,
                                                &payload, &has_more, ZLINK_RECV_OK, 0);
    TEST_ASSERT_EQUAL_UINT64 (7, topic_len);
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("keep", sentinel, sizeof (sentinel));
    TEST_ASSERT_EQUAL_MEMORY ("payload-2", zlink_msg_data (&payload), 9);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_recv_part_reads_each_part_and_tracks_has_more);
    RUN_TEST (test_router_recv_part_metadata_view_invalidates_on_next_recv_like_call);
    RUN_TEST (test_subscribe_part_reports_needed_topic_size_and_recovers_after_emsgsize);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
