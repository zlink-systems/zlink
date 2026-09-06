/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_test_helpers.hpp"
#include "contract_socket_pair_fixture.hpp"

SETUP_TEARDOWN_TESTCONTEXT
extern "C" void zlink_test_set_submit_retry_fault (int count_, int err_);

void test_none_pre_return_out_of_memory_and_internal_error_are_distinct ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (sender, receiver);

    const int injected_errnos[] = {ENOMEM, EIO};
    const zlink_submit_result_t expected_results[] = {
      ZLINK_SUBMIT_OUT_OF_MEMORY, ZLINK_SUBMIT_INTERNAL_ERROR};
    for (size_t i = 0; i != 2; ++i) {
        zlink_test_set_submit_retry_fault (1, injected_errnos[i]);
        zlink_msg_t part;
        init_part (&part, i == 0 ? "oom" : "runtime");
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          expected_results[i],
          zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        TEST_ASSERT_EQUAL_INT (injected_errnos[i], zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        assert_part_consumed (&part);

        zlink_completion_t completion;
        init_empty_completion (&completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_completion_recv (sender, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        assert_empty_completion (completion);
    }
    zlink_test_set_submit_retry_fault (0, 0);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_dontwait_non_admission_failures_are_synchronous_zero_id ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    contract_socket_pair_t pair (sender, receiver);

    const int injected_errnos[] = {ENOMEM, EIO};
    const zlink_submit_result_t expected_results[] = {
      ZLINK_SUBMIT_OUT_OF_MEMORY, ZLINK_SUBMIT_INTERNAL_ERROR};
    for (size_t i = 0; i != 2; ++i) {
        zlink_test_set_submit_retry_fault (1, injected_errnos[i]);

        zlink_msg_t part;
        init_part (&part, "synchronous-dontwait-failure");
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          expected_results[i],
          zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        TEST_ASSERT_EQUAL_INT (injected_errnos[i], zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        assert_part_consumed (&part);
        zlink_test_set_submit_retry_fault (0, 0);
        assert_no_completion (sender);
    }

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_none_pre_return_out_of_memory_and_internal_error_are_distinct);
    RUN_TEST (test_dontwait_non_admission_failures_are_synchronous_zero_id);
    return UNITY_END ();
}
