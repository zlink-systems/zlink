/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include <cstring>
#include <string>
#include <sstream>
#include <atomic>
#include <chrono>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

static const char k_pubsub_topic[] = "bench";

struct publish_commit_count_t
{
    zlink::pipe_t *pipe;
    int frames;
};

void count_publish_frame (zlink::pipe_t *pipe_, bool,
                          void *userdata_)
{
    publish_commit_count_t *count =
      static_cast<publish_commit_count_t *> (userdata_);
    if (pipe_ == count->pipe)
        ++count->frames;
}

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

void recv_subscribe_expect_two_parts (void *sub_)
{
    char topic[32] = {};
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_subscribe (
      sub_, NULL, &parts, &part_count, topic, &topic_len, 0));
    TEST_ASSERT_EQUAL_UINT64 (std::strlen (k_pubsub_topic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (k_pubsub_topic, topic, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    for (size_t i = 0; i != part_count; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (50, zlink_msg_size (&parts[i]));
        const std::string expected (50, static_cast<char> ('a' + i));
        TEST_ASSERT_EQUAL_MEMORY (expected.data (), zlink_msg_data (&parts[i]), 50);
    }
    zlink_multipart_close (parts, part_count);
}

void init_two_publish_parts (zlink_msg_t *parts_)
{
    for (int i = 0; i != 2; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts_[i], 50));
        memset (zlink_msg_data (&parts_[i]), 'a' + i, 50);
    }
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
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_xpub_recv (
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

void test_publish_record_admits_before_writing_any_part ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *slow = test_context_socket (ZLINK_SOCKET_SUB);
    void *fast = test_context_socket (ZLINK_SOCKET_SUB);
    set_timeout_opts (pub);
    set_timeout_opts (slow);
    set_timeout_opts (fast);
    const uint64_t pub_hwm = 350;
    const uint64_t slow_hwm = 350;
    const uint64_t fast_hwm = 4096;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_SNDHWM, &pub_hwm, sizeof (pub_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      slow, ZLINK_OPT_RCVHWM, &slow_hwm, sizeof (slow_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      fast, ZLINK_OPT_RCVHWM, &fast_hwm, sizeof (fast_hwm)));
    const int nodrop = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (
      pub, ZLINK_PUB_OPT_NODROP, &nodrop, sizeof (nodrop)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (slow, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (fast, k_pubsub_topic));
    contract_socket_pair_t slow_pair (pub, slow, 101, 1, true, 350);
    contract_socket_pair_t fast_pair (pub, fast, 102, 1, true, 4096);
    int subscribed = 0;
    char subscription_topic[16];
    size_t subscription_topic_size = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_xpub_recv (
      pub, NULL, &subscribed, subscription_topic,
      sizeof (subscription_topic), &subscription_topic_size,
      ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (1, subscribed);

    publish_payload (pub, std::string (100, 'p'));
    recv_subscribe_expect_topic_and_payload (fast, std::string (100, 'p'));

    zlink_msg_t parts[2];
    init_two_publish_parts (parts);
    publish_commit_count_t committed = {slow_pair.application[0], 0};
    zlink::test_set_pipe_write_commit_hook (&count_publish_frame, &committed);
    const zlink_submit_result_t result =
      zlink_publish (pub, k_pubsub_topic, parts, 2, ZLINK_DONTWAIT);
    zlink::test_set_pipe_write_commit_hook (NULL, NULL);
    TEST_ASSERT_EQUAL_INT (3, committed.frames);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[i]));

    recv_subscribe_expect_two_parts (fast);
    std::atomic<bool> finished (false);
    std::atomic<int> blocking_result (ZLINK_SUBMIT_BACKPRESSURED);
    std::atomic<int> sender_error (0);
    std::thread sender ([&] {
        zlink_msg_t blocked_parts[2];
        for (int i = 0; i != 2; ++i) {
            if (zlink_msg_init_size (&blocked_parts[i], 50) != 0) {
                sender_error.store (zlink_errno ());
                finished.store (true);
                return;
            }
            memset (zlink_msg_data (&blocked_parts[i]), 'a' + i, 50);
        }
        blocking_result.store (
          zlink_publish (pub, k_pubsub_topic, blocked_parts, 2, 0));
        for (int i = 0; i != 2; ++i)
            if (zlink_msg_close (&blocked_parts[i]) != 0)
                sender_error.store (zlink_errno ());
        finished.store (true);
    });
    bool credit_waiter = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!credit_waiter && !finished.load ()
           && std::chrono::steady_clock::now () < deadline) {
        slow_pair.application[0]->test_flow_probe (
          NULL, NULL, NULL, &credit_waiter, NULL);
        std::this_thread::yield ();
    }
    const bool waited_for_credit = credit_waiter && !finished.load ();
    recv_subscribe_expect_topic_and_payload (slow, std::string (100, 'p'));
    recv_subscribe_expect_two_parts (slow);
    sender.join ();
    TEST_ASSERT_TRUE (waited_for_credit);
    TEST_ASSERT_EQUAL_INT (0, sender_error.load ());
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, blocking_result.load ());
    recv_subscribe_expect_two_parts (fast);
    recv_subscribe_expect_two_parts (slow);

    publish_payload (pub, std::string (100, 'p'));
    recv_subscribe_expect_topic_and_payload (fast, std::string (100, 'p'));
    zlink_msg_t again[2];
    init_two_publish_parts (again);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_publish (
      pub, k_pubsub_topic, again, 2, ZLINK_DONTWAIT));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&again[i]));
    recv_subscribe_expect_two_parts (fast);
    zlink_msg_t refused[2];
    init_two_publish_parts (refused);
    committed.frames = 0;
    zlink::test_set_pipe_write_commit_hook (&count_publish_frame, &committed);
    const zlink_submit_result_t refused_result =
      zlink_publish (pub, k_pubsub_topic, refused, 2, ZLINK_DONTWAIT);
    zlink::test_set_pipe_write_commit_hook (NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, refused_result);
    TEST_ASSERT_EQUAL_INT (0, committed.frames);
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&refused[i]));
    const int zero_timeout = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_SNDTIMEO, &zero_timeout, sizeof (zero_timeout)));
    zlink_msg_t zero_timeout_parts[2];
    init_two_publish_parts (zero_timeout_parts);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, zlink_publish (
      pub, k_pubsub_topic, zero_timeout_parts, 2, 0));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&zero_timeout_parts[i]));
    char no_topic[32] = {};
    size_t no_topic_len = sizeof (no_topic);
    zlink_msg_t *no_parts = NULL;
    size_t no_part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (
      fast, NULL, &no_parts, &no_part_count, no_topic, &no_topic_len,
      ZLINK_RECV_FLAGS_DONTWAIT));

    test_context_socket_close_zero_linger (fast);
    test_context_socket_close_zero_linger (slow);
    test_context_socket_close_zero_linger (pub);
}

void test_lossy_publish_drops_whole_record_for_full_pipe ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *slow = test_context_socket (ZLINK_SOCKET_SUB);
    void *fast = test_context_socket (ZLINK_SOCKET_SUB);
    set_timeout_opts (pub);
    set_timeout_opts (slow);
    set_timeout_opts (fast);
    const uint64_t hwm = 350;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (slow, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (fast, k_pubsub_topic));
    contract_socket_pair_t slow_pair (pub, slow, 103, 1, true, hwm);
    contract_socket_pair_t fast_pair (pub, fast, 104, 1, true, 4096);
    int subscribed = 0;
    char subscription_topic[16];
    size_t subscription_topic_size = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_xpub_recv (
      pub, NULL, &subscribed, subscription_topic,
      sizeof (subscription_topic), &subscription_topic_size,
      ZLINK_RECV_FLAGS_DONTWAIT));
    publish_payload (pub, std::string (100, 'p'));
    zlink_msg_t second[2];
    init_two_publish_parts (second);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_publish (
      pub, k_pubsub_topic, second, 2, ZLINK_DONTWAIT));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&second[i]));
    recv_subscribe_expect_topic_and_payload (fast, std::string (100, 'p'));
    recv_subscribe_expect_two_parts (fast);
    zlink_msg_t third[2];
    init_two_publish_parts (third);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_publish (
      pub, k_pubsub_topic, third, 2, ZLINK_DONTWAIT));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&third[i]));
    recv_subscribe_expect_topic_and_payload (slow, std::string (100, 'p'));
    recv_subscribe_expect_two_parts (slow);
    char topic[32] = {};
    size_t topic_len = sizeof (topic);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (
      slow, NULL, &parts, &part_count, topic, &topic_len,
      ZLINK_RECV_FLAGS_DONTWAIT));
    recv_subscribe_expect_two_parts (fast);
    test_context_socket_close_zero_linger (fast);
    test_context_socket_close_zero_linger (slow);
    test_context_socket_close_zero_linger (pub);
}

void test_null_topic_and_empty_pipe_oversize_record ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    set_timeout_opts (pub);
    set_timeout_opts (sub);
    const uint64_t hwm = 50;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));
    contract_socket_pair_t pair (pub, sub, 105, 1, true, hwm);
    const zlink_auto_hwm_budget_snapshot_t before =
      read_auto_hwm_budget_snapshot (get_test_context ());
    zlink_msg_t parts[3];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], 5));
    memcpy (zlink_msg_data (&parts[0]), k_pubsub_topic, 5);
    for (int i = 1; i != 3; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[i], 50));
        memset (zlink_msg_data (&parts[i]), 'a' + i - 1, 50);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_publish (
      pub, NULL, parts, 3, ZLINK_DONTWAIT));
    const zlink_auto_hwm_budget_snapshot_t after =
      read_auto_hwm_budget_snapshot (get_test_context ());
    TEST_ASSERT_GREATER_THAN_UINT64 (before.oversize_admission_count,
                                     after.oversize_admission_count);
    TEST_ASSERT_GREATER_THAN_UINT64 (hwm,
                                     after.largest_oversize_message_bytes);
    for (int i = 0; i != 3; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[i]));
    recv_subscribe_expect_two_parts (sub);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_publish_size_failure_rolls_back_all_subscribers ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *first = test_context_socket (ZLINK_SOCKET_SUB);
    void *second = test_context_socket (ZLINK_SOCKET_SUB);
    set_timeout_opts (pub);
    set_timeout_opts (first);
    set_timeout_opts (second);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (first, k_pubsub_topic));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (second, k_pubsub_topic));
    contract_socket_pair_t first_pair (pub, first, 106, 1);
    contract_socket_pair_t second_pair (pub, second, 107, 1);
    second_pair.application[0]->set_max_message_bytes (100);
    zlink_msg_t parts[2];
    init_two_publish_parts (parts);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, zlink_publish (
      pub, k_pubsub_topic, parts, 2, ZLINK_DONTWAIT));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[i]));
    char topic[32] = {};
    size_t topic_len = sizeof (topic);
    zlink_msg_t *received = NULL;
    size_t received_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (
      first, NULL, &received, &received_count, topic, &topic_len,
      ZLINK_RECV_FLAGS_DONTWAIT));
    topic_len = sizeof (topic);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (
      second, NULL, &received, &received_count, topic, &topic_len,
      ZLINK_RECV_FLAGS_DONTWAIT));
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (first);
    test_context_socket_close_zero_linger (pub);
}

void test_pub_nodrop_admits_whole_record_once ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    set_timeout_opts (pub);
    set_timeout_opts (sub);
    const uint64_t hwm = 350;
    const int nodrop = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (
      pub, ZLINK_PUB_OPT_NODROP, &nodrop, sizeof (nodrop)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, k_pubsub_topic));
    contract_socket_pair_t pair (pub, sub, 108, 1, true, hwm);
    publish_payload (pub, std::string (100, 'p'));
    zlink_msg_t admitted[2];
    init_two_publish_parts (admitted);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_publish (
      pub, k_pubsub_topic, admitted, 2, ZLINK_DONTWAIT));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&admitted[i]));
    zlink_msg_t refused[2];
    init_two_publish_parts (refused);
    publish_commit_count_t committed = {pair.application[0], 0};
    zlink::test_set_pipe_write_commit_hook (&count_publish_frame, &committed);
    const zlink_submit_result_t result =
      zlink_publish (pub, k_pubsub_topic, refused, 2, ZLINK_DONTWAIT);
    zlink::test_set_pipe_write_commit_hook (NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
    TEST_ASSERT_EQUAL_INT (0, committed.frames);
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&refused[i]));
    recv_subscribe_expect_topic_and_payload (sub, std::string (100, 'p'));
    recv_subscribe_expect_two_parts (sub);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pubsub_publish_rollback_preserves_next_topic_boundary);
    RUN_TEST (test_publish_record_admits_before_writing_any_part);
    RUN_TEST (test_lossy_publish_drops_whole_record_for_full_pipe);
    RUN_TEST (test_null_topic_and_empty_pipe_oversize_record);
    RUN_TEST (test_publish_size_failure_rolls_back_all_subscribers);
    RUN_TEST (test_pub_nodrop_admits_whole_record_once);
    return UNITY_END ();
}
