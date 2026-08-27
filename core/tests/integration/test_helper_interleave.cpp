/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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

void ignore_reply (zlink_request_result_t, zlink_msg_t *, size_t, void *)
{
}

void init_tagged_part (zlink_msg_t *part_, unsigned char kind_, int round_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, 1 + sizeof (round_)));
    unsigned char *data = static_cast<unsigned char *> (zlink_msg_data (part_));
    data[0] = kind_;
    memcpy (data + 1, &round_, sizeof (round_));
}
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
          zlink_send_part (sender, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

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
                           ZLINK_PART_FINAL));
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
      zlink_send_part (dealer, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_msg_t wrong_family;
    init_part (&wrong_family, "request");
    const zlink_submit_result_t wrong_rc =
      zlink_dealer_request_part (dealer, &wrong_family, static_cast<zlink_send_flags_t> (0),
                                 ZLINK_PART_FINAL, 1000, &ignore_reply, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_family));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_family));

    zlink_msg_t last;
    init_part (&last, "world");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &last, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

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
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_msg_t wrong_target;
    init_part (&wrong_target, "frame-b");
    const zlink_submit_result_t wrong_rc = zlink_send_part_rid (
      router, &rid2, &wrong_target, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_target));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_target));

    zlink_msg_t last;
    init_part (&last, "frame-c");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (router, &rid1, &last,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    recv_string_expect_success (dealer1, "frame-c", 0);

    test_context_socket_close_zero_linger (dealer2);
    test_context_socket_close_zero_linger (dealer1);
    test_context_socket_close_zero_linger (router);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_open_send_part_sequence_rejects_concurrent_single_records);
    RUN_TEST (test_wrong_send_helper_aborts_open_sequence);
    RUN_TEST (test_target_change_aborts_open_routed_sequence);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
