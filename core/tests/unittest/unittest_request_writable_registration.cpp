/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"
#include "api/socket/socket_completion_queue_internal.hpp"
#include "core/pipe.hpp"

#include <atomic>
#include <thread>
#include <unity.h>

namespace
{
const size_t registration_count = 256;
const size_t saturated_payload_bytes = 65536;
const int request_timeout_ms = 5000;
enum release_order_t
{
    release_before_reserve,
    release_during_reserve,
    release_after_reserve
};
std::atomic<size_t> epoch_reads (0);
std::atomic<uint64_t> simulated_epoch (0);
std::atomic<bool> recheck_entered (false);
std::atomic<bool> release_during_recheck (false);
int simulated_pipe_storage;
const zlink::pipe_t *const simulated_pipe =
  reinterpret_cast<const zlink::pipe_t *> (&simulated_pipe_storage);

zlink::socket_completion::request_writable_wait_t make_simulated_wait (uint64_t epoch_)
{
    zlink::socket_completion::request_writable_wait_t wait;
    wait.push_back (
      std::make_pair (std::shared_ptr<zlink::pipe_t> (const_cast<zlink::pipe_t *> (simulated_pipe),
                                                      [] (zlink::pipe_t *) {}),
                      epoch_));
    return wait;
}
}

// GNU ld redirects calls from the non-LTO test archive at the existing
// observation point. Real pipes retain the production acquire-load predicate.
extern "C" uint64_t
__real__ZNK5zlink6pipe_t33request_correlation_release_epochEv (const zlink::pipe_t *pipe_);
extern "C" uint64_t
__wrap__ZNK5zlink6pipe_t33request_correlation_release_epochEv (const zlink::pipe_t *pipe_)
{
    epoch_reads.fetch_add (1, std::memory_order_relaxed);
    if (pipe_ != simulated_pipe)
        return __real__ZNK5zlink6pipe_t33request_correlation_release_epochEv (pipe_);
    if (release_during_recheck.load (std::memory_order_acquire)) {
        recheck_entered.store (true, std::memory_order_release);
        while (simulated_epoch.load (std::memory_order_acquire) == 0)
            std::this_thread::yield ();
    }
    return simulated_epoch.load (std::memory_order_acquire);
}

void setUp ()
{
    setup_test_context ();
    simulated_epoch.store (0);
    recheck_entered.store (false);
    release_during_recheck.store (false);
}

void tearDown ()
{
    teardown_test_context ();
}

namespace
{
zlink_submit_result_t request (void *dealer_, zlink_completion_id_t *id_)
{
    zlink_msg_t parts[2];
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&parts[0], saturated_payload_bytes));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&parts[1]));
    const zlink_submit_result_t result = zlink_request (
      dealer_, NULL, parts, 2, ZLINK_SEND_FLAGS_DONTWAIT, request_timeout_ms, NULL, id_);
    zlink_multipart_close (parts, 2);
    return result;
}

zlink_reply_token_t receive_request (void *router_, zlink_routing_id_t *rid_)
{
    zlink_msg_t parts[2];
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv (router_, &source, &token, parts, 2,
                                                             &count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_NOT_EQUAL (0, token);
    *rid_ = *source;
    zlink_multipart_close (parts, count);
    return token;
}

void reply (void *router_, const zlink_routing_id_t *source_, zlink_reply_token_t token_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_reply (router_, source_, token_, &part, 1));
    zlink_msg_close (&part);
}

void connect_pair (void **dealer_, void **router_)
{
    *router_ = test_context_socket (ZLINK_SOCKET_ROUTER);
    *dealer_ = test_context_socket (ZLINK_SOCKET_DEALER);
    const int receive_timeout_ms = request_timeout_ms;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (*router_, ZLINK_OPT_RCVTIMEO, &receive_timeout_ms,
                                             sizeof (receive_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (*router_, "inproc://writable-registration-cost"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (*dealer_, "inproc://writable-registration-cost"));
}

void check_single_writable (zlink::socket_completion::queue_state_t *state_,
                            zlink_completion_id_t id_,
                            void *context_)
{
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_completion::recv (state_, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    zlink_completion_close (&completion);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink::socket_completion::outstanding (state_));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_writable_wait (state_));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready_writable (state_));
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_completion::recv (state_, &completion, ZLINK_RECV_FLAGS_DONTWAIT, 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}

void run_release_order (release_order_t order_)
{
    zlink::socket_completion::queue_state_t state;
    zlink::socket_completion::request_writable_wait_t wait = make_simulated_wait (0);
    zlink::socket_completion::reservation_t *reservation = NULL;
    zlink_completion_id_t id = 0;
    int context = 1;
    if (order_ == release_before_reserve)
        simulated_epoch.store (1, std::memory_order_release);

    release_during_recheck.store (order_ == release_during_reserve, std::memory_order_release);
    // The publisher must wait for the queue owner while registration checks
    // the newly linked record. Only the independent release value advances.
    std::thread publisher;
    if (order_ == release_during_reserve) {
        publisher = std::thread ([&state] {
            while (!recheck_entered.load (std::memory_order_acquire))
                std::this_thread::yield ();
            simulated_epoch.store (1, std::memory_order_release);
            zlink::socket_completion::publish_writable_waiters (&state, NULL, ZLINK_SEND_ADMITTED,
                                                                0, true);
        });
    }
    bool published = false;
    const int rc = zlink::socket_completion::reserve_writable_wait (
      &state, &context, NULL, &reservation, &id, &wait, &published);
    const bool checked_during_reserve = recheck_entered.load (std::memory_order_acquire);
    // A pre-fix reserve does not observe the epoch. Let the worker exit so
    // the assertion reports a failure instead of hanging the test process.
    recheck_entered.store (true, std::memory_order_release);
    if (publisher.joinable ())
        publisher.join ();
    TEST_ASSERT_SUCCESS_ERRNO (rc);
    TEST_ASSERT_EQUAL_INT (order_ != release_after_reserve, published);
    if (order_ == release_during_reserve)
        TEST_ASSERT_TRUE (checked_during_reserve);
    TEST_ASSERT_NOT_EQUAL (0, id);
    TEST_ASSERT_TRUE (wait.empty ());
    if (order_ == release_after_reserve) {
        TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
        simulated_epoch.store (1, std::memory_order_release);
        TEST_ASSERT_EQUAL_INT (1, zlink::socket_completion::publish_writable_waiters (
                                    &state, NULL, ZLINK_SEND_ADMITTED, 0, true));
    }
    check_single_writable (&state, id, &context);
    TEST_ASSERT_EQUAL_INT (0, zlink::socket_completion::publish_writable_waiters (
                                &state, NULL, ZLINK_SEND_ADMITTED, 0, true));
}
}

void test_public_request_registration_observes_only_the_new_waiter ()
{
    void *dealer = NULL;
    void *router = NULL;
    connect_pair (&dealer, &router);
    zlink_completion_id_t admitted = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request (dealer, &admitted));
    zlink_routing_id_t source = {};
    const zlink_reply_token_t reply_token = receive_request (router, &source);
    std::vector<zlink_completion_id_t> ids (1, admitted);
    epoch_reads.store (0, std::memory_order_relaxed);
    for (size_t i = 0; i != registration_count; ++i) {
        zlink_completion_id_t token = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, request (dealer, &token));
        TEST_ASSERT_NOT_EQUAL (0, token);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        ids.push_back (token);
    }
    const size_t reads = epoch_reads.load (std::memory_order_relaxed);
    printf ("registration_count=%zu epoch_reads=%zu linear_bound=%zu\n", registration_count, reads,
            2 * registration_count);
    // At most one refusal snapshot plus one recheck per token; no release occurs.
    TEST_ASSERT_GREATER_THAN_UINT64 (0, reads);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64 (2 * registration_count, reads);
    reply (router, &source, reply_token);
    std::vector<bool> seen (ids.size (), false);
    for (size_t i = 0; i != ids.size (); ++i) {
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                               zlink_completion_recv (dealer, &completion, ZLINK_RECV_FLAGS_NONE));
        const std::vector<zlink_completion_id_t>::const_iterator found =
          std::find (ids.begin (), ids.end (), completion.completion_id);
        TEST_ASSERT_TRUE (found != ids.end ());
        const size_t index = found - ids.begin ();
        TEST_ASSERT_FALSE (seen[index]);
        seen[index] = true;
        TEST_ASSERT_EQUAL_INT (index == 0 ? ZLINK_COMPLETION_REQUEST : ZLINK_COMPLETION_WRITABLE,
                               completion.kind);
        if (index == 0)
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        else
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        zlink_completion_close (&completion);
    }
    zlink_completion_t empty = {};
    empty.struct_size = sizeof (empty);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_completion_recv (dealer, &empty, ZLINK_RECV_FLAGS_DONTWAIT));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_public_request_control_has_no_waiter_rechecks ()
{
    void *dealer = NULL;
    void *router = NULL;
    connect_pair (&dealer, &router);
    epoch_reads.store (0, std::memory_order_relaxed);
    for (size_t i = 0; i != registration_count; ++i) {
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request (dealer, &id));
        zlink_routing_id_t source = {};
        const zlink_reply_token_t token = receive_request (router, &source);
        reply (router, &source, token);
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                               zlink_completion_recv (dealer, &completion, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        zlink_completion_close (&completion);
    }
    TEST_ASSERT_EQUAL_UINT64 (0, epoch_reads.load ());
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_release_before_reserve ()
{
    run_release_order (release_before_reserve);
}
void test_release_during_reserve ()
{
    run_release_order (release_during_reserve);
}
void test_release_after_reserve ()
{
    run_release_order (release_after_reserve);
}

void test_close_and_recycle_correlation_waiter ()
{
    zlink::socket_completion::queue_state_t state;
    zlink::socket_completion::request_writable_wait_t wait = make_simulated_wait (0);
    zlink::socket_completion::reservation_t *first = NULL;
    zlink_completion_id_t first_id = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve_writable_wait (
      &state, NULL, NULL, &first, &first_id, &wait));
    zlink::socket_completion::release (&state, first);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink::socket_completion::outstanding (&state));
    simulated_epoch.store (1, std::memory_order_release);
    zlink::socket_completion::reservation_t *second = NULL;
    zlink_completion_id_t second_id = 0;
    wait = make_simulated_wait (1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::socket_completion::reserve_writable_wait (
      &state, NULL, NULL, &second, &second_id, &wait));
    TEST_ASSERT_EQUAL_PTR (first, second);
    TEST_ASSERT_NOT_EQUAL (first_id, second_id);
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    zlink::socket_completion::close (&state, ESHUTDOWN);
    TEST_ASSERT_FALSE (zlink::socket_completion::has_writable_wait (&state));
    TEST_ASSERT_FALSE (zlink::socket_completion::has_ready (&state));
    TEST_ASSERT_TRUE (second->request_wait.empty ());
    TEST_ASSERT_EQUAL_INT (-1, zlink::socket_completion::reserve_writable_wait (
                                 &state, NULL, NULL, &second, &second_id, &wait));
    TEST_ASSERT_EQUAL_INT (ESHUTDOWN, errno);
    TEST_ASSERT_NULL (second);
    TEST_ASSERT_EQUAL_UINT64 (0, second_id);
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_public_request_registration_observes_only_the_new_waiter);
    RUN_TEST (test_public_request_control_has_no_waiter_rechecks);
    RUN_TEST (test_release_before_reserve);
    RUN_TEST (test_release_during_reserve);
    RUN_TEST (test_release_after_reserve);
    RUN_TEST (test_close_and_recycle_correlation_waiter);
    return UNITY_END ();
}
