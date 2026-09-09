/* SPDX-License-Identifier: MPL-2.0 */

//  Regression: SUB connect/close churn against a live PUB/XPUB under publish
//  load must not corrupt the publisher's dist_t pipe array.
//
//  When a SUB connects and is closed immediately, its pipe can complete
//  termination before the publisher processes the matching attach command.
//  socket_base_t::attach_pipe then returns early (has_completed_termination)
//  and never calls dist_t::attach() for that pipe, leaving its array index at
//  the default -1. The later termination callback still reached
//  dist_t::pipe_terminated, which skipped every counter decrement and then
//  erased at (size_type)-1 -- an 8-byte out-of-bounds write before the pipe
//  array (ASan: heap-buffer-overflow in array_t::erase; intermittently glibc
//  "double free or corruption (out)" under load).
//
//  The fix makes attach/terminate accounting symmetric at the container: a
//  pipe is dist-terminated only if it was dist-attached (dist_t::has_pipe
//  guard, matching the membership rule fq_t::pipe_terminated already applies).
//  Public C API only; the failure mode is a crash / sanitizer report, so the
//  test's success is simply completing every iteration without aborting.

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
//  Enough iterations to cover the attach/terminate race window seen in the
//  public-API repro (crash reproduced around iteration 55 of 200 x 3 there).
const int kIterations = 500;
const int kConcurrentSubs = 3;
const char *const kTopic = "topic";

void configure_linger_zero (void *socket_)
{
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
}

void publish_once (void *pub_)
{
    zlink_msg_t m;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&m, 4));
    memcpy (zlink_msg_data (&m), "beac", 4);
    //  DONTWAIT: the publisher never blocks on the churning subscribers.
    (void) zlink_publish_part (pub_, kTopic, &m, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL);
    zlink_msg_close (&m);
}

void run_churn (int publisher_type_)
{
    void *pub = test_context_socket (publisher_type_);
    configure_linger_zero (pub);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof (endpoint));

    std::atomic<bool> stop (false);
    std::thread pub_thread ([&] {
        while (!stop.load (std::memory_order_relaxed))
            publish_once (pub);
    });

    for (int i = 0; i < kIterations; ++i) {
        std::vector<void *> subs;
        subs.reserve (kConcurrentSubs);
        for (int c = 0; c < kConcurrentSubs; ++c) {
            void *sub = test_context_socket (ZLINK_SOCKET_SUB);
            configure_linger_zero (sub);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
            subs.push_back (sub);
        }
        //  Immediate close: SUB pipe termination races the PUB's attach.
        for (std::vector<void *>::iterator it = subs.begin ();
             it != subs.end (); ++it)
            test_context_socket_close_zero_linger (*it);
    }

    stop.store (true, std::memory_order_relaxed);
    pub_thread.join ();
    test_context_socket_close_zero_linger (pub);
}
} // namespace

void test_sub_churn_against_pub ()
{
    run_churn (ZLINK_SOCKET_PUB);
}

void test_sub_churn_against_xpub ()
{
    run_churn (ZLINK_SOCKET_XPUB);
}

int main ()
{
    setup_test_environment (180);
    UNITY_BEGIN ();
    RUN_TEST (test_sub_churn_against_pub);
    RUN_TEST (test_sub_churn_against_xpub);
    return UNITY_END ();
}
