/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

namespace
{
void test_timer_one_shot_recv ()
{
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 20 * 1000 * 1000ULL, 1));

    uint64_t fire_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (1, fire_count);

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer));
}

void test_timer_repeat_recv_sequence ()
{
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 10 * 1000 * 1000ULL, 3));

    uint64_t fire_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (1, fire_count);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (2, fire_count);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (3, fire_count);

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer));
}

void test_timer_poller_and_recv ()
{
    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);

    int user_tag = 7;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add_timer (poller, timer, &user_tag));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 20 * 1000 * 1000ULL, 2));

    zlink_poller_event_t ev;
    bool got_event = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const int rc = zlink_poller_wait (poller, &ev, 1, 500, NULL);
        if (rc == 1) {
            got_event = true;
            break;
        }
        if (rc == 0)
            continue;

        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    }
    TEST_ASSERT_TRUE (got_event);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_TIMER, ev.source_kind);
    TEST_ASSERT_NULL (ev.socket);
    TEST_ASSERT_EQUAL_PTR (timer, ev.timer);
    TEST_ASSERT_EQUAL_PTR (&user_tag, ev.user_data);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, ev.events);

    uint64_t fire_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (1, fire_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_stop (timer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
}

void test_timer_poller_single_owner_and_destroy_busy ()
{
    void *timer = zlink_timer_new ();
    void *poller_a = zlink_poller_new ();
    void *poller_b = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (timer);
    TEST_ASSERT_NOT_NULL (poller_a);
    TEST_ASSERT_NOT_NULL (poller_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add_timer (poller_a, timer, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink_poller_add_timer (poller_b, timer, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, zlink_timer_destroy (&timer));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_remove_timer (poller_a, timer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller_b));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller_a));
}

void test_timer_stop_and_restart_reset_sequence ()
{
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 1000 * 1000 * 1000ULL, 0));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_stop (timer));

    uint64_t fire_count = 99;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 10 * 1000 * 1000ULL, 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_recv (timer, &fire_count));
    TEST_ASSERT_EQUAL_UINT64 (1, fire_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer));
}

}

SETUP_TEARDOWN_TESTCONTEXT

int main (void)
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (test_timer_one_shot_recv);
    RUN_TEST (test_timer_repeat_recv_sequence);
    RUN_TEST (test_timer_poller_and_recv);
    RUN_TEST (test_timer_poller_single_owner_and_destroy_busy);
    RUN_TEST (test_timer_stop_and_restart_reset_sequence);
    return UNITY_END ();
}
