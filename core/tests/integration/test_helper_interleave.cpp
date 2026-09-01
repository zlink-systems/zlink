/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string.h>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void init_tagged_part (zlink_msg_t *part_, unsigned char kind_, int round_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, 1 + sizeof (round_)));
    unsigned char *data = static_cast<unsigned char *> (zlink_msg_data (part_));
    data[0] = kind_;
    memcpy (data + 1, &round_, sizeof (round_));
}

bool pair_has_no_record_for (void *receiver_, int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            zlink_multipart_close (parts, part_count);
            return false;
        }
        if (rc != ZLINK_RECV_NO_DATA)
            return false;
        msleep (1);
    }
    return true;
}

bool recv_pair_record_eventually (
  void *receiver_, const std::vector<std::string> &expected_parts_,
  int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        bool matches = part_count == expected_parts_.size ();
        for (size_t i = 0; matches && i < part_count; ++i) {
            matches = zlink_msg_size (&parts[i]) == expected_parts_[i].size ()
                      && memcmp (zlink_msg_data (&parts[i]),
                                 expected_parts_[i].data (),
                                 expected_parts_[i].size ()) == 0;
        }
        zlink_multipart_close (parts, part_count);
        return matches;
    }
    return false;
}

bool recv_pair_record_eventually (void *receiver_,
                                  std::vector<std::string> *parts_out_,
                                  int timeout_ms_)
{
    if (!parts_out_)
        return false;
    parts_out_->clear ();
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        for (size_t i = 0; i != part_count; ++i) {
            parts_out_->push_back (std::string (
              static_cast<const char *> (zlink_msg_data (&parts[i])),
              zlink_msg_size (&parts[i])));
        }
        zlink_multipart_close (parts, part_count);
        return true;
    }
    return false;
}

bool recv_published_record_eventually (
  void *subscriber_, const char *expected_topic_,
  const std::vector<std::string> &expected_parts_, int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        char topic[64];
        size_t topic_len = sizeof (topic);
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = zlink_subscribe (
          subscriber_, NULL, &parts, &part_count, topic, &topic_len,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        const size_t expected_topic_len = strlen (expected_topic_);
        bool matches = topic_len == expected_topic_len
                       && memcmp (topic, expected_topic_, topic_len) == 0
                       && part_count == expected_parts_.size ();
        for (size_t i = 0; matches && i < part_count; ++i) {
            matches = zlink_msg_size (&parts[i]) == expected_parts_[i].size ()
                      && memcmp (zlink_msg_data (&parts[i]),
                                 expected_parts_[i].data (),
                                 expected_parts_[i].size ()) == 0;
        }
        zlink_multipart_close (parts, part_count);
        return matches;
    }
    return false;
}

struct close_between_parts_probe_t
{
    close_between_parts_probe_t () : ready (false), go (false), done (false),
                                      result (ZLINK_CLOSE_INTERNAL_ERROR)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    bool done;
    zlink_close_result_t result;
};

struct option_query_probe_t
{
    option_query_probe_t () : ready (false), go (false), done (false),
                               result (ZLINK_CONFIG_INTERNAL_ERROR),
                               value (-1), value_size (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    bool done;
    zlink_config_result_t result;
    int value;
    size_t value_size;
};

struct one_call_send_probe_t
{
    one_call_send_probe_t () : ready (false), go (false), calling (false),
                                done (false), result (-1), terminal_errno (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ready;
    bool go;
    std::atomic<bool> calling;
    std::atomic<bool> done;
    int result;
    int terminal_errno;
};

struct paired_close_probe_t
{
    paired_close_probe_t () : ready (0), go (false), local_done (false),
                              peer_done (false),
                              local_result (ZLINK_CLOSE_INTERNAL_ERROR),
                              peer_result (ZLINK_CLOSE_INTERNAL_ERROR),
                              local_errno (0), peer_errno (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    int ready;
    bool go;
    bool local_done;
    bool peer_done;
    zlink_close_result_t local_result;
    zlink_close_result_t peer_result;
    int local_errno;
    int peer_errno;
};

class complete_record_admission_guard_t
{
  public:
    explicit complete_record_admission_guard_t (zlink::socket_base_t *socket_) :
        _scope ()
    {
        if (socket_)
            _scope = socket_->begin_complete_send_scope (false);
    }

    ~complete_record_admission_guard_t ()
    {
        release ();
    }

    bool acquired () const
    {
        return _scope && _scope->acquired ();
    }

    bool multipart_active () const
    {
        return _scope && _scope->multipart_active ();
    }

    void release ()
    {
        _scope.reset ();
    }

  private:
    std::unique_ptr<zlink::socket_public_send_scope_t> _scope;
};
}

void test_complete_record_admission_rejects_new_multipart_sequence ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://helper-interleave-complete-first"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://helper-interleave-complete-first"));

    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    complete_record_admission_guard_t complete_record (sender_handle.socket);
    TEST_ASSERT_TRUE (complete_record.acquired ());
    TEST_ASSERT_FALSE (complete_record.multipart_active ());

    zlink_submit_result_t rejected_rc = ZLINK_SUBMIT_OK;
    int rejected_errno = 0;
    size_t rejected_remaining_size = 0;
    int rejected_close_rc = ZLINK_CONFIG_OK;
    bool contender_init_failed = false;
    std::thread contender ([&] {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, 12) != ZLINK_CONFIG_OK) {
            contender_init_failed = true;
            return;
        }
        memcpy (zlink_msg_data (&part), "blocked-more", 12);
        errno = 0;
        rejected_rc = zlink_send_part (sender, &part,
                                       static_cast<zlink_send_flags_t> (0),
                                       ZLINK_PART_MORE, NULL, NULL);
        rejected_errno = zlink_errno ();
        rejected_remaining_size = zlink_msg_size (&part);
        rejected_close_rc = zlink_msg_close (&part);
    });
    contender.join ();

    // Release the simulated complete-record public-boundary scope before any
    // assertion can return early, then prove the rejected attempt did not
    // poison the next multipart sequence.
    complete_record.release ();
    sender_handle = socket_handle_t ();

    TEST_ASSERT_FALSE (contender_init_failed);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, rejected_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, rejected_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_remaining_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, rejected_close_rc);

    zlink_msg_t accepted_more;
    init_part (&accepted_more, "accepted-more");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_more, static_cast<zlink_send_flags_t> (0),
                       ZLINK_PART_MORE, NULL, NULL));
    zlink_msg_t accepted_final;
    init_part (&accepted_final, "accepted-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_final, static_cast<zlink_send_flags_t> (0),
                       ZLINK_PART_FINAL, NULL, NULL));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &parts, &part_count,
                  static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (13, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("accepted-more", zlink_msg_data (&parts[0]), 13);
    TEST_ASSERT_EQUAL_UINT64 (14, zlink_msg_size (&parts[1]));
    TEST_ASSERT_EQUAL_MEMORY ("accepted-final", zlink_msg_data (&parts[1]), 14);
    zlink_multipart_close (parts, part_count);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

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

void run_pair_one_call_multipart_backpressure_abort_round (int round_)
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t hwm = 1024;
    const int send_timeout_ms = 1500;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      sender, ZLINK_OPT_SNDTIMEO, &send_timeout_ms, sizeof (send_timeout_ms)));
    char endpoint[96];
    snprintf (endpoint, sizeof (endpoint),
              "inproc://helper-interleave-pair-retry-final-%d", round_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
    // With byte accounting included these sizes produce:
    // filler + prefix <= HWM, but filler + prefix + original FINAL > HWM.
    // PAIR must atomically roll back the staged prefix and return EAGAIN when
    // the later frame is rejected. The smaller concurrent complete record
    // would otherwise fit and expose a mixed record through that prefix.
    const std::string filler_payload (400, 'L');
    const std::string prefix_payload (128, 'P');
    const std::string original_final_payload (400, 'O');
    const std::string concurrent_final_payload (32, 'C');

    zlink_msg_t filler;
    init_part (&filler, filler_payload.c_str ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink_socket_send_internal (
           sender, &filler, 1, static_cast<zlink_send_flags_t> (0)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));

    zlink_msg_t original_parts[2];
    init_part (&original_parts[0], prefix_payload.c_str ());
    init_part (&original_parts[1], original_final_payload.c_str ());
    zlink_msg_t concurrent_final;
    init_part (&concurrent_final, concurrent_final_payload.c_str ());

    one_call_send_probe_t original_probe;
    one_call_send_probe_t concurrent_probe;
    std::thread original ([&] {
        {
            std::unique_lock<std::mutex> lock (original_probe.mutex);
            original_probe.ready = true;
            original_probe.changed.notify_all ();
            original_probe.changed.wait (lock, [&] { return original_probe.go; });
        }
        original_probe.calling.store (true, std::memory_order_release);
        original_probe.changed.notify_all ();
        errno = 0;
        original_probe.result = zlink_socket_send_internal (
          sender, original_parts, 2, static_cast<zlink_send_flags_t> (0));
        original_probe.terminal_errno = errno;
        original_probe.done.store (true, std::memory_order_release);
        original_probe.changed.notify_all ();
    });
    std::thread concurrent ([&] {
        {
            std::unique_lock<std::mutex> lock (concurrent_probe.mutex);
            concurrent_probe.ready = true;
            concurrent_probe.changed.notify_all ();
            concurrent_probe.changed.wait (lock, [&] { return concurrent_probe.go; });
        }
        concurrent_probe.calling.store (true, std::memory_order_release);
        concurrent_probe.changed.notify_all ();
        errno = 0;
        concurrent_probe.result = zlink_socket_send_internal (
          sender, &concurrent_final, 1,
          static_cast<zlink_send_flags_t> (0));
        concurrent_probe.terminal_errno = errno;
        concurrent_probe.done.store (true, std::memory_order_release);
        concurrent_probe.changed.notify_all ();
    });

    bool original_ready = false;
    bool concurrent_ready = false;
    {
        std::unique_lock<std::mutex> lock (original_probe.mutex);
        original_ready = original_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return original_probe.ready; });
    }
    {
        std::unique_lock<std::mutex> lock (concurrent_probe.mutex);
        concurrent_ready = concurrent_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750),
          [&] { return concurrent_probe.ready; });
    }

    {
        std::lock_guard<std::mutex> lock (original_probe.mutex);
        original_probe.go = true;
    }
    original_probe.changed.notify_all ();

    bool original_calling = false;
    {
        std::unique_lock<std::mutex> lock (original_probe.mutex);
        original_calling = original_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750), [&] {
              return original_probe.calling.load (std::memory_order_acquire);
          });
    }

    // Release the already-ready complete call as soon as the multipart call
    // crosses its boundary. There is no source hook at the xsend failure, so
    // the record-boundary assertions below are the deterministic invariant;
    // the paired start still exercises the concurrent admission path.
    {
        std::lock_guard<std::mutex> lock (concurrent_probe.mutex);
        concurrent_probe.go = true;
    }
    concurrent_probe.changed.notify_all ();

    bool original_aborted_before_drain = false;
    if (original_calling) {
        std::unique_lock<std::mutex> lock (original_probe.mutex);
        original_aborted_before_drain = original_probe.changed.wait_for (
          lock, std::chrono::milliseconds (750), [&] {
              return original_probe.done.load (std::memory_order_acquire);
          });
    }

    // Drain only the already-committed filler after observing the required
    // immediate abort. This also gives both workers a bounded cleanup path if
    // an older implementation incorrectly remained in its blocking retry.
    const bool filler_received = recv_pair_record_eventually (
      receiver, {filler_payload}, 750);

    bool original_completed_after_drain =
      original_probe.done.load (std::memory_order_acquire);
    if (!original_completed_after_drain) {
        std::unique_lock<std::mutex> lock (original_probe.mutex);
        original_completed_after_drain = original_probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&] {
              return original_probe.done.load (std::memory_order_acquire);
          });
    }
    bool concurrent_completed_after_drain =
      concurrent_probe.done.load (std::memory_order_acquire);
    if (!concurrent_completed_after_drain) {
        std::unique_lock<std::mutex> lock (concurrent_probe.mutex);
        concurrent_completed_after_drain = concurrent_probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&] {
              return concurrent_probe.done.load (std::memory_order_acquire);
          });
    }
    original.join ();
    concurrent.join ();

    int close_errors = 0;
    for (size_t i = 0; i != 2; ++i) {
        if (zlink_msg_close (&original_parts[i]) != ZLINK_CONFIG_OK)
            ++close_errors;
    }
    if (zlink_msg_close (&concurrent_final) != ZLINK_CONFIG_OK)
        ++close_errors;

    const bool original_retryable_abort =
      original_probe.result == -1 && original_probe.terminal_errno == EAGAIN;
    std::vector<std::vector<std::string> > received_records;
    for (size_t i = 0; i != 1; ++i) {
        std::vector<std::string> record;
        if (!recv_pair_record_eventually (receiver, &record, 750))
            break;
        received_records.push_back (record);
    }
    const std::vector<std::string> expected_original = {
      prefix_payload, original_final_payload};
    const std::vector<std::string> expected_concurrent = {
      concurrent_final_payload};
    size_t original_boundary_count = 0;
    size_t concurrent_boundary_count = 0;
    for (size_t i = 0; i != received_records.size (); ++i) {
        if (received_records[i] == expected_original)
            ++original_boundary_count;
        if (received_records[i] == expected_concurrent)
            ++concurrent_boundary_count;
    }
    const bool original_boundary_ok = original_boundary_count == 0;
    const bool concurrent_boundary_ok = concurrent_boundary_count == 1;
    const bool expected_record_count_received =
      received_records.size () == 1;
    const bool no_extra_record = pair_has_no_record_for (receiver, 50);

    // Close before reporting any assertion. Unity returns from this helper on
    // failure, and the outer bounded repetition must never inherit live async
    // owners or sockets from an earlier corrupt round.
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);

    TEST_ASSERT_TRUE (original_ready);
    TEST_ASSERT_TRUE (concurrent_ready);
    TEST_ASSERT_TRUE_MESSAGE (
      original_calling,
      "one-call multipart worker did not reach its call boundary");
    TEST_ASSERT_TRUE_MESSAGE (
      original_aborted_before_drain,
      "PAIR later-frame EAGAIN did not atomically abort before peer credit");
    TEST_ASSERT_TRUE (filler_received);
    TEST_ASSERT_TRUE_MESSAGE (
      original_completed_after_drain,
      "original multipart remained blocked after peer credit was returned");
    TEST_ASSERT_TRUE_MESSAGE (
      concurrent_completed_after_drain,
      "concurrent FINAL remained blocked after peer credit was returned");
    TEST_ASSERT_TRUE (original_retryable_abort);
    TEST_ASSERT_EQUAL_INT (0, concurrent_probe.result);
    TEST_ASSERT_EQUAL_INT (0, close_errors);
    TEST_ASSERT_TRUE_MESSAGE (
      expected_record_count_received,
      "multipart abort did not produce the expected number of records");
    TEST_ASSERT_TRUE_MESSAGE (
      original_boundary_ok,
      "original multipart prefix was committed with a concurrent FINAL");
    TEST_ASSERT_TRUE_MESSAGE (
      concurrent_boundary_ok,
      "concurrent FINAL was not delivered as one standalone record");
    TEST_ASSERT_TRUE_MESSAGE (
      no_extra_record,
      "multipart abort exposed an additional partial record");
}

void test_pair_one_call_multipart_backpressure_aborts_before_concurrent_final ()
{
    // Repeat the bounded concurrent handoff so TSAN and ordinary builds both
    // exercise more than one owner ordering. Correctness does not depend on a
    // particular ordering: every later-frame rejection must abort atomically.
    for (int round = 0; round != 4; ++round)
        run_pair_one_call_multipart_backpressure_abort_round (round);
}

void test_pair_whole_multipart_does_not_interleave_concurrent_final_records ()
{
    const int multipart_rounds = 1000;
    const int single_rounds = 4000;
    const size_t multipart_part_count = 8;
    const size_t tagged_size = 1 + sizeof (int);

    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t hwm = 64u * 1024u * 1024u;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://helper-interleave-pair-complete-records"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://helper-interleave-pair-complete-records"));

    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start = false;
    std::atomic<int> send_errors (0);
    std::atomic<int> close_errors (0);
    std::atomic<bool> senders_done (false);

    int received_records = 0;
    int received_multipart = 0;
    int received_single = 0;
    int bad_records = 0;
    int recv_errors = 0;
    std::vector<unsigned char> multipart_seen (multipart_rounds, 0);
    std::vector<unsigned char> single_seen (single_rounds, 0);

    std::thread receiver_thread ([&] {
        const int expected_records = multipart_rounds + single_rounds;
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (10);
        while (received_records < expected_records
               && std::chrono::steady_clock::now () < deadline) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            const zlink_recv_result_t rc = zlink_recv (
              receiver, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_NO_DATA) {
                if (senders_done.load (std::memory_order_acquire))
                    std::this_thread::yield ();
                else
                    msleep (1);
                continue;
            }
            if (rc != ZLINK_RECV_OK) {
                ++recv_errors;
                break;
            }

            ++received_records;
            bool valid = part_count == 1
                           || part_count == multipart_part_count;
            unsigned char kind = 0;
            int round = -1;
            for (size_t i = 0; valid && i < part_count; ++i) {
                valid = zlink_msg_size (&parts[i]) == tagged_size;
                if (!valid)
                    break;
                const unsigned char *data =
                  static_cast<const unsigned char *> (zlink_msg_data (&parts[i]));
                int part_round = -1;
                memcpy (&part_round, data + 1, sizeof (part_round));
                if (i == 0) {
                    kind = data[0];
                    round = part_round;
                } else {
                    valid = data[0] == kind && part_round == round;
                }
            }

            if (valid && part_count == 1 && kind == 'S'
                && round >= 0 && round < single_rounds
                && single_seen[round] == 0) {
                single_seen[round] = 1;
                ++received_single;
            } else if (valid && part_count == multipart_part_count
                       && kind == 'M' && round >= 0
                       && round < multipart_rounds
                       && multipart_seen[round] == 0) {
                multipart_seen[round] = 1;
                ++received_multipart;
            } else {
                ++bad_records;
            }
            zlink_multipart_close (parts, part_count);
        }
    });

    std::thread multipart_sender ([&] {
        {
            std::unique_lock<std::mutex> lock (start_mutex);
            start_cv.wait (lock, [&] { return start; });
        }
        for (int round = 0; round < multipart_rounds; ++round) {
            zlink_msg_t parts[multipart_part_count];
            size_t initialized = 0;
            for (; initialized < multipart_part_count; ++initialized) {
                if (zlink_msg_init_size (&parts[initialized], tagged_size)
                    != ZLINK_CONFIG_OK)
                    break;
                unsigned char *data = static_cast<unsigned char *> (
                  zlink_msg_data (&parts[initialized]));
                data[0] = 'M';
                memcpy (data + 1, &round, sizeof (round));
            }
            if (initialized != multipart_part_count) {
                send_errors.fetch_add (1, std::memory_order_relaxed);
            } else if (zlink_socket_send_internal (
                         sender, parts, multipart_part_count,
                         static_cast<zlink_send_flags_t> (0))
                       != 0) {
                send_errors.fetch_add (1, std::memory_order_relaxed);
            }
            for (size_t i = 0; i < initialized; ++i) {
                if (zlink_msg_close (&parts[i]) != ZLINK_CONFIG_OK)
                    close_errors.fetch_add (1, std::memory_order_relaxed);
            }
        }
    });

    std::thread single_sender ([&] {
        {
            std::unique_lock<std::mutex> lock (start_mutex);
            start_cv.wait (lock, [&] { return start; });
        }
        for (int round = 0; round < single_rounds; ++round) {
            zlink_msg_t part;
            if (zlink_msg_init_size (&part, tagged_size) != ZLINK_CONFIG_OK) {
                send_errors.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            unsigned char *data =
              static_cast<unsigned char *> (zlink_msg_data (&part));
            data[0] = 'S';
            memcpy (data + 1, &round, sizeof (round));
            if (zlink_send_part (sender, &part,
                                 static_cast<zlink_send_flags_t> (0),
                                 ZLINK_PART_FINAL, NULL, NULL)
                != ZLINK_SUBMIT_OK)
                send_errors.fetch_add (1, std::memory_order_relaxed);
            if (zlink_msg_close (&part) != ZLINK_CONFIG_OK)
                close_errors.fetch_add (1, std::memory_order_relaxed);
        }
    });

    {
        std::lock_guard<std::mutex> lock (start_mutex);
        start = true;
    }
    start_cv.notify_all ();
    multipart_sender.join ();
    single_sender.join ();
    senders_done.store (true, std::memory_order_release);
    receiver_thread.join ();

    std::printf (
      "pair_complete_record_interleave multipart=%d single=%d records=%d "
      "bad=%d send_errors=%d recv_errors=%d close_errors=%d\n",
      received_multipart, received_single, received_records, bad_records,
      send_errors.load (std::memory_order_relaxed), recv_errors,
      close_errors.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, close_errors.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, recv_errors);
    TEST_ASSERT_EQUAL_INT (multipart_rounds + single_rounds,
                           received_records);
    TEST_ASSERT_EQUAL_INT (0, bad_records);
    TEST_ASSERT_EQUAL_INT (multipart_rounds, received_multipart);
    TEST_ASSERT_EQUAL_INT (single_rounds, received_single);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_open_send_part_sequence_rejects_concurrent_single_records ()
{
    const int rounds = 1000;
    const int contender_count = 3;

    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://helper-interleave-concurrent-single"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://helper-interleave-concurrent-single"));

    std::mutex mutex;
    std::condition_variable cv;
    int generation = 0;
    int completed = 0;
    bool stop = false;
    std::atomic<int> accepted (0);
    std::atomic<int> rejected (0);
    std::atomic<int> wrong_rejection (0);
    std::atomic<int> ownership_errors (0);
    std::atomic<int> bad_records (0);
    std::atomic<int> thread_errors (0);

    std::vector<std::thread> contenders;
    contenders.reserve (contender_count);
    for (int contender = 0; contender < contender_count; ++contender) {
        contenders.emplace_back ([&, contender] {
            int observed_generation = 0;
            while (true) {
                int round = 0;
                {
                    std::unique_lock<std::mutex> lock (mutex);
                    cv.wait (lock, [&] { return stop || generation > observed_generation; });
                    if (stop)
                        return;
                    observed_generation = generation;
                    round = generation - 1;
                }

                zlink_msg_t single;
                const size_t payload_size = 1 + sizeof (round);
                if (zlink_msg_init_size (&single, payload_size) != ZLINK_CONFIG_OK) {
                    thread_errors.fetch_add (1, std::memory_order_relaxed);
                } else {
                    unsigned char *data =
                      static_cast<unsigned char *> (zlink_msg_data (&single));
                    data[0] = static_cast<unsigned char> ('0' + contender);
                    memcpy (data + 1, &round, sizeof (round));
                    const int rc = zlink_socket_send_internal (
                      sender, &single, 1, static_cast<zlink_send_flags_t> (0));
                    const int err = zlink_errno ();
                    if (rc == 0) {
                        accepted.fetch_add (1, std::memory_order_relaxed);
                    } else {
                        rejected.fetch_add (1, std::memory_order_relaxed);
                        if (err != EINVAL)
                            wrong_rejection.fetch_add (1, std::memory_order_relaxed);
                        if (zlink_msg_size (&single) != payload_size)
                            ownership_errors.fetch_add (1, std::memory_order_relaxed);
                    }
                    if (zlink_msg_close (&single) != ZLINK_CONFIG_OK)
                        thread_errors.fetch_add (1, std::memory_order_relaxed);
                }

                {
                    std::lock_guard<std::mutex> lock (mutex);
                    ++completed;
                }
                cv.notify_all ();
            }
        });
    }

    std::thread receiver_thread ([&] {
        for (int round = 0; round < rounds; ++round) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            if (zlink_recv (receiver, NULL, &parts, &part_count,
                            static_cast<zlink_recv_flags_t> (0))
                != ZLINK_RECV_OK) {
                bad_records.fetch_add (1, std::memory_order_relaxed);
                continue;
            }

            bool valid = part_count == 2;
            for (size_t i = 0; valid && i < part_count; ++i) {
                valid = zlink_msg_size (&parts[i]) == 1 + sizeof (round);
                if (!valid)
                    break;
                const unsigned char *data =
                  static_cast<const unsigned char *> (zlink_msg_data (&parts[i]));
                int received_round = -1;
                memcpy (&received_round, data + 1, sizeof (received_round));
                valid = received_round == round
                        && data[0] == static_cast<unsigned char> (i == 0 ? 'M' : 'F');
            }
            if (!valid)
                bad_records.fetch_add (1, std::memory_order_relaxed);
            zlink_multipart_close (parts, part_count);
        }
    });

    for (int round = 0; round < rounds; ++round) {
        zlink_msg_t first;
        init_tagged_part (&first, 'M', round);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE, NULL, NULL));

        {
            std::lock_guard<std::mutex> lock (mutex);
            completed = 0;
            ++generation;
        }
        cv.notify_all ();
        {
            std::unique_lock<std::mutex> lock (mutex);
            cv.wait (lock, [&] { return completed == contender_count; });
        }

        zlink_msg_t final_part;
        init_tagged_part (&final_part, 'F', round);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &final_part, static_cast<zlink_send_flags_t> (0),
                           ZLINK_PART_FINAL, NULL, NULL));
    }

    receiver_thread.join ();
    {
        std::lock_guard<std::mutex> lock (mutex);
        stop = true;
    }
    cv.notify_all ();
    for (std::vector<std::thread>::iterator it = contenders.begin (); it != contenders.end (); ++it)
        it->join ();

    std::printf ("mixed_record_regression rounds=%d contenders=%d accepted=%d rejected=%d "
                 "wrong_rejection=%d ownership_errors=%d bad_records=%d thread_errors=%d\n",
                 rounds, contender_count, accepted.load (std::memory_order_relaxed),
                 rejected.load (std::memory_order_relaxed),
                 wrong_rejection.load (std::memory_order_relaxed),
                 ownership_errors.load (std::memory_order_relaxed),
                 bad_records.load (std::memory_order_relaxed),
                 thread_errors.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, accepted.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (rounds * contender_count,
                           rejected.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, wrong_rejection.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, ownership_errors.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, bad_records.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, thread_errors.load (std::memory_order_relaxed));

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
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

    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "world", 0);

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
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "prime-1", 0);
    send_string_expect_success (dealer2, "prime-2", 0);
    recv_string_expect_success (router, "D2", 0);
    recv_string_expect_success (router, "prime-2", 0);

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
    RUN_TEST (test_complete_record_admission_rejects_new_multipart_sequence);
    RUN_TEST (test_pair_close_aborts_suspended_multipart_without_exposing_prefix);
    RUN_TEST (
      test_pair_peer_termination_races_local_multipart_cleanup);
    RUN_TEST (test_publish_validation_failure_releases_sync_for_query_before_final);
    RUN_TEST (
      test_pair_one_call_multipart_backpressure_aborts_before_concurrent_final);
    RUN_TEST (test_pair_whole_multipart_does_not_interleave_concurrent_final_records);
    RUN_TEST (test_open_send_part_sequence_rejects_concurrent_single_records);
    RUN_TEST (test_wrong_send_helper_aborts_open_sequence);
    RUN_TEST (test_target_change_aborts_open_routed_sequence);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
