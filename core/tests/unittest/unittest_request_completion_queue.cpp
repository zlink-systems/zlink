/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "api/socket/request_completion_queue_internal.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

#include <unity.h>

namespace
{
struct blocking_callback_t
{
    bool entered;
    bool release;
    std::mutex mutex;
    std::condition_variable cv;
};

void block_completion_callback (zlink_request_result_t,
                                zlink_msg_t *,
                                size_t,
                                void *userdata_)
{
    blocking_callback_t *state =
      static_cast<blocking_callback_t *> (userdata_);
    std::unique_lock<std::mutex> lock (state->mutex);
    state->entered = true;
    state->cv.notify_all ();
    while (!state->release)
        state->cv.wait (lock);
}
}

void test_completion_reservations_have_a_finite_admission_limit ()
{
    zlink::request_completion::queue_state_t state;
    for (size_t i = 0;
         i < zlink::request_completion::max_pending_completions; ++i)
        TEST_ASSERT_TRUE (zlink::request_completion::try_reserve (&state));

    errno = 0;
    TEST_ASSERT_FALSE (zlink::request_completion::try_reserve (&state));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    for (size_t i = 0;
         i < zlink::request_completion::max_pending_completions; ++i)
        zlink::request_completion::release_reservation (&state);

    TEST_ASSERT_TRUE (zlink::request_completion::try_reserve (&state));
    zlink::request_completion::release_reservation (&state);
}

void test_reservation_is_held_until_callback_returns ()
{
    zlink::request_completion::queue_state_t state;
    for (size_t i = 0;
         i < zlink::request_completion::max_pending_completions; ++i)
        TEST_ASSERT_TRUE (zlink::request_completion::try_reserve (&state));

    blocking_callback_t callback_state = {false, false};
    zlink::request_completion::control_t control;
    control.handler = &block_completion_callback;
    control.userdata = &callback_state;
    {
        std::lock_guard<std::mutex> lock (state.mutex);
        state.controls.push_back (control);
    }

    std::thread owner ([&] {
        TEST_ASSERT_EQUAL_INT (
          1, zlink::request_completion::drain (&state, &state));
    });
    {
        std::unique_lock<std::mutex> lock (callback_state.mutex);
        while (!callback_state.entered)
            callback_state.cv.wait (lock);
    }

    errno = 0;
    TEST_ASSERT_FALSE (zlink::request_completion::try_reserve (&state));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    {
        std::lock_guard<std::mutex> lock (callback_state.mutex);
        callback_state.release = true;
    }
    callback_state.cv.notify_all ();
    owner.join ();

    for (size_t i = 1;
         i < zlink::request_completion::max_pending_completions; ++i)
        zlink::request_completion::release_reservation (&state);
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_completion_reservations_have_a_finite_admission_limit);
    RUN_TEST (test_reservation_is_held_until_callback_returns);
    return UNITY_END ();
}
