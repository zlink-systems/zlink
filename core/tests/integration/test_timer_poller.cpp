/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{
struct timer_callback_probe_t
{
    timer_callback_probe_t () : fire_count (0), call_count (0), called (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    uint64_t fire_count;
    int call_count;
    bool called;
};

void on_timer (void *, uint64_t fire_count_, void *userdata_)
{
    timer_callback_probe_t *probe = static_cast<timer_callback_probe_t *> (userdata_);
    std::lock_guard<std::mutex> lock (probe->mutex);
    probe->fire_count = fire_count_;
    ++probe->call_count;
    probe->called = true;
    probe->cv.notify_all ();
}

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

void test_timer_callback_conflicts_and_destroy_busy ()
{
    void *timer_callback = zlink_timer_new ();
    void *timer_poller = zlink_timer_new ();
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (timer_callback);
    TEST_ASSERT_NOT_NULL (timer_poller);
    TEST_ASSERT_NOT_NULL (poller);

    timer_callback_probe_t probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_handler (timer_callback, &on_timer, &probe));

    uint64_t fire_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUSY, zlink_timer_recv (timer_callback, &fire_count));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink_poller_add_timer (poller, timer_callback, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer_callback, 20 * 1000 * 1000ULL, 1));
    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        const bool fired = probe.cv.wait_for (lock, std::chrono::milliseconds (500),
                                              [&probe] () { return probe.called; });
        TEST_ASSERT_TRUE (fired);
        TEST_ASSERT_EQUAL_UINT64 (1, probe.fire_count);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add_timer (poller, timer_poller, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY,
                           zlink_timer_handler (timer_poller, &on_timer, &probe));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, zlink_timer_destroy (&timer_poller));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_remove_timer (poller, timer_poller));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer_poller));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_stop (timer_callback));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_destroy (&timer_callback));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
}

void test_timer_callback_repeat_sequence ()
{
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);

    timer_callback_probe_t probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_handler (timer, &on_timer, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_timer_start (timer, 10 * 1000 * 1000ULL, 3));
    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        const bool fired = probe.cv.wait_for (lock, std::chrono::milliseconds (1000),
                                              [&probe] () { return probe.call_count >= 3; });
        TEST_ASSERT_TRUE (fired);
        TEST_ASSERT_EQUAL_INT (3, probe.call_count);
        TEST_ASSERT_EQUAL_UINT64 (3, probe.fire_count);
    }

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
    RUN_TEST (test_timer_callback_conflicts_and_destroy_busy);
    RUN_TEST (test_timer_callback_repeat_sequence);
    return UNITY_END ();
}
