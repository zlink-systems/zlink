/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "api/socket/socket_completion_queue_internal.hpp"

#include <cstring>
#include <limits>
#include <vector>

#include <unity.h>

void setUp ()
{
}

void tearDown ()
{
}

void test_shared_completion_reservations_have_a_finite_admission_limit ()
{
    zlink::socket_completion::queue_state_t state;
    std::vector<zlink::socket_completion::reservation_t *> reservations;
    reservations.reserve (
      zlink::socket_completion::max_outstanding_completions);

    for (size_t i = 0;
         i < zlink::socket_completion::max_outstanding_completions; ++i) {
        zlink::socket_completion::reservation_t *reservation = NULL;
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
          &state,
          i % 2 == 0 ? ZLINK_COMPLETION_SEND : ZLINK_COMPLETION_REQUEST,
          NULL, NULL, &reservation, &completion_id));
        TEST_ASSERT_NOT_NULL (reservation);
        TEST_ASSERT_EQUAL_UINT64 (i + 1, completion_id);
        reservations.push_back (reservation);
    }

    TEST_ASSERT_EQUAL_UINT64 (
      zlink::socket_completion::max_outstanding_completions,
      zlink::socket_completion::outstanding (&state));
    zlink::socket_completion::reservation_t *rejected =
      reinterpret_cast<zlink::socket_completion::reservation_t *> (1);
    zlink_completion_id_t rejected_id = 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::reserve (
            &state, ZLINK_COMPLETION_SEND, NULL, NULL, &rejected,
            &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NULL (rejected);
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);

    for (size_t i = 0; i < reservations.size (); ++i)
        zlink::socket_completion::release (&state, reservations[i]);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));

    zlink::socket_completion::reservation_t *reused = NULL;
    zlink_completion_id_t reused_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_SEND, NULL, NULL, &reused, &reused_id));
    TEST_ASSERT_NOT_NULL (reused);
    TEST_ASSERT_NOT_EQUAL (0, reused_id);
    zlink::socket_completion::release (&state, reused);
}

void test_published_reservation_is_held_until_public_dequeue ()
{
    zlink::socket_completion::queue_state_t state;
    std::vector<zlink::socket_completion::reservation_t *> reservations;
    reservations.reserve (
      zlink::socket_completion::max_outstanding_completions);

    zlink_completion_id_t published_id = 0;
    for (size_t i = 0;
         i < zlink::socket_completion::max_outstanding_completions; ++i) {
        zlink::socket_completion::reservation_t *reservation = NULL;
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
          &state, ZLINK_COMPLETION_SEND, NULL, NULL, &reservation,
          &completion_id));
        if (i == 0)
            published_id = completion_id;
        reservations.push_back (reservation);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_send (
      &state, reservations[0], ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready (&state));

    zlink::socket_completion::reservation_t *rejected = NULL;
    zlink_completion_id_t rejected_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::reserve (
            &state, ZLINK_COMPLETION_SEND, NULL, NULL, &rejected,
            &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
      &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (published_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_UINT64 (
      zlink::socket_completion::max_outstanding_completions - 1,
      zlink::socket_completion::outstanding (&state));
    zlink_completion_close (&completion);

    zlink::socket_completion::reservation_t *after_dequeue = NULL;
    zlink_completion_id_t after_dequeue_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &after_dequeue,
      &after_dequeue_id));
    TEST_ASSERT_NOT_NULL (after_dequeue);
    TEST_ASSERT_NOT_EQUAL (0, after_dequeue_id);
    zlink::socket_completion::release (&state, after_dequeue);
    for (size_t i = 1; i < reservations.size (); ++i)
        zlink::socket_completion::release (&state, reservations[i]);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));
}

void test_completion_id_wrap_rejects_zero_without_reserving ()
{
    zlink::socket_completion::queue_state_t state;
    state.next_id = std::numeric_limits<uint64_t>::max ();

    zlink::socket_completion::reservation_t *last = NULL;
    zlink_completion_id_t last_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &last, &last_id));
    TEST_ASSERT_EQUAL_UINT64 (std::numeric_limits<uint64_t>::max (), last_id);

    zlink::socket_completion::reservation_t *wrapped =
      reinterpret_cast<zlink::socket_completion::reservation_t *> (1);
    zlink_completion_id_t wrapped_id = 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::reserve (
            &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &wrapped,
            &wrapped_id));
    TEST_ASSERT_EQUAL_INT (EOVERFLOW, errno);
    TEST_ASSERT_NULL (wrapped);
    TEST_ASSERT_EQUAL_UINT64 (0, wrapped_id);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              zlink::socket_completion::outstanding (&state));

    zlink::socket_completion::release (&state, last);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (
      test_shared_completion_reservations_have_a_finite_admission_limit);
    RUN_TEST (test_published_reservation_is_held_until_public_dequeue);
    RUN_TEST (test_completion_id_wrap_rejects_zero_without_reserving);
    return UNITY_END ();
}
