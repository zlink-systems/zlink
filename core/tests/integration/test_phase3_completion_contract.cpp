/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/socket_api_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

extern "C" void zlink_test_set_submit_retry_fault (int count_, int err_);

namespace
{
const size_t kPayloadSize = 64;
const size_t kMaxFillAttempts = 512;

bool should_run_phase3_completion_test (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

void assert_part_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_NOT_NULL (part_);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

void init_empty_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

void assert_empty_completion (const zlink_completion_t &completion_)
{
    TEST_ASSERT_EQUAL_UINT32 (sizeof (zlink_completion_t),
                              completion_.struct_size);
    TEST_ASSERT_EQUAL_INT (0, completion_.kind);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_.completion_id);
    TEST_ASSERT_NULL (completion_.user_context);
    TEST_ASSERT_EQUAL_UINT (0, completion_.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (0, completion_.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion_.send_terminal_errno);
    TEST_ASSERT_EQUAL_INT (0, completion_.request_result);
    TEST_ASSERT_NULL (completion_.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_.reply_part_count);
}

void configure_small_pair_hwm (void *socket_)
{
    const uint64_t hwm =
      4u * (static_cast<uint64_t> (kPayloadSize) + sizeof (zlink_msg_t));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
}

void setup_pair (const char *endpoint_, void **sender_out_, void **receiver_out_,
                 bool small_hwm_)
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_NOT_NULL (sender);
    const int zero_linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    if (small_hwm_) {
        configure_small_pair_hwm (receiver);
        configure_small_pair_hwm (sender);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver, endpoint_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender, endpoint_));
    msleep (SETTLE_TIME);
    *sender_out_ = sender;
    *receiver_out_ = receiver;
}

bool recv_one_pair_part (void *receiver_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const zlink_recv_result_t result = zlink_recv_part (
      receiver_, NULL, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (result == ZLINK_RECV_NO_DATA) {
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        return false;
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return true;
}

zlink_routing_id_t make_text_rid (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    TEST_ASSERT_TRUE (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    if (size != 0)
        memcpy (rid.data, value_, size);
    return rid;
}

void receive_router_data_eventually (void *router_, const char *expected_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = UINT64_MAX;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_MORE;
        errno = 0;
        const zlink_recv_result_t result = zlink_router_recv_part (
          router_, &source_rid, &reply_token, &part, &part_flag,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source_rid);
            TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
            const std::string payload (
              static_cast<const char *> (zlink_msg_data (&part)),
              zlink_msg_size (&part));
            TEST_ASSERT_EQUAL_STRING (expected_, payload.c_str ());
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ROUTER DATA");
}

size_t submit_until_pending (void *sender_, void *user_context_,
                             zlink_completion_id_t *completion_id_out_)
{
    for (size_t i = 0; i != kMaxFillAttempts; ++i) {
        zlink_msg_t part;
        init_part (&part, std::string (kPayloadSize, 'p'));
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        const zlink_submit_result_t result = zlink_send_part (
          sender_, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
          user_context_, &completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        assert_part_consumed (&part);
        if (completion_id != 0) {
            *completion_id_out_ = completion_id;
            return i;
        }
    }
    TEST_FAIL_MESSAGE (
      "PAIR DONTWAIT send never transferred a record to the pending owner");
    return 0;
}

zlink_completion_id_t submit_routed_until_pending (
  void *sender_, const zlink_routing_id_t *target_rid_)
{
    for (size_t i = 0; i != kMaxFillAttempts; ++i) {
        zlink_msg_t part;
        init_part (&part, std::string (kPayloadSize, 'r'));
        zlink_completion_id_t completion_id = UINT64_MAX;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part_rid (sender_, target_rid_, &part,
                               ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, NULL, &completion_id));
        assert_part_consumed (&part);
        if (completion_id != 0)
            return completion_id;
    }
    TEST_FAIL_MESSAGE (
      "ROUTER DONTWAIT send never transferred a record to pending ownership");
    return 0;
}

zlink_poller_event_t wait_for_completion_after_drain (void *poller_,
                                                       void *receiver_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        const int count = zlink_poller_wait (poller_, &event, 1, 0, &error);
        TEST_ASSERT_TRUE (count == 0 || count == 1);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        if (count == 1 && (event.events & ZLINK_POLLCOMPLETION) != 0)
            return event;

        (void) recv_one_pair_part (receiver_);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for PAIR SEND completion readiness");
    zlink_poller_event_t empty;
    memset (&empty, 0, sizeof (empty));
    return empty;
}

void leave_one_pair_completion_unread (void *sender_, void *receiver_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, sender_, sender_, ZLINK_POLLCOMPLETION));

    zlink_completion_id_t completion_id = 0;
    (void) submit_until_pending (sender_, NULL, &completion_id);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    const zlink_poller_event_t ready =
      wait_for_completion_after_drain (poller, receiver_);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, ready.events);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, sender_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

void test_pair_none_and_immediate_dontwait_have_zero_id_and_no_completion ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-completion-immediate", &sender, &receiver,
                false);

    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    zlink_msg_t none_part;
    init_part (&none_part, "none-immediate");
    zlink_completion_id_t none_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &none_part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &none_id));
    TEST_ASSERT_EQUAL_UINT64 (0, none_id);
    assert_part_consumed (&none_part);

    int immediate_context = 11;
    zlink_msg_t dontwait_part;
    init_part (&dontwait_part, "dontwait-immediate");
    zlink_completion_id_t dontwait_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &dontwait_part, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, &immediate_context, &dontwait_id));
    TEST_ASSERT_EQUAL_UINT64 (0, dontwait_id);
    assert_part_consumed (&dontwait_part);

    zlink_msg_t more;
    init_part (&more, "multi-");
    zlink_completion_id_t more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &more, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&more);

    zlink_msg_t final;
    init_part (&final, "final");
    zlink_completion_id_t final_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &final, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, &immediate_context, &final_id));
    TEST_ASSERT_EQUAL_UINT64 (0, final_id);
    assert_part_consumed (&final);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    zlink_completion_close (&completion);
    assert_empty_completion (completion);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);
    zlink_completion_close (NULL);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_send_id_is_zeroed_before_validation_and_input_is_consumed ()
{
    zlink_msg_t part;
    init_part (&part, "invalid-handle");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_send_part (NULL, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                       NULL, &completion_id));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&part);
}

void test_pair_none_timeout_has_zero_id_no_completion_and_consumes_input ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-completion-none-timeout", &sender, &receiver,
                true);

    const int send_timeout_ms = 50;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));

    int pending_context = 23;
    zlink_completion_id_t pending_id = 0;
    (void) submit_until_pending (sender, &pending_context, &pending_id);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);

    zlink_msg_t timed_out;
    init_part (&timed_out, "none-timeout");
    zlink_completion_id_t timed_out_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (sender, &timed_out, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &timed_out_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, timed_out_id);
    assert_part_consumed (&timed_out);

    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_none_pre_return_out_of_memory_and_internal_error_are_distinct ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-none-pre-return-errors", &sender, &receiver,
                false);

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

void test_accepted_dontwait_allocation_and_runtime_failures_are_terminal ()
{
    const int injected_errnos[] = {ENOMEM, EIO};
    const char *const endpoints[] = {
      "inproc://phase3-pending-terminal-oom",
      "inproc://phase3-pending-terminal-runtime"};

    for (size_t i = 0; i != 2; ++i) {
        void *sender = NULL;
        void *receiver = NULL;
        setup_pair (endpoints[i], &sender, &receiver, true);

        zlink_completion_id_t pending_id = 0;
        (void) submit_until_pending (sender, NULL, &pending_id);
        TEST_ASSERT_NOT_EQUAL (0, pending_id);
        zlink_test_set_submit_retry_fault (1, injected_errnos[i]);

        zlink_completion_t completion;
        init_empty_completion (&completion);
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (3);
        while (std::chrono::steady_clock::now () < deadline) {
            (void) recv_one_pair_part (receiver);
            const zlink_recv_result_t recv_result = zlink_completion_recv (
              sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_result == ZLINK_RECV_OK)
                break;
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_result);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            msleep (1);
        }
        zlink_test_set_submit_retry_fault (0, 0);

        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (pending_id, completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL,
                               completion.send_result);
        TEST_ASSERT_EQUAL_INT (injected_errnos[i],
                               completion.send_terminal_errno);
        TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
        zlink_completion_close (&completion);

        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_pair_pending_completion_is_level_triggered_single_owner_and_reusable ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-completion-level", &sender, &receiver, true);

    void *owner_poller = zlink_poller_new ();
    void *contender_poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (owner_poller);
    TEST_ASSERT_NOT_NULL (contender_poller);
    int owner_tag = 31;
    int contender_tag = 32;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (owner_poller, sender, &owner_tag,
                        ZLINK_POLLCOMPLETION));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_STATE,
      zlink_poller_add (contender_poller, sender, &contender_tag,
                        ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (contender_poller, sender, &contender_tag,
                        ZLINK_POLLIN));

    int pending_context = 33;
    zlink_completion_id_t pending_id = 0;
    (void) submit_until_pending (sender, &pending_context, &pending_id);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);

    const zlink_poller_event_t first =
      wait_for_completion_after_drain (owner_poller, receiver);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, first.source_kind);
    TEST_ASSERT_EQUAL_PTR (sender, first.socket);
    TEST_ASSERT_EQUAL_PTR (&owner_tag, first.user_data);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, first.events);

    zlink_poller_event_t repeated;
    memset (&repeated, 0, sizeof (repeated));
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1,
      zlink_poller_wait (owner_poller, &repeated, 1, 0, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, repeated.events);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_STATE,
      zlink_poller_modify (
        contender_poller, sender,
        static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION)));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_modify (owner_poller, sender, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_modify (
        contender_poller, sender,
        static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION)));

    zlink_poller_event_t transferred;
    memset (&transferred, 0, sizeof (transferred));
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1,
      zlink_poller_wait (contender_poller, &transferred, 1, 1000,
                         &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_PTR (&contender_tag, transferred.user_data);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, transferred.events);

    zlink_completion_t wrong_size;
    memset (&wrong_size, 0, sizeof (wrong_size));
    wrong_size.struct_size = sizeof (wrong_size) - 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (sender, &wrong_size,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT32 (sizeof (wrong_size) - 1,
                              wrong_size.struct_size);

    zlink_completion_t nonempty;
    init_empty_completion (&nonempty);
    nonempty.completion_id = 97;
    nonempty.user_context = &owner_tag;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (sender, &nonempty,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (97, nonempty.completion_id);
    TEST_ASSERT_EQUAL_PTR (&owner_tag, nonempty.user_context);

    zlink_completion_t completion;
    init_empty_completion (&completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (sender, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (pending_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&pending_context, completion.user_context);
    TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    TEST_ASSERT_EQUAL_INT (0, completion.request_result);
    TEST_ASSERT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);

    zlink_poller_event_t after_drain;
    memset (&after_drain, 0, sizeof (after_drain));
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      0,
      zlink_poller_wait (contender_poller, &after_drain, 1, 10,
                         &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);

    zlink_completion_close (&completion);
    assert_empty_completion (completion);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (owner_poller, sender));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (contender_poller, sender));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&owner_poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&contender_poller));
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_pair_pending_send_redrives_without_a_later_sender_call ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-pending-autonomous-redrive", &sender,
                &receiver, true);

    zlink_completion_id_t pending_id = 0;
    (void) submit_until_pending (sender, NULL, &pending_id);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);

    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    TEST_ASSERT_TRUE (sender_handle.socket->has_send_pending ());

    // Return enough reader credit to cross the pipe LWM, then make no public
    // call on the sender. Its retained mailbox owner must consume
    // activate_write and drive the accepted record on its own.
    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    size_t drained = 0;
    while (std::chrono::steady_clock::now () < drain_deadline
           && drained < kMaxFillAttempts) {
        if (recv_one_pair_part (receiver)) {
            ++drained;
            continue;
        }
        if (drained != 0)
            break;
        msleep (1);
    }
    TEST_ASSERT_TRUE (drained != 0);

    const std::chrono::steady_clock::time_point redrive_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (sender_handle.socket->has_send_pending ()
           && std::chrono::steady_clock::now () < redrive_deadline)
        msleep (1);
    TEST_ASSERT_FALSE_MESSAGE (
      sender_handle.socket->has_send_pending (),
      "accepted SEND still required a later public sender call to redrive");

    zlink_completion_t completion;
    init_empty_completion (&completion);
    const std::chrono::steady_clock::time_point completion_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < completion_deadline) {
        const zlink_recv_result_t result = zlink_completion_recv (
          sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (pending_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    zlink_completion_close (&completion);

    sender_handle = socket_handle_t ();
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_completion_recv_rejects_dirty_zero_size_routing_id ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    zlink_completion_t completion;
    init_empty_completion (&completion);
    completion.peer_rid.data[sizeof (completion.peer_rid.data) - 1] = 0x5a;

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (socket, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT8 (0, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_UINT8 (
      0x5a,
      completion.peer_rid.data[sizeof (completion.peer_rid.data) - 1]);

    test_context_socket_close_zero_linger (socket);
}

void test_completion_recv_timeout_and_context_termination_keep_output_empty ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    int recv_timeout_ms = 30;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));

    zlink_completion_t timeout_completion;
    init_empty_completion (&timeout_completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (socket, &timeout_completion,
                             ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (timeout_completion);

    recv_timeout_ms = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));

    zlink_completion_t terminated_completion;
    init_empty_completion (&terminated_completion);
    zlink_recv_result_t recv_result = ZLINK_RECV_INTERNAL_ERROR;
    int recv_errno = 0;
    std::thread receiver ([&] () {
        errno = 0;
        recv_result = zlink_completion_recv (
          socket, &terminated_completion, ZLINK_RECV_FLAGS_NONE);
        recv_errno = zlink_errno ();
    });

    msleep (20);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    receiver.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_TERMINATED, recv_result);
    TEST_ASSERT_EQUAL_INT (ETERM, recv_errno);
    assert_empty_completion (terminated_completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_blocking_completion_recv_wakes_as_socket_shutdown ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    const int recv_timeout_ms = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));

    zlink_completion_t completion;
    init_empty_completion (&completion);
    zlink_recv_result_t recv_result = ZLINK_RECV_INTERNAL_ERROR;
    int recv_errno = 0;
    std::thread receiver ([&] () {
        errno = 0;
        recv_result = zlink_completion_recv (
          socket, &completion, ZLINK_RECV_FLAGS_NONE);
        recv_errno = zlink_errno ();
    });

    msleep (20);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    receiver.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INVALID_STATE, recv_result);
    TEST_ASSERT_EQUAL_INT (ESHUTDOWN, recv_errno);
    assert_empty_completion (completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_unread_completion_is_discarded_by_socket_and_context_lifecycle ()
{
    {
        void *sender = NULL;
        void *receiver = NULL;
        setup_pair ("inproc://phase3-unread-completion-socket-close",
                    &sender, &receiver, true);
        leave_one_pair_completion_unread (sender, receiver);

        test_context_socket_close_zero_linger (sender);
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_INVALID_STATE,
          zlink_completion_recv (sender, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ESHUTDOWN, zlink_errno ());
        assert_empty_completion (completion);
        test_context_socket_close_zero_linger (receiver);
    }

    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *receiver = zlink_socket (context, ZLINK_SOCKET_PAIR);
        void *sender = zlink_socket (context, ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (receiver);
        TEST_ASSERT_NOT_NULL (sender);
        const int zero_linger = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (sender, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        configure_small_pair_hwm (receiver);
        configure_small_pair_hwm (sender);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_BIND_OK,
          zlink_bind (receiver,
                      "inproc://phase3-unread-completion-context-term"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (sender,
                         "inproc://phase3-unread-completion-context-term"));
        msleep (SETTLE_TIME);
        leave_one_pair_completion_unread (sender, receiver);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_TERMINATED,
          zlink_completion_recv (sender, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_empty_completion (completion);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (sender));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (receiver));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}

struct blocking_send_observation_t
{
    blocking_send_observation_t () :
        init_result (-1), submit_result (ZLINK_SUBMIT_INTERNAL_ERROR),
        submit_errno (0), completion_id (UINT64_MAX), part_size (UINT64_MAX),
        close_result (-1)
    {
    }

    std::atomic<int> init_result;
    std::atomic<int> submit_result;
    std::atomic<int> submit_errno;
    std::atomic<uint64_t> completion_id;
    std::atomic<uint64_t> part_size;
    std::atomic<int> close_result;
};

void run_blocking_pair_send (void *socket_, blocking_send_observation_t *out_)
{
    zlink_msg_t part;
    const char payload[] = "blocking-none-lifecycle";
    const int init_result = zlink_msg_init_size (&part, sizeof (payload) - 1);
    out_->init_result.store (init_result, std::memory_order_release);
    if (init_result != ZLINK_CONFIG_OK)
        return;
    memcpy (zlink_msg_data (&part), payload, sizeof (payload) - 1);
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    out_->submit_result.store (
      zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &completion_id),
      std::memory_order_release);
    out_->submit_errno.store (zlink_errno (), std::memory_order_release);
    out_->completion_id.store (completion_id, std::memory_order_release);
    out_->part_size.store (zlink_msg_size (&part), std::memory_order_release);
    out_->close_result.store (zlink_msg_close (&part),
                              std::memory_order_release);
}

void assert_blocking_send_lifecycle_result (
  const blocking_send_observation_t &observed_, int expected_errno_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      observed_.init_result.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_TERMINATED,
      observed_.submit_result.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      expected_errno_, observed_.submit_errno.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT64 (
      0, observed_.completion_id.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT64 (
      0, observed_.part_size.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      observed_.close_result.load (std::memory_order_acquire));
}

void test_blocking_none_send_distinguishes_socket_and_context_shutdown ()
{
    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (socket);
        const int infinite_timeout = -1;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &infinite_timeout,
                            sizeof (infinite_timeout)));

        blocking_send_observation_t observed;
        std::thread sender (run_blocking_pair_send, socket, &observed);
        msleep (20);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
        sender.join ();
        assert_blocking_send_lifecycle_result (observed, ESHUTDOWN);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }

    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (socket);
        const int infinite_timeout = -1;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &infinite_timeout,
                            sizeof (infinite_timeout)));

        blocking_send_observation_t observed;
        std::thread sender (run_blocking_pair_send, socket, &observed);
        msleep (20);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_ctx_shutdown (context));
        sender.join ();
        assert_blocking_send_lifecycle_result (observed, ETERM);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}

void test_public_completion_reservation_limit_releases_only_after_recv ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-completion-reservation-limit", &sender,
                &receiver, true);

    zlink_completion_id_t first_pending_id = 0;
    (void) submit_until_pending (sender, NULL, &first_pending_id);
    TEST_ASSERT_NOT_EQUAL (0, first_pending_id);

    const size_t reservation_limit = 65536;
    for (size_t i = 1; i < reservation_limit; ++i) {
        zlink_msg_t part;
        init_part (&part, "x");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        TEST_ASSERT_NOT_EQUAL (0, completion_id);
        assert_part_consumed (&part);
    }

    zlink_msg_t rejected;
    init_part (&rejected, "capacity-rejected");
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (sender, &rejected, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);
    assert_part_consumed (&rejected);

    // A caller that hit the shared completion cap can still make immediate
    // ID-0 progress after all older pending sends admit and the transport
    // becomes writable. Keep the receiver active while the caller waits just
    // as a real peer would be; POLLOUT must not depend on draining the
    // completion queue first.
    std::atomic<bool> stop_drainer (false);
    std::atomic<int> drainer_error (0);
    std::thread drainer ([&] {
        while (!stop_drainer.load (std::memory_order_acquire)) {
            zlink_msg_t part;
            if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
                drainer_error.store (EFAULT, std::memory_order_release);
                return;
            }
            zlink_part_flag_t has_more = ZLINK_PART_MORE;
            const zlink_recv_result_t result = zlink_recv_part (
              receiver, NULL, &part, &has_more,
              ZLINK_RECV_FLAGS_DONTWAIT);
            const int recv_errno = zlink_errno ();
            if (zlink_msg_close (&part) != ZLINK_CONFIG_OK) {
                drainer_error.store (EFAULT, std::memory_order_release);
                return;
            }
            if (result == ZLINK_RECV_OK)
                continue;
            if (result != ZLINK_RECV_NO_DATA || recv_errno != EAGAIN) {
                drainer_error.store (recv_errno != 0 ? recv_errno : EIO,
                                     std::memory_order_release);
                return;
            }
            std::this_thread::yield ();
        }
    });
    zlink_pollitem_t writable = {sender, 0, ZLINK_POLLOUT, 0};
    const int writable_count = zlink_poll (&writable, 1, 5000, NULL);
    stop_drainer.store (true, std::memory_order_release);
    drainer.join ();
    TEST_ASSERT_EQUAL_INT (0, drainer_error.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (1, writable_count);
    TEST_ASSERT_TRUE ((writable.revents & ZLINK_POLLOUT) != 0);

    zlink_msg_t immediate;
    init_part (&immediate, "capacity-full-immediate");
    zlink_completion_id_t immediate_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &immediate, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &immediate_id));
    TEST_ASSERT_EQUAL_UINT64 (0, immediate_id);
    assert_part_consumed (&immediate);

    while (recv_one_pair_part (receiver)) {
    }
    const int recv_timeout_ms = 5000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));

    zlink_completion_t completion;
    init_empty_completion (&completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (first_pending_id, completion.completion_id);
    zlink_completion_close (&completion);

    zlink_completion_id_t accepted_id = 0;
    (void) submit_until_pending (sender, NULL, &accepted_id);
    TEST_ASSERT_NOT_EQUAL (0, accepted_id);

    const std::chrono::steady_clock::time_point admitted_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!recv_one_pair_part (receiver)
           && std::chrono::steady_clock::now () < admitted_deadline)
        msleep (1);
    TEST_ASSERT_TRUE_MESSAGE (
      std::chrono::steady_clock::now () < admitted_deadline,
      "accepted reservation-reuse record was not admitted before close");

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_pending_byte_limit_is_multipart_atomic_and_released_on_admission ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-pending-byte-multipart", &sender,
                &receiver, true);

    zlink_completion_id_t first_pending_id = 0;
    (void) submit_until_pending (sender, NULL, &first_pending_id);
    TEST_ASSERT_NOT_EQUAL (0, first_pending_id);

    const uint64_t frame_charge =
      kPayloadSize > sizeof (zlink_msg_t) ? kPayloadSize
                                         : sizeof (zlink_msg_t);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_PENDING_MAX_BYTES, &frame_charge,
                        sizeof (frame_charge)));

    zlink_msg_t rejected_more;
    init_part (&rejected_more, "h");
    zlink_completion_id_t more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &rejected_more, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&rejected_more);

    zlink_msg_t rejected_final;
    init_part (&rejected_final, "t");
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (sender, &rejected_final,
                       ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                       &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);
    assert_part_consumed (&rejected_final);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, sender, sender, ZLINK_POLLCOMPLETION));
    (void) wait_for_completion_after_drain (poller, receiver);

    zlink_completion_t first_completion;
    init_empty_completion (&first_completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (sender, &first_completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, first_completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (first_pending_id,
                              first_completion.completion_id);
    zlink_completion_close (&first_completion);

    const uint64_t multipart_charge = 2u * frame_charge;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_PENDING_MAX_BYTES,
                        &multipart_charge, sizeof (multipart_charge)));

    zlink_msg_t accepted_more;
    init_part (&accepted_more, "H");
    more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_more, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&accepted_more);

    zlink_msg_t accepted_final;
    init_part (&accepted_final, "T");
    zlink_completion_id_t accepted_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_final,
                       ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                       &accepted_id));
    TEST_ASSERT_NOT_EQUAL (0, accepted_id);
    assert_part_consumed (&accepted_final);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, sender));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_router_none_wait_keeps_logical_rid_across_physical_reconnect ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *replacement = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (replacement);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    const int reconnect_ivl = 20;
    const int send_timeout_ms = 2000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (replacement, ZLINK_OPT_LINGER, &zero,
                        sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl,
                        sizeof (reconnect_ivl)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));

    const char *const target_name = "phase3-none-logical-target";
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (server, target_name, strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (replacement, target_name,
                            strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               target_name, strlen (target_name)));

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (client, endpoint));

    const zlink_routing_id_t target_rid = make_text_rid (target_name);
    zlink_msg_t prime;
    init_part (&prime, "prime-before-detach");
    zlink_completion_id_t completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &prime,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                           &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&prime);
    receive_router_data_eventually (server, "prime-before-detach");

    test_context_socket_close_zero_linger (server);
    msleep (SETTLE_TIME * 2);

    std::atomic<int> rebind_result (ZLINK_BIND_INTERNAL_ERROR);
    std::thread rebinder ([&] () {
        msleep (50);
        rebind_result.store (zlink_bind (replacement, endpoint),
                             std::memory_order_release);
    });

    zlink_msg_t retried;
    init_part (&retried, "none-after-reconnect");
    completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &retried,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                           &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&retried);
    rebinder.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           rebind_result.load (std::memory_order_acquire));
    receive_router_data_eventually (replacement, "none-after-reconnect");

    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (client, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (replacement);
}

void test_router_none_wait_explicit_rid_removal_is_synchronous_not_found ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_small_pair_hwm (server);
    configure_small_pair_hwm (client);

    const int infinite_timeout = -1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_SNDTIMEO, &infinite_timeout,
                        sizeof (infinite_timeout)));
    const char *const target_name = "phase3-none-explicit-remove";
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (server, target_name, strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               target_name, strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (server, "inproc://phase3-none-explicit-remove"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (client, "inproc://phase3-none-explicit-remove"));

    const zlink_routing_id_t target_rid = make_text_rid (target_name);
    zlink_msg_t prime;
    init_part (&prime, "prime");
    zlink_completion_id_t completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &prime,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                           &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&prime);
    receive_router_data_eventually (server, "prime");

    TEST_ASSERT_NOT_EQUAL (0,
                           submit_routed_until_pending (client, &target_rid));

    std::atomic<int> submit_result (ZLINK_SUBMIT_INTERNAL_ERROR);
    std::atomic<int> submit_errno (0);
    std::atomic<int> part_init_result (-1);
    std::atomic<int> part_close_result (-1);
    std::atomic<uint64_t> observed_completion_id (UINT64_MAX);
    std::atomic<uint64_t> observed_part_size (UINT64_MAX);
    std::thread blocked_sender ([&] () {
        zlink_msg_t part;
        const std::string payload (kPayloadSize, 'n');
        const int init_result = zlink_msg_init_size (&part, payload.size ());
        part_init_result.store (init_result, std::memory_order_release);
        if (init_result != ZLINK_CONFIG_OK)
            return;
        memcpy (zlink_msg_data (&part), payload.data (), payload.size ());
        zlink_completion_id_t id = UINT64_MAX;
        errno = 0;
        submit_result.store (
          zlink_send_part_rid (client, &target_rid, &part,
                               ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                               &id),
          std::memory_order_release);
        submit_errno.store (zlink_errno (), std::memory_order_release);
        observed_completion_id.store (id, std::memory_order_release);
        observed_part_size.store (zlink_msg_size (&part),
                                  std::memory_order_release);
        part_close_result.store (zlink_msg_close (&part),
                                 std::memory_order_release);
    });

    msleep (30);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect_rid (client, &target_rid));
    blocked_sender.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           part_init_result.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_FOUND,
                           submit_result.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (ENOENT,
                           submit_errno.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT64 (
      0, observed_completion_id.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT64 (0,
                              observed_part_size.load (
                                std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           part_close_result.load (
                             std::memory_order_acquire));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_router_pending_detach_reconnect_has_one_completion_and_no_replay ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *replacement = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (replacement);
    TEST_ASSERT_NOT_NULL (client);
    configure_small_pair_hwm (server);
    configure_small_pair_hwm (replacement);
    configure_small_pair_hwm (client);

    const int zero = 0;
    const int reconnect_ivl = 10;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (replacement, ZLINK_OPT_LINGER, &zero,
                        sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl,
                        sizeof (reconnect_ivl)));

    const char *const target_name = "phase3-pending-race-target";
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (server, target_name, strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (replacement, target_name,
                            strlen (target_name)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               target_name, strlen (target_name)));

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (client, endpoint));
    const zlink_routing_id_t target_rid = make_text_rid (target_name);

    zlink_msg_t prime;
    init_part (&prime, "prime");
    zlink_completion_id_t id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &prime,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                           &id));
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    assert_part_consumed (&prime);
    receive_router_data_eventually (server, "prime");

    const char *const marker = "phase3-detach-reconnect-marker";
    zlink_msg_t pending_prefix;
    init_part (&pending_prefix, "pending-prefix");
    zlink_completion_id_t prefix_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &pending_prefix,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, NULL,
                           &prefix_id));
    TEST_ASSERT_EQUAL_UINT64 (0, prefix_id);
    assert_part_consumed (&pending_prefix);

    zlink_msg_t pending_marker;
    init_part (&pending_marker, marker);
    zlink_completion_id_t marker_id = 0;
    // Autonomous pending redrive can turn a transient HWM-full observation
    // writable before the following marker call. Hold physical admission in
    // retryable backpressure until detach so this test deterministically
    // exercises the pending-record reconnect path it names.
    zlink_test_set_submit_retry_fault (static_cast<int> (kMaxFillAttempts),
                                       EAGAIN);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (client, &target_rid, &pending_marker,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &marker_id));
    TEST_ASSERT_NOT_EQUAL (0, marker_id);
    assert_part_consumed (&pending_marker);

    test_context_socket_close_zero_linger (server);
    msleep (30);
    zlink_test_set_submit_retry_fault (0, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (replacement, endpoint));

    bool marker_completion_seen = false;
    size_t marker_completion_count = 0;
    size_t marker_payload_count = 0;
    std::chrono::steady_clock::time_point completion_seen_at;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        const zlink_recv_result_t completion_result = zlink_completion_recv (
          client, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (completion_result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                                   completion.send_result);
            TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
            if (completion.completion_id == marker_id) {
                ++marker_completion_count;
                marker_completion_seen = true;
                completion_seen_at = std::chrono::steady_clock::now ();
            }
            zlink_completion_close (&completion);
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, completion_result);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            assert_empty_completion (completion);
        }

        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = UINT64_MAX;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_MORE;
        errno = 0;
        const zlink_recv_result_t recv_result = zlink_router_recv_part (
          replacement, &source_rid, &reply_token, &part, &part_flag,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_result == ZLINK_RECV_OK) {
            const std::string payload (
              static_cast<const char *> (zlink_msg_data (&part)),
              zlink_msg_size (&part));
            if (payload == marker)
                ++marker_payload_count;
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_result);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        }

        if (marker_completion_seen
            && std::chrono::steady_clock::now () - completion_seen_at
                 >= std::chrono::milliseconds (250))
            break;
        msleep (1);
    }

    TEST_ASSERT_TRUE_MESSAGE (marker_completion_seen,
                              "pending marker completion was lost");
    TEST_ASSERT_EQUAL_UINT64 (1, marker_completion_count);
    // detach-wins delivers once on the replacement; admission-wins leaves no
    // application shadow to replay there. Both sides of the linearization are
    // legal, but duplicate delivery is never legal.
    TEST_ASSERT_TRUE (marker_payload_count == 0 || marker_payload_count == 1);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (replacement);
}
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
#define RUN_PHASE3_COMPLETION_TEST(test_)                                  \
    do {                                                                   \
        if (should_run_phase3_completion_test (#test_))                    \
            RUN_TEST (test_);                                              \
    } while (false)

    RUN_PHASE3_COMPLETION_TEST (
      test_pair_none_and_immediate_dontwait_have_zero_id_and_no_completion);
    RUN_PHASE3_COMPLETION_TEST (
      test_send_id_is_zeroed_before_validation_and_input_is_consumed);
    RUN_PHASE3_COMPLETION_TEST (
      test_pair_none_timeout_has_zero_id_no_completion_and_consumes_input);
    RUN_PHASE3_COMPLETION_TEST (
      test_none_pre_return_out_of_memory_and_internal_error_are_distinct);
    RUN_PHASE3_COMPLETION_TEST (
      test_accepted_dontwait_allocation_and_runtime_failures_are_terminal);
    RUN_PHASE3_COMPLETION_TEST (
      test_pair_pending_completion_is_level_triggered_single_owner_and_reusable);
    RUN_PHASE3_COMPLETION_TEST (
      test_pair_pending_send_redrives_without_a_later_sender_call);
    RUN_PHASE3_COMPLETION_TEST (
      test_completion_recv_rejects_dirty_zero_size_routing_id);
    RUN_PHASE3_COMPLETION_TEST (
      test_completion_recv_timeout_and_context_termination_keep_output_empty);
    RUN_PHASE3_COMPLETION_TEST (
      test_blocking_completion_recv_wakes_as_socket_shutdown);
    RUN_PHASE3_COMPLETION_TEST (
      test_unread_completion_is_discarded_by_socket_and_context_lifecycle);
    RUN_PHASE3_COMPLETION_TEST (
      test_blocking_none_send_distinguishes_socket_and_context_shutdown);
    RUN_PHASE3_COMPLETION_TEST (
      test_public_completion_reservation_limit_releases_only_after_recv);
    RUN_PHASE3_COMPLETION_TEST (
      test_pending_byte_limit_is_multipart_atomic_and_released_on_admission);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_none_wait_keeps_logical_rid_across_physical_reconnect);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_none_wait_explicit_rid_removal_is_synchronous_not_found);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_pending_detach_reconnect_has_one_completion_and_no_replay);

#undef RUN_PHASE3_COMPLETION_TEST
    return UNITY_END ();
}
