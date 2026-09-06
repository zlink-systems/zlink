/* SPDX-License-Identifier: MPL-2.0 */

#include "helper_interleave_fixture.hpp"

SETUP_TEARDOWN_TESTCONTEXT


void test_pair_close_aborts_suspended_multipart_without_exposing_prefix ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://helper-interleave-close-between-parts"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://helper-interleave-close-between-parts"));

    zlink_msg_t head;
    init_part (&head, "close-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &head, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, NULL, NULL));
    const bool prefix_hidden_before_close = pair_has_no_record_for (receiver, 25);

    // Initialize the cleanup part before close starts. If the implementation
    // incorrectly waits for the suspended sequence, the original owner can
    // finish that sequence and deterministically unblock the closer.
    zlink_msg_t cleanup_final;
    init_part (&cleanup_final, "cleanup-final");
    close_between_parts_probe_t close_probe;
    std::thread closer ([&] {
        {
            std::unique_lock<std::mutex> lock (close_probe.mutex);
            close_probe.ready = true;
            close_probe.changed.notify_all ();
            close_probe.changed.wait (lock, [&] { return close_probe.go; });
        }

        errno = 0;
        const zlink_close_result_t result = zlink_close (sender);
        {
            std::lock_guard<std::mutex> lock (close_probe.mutex);
            close_probe.result = result;
            close_probe.done = true;
        }
        close_probe.changed.notify_all ();
    });

    bool close_worker_ready = false;
    {
        std::unique_lock<std::mutex> lock (close_probe.mutex);
        close_worker_ready = close_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return close_probe.ready; });
        close_probe.go = true;
    }
    close_probe.changed.notify_all ();

    bool close_completed_before_cleanup = false;
    {
        std::unique_lock<std::mutex> lock (close_probe.mutex);
        close_completed_before_cleanup = close_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return close_probe.done; });
    }

    bool cleanup_final_sent = false;
    zlink_submit_result_t cleanup_final_result = ZLINK_SUBMIT_OK;
    int cleanup_final_errno = 0;
    if (!close_completed_before_cleanup) {
        cleanup_final_sent = true;
        errno = 0;
        cleanup_final_result = zlink_send_part (
          sender, &cleanup_final, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
        cleanup_final_errno = errno;
    }

    bool close_completed_after_cleanup = close_completed_before_cleanup;
    if (!close_completed_after_cleanup) {
        std::unique_lock<std::mutex> lock (close_probe.mutex);
        close_completed_after_cleanup = close_probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&] { return close_probe.done; });
    }
    // FINAL above releases the only sequence state that can delay this close.
    // Joining is therefore safe even on the regression path, and no worker is
    // left behind when the assertion below reports the bounded-wait failure.
    closer.join ();

    if (close_probe.result != ZLINK_CLOSE_OK && !cleanup_final_sent) {
        cleanup_final_sent = true;
        errno = 0;
        cleanup_final_result = zlink_send_part (
          sender, &cleanup_final, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
        cleanup_final_errno = errno;
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&cleanup_final));

    zlink_close_result_t cleanup_close_result = ZLINK_CLOSE_OK;
    if (close_probe.result == ZLINK_CLOSE_OK) {
        test_context_socket_mark_closed (sender);
    } else {
        errno = 0;
        cleanup_close_result = zlink_close (sender);
        if (cleanup_close_result == ZLINK_CLOSE_OK)
            test_context_socket_mark_closed (sender);
    }

    const bool prefix_hidden_after_close =
      close_probe.result == ZLINK_CLOSE_OK && !cleanup_final_sent
        ? pair_has_no_record_for (receiver, 100)
        : true;

    TEST_ASSERT_TRUE_MESSAGE (
      prefix_hidden_before_close,
      "PAIR exposed an open multipart prefix before close");
    TEST_ASSERT_TRUE_MESSAGE (close_worker_ready, "close worker did not start");
    TEST_ASSERT_TRUE_MESSAGE (
      close_completed_before_cleanup,
      "zlink_close waited for a suspended multipart sequence");
    TEST_ASSERT_TRUE_MESSAGE (
      close_completed_after_cleanup,
      "zlink_close remained blocked after the multipart sequence was released");
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_probe.result);
    TEST_ASSERT_FALSE_MESSAGE (
      cleanup_final_sent,
      "close did not accept the suspended multipart sequence directly");
    TEST_ASSERT_TRUE_MESSAGE (
      prefix_hidden_after_close,
      "PAIR close exposed a staged multipart prefix to its peer");
    if (cleanup_final_sent) {
        TEST_ASSERT_TRUE (cleanup_final_result == ZLINK_SUBMIT_OK
                          || cleanup_final_errno == ESHUTDOWN);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, cleanup_close_result);

    test_context_socket_close_zero_linger (receiver);
}

void test_pair_peer_termination_races_local_multipart_cleanup ()
{
    const int rounds = 16;
    bool all_workers_ready = true;
    bool all_prefixes_hidden = true;
    bool all_closes_completed = true;
    bool all_close_results_ok = true;
    bool rescue_shutdown_needed = false;
    int completed_rounds = 0;

    for (int round = 0; round != rounds; ++round) {
        void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
        void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://helper-interleave-pair-close-%d", round);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));

        zlink_msg_t head;
        init_part (&head, "async-close-prefix");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &head, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE, NULL, NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&head));
        all_prefixes_hidden =
          pair_has_no_record_for (receiver, 10) && all_prefixes_hidden;

        paired_close_probe_t probe;
        std::thread local_close ([&] {
            {
                std::unique_lock<std::mutex> lock (probe.mutex);
                ++probe.ready;
                probe.changed.notify_all ();
                probe.changed.wait (lock, [&] { return probe.go; });
            }
            errno = 0;
            const zlink_close_result_t result = zlink_close (sender);
            const int terminal_errno = errno;
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                probe.local_result = result;
                probe.local_errno = terminal_errno;
                probe.local_done = true;
            }
            probe.changed.notify_all ();
        });
        std::thread peer_close ([&] {
            {
                std::unique_lock<std::mutex> lock (probe.mutex);
                ++probe.ready;
                probe.changed.notify_all ();
                probe.changed.wait (lock, [&] { return probe.go; });
            }
            errno = 0;
            const zlink_close_result_t result = zlink_close (receiver);
            const int terminal_errno = errno;
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                probe.peer_result = result;
                probe.peer_errno = terminal_errno;
                probe.peer_done = true;
            }
            probe.changed.notify_all ();
        });

        bool workers_ready = false;
        {
            std::unique_lock<std::mutex> lock (probe.mutex);
            workers_ready = probe.changed.wait_for (
              lock, std::chrono::seconds (3), [&] { return probe.ready == 2; });
            probe.go = true;
        }
        probe.changed.notify_all ();

        bool closes_completed = false;
        {
            std::unique_lock<std::mutex> lock (probe.mutex);
            closes_completed = probe.changed.wait_for (
              lock, std::chrono::seconds (2),
              [&] { return probe.local_done && probe.peer_done; });
        }

        // A context shutdown is used only as a failure-path escape hatch, so
        // even a lifecycle deadlock cannot leave joinable workers behind.
        if (!closes_completed) {
            rescue_shutdown_needed = true;
            (void) zlink_ctx_shutdown (get_test_context ());
            std::unique_lock<std::mutex> lock (probe.mutex);
            closes_completed = probe.changed.wait_for (
              lock, std::chrono::seconds (3),
              [&] { return probe.local_done && probe.peer_done; });
        }

        local_close.join ();
        peer_close.join ();

        all_workers_ready = workers_ready && all_workers_ready;
        all_closes_completed = closes_completed && all_closes_completed;
        all_close_results_ok =
          probe.local_result == ZLINK_CLOSE_OK
          && probe.peer_result == ZLINK_CLOSE_OK && all_close_results_ok;

        if (probe.local_result == ZLINK_CLOSE_OK)
            test_context_socket_mark_closed (sender);
        if (probe.peer_result == ZLINK_CLOSE_OK)
            test_context_socket_mark_closed (receiver);

        ++completed_rounds;
        if (rescue_shutdown_needed)
            break;
    }

    TEST_ASSERT_TRUE_MESSAGE (
      all_workers_ready, "PAIR close-race workers did not reach their barrier");
    TEST_ASSERT_TRUE_MESSAGE (
      all_prefixes_hidden,
      "PAIR exposed a parked multipart prefix before peer termination");
    TEST_ASSERT_FALSE_MESSAGE (
      rescue_shutdown_needed,
      "PAIR termination/cleanup race required context shutdown");
    TEST_ASSERT_TRUE_MESSAGE (
      all_closes_completed,
      "PAIR termination/cleanup race did not complete both closes");
    TEST_ASSERT_TRUE_MESSAGE (
      all_close_results_ok,
      "PAIR termination/cleanup race returned a close error");
    TEST_ASSERT_EQUAL_INT (rounds, completed_rounds);
}

void test_publish_validation_failure_releases_sync_for_query_before_final ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    const char *const endpoint =
      "inproc://helper-interleave-publish-query-after-reject";
    const char *const topic = "topic-a";

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME * 2);

    zlink_msg_t head;
    init_part (&head, "publish-head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_publish_part (pub, topic, &head, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_MORE));

    zlink_msg_t wrong_topic;
    init_part (&wrong_topic, "wrong-topic-final");
    errno = 0;
    const zlink_submit_result_t wrong_topic_result = zlink_publish_part (
      pub, "topic-b", &wrong_topic, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
    const int wrong_topic_errno = errno;
    const size_t wrong_topic_remaining_size = zlink_msg_size (&wrong_topic);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_topic));

    // Prepare FINAL before starting the query so every failure path can
    // release the persistent sequence marker before joining the worker.
    zlink_msg_t tail;
    init_part (&tail, "publish-tail");
    option_query_probe_t query_probe;
    std::thread query ([&] {
        {
            std::unique_lock<std::mutex> lock (query_probe.mutex);
            query_probe.ready = true;
            query_probe.changed.notify_all ();
            query_probe.changed.wait (lock, [&] { return query_probe.go; });
        }

        int value = -1;
        size_t value_size = sizeof (value);
        errno = 0;
        const zlink_config_result_t result = zlink_get_pub_option (
          pub, ZLINK_PUB_OPT_NODROP, &value, &value_size);
        {
            std::lock_guard<std::mutex> lock (query_probe.mutex);
            query_probe.result = result;
            query_probe.value = value;
            query_probe.value_size = value_size;
            query_probe.done = true;
        }
        query_probe.changed.notify_all ();
    });

    bool query_ready = false;
    {
        std::unique_lock<std::mutex> lock (query_probe.mutex);
        query_ready = query_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return query_probe.ready; });
        query_probe.go = true;
    }
    query_probe.changed.notify_all ();

    bool query_completed_before_final = false;
    {
        std::unique_lock<std::mutex> lock (query_probe.mutex);
        query_completed_before_final = query_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return query_probe.done; });
    }

    errno = 0;
    const zlink_submit_result_t tail_result = zlink_publish_part (
      pub, topic, &tail, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);

    bool query_completed_after_final = query_completed_before_final;
    if (!query_completed_after_final) {
        std::unique_lock<std::mutex> lock (query_probe.mutex);
        query_completed_after_final = query_probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&] { return query_probe.done; });
    }
    // A leaked sync bit is released by the owner-thread FINAL above. Join
    // before asserting the pre-FINAL deadline so the regression is reported
    // without leaving a waiter alive in the test process.
    query.join ();

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_topic_result);
    TEST_ASSERT_EQUAL_INT (EINVAL, wrong_topic_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, wrong_topic_remaining_size);
    TEST_ASSERT_TRUE_MESSAGE (query_ready, "query worker did not start");
    TEST_ASSERT_TRUE_MESSAGE (
      query_completed_before_final,
      "publish prevalidation failure retained the public sync bit until FINAL");
    TEST_ASSERT_TRUE_MESSAGE (
      query_completed_after_final,
      "option query remained blocked after publish FINAL released the sequence");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, query_probe.result);
    TEST_ASSERT_EQUAL_INT (0, query_probe.value);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (int), query_probe.value_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, tail_result);
    TEST_ASSERT_TRUE (recv_published_record_eventually (
      sub, topic, {"publish-head", "publish-tail"}));

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}





void test_wrong_send_helper_aborts_open_sequence ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-interleave-dealer"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://helper-interleave-dealer"));
    msleep (SETTLE_TIME);

    zlink_msg_t first;
    init_part (&first, "hello");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t wrong_family;
    init_part (&wrong_family, "request");
    zlink_completion_id_t ignored_completion_id = 0;
    const zlink_submit_result_t wrong_rc =
      zlink_request_part (dealer, NULL, &wrong_family,
                          static_cast<zlink_send_flags_t> (0),
                          ZLINK_PART_FINAL, 1000, NULL,
                          &ignored_completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_family));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_family));

    zlink_msg_t last;
    init_part (&last, "world");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &last, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));

    recv_routed_string_expect_success (router, "world", "D1");

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_target_change_aborts_open_routed_sequence ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = test_context_socket (ZLINK_SOCKET_DEALER);

    const int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-interleave-rid"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://helper-interleave-rid"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://helper-interleave-rid"));
    msleep (SETTLE_TIME);

    send_string_expect_success (dealer1, "prime-1", 0);
    recv_routed_string_expect_success (router, "prime-1", "D1");
    send_string_expect_success (dealer2, "prime-2", 0);
    recv_routed_string_expect_success (router, "prime-2", "D2");

    zlink_routing_id_t rid1;
    zlink_routing_id_t rid2;
    memset (&rid1, 0, sizeof (rid1));
    memset (&rid2, 0, sizeof (rid2));
    memcpy (rid1.data, "D1", 2);
    memcpy (rid2.data, "D2", 2);
    rid1.size = 2;
    rid2.size = 2;

    zlink_msg_t first;
    init_part (&first, "frame-a");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (router, &rid1, &first,
                                            static_cast<zlink_send_flags_t> (0),
                                            ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t wrong_target;
    init_part (&wrong_target, "frame-b");
    const zlink_submit_result_t wrong_rc = zlink_send_part_rid (
      router, &rid2, &wrong_target, static_cast<zlink_send_flags_t> (0),
      ZLINK_PART_MORE, NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_target));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_target));

    zlink_msg_t last;
    init_part (&last, "frame-c");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (router, &rid1, &last,
                                            static_cast<zlink_send_flags_t> (0),
                                            ZLINK_PART_FINAL, NULL, NULL));

    recv_string_expect_success (dealer1, "frame-c", 0);

    test_context_socket_close_zero_linger (dealer2);
    test_context_socket_close_zero_linger (dealer1);
    test_context_socket_close_zero_linger (router);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pair_close_aborts_suspended_multipart_without_exposing_prefix);
    RUN_TEST (
      test_pair_peer_termination_races_local_multipart_cleanup);
    RUN_TEST (test_publish_validation_failure_releases_sync_for_query_before_final);
    RUN_TEST (test_wrong_send_helper_aborts_open_sequence);
    RUN_TEST (test_target_change_aborts_open_routed_sequence);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
