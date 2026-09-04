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

void test_request_completion_reservations_have_a_finite_admission_limit ()
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
          &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &reservation,
          &completion_id));
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
            &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &rejected,
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
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &reused, &reused_id));
    TEST_ASSERT_NOT_NULL (reused);
    TEST_ASSERT_EQUAL_UINT64 (
      zlink::socket_completion::max_outstanding_completions + 1, reused_id);
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
          &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &reservation,
          &completion_id));
        if (i == 0)
            published_id = completion_id;
        reservations.push_back (reservation);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_request (
      &state, reservations[0], ZLINK_REQUEST_TIMED_OUT, NULL, 0));
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready (&state));

    zlink::socket_completion::reservation_t *rejected = NULL;
    zlink_completion_id_t rejected_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::reserve (
            &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &rejected,
            &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
      &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (published_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           completion.request_result);
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

void test_dequeued_request_reservation_is_recycled_with_fresh_id ()
{
    zlink::socket_completion::queue_state_t state;
    zlink_routing_id_t peer;
    memset (&peer, 0, sizeof (peer));
    peer.size = 3;
    memcpy (peer.data, "RID", 3);

    zlink::socket_completion::reservation_t *first = NULL;
    zlink_completion_id_t first_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, reinterpret_cast<void *> (1), &peer,
      &first, &first_id));
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_request (
      &state, first, ZLINK_REQUEST_TIMED_OUT, NULL, 0));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
      &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (first_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           completion.request_result);
    zlink_completion_close (&completion);

    zlink::socket_completion::reservation_t *second = NULL;
    zlink_completion_id_t second_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &second, &second_id));
    TEST_ASSERT_EQUAL_PTR (first, second);
    TEST_ASSERT_NOT_EQUAL (first_id, second_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                           second->completion.kind);
    TEST_ASSERT_NULL (second->completion.user_context);
    TEST_ASSERT_EQUAL_UINT8 (0, second->completion.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (0, second->completion.request_result);
    TEST_ASSERT_NULL (second->completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, second->completion.reply_part_count);
    zlink::socket_completion::release (&state, second);
}

void test_request_completions_preserve_publish_queue_order ()
{
    zlink::socket_completion::queue_state_t state;
    int contexts[3] = {10, 20, 30};
    zlink::socket_completion::reservation_t *reservations[3] = {NULL, NULL,
                                                                 NULL};
    zlink_completion_id_t completion_ids[3] = {0, 0, 0};
    for (size_t i = 0; i != 3; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
          &state, ZLINK_COMPLETION_REQUEST, &contexts[i], NULL,
          &reservations[i], &completion_ids[i]));
    }

    const size_t publish_order[3] = {2, 0, 1};
    const zlink_request_result_t results[3] = {
      ZLINK_REQUEST_TIMED_OUT, ZLINK_REQUEST_NOT_FOUND,
      ZLINK_REQUEST_INTERNAL_ERROR};
    for (size_t i = 0; i != 3; ++i) {
        const size_t reservation_index = publish_order[i];
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_request (
          &state, reservations[reservation_index], results[i], NULL, 0));
    }
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready (&state));

    for (size_t i = 0; i != 3; ++i) {
        const size_t reservation_index = publish_order[i];
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
          &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_ids[reservation_index],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[reservation_index],
                               completion.user_context);
        TEST_ASSERT_EQUAL_INT (results[i], completion.request_result);
        TEST_ASSERT_NULL (completion.reply_parts);
        TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
        zlink_completion_close (&completion);
    }

    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));
    zlink_completion_t empty;
    memset (&empty, 0, sizeof (empty));
    empty.struct_size = sizeof (empty);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::recv (
            &state, &empty, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}

void test_writable_waiters_publish_selectively_in_issuance_order ()
{
    zlink::socket_completion::queue_state_t state;
    zlink_routing_id_t rid_a;
    zlink_routing_id_t rid_b;
    memset (&rid_a, 0, sizeof (rid_a));
    memset (&rid_b, 0, sizeof (rid_b));
    rid_a.size = 1;
    rid_a.data[0] = 'A';
    rid_b.size = 1;
    rid_b.data[0] = 'B';

    int request_context = 7;
    zlink::socket_completion::reservation_t *request = NULL;
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, &request_context, NULL, &request,
      &request_id));

    int contexts[4] = {10, 20, 30, 40};
    const zlink_routing_id_t *targets[4] = {NULL, &rid_a, &rid_b, &rid_a};
    zlink::socket_completion::reservation_t *waiters[4] = {NULL, NULL,
                                                             NULL, NULL};
    zlink_completion_id_t waiter_ids[4] = {0, 0, 0, 0};
    for (size_t i = 0; i != 4; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink::socket_completion::reserve_writable_wait (
            &state, &contexts[i], targets[i], &waiters[i], &waiter_ids[i]));
        TEST_ASSERT_TRUE (waiters[i]->writable_wait_linked);
        TEST_ASSERT_EQUAL_UINT64 (i + 2, waiter_ids[i]);
    }
    TEST_ASSERT_EQUAL_PTR (waiters[0], state.writable_wait_head);
    TEST_ASSERT_EQUAL_PTR (waiters[3], state.writable_wait_tail);
    TEST_ASSERT_EQUAL_UINT64 (4, state.writable_waiting_count.load ());
    TEST_ASSERT_TRUE (zlink::socket_completion::has_writable_wait (&state));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (&state));

    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_request (
      &state, request, ZLINK_REQUEST_TIMED_OUT, NULL, 0));
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (&state));

    TEST_ASSERT_EQUAL_INT (
      2, zlink::socket_completion::publish_writable_waiters (
           &state, &rid_a, ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, state.writable_waiting_count.load ());
    TEST_ASSERT_EQUAL_UINT64 (2, state.ready_writable_count.load ());
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready_writable (&state));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
      &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, state.ready_writable_count.load ());
    zlink_completion_close (&completion);

    const size_t rid_a_order[2] = {1, 3};
    for (size_t i = 0; i != 2; ++i) {
        const size_t waiter_index = rid_a_order[i];
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
          &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (waiter_ids[waiter_index],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[waiter_index],
                               completion.user_context);
        TEST_ASSERT_EQUAL_UINT8 (rid_a.size, completion.peer_rid.size);
        TEST_ASSERT_EQUAL_MEMORY (rid_a.data, completion.peer_rid.data,
                                  rid_a.size);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
        TEST_ASSERT_EQUAL_UINT64 (1 - i,
                                  state.ready_writable_count.load ());
        zlink_completion_close (&completion);
    }
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (&state));

    TEST_ASSERT_EQUAL_INT (
      1, zlink::socket_completion::publish_writable_waiters (
           &state, NULL, ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_EQUAL_INT (
      1, zlink::socket_completion::publish_writable_waiters (
           &state, &rid_b, ZLINK_SEND_TERMINAL, EHOSTUNREACH));
    TEST_ASSERT_EQUAL_UINT64 (0, state.writable_waiting_count.load ());
    TEST_ASSERT_FALSE (zlink::socket_completion::has_writable_wait (&state));
    TEST_ASSERT_EQUAL_UINT64 (2, state.ready_writable_count.load ());

    const size_t remaining_order[2] = {0, 2};
    for (size_t i = 0; i != 2; ++i) {
        const size_t waiter_index = remaining_order[i];
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::recv (
          &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (waiter_ids[waiter_index],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[waiter_index],
                               completion.user_context);
        if (waiter_index == 0) {
            TEST_ASSERT_EQUAL_UINT8 (0, completion.peer_rid.size);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                                   completion.send_result);
            TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
        } else {
            TEST_ASSERT_EQUAL_UINT8 (rid_b.size, completion.peer_rid.size);
            TEST_ASSERT_EQUAL_MEMORY (rid_b.data, completion.peer_rid.data,
                                      rid_b.size);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL,
                                   completion.send_result);
            TEST_ASSERT_EQUAL_INT (EHOSTUNREACH,
                                   completion.send_terminal_errno);
        }
        zlink_completion_close (&completion);
    }

    TEST_ASSERT_EQUAL_UINT64 (0, state.ready_writable_count.load ());
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (&state));
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));
}

void test_close_terminalizes_and_drops_all_writable_waiters ()
{
    zlink::socket_completion::queue_state_t state;
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 3;
    memcpy (rid.data, "RID", 3);

    int contexts[2] = {1, 2};
    zlink::socket_completion::reservation_t *waiters[2] = {NULL, NULL};
    zlink_completion_id_t waiter_ids[2] = {0, 0};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_completion::reserve_writable_wait (
        &state, &contexts[0], NULL, &waiters[0], &waiter_ids[0]));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_completion::reserve_writable_wait (
        &state, &contexts[1], &rid, &waiters[1], &waiter_ids[1]));
    TEST_ASSERT_EQUAL_INT (
      1, zlink::socket_completion::publish_writable_waiters (
           &state, NULL, ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, state.writable_waiting_count.load ());
    TEST_ASSERT_EQUAL_UINT64 (1, state.ready_writable_count.load ());

    zlink::socket_completion::close (&state, ETERM);

    TEST_ASSERT_NULL (state.writable_wait_head);
    TEST_ASSERT_NULL (state.writable_wait_tail);
    TEST_ASSERT_EQUAL_UINT64 (0, state.writable_waiting_count.load ());
    TEST_ASSERT_FALSE (zlink::socket_completion::has_writable_wait (&state));
    TEST_ASSERT_EQUAL_UINT64 (0, state.ready_writable_count.load ());
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (&state));
    TEST_ASSERT_EQUAL_UINT64 (2,
                              zlink::socket_completion::outstanding (&state));

    for (size_t i = 0; i != 2; ++i) {
        TEST_ASSERT_FALSE (waiters[i]->writable_wait_linked);
        TEST_ASSERT_NULL (waiters[i]->writable_wait_next);
        TEST_ASSERT_TRUE (waiters[i]->ready);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE,
                               waiters[i]->completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (waiter_ids[i],
                                  waiters[i]->completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[i],
                               waiters[i]->completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL,
                               waiters[i]->completion.send_result);
        TEST_ASSERT_EQUAL_INT (ETERM,
                               waiters[i]->completion.send_terminal_errno);
    }

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::recv (
            &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
}

void test_released_writable_waiter_unlinks_and_recycles_cleanly ()
{
    zlink::socket_completion::queue_state_t state;
    zlink::socket_completion::reservation_t *first = NULL;
    zlink::socket_completion::reservation_t *second = NULL;
    zlink_completion_id_t first_id = 0;
    zlink_completion_id_t second_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_completion::reserve_writable_wait (
        &state, NULL, NULL, &first, &first_id));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_completion::reserve_writable_wait (
        &state, NULL, NULL, &second, &second_id));
    TEST_ASSERT_NOT_EQUAL (first_id, second_id);
    TEST_ASSERT_EQUAL_PTR (first, state.writable_wait_head);
    TEST_ASSERT_EQUAL_PTR (second, state.writable_wait_tail);

    zlink::socket_completion::release (&state, first);
    TEST_ASSERT_EQUAL_PTR (second, state.writable_wait_head);
    TEST_ASSERT_EQUAL_PTR (second, state.writable_wait_tail);
    TEST_ASSERT_EQUAL_UINT64 (1, state.writable_waiting_count.load ());

    zlink::socket_completion::reservation_t *reused = NULL;
    zlink_completion_id_t reused_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &reused, &reused_id));
    TEST_ASSERT_EQUAL_PTR (first, reused);
    TEST_ASSERT_NOT_EQUAL (first_id, reused_id);
    TEST_ASSERT_NULL (reused->writable_wait_next);
    TEST_ASSERT_FALSE (reused->writable_wait_linked);
    TEST_ASSERT_FALSE (reused->ready);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, reused->completion.kind);

    zlink::socket_completion::release (&state, reused);
    zlink::socket_completion::release (&state, second);
    TEST_ASSERT_NULL (state.writable_wait_head);
    TEST_ASSERT_NULL (state.writable_wait_tail);
    TEST_ASSERT_FALSE (zlink::socket_completion::has_writable_wait (&state));
    TEST_ASSERT_EQUAL_UINT64 (0,
                              zlink::socket_completion::outstanding (&state));
}

void test_close_drops_ready_request_delivery_and_rejects_new_work ()
{
    zlink::socket_completion::queue_state_t state;
    zlink::socket_completion::reservation_t *ready = NULL;
    zlink::socket_completion::reservation_t *pending = NULL;
    zlink_completion_id_t ready_id = 0;
    zlink_completion_id_t pending_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &ready, &ready_id));
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve (
      &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &pending, &pending_id));
    TEST_ASSERT_NOT_EQUAL (0, ready_id);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);
    TEST_ASSERT_NOT_EQUAL (ready_id, pending_id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::publish_request (
      &state, ready, ZLINK_REQUEST_TIMED_OUT, NULL, 0));
    TEST_ASSERT_TRUE (zlink::socket_completion::has_ready (&state));

    zlink::socket_completion::close (&state, ETERM);
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_EQUAL_UINT64 (2,
                              zlink::socket_completion::outstanding (&state));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::recv (
            &state, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    TEST_ASSERT_EQUAL_INT (0, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.completion_id);

    zlink::socket_completion::reservation_t *rejected =
      reinterpret_cast<zlink::socket_completion::reservation_t *> (1);
    zlink_completion_id_t rejected_id = 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::reserve (
            &state, ZLINK_COMPLETION_REQUEST, NULL, NULL, &rejected,
            &rejected_id));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    TEST_ASSERT_NULL (rejected);
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::publish_request (
            &state, pending, ZLINK_REQUEST_TERMINATED, NULL, 0));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    zlink::socket_completion::release (&state, pending);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              zlink::socket_completion::outstanding (&state));
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (
      test_request_completion_reservations_have_a_finite_admission_limit);
    RUN_TEST (test_published_reservation_is_held_until_public_dequeue);
    RUN_TEST (test_completion_id_wrap_rejects_zero_without_reserving);
    RUN_TEST (
      test_dequeued_request_reservation_is_recycled_with_fresh_id);
    RUN_TEST (test_request_completions_preserve_publish_queue_order);
    RUN_TEST (test_writable_waiters_publish_selectively_in_issuance_order);
    RUN_TEST (test_close_terminalizes_and_drops_all_writable_waiters);
    RUN_TEST (test_released_writable_waiter_unlinks_and_recycles_cleanly);
    RUN_TEST (test_close_drops_ready_request_delivery_and_rejects_new_work);
    return UNITY_END ();
}
