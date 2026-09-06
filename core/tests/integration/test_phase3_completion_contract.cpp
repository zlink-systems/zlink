/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "completion_test_helpers.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT


namespace
{
const size_t kPayloadSize = 64;
const size_t kMaxFillAttempts = 512;

bool should_run_phase3_completion_test (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
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

    // A blocking probe is the synchronization point for the inproc attach. It
    // leaves no timer-based connection-settle assumption in the DONTWAIT tests
    // and is drained before either side is returned to the caller.
    zlink_msg_t probe;
    init_part (&probe, "pair-ready");
    zlink_completion_id_t probe_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &probe, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &probe_id));
    TEST_ASSERT_EQUAL_UINT64 (0, probe_id);
    assert_part_consumed (&probe);

    zlink_msg_t received_probe;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init (&received_probe));
    zlink_part_flag_t probe_flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (receiver, NULL, &received_probe, &probe_flag,
                       ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, probe_flag);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("pair-ready"),
                              zlink_msg_size (&received_probe));
    TEST_ASSERT_EQUAL_MEMORY ("pair-ready",
                              zlink_msg_data (&received_probe),
                              strlen ("pair-ready"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&received_probe));
    *sender_out_ = sender;
    *receiver_out_ = receiver;
}

void receive_one_pair_part (void *receiver_, const char *expected_ = NULL)
{
    zlink_pollitem_t readable = {receiver_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&readable, 1, 5000, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, readable.revents);

    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const zlink_recv_result_t result = zlink_recv_part (
      receiver_, NULL, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    if (expected_) {
        TEST_ASSERT_EQUAL_UINT64 (strlen (expected_), zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY (expected_, zlink_msg_data (&part),
                                  strlen (expected_));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void receive_pair_multipart (void *receiver_, const std::string &first_,
                             const std::string &final_)
{
    zlink_pollitem_t readable = {receiver_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&readable, 1, 5000, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, readable.revents);

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver_, NULL, &parts, &part_count,
                  ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (first_.size (), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (first_.data (), zlink_msg_data (&parts[0]),
                              first_.size ());
    TEST_ASSERT_EQUAL_UINT64 (final_.size (), zlink_msg_size (&parts[1]));
    TEST_ASSERT_EQUAL_MEMORY (final_.data (), zlink_msg_data (&parts[1]),
                              final_.size ());
    zlink_multipart_close (parts, part_count);
}


void *make_writable_completion_poller (void *socket_, void *user_data_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (
        poller, socket_, user_data_,
        static_cast<short> (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)));
    return poller;
}

void destroy_writable_completion_poller (void **poller_, void *socket_)
{
    TEST_ASSERT_NOT_NULL (poller_);
    TEST_ASSERT_NOT_NULL (*poller_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (*poller_, socket_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (poller_));
    TEST_ASSERT_NULL (*poller_);
}

void assert_writable_poller_quiet (void *poller_)
{
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (0,
                           zlink_poller_wait (poller_, &event, 1, 0,
                                              &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
}

void receive_writable_completion (
  void *poller_, void *socket_, void *poller_user_data_,
  zlink_completion_id_t expected_id_, void *expected_context_,
  const zlink_routing_id_t *expected_rid_ = NULL)
{
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller_, &event, 1, 5000, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, event.source_kind);
    TEST_ASSERT_EQUAL_PTR (socket_, event.socket);
    TEST_ASSERT_EQUAL_PTR (poller_user_data_, event.user_data);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, event.events);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);

    // Both readiness bits are level-triggered until the WRITABLE record is
    // delivered, not merely an edge carried by the first wake command.
    memset (&event, 0, sizeof (event));
    error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller_, &event, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, event.events);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);

    zlink_completion_t completion;
    init_empty_completion (&completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (expected_context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    if (expected_rid_) {
        TEST_ASSERT_EQUAL_UINT (expected_rid_->size,
                                completion.peer_rid.size);
        TEST_ASSERT_EQUAL_MEMORY (expected_rid_->data,
                                  completion.peer_rid.data,
                                  expected_rid_->size);
    }
    zlink_completion_close (&completion);
    assert_empty_completion (completion);
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

std::string receive_router_part_now (void *router_)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source_rid, &reply_token, &part,
                              &part_flag, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
    const std::string payload (
      static_cast<const char *> (zlink_msg_data (&part)),
      zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return payload;
}

std::string receive_one_router_part (void *router_)
{
    zlink_pollitem_t readable = {router_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&readable, 1, 5000, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, readable.revents);
    return receive_router_part_now (router_);
}

void receive_router_data_eventually (void *router_, const char *expected_)
{
    const std::string payload = receive_one_router_part (router_);
    TEST_ASSERT_EQUAL_STRING (expected_, payload.c_str ());
}

size_t fill_unrouted_until_backpressured (
  void *sender_, void *user_context_ = NULL,
  zlink_completion_id_t *completion_id_out_ = NULL)
{
    for (size_t i = 0; i != kMaxFillAttempts; ++i) {
        zlink_msg_t part;
        init_part (&part, std::string (kPayloadSize, 'p'));
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        const zlink_submit_result_t result = zlink_send_part (
          sender_, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
          user_context_, &completion_id);
        assert_part_consumed (&part);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            TEST_ASSERT_NOT_EQUAL (0, completion_id);
            TEST_ASSERT_TRUE_MESSAGE (i != 0,
                                      "PAIR reached HWM before any send");
            if (completion_id_out_)
                *completion_id_out_ = completion_id;
            return i;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    }
    TEST_FAIL_MESSAGE (
      "unrouted DONTWAIT send did not report physical HWM backpressure");
    return 0;
}

size_t fill_routed_until_backpressured (
  void *sender_, const zlink_routing_id_t *target_rid_,
  void *user_context_ = NULL,
  zlink_completion_id_t *completion_id_out_ = NULL)
{
    for (size_t i = 0; i != kMaxFillAttempts; ++i) {
        zlink_msg_t part;
        init_part (&part, std::string (kPayloadSize, 'r'));
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        const zlink_submit_result_t result = zlink_send_part_rid (
          sender_, target_rid_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, user_context_, &completion_id);
        assert_part_consumed (&part);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            TEST_ASSERT_NOT_EQUAL (0, completion_id);
            TEST_ASSERT_TRUE_MESSAGE (i != 0,
                                      "ROUTER reached HWM before any send");
            if (completion_id_out_)
                *completion_id_out_ = completion_id;
            return i;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    }
    TEST_FAIL_MESSAGE (
      "ROUTER DONTWAIT send did not report physical HWM backpressure");
    return 0;
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

    const size_t accepted = fill_unrouted_until_backpressured (sender);
    TEST_ASSERT_TRUE (accepted != 0);

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

    assert_no_completion (sender);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}



void test_socket_close_terminalizes_and_reclaims_wait_tokens ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *socket = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    const int zero_linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));

    // Cross the inline reservation capacity so close also exercises pooled
    // heap nodes. No endpoint exists, hence every token remains waiting and
    // socket teardown is solely responsible for terminal cleanup.
    static const size_t token_count = 128;
    zlink_completion_id_t previous_id = 0;
    int contexts[token_count];
    for (size_t i = 0; i != token_count; ++i) {
        contexts[i] = static_cast<int> (i);
        zlink_msg_t part;
        init_part (&part, "close-wait-token");
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_BACKPRESSURED,
          zlink_send_part (socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, &contexts[i], &completion_id));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_TRUE (completion_id > previous_id);
        previous_id = completion_id;
        assert_part_consumed (&part);
    }
    assert_no_completion (socket);

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_completion_recv_rejects_dirty_zero_size_routing_id ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    zlink_completion_t wrong_size;
    memset (&wrong_size, 0, sizeof (wrong_size));
    wrong_size.struct_size = sizeof (wrong_size) - 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (socket, &wrong_size,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT32 (sizeof (wrong_size) - 1,
                              wrong_size.struct_size);

    zlink_completion_t nonempty;
    init_empty_completion (&nonempty);
    nonempty.completion_id = 97;
    nonempty.user_context = socket;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_completion_recv (socket, &nonempty,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (97, nonempty.completion_id);
    TEST_ASSERT_EQUAL_PTR (socket, nonempty.user_context);

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

void test_dontwait_connect_before_bind_is_not_retained_and_can_be_retried ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    void *receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (sender);
    TEST_ASSERT_NOT_NULL (receiver);
    const int zero_linger = 0;
    const int immediate = 1;
    const int receive_timeout = 5000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_IMMEDIATE, &immediate,
                        sizeof (immediate)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (receiver, ZLINK_OPT_RCVTIMEO, &receive_timeout,
                        sizeof (receive_timeout)));

    char endpoint[MAX_SOCKET_STRING];
    fd_t reserved = bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    close (reserved);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender, endpoint));

    int poller_tag = 41;
    void *poller = make_writable_completion_poller (sender, &poller_tag);

    const std::string logical_payload = "caller-retained-connect-later";
    int rejected_context = 42;
    zlink_msg_t rejected;
    init_part (&rejected, logical_payload);
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (sender, &rejected, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, &rejected_context, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, rejected_id);
    assert_part_consumed (&rejected);
    TEST_ASSERT_EQUAL_STRING ("caller-retained-connect-later",
                              logical_payload.c_str ());
    assert_no_completion (sender);
    assert_writable_poller_quiet (poller);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver, endpoint));
    receive_writable_completion (poller, sender, &poller_tag, rejected_id,
                                 &rejected_context);

    zlink_msg_t retried;
    init_part (&retried, logical_payload);
    zlink_completion_id_t retried_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &retried, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &retried_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retried_id);
    assert_part_consumed (&retried);
    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t received;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&received));
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (receiver, &source_rid, &reply_token, &received,
                              &part_flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
    TEST_ASSERT_EQUAL_UINT64 (logical_payload.size (),
                              zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY (logical_payload.data (),
                              zlink_msg_data (&received),
                              logical_payload.size ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&received));
    assert_no_completion (sender);

    destroy_writable_completion_poller (&poller, sender);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_dealer_writable_completion_uses_any_open_candidate ()
{
    const char *const endpoints[2] = {
      "inproc://phase3-dealer-candidate-a",
      "inproc://phase3-dealer-candidate-b"};
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *routers[2] = {test_context_socket (ZLINK_SOCKET_ROUTER),
                        test_context_socket (ZLINK_SOCKET_ROUTER)};
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (routers[0]);
    TEST_ASSERT_NOT_NULL (routers[1]);

    const int zero_linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    configure_small_pair_hwm (dealer);
    for (size_t i = 0; i != 2; ++i) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (routers[i], ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        configure_small_pair_hwm (routers[i]);
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (routers[i], endpoints[i]));
    }

    // Establish candidate A first, then prove candidate B joined the DEALER
    // load-balancing set by observing a bounded sequence of public sends.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoints[0]));
    zlink_msg_t first_prime;
    init_part (&first_prime, "dealer-candidate-prime");
    zlink_completion_id_t first_prime_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &first_prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &first_prime_id));
    TEST_ASSERT_EQUAL_UINT64 (0, first_prime_id);
    assert_part_consumed (&first_prime);
    receive_router_data_eventually (routers[0], "dealer-candidate-prime");

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoints[1]));
    bool saw_candidate_b = false;
    for (size_t attempt = 0; attempt != kMaxFillAttempts
                             && !saw_candidate_b;
         ++attempt) {
        zlink_msg_t probe;
        init_part (&probe, "dealer-candidate-probe");
        zlink_completion_id_t probe_id = UINT64_MAX;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &probe, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, &probe_id));
        TEST_ASSERT_EQUAL_UINT64 (0, probe_id);
        assert_part_consumed (&probe);

        zlink_pollitem_t readable[2] = {
          {routers[0], 0, ZLINK_POLLIN, 0},
          {routers[1], 0, ZLINK_POLLIN, 0}};
        zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
        const int count = zlink_poll (readable, 2, 5000, &poll_error);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
        TEST_ASSERT_TRUE (count >= 1);
        for (size_t i = 0; i != 2; ++i) {
            if ((readable[i].revents & ZLINK_POLLIN) == 0)
                continue;
            const std::string payload = receive_router_part_now (routers[i]);
            TEST_ASSERT_EQUAL_STRING ("dealer-candidate-probe",
                                      payload.c_str ());
            if (i == 1)
                saw_candidate_b = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (saw_candidate_b,
                              "second DEALER candidate did not attach");

    int poller_tag = 81;
    void *poller = make_writable_completion_poller (dealer, &poller_tag);
    int wait_context = 82;
    zlink_completion_id_t wait_id = 0;
    const size_t accepted = fill_unrouted_until_backpressured (
      dealer, &wait_context, &wait_id);
    TEST_ASSERT_TRUE (accepted != 0);
    assert_no_completion (dealer);
    assert_writable_poller_quiet (poller);

    int router_tag = 83;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, routers[0], &router_tag, ZLINK_POLLIN));
    bool saw_dealer_writable = false;
    const std::string filler (kPayloadSize, 'p');
    for (size_t attempt = 0; attempt != kMaxFillAttempts
                             && !saw_dealer_writable;
         ++attempt) {
        zlink_poller_event_t events[2];
        memset (events, 0, sizeof (events));
        zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
        const int count =
          zlink_poller_wait (poller, events, 2, 5000, &poll_error);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
        TEST_ASSERT_TRUE (count >= 1);
        for (int i = 0; i != count; ++i) {
            TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET,
                                   events[i].source_kind);
            if (events[i].socket == routers[0]) {
                TEST_ASSERT_EQUAL_PTR (&router_tag, events[i].user_data);
                TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, events[i].events);
                const std::string payload =
                  receive_router_part_now (routers[0]);
                TEST_ASSERT_EQUAL_STRING (filler.c_str (), payload.c_str ());
            } else if (events[i].socket == dealer) {
                TEST_ASSERT_EQUAL_PTR (&poller_tag, events[i].user_data);
                TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, events[i].events);
                TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION,
                                       events[i].events);
                saw_dealer_writable = true;
            } else {
                TEST_FAIL_MESSAGE ("unexpected socket in DEALER poller");
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (saw_dealer_writable,
                              "drained candidate did not wake DEALER");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, routers[0]));
    receive_writable_completion (poller, dealer, &poller_tag, wait_id,
                                 &wait_context);

    zlink_msg_t retry;
    init_part (&retry, "dealer-open-candidate-retry");
    zlink_completion_id_t retry_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &retry, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &retry_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retry_id);
    assert_part_consumed (&retry);

    bool received_retry = false;
    for (size_t attempt = 0; attempt != accepted + 1 && !received_retry;
         ++attempt) {
        const std::string payload = receive_one_router_part (routers[0]);
        if (payload == "dealer-open-candidate-retry") {
            received_retry = true;
        } else {
            TEST_ASSERT_EQUAL_STRING (filler.c_str (), payload.c_str ());
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (
      received_retry, "DEALER retry did not use the drained candidate");
    assert_no_completion (dealer);

    destroy_writable_completion_poller (&poller, dealer);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (routers[0]);
    test_context_socket_close_zero_linger (routers[1]);
}

void test_dontwait_hwm_is_immediate_atomic_and_pending_options_do_not_apply ()
{
    void *sender = NULL;
    void *receiver = NULL;
    setup_pair ("inproc://phase3-dontwait-hwm-multipart", &sender,
                &receiver, true);

    const uint64_t one = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_PENDING_MAX_MSGS, &one,
                        sizeof (one)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_PENDING_MAX_BYTES, &one,
                        sizeof (one)));
    uint64_t observed = 0;
    size_t observed_size = sizeof (observed);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (sender, ZLINK_OPT_PENDING_MAX_MSGS, &observed,
                        &observed_size));
    TEST_ASSERT_EQUAL_UINT64 (one, observed);
    observed = 0;
    observed_size = sizeof (observed);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (sender, ZLINK_OPT_PENDING_MAX_BYTES, &observed,
                        &observed_size));
    TEST_ASSERT_EQUAL_UINT64 (one, observed);

    int poller_tag = 51;
    void *poller = make_writable_completion_poller (sender, &poller_tag);
    int fill_context = 52;
    zlink_completion_id_t fill_id = 0;
    const size_t accepted =
      fill_unrouted_until_backpressured (sender, &fill_context, &fill_id);
    TEST_ASSERT_TRUE_MESSAGE (
      accepted > 1,
      "SEND was limited by ZLINK_OPT_PENDING_MAX_MSGS instead of HWM");
    assert_no_completion (sender);
    assert_writable_poller_quiet (poller);

    zlink_pollitem_t not_writable = {sender, 0, ZLINK_POLLOUT, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&not_writable, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (0, not_writable.revents);

    const std::string logical_prefix = "caller-retained-prefix";
    const std::string logical_final = "caller-retained-final";
    zlink_msg_t rejected_more;
    init_part (&rejected_more, logical_prefix);
    zlink_completion_id_t more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &rejected_more, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&rejected_more);

    zlink_msg_t rejected_final;
    init_part (&rejected_final, logical_final);
    int rejected_context = 53;
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (sender, &rejected_final,
                       ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                       &rejected_context, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, rejected_id);
    TEST_ASSERT_NOT_EQUAL (fill_id, rejected_id);
    assert_part_consumed (&rejected_final);
    TEST_ASSERT_EQUAL_STRING ("caller-retained-prefix",
                              logical_prefix.c_str ());
    TEST_ASSERT_EQUAL_STRING ("caller-retained-final",
                              logical_final.c_str ());
    assert_no_completion (sender);
    assert_writable_poller_quiet (poller);

    for (size_t i = 0; i != accepted; ++i)
        receive_one_pair_part (receiver);
    receive_writable_completion (poller, sender, &poller_tag, fill_id,
                                 &fill_context);
    receive_writable_completion (poller, sender, &poller_tag, rejected_id,
                                 &rejected_context);
    zlink_poller_event_t completion_drained_event;
    memset (&completion_drained_event, 0, sizeof (completion_drained_event));
    zlink_config_result_t completion_drained_error =
      ZLINK_CONFIG_INTERNAL_ERROR;
    const int completion_drained_count = zlink_poller_wait (
      poller, &completion_drained_event, 1, 0, &completion_drained_error);
    TEST_ASSERT_TRUE (completion_drained_count == 0
                      || completion_drained_count == 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, completion_drained_error);
    if (completion_drained_count == 1)
        TEST_ASSERT_EQUAL_INT (
          0, completion_drained_event.events & ZLINK_POLLCOMPLETION);

    zlink_msg_t accepted_more;
    init_part (&accepted_more, logical_prefix);
    more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_more, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&accepted_more);

    zlink_msg_t accepted_final;
    init_part (&accepted_final, logical_final);
    zlink_completion_id_t accepted_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &accepted_final,
                       ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                       &accepted_id));
    TEST_ASSERT_EQUAL_UINT64 (0, accepted_id);
    assert_part_consumed (&accepted_final);
    receive_pair_multipart (receiver, logical_prefix, logical_final);
    assert_no_completion (sender);

    destroy_writable_completion_poller (&poller, sender);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_router_directed_multipart_hwm_rollback_pollout_retry ()
{
    const char *const endpoint =
      "inproc://phase3-router-directed-multipart-hwm";
    const char *const dealer_name = "phase3-directed-multipart-peer";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const int zero_linger = 0;
    const int mandatory = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY,
                               &mandatory, sizeof (mandatory)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (dealer, dealer_name, strlen (dealer_name)));
    configure_small_pair_hwm (router);
    configure_small_pair_hwm (dealer);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint));

    // The blocking prime is the connection synchronization point and makes
    // the DEALER routing id visible to the ROUTER send plane. Public POLLOUT
    // is reserved for a ready WRITABLE wait token, not initial connectivity.
    zlink_msg_t prime;
    init_part (&prime, "route-prime");
    zlink_completion_id_t prime_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &prime_id));
    TEST_ASSERT_EQUAL_UINT64 (0, prime_id);
    assert_part_consumed (&prime);

    zlink_pollitem_t router_readable = {router, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poll (&router_readable, 1, 5000, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, router_readable.revents);

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t received_prime;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init (&received_prime));
    zlink_part_flag_t prime_flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token,
                              &received_prime, &prime_flag,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, prime_flag);
    TEST_ASSERT_EQUAL_UINT64 (strlen (dealer_name), source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (dealer_name, source_rid->data,
                              strlen (dealer_name));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&received_prime));

    const zlink_routing_id_t target_rid = make_text_rid (dealer_name);
    int poller_tag = 61;
    void *poller = make_writable_completion_poller (router, &poller_tag);
    int fill_context = 62;
    zlink_completion_id_t fill_id = 0;
    const size_t accepted =
      fill_routed_until_backpressured (router, &target_rid, &fill_context,
                                       &fill_id);
    TEST_ASSERT_TRUE (accepted != 0);
    assert_no_completion (router);
    assert_writable_poller_quiet (poller);

    zlink_pollitem_t not_writable = {router, 0, ZLINK_POLLOUT, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&not_writable, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (0, not_writable.revents);

    const std::string logical_prefix = "directed-caller-prefix";
    const std::string logical_final = "directed-caller-final";
    zlink_msg_t rejected_more;
    init_part (&rejected_more, logical_prefix);
    zlink_completion_id_t more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &target_rid, &rejected_more,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, NULL,
                           &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&rejected_more);

    zlink_msg_t rejected_final;
    init_part (&rejected_final, logical_final);
    int rejected_context = 63;
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part_rid (router, &target_rid, &rejected_final,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                           &rejected_context, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, rejected_id);
    TEST_ASSERT_NOT_EQUAL (fill_id, rejected_id);
    assert_part_consumed (&rejected_final);
    TEST_ASSERT_EQUAL_STRING ("directed-caller-prefix",
                              logical_prefix.c_str ());
    TEST_ASSERT_EQUAL_STRING ("directed-caller-final",
                              logical_final.c_str ());
    assert_no_completion (router);
    assert_writable_poller_quiet (poller);

    const std::string filler (kPayloadSize, 'r');
    for (size_t i = 0; i != accepted; ++i)
        receive_one_pair_part (dealer, filler.c_str ());

    // The failed FINAL rolled back the staged prefix: the peer sees neither
    // a partial record nor a Core-retained copy before the caller retries.
    zlink_pollitem_t no_partial = {dealer, 0, ZLINK_POLLIN, 0};
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poll (&no_partial, 1, 20, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_INT (0, no_partial.revents);

    receive_writable_completion (poller, router, &poller_tag, fill_id,
                                 &fill_context, &target_rid);
    receive_writable_completion (poller, router, &poller_tag, rejected_id,
                                 &rejected_context, &target_rid);

    zlink_msg_t retry_more;
    init_part (&retry_more, logical_prefix);
    more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &target_rid, &retry_more,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, NULL,
                           &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&retry_more);

    zlink_msg_t retry_final;
    init_part (&retry_final, logical_final);
    zlink_completion_id_t retry_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &target_rid, &retry_final,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &retry_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retry_id);
    assert_part_consumed (&retry_final);

    receive_pair_multipart (dealer, logical_prefix, logical_final);
    assert_no_completion (router);

    zlink_pollitem_t no_duplicate = {dealer, 0, ZLINK_POLLIN, 0};
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poll (&no_duplicate, 1, 20, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_INT (0, no_duplicate.revents);

    destroy_writable_completion_poller (&poller, router);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_writable_completion_is_scoped_to_the_drained_rid ()
{
    const char *const endpoint = "inproc://phase3-router-selective-writable";
    const char *const dealer_names[2] = {"phase3-selective-a",
                                         "phase3-selective-b"};
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealers[2] = {test_context_socket (ZLINK_SOCKET_DEALER),
                        test_context_socket (ZLINK_SOCKET_DEALER)};
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealers[0]);
    TEST_ASSERT_NOT_NULL (dealers[1]);

    const int zero_linger = 0;
    const int mandatory = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY,
                               &mandatory, sizeof (mandatory)));
    configure_small_pair_hwm (router);
    for (size_t i = 0; i != 2; ++i) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealers[i], ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_routing_id (dealers[i], dealer_names[i],
                                strlen (dealer_names[i])));
        configure_small_pair_hwm (dealers[i]);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    for (size_t i = 0; i != 2; ++i) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (dealers[i], endpoint));

        const std::string prime = std::string ("selective-prime-")
                                  + static_cast<char> ('a' + i);
        zlink_msg_t part;
        init_part (&part, prime);
        zlink_completion_id_t completion_id = UINT64_MAX;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealers[i], &part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        assert_part_consumed (&part);
        receive_router_data_eventually (router, prime.c_str ());
    }

    const zlink_routing_id_t target_rids[2] = {
      make_text_rid (dealer_names[0]), make_text_rid (dealer_names[1])};
    int poller_tag = 71;
    void *poller = make_writable_completion_poller (router, &poller_tag);
    int contexts[2] = {72, 73};
    zlink_completion_id_t completion_ids[2] = {0, 0};
    const size_t accepted_a = fill_routed_until_backpressured (
      router, &target_rids[0], &contexts[0], &completion_ids[0]);
    const size_t accepted_b = fill_routed_until_backpressured (
      router, &target_rids[1], &contexts[1], &completion_ids[1]);
    TEST_ASSERT_TRUE (accepted_a != 0);
    TEST_ASSERT_TRUE (accepted_b != 0);
    TEST_ASSERT_NOT_EQUAL (completion_ids[0], completion_ids[1]);
    assert_no_completion (router);
    assert_writable_poller_quiet (poller);

    const std::string filler (kPayloadSize, 'r');
    for (size_t i = 0; i != accepted_a; ++i)
        receive_one_pair_part (dealers[0], filler.c_str ());

    receive_writable_completion (poller, router, &poller_tag,
                                 completion_ids[0], &contexts[0],
                                 &target_rids[0]);
    // Credit returned only on RID A. RID B's token must remain pending even
    // though the ROUTER is now generically writable through RID A.
    assert_no_completion (router);

    zlink_msg_t retry;
    init_part (&retry, "selective-retry-a");
    zlink_completion_id_t retry_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &target_rids[0], &retry,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &retry_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retry_id);
    assert_part_consumed (&retry);
    receive_one_pair_part (dealers[0], "selective-retry-a");
    assert_no_completion (router);

    // Explicitly removing RID B retires its wait token as a TERMINAL
    // WRITABLE record so a waiter parked on that token is woken with ENOENT
    // instead of surviving until socket close.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect_rid (router, &target_rids[1]));
    {
        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poller_wait (poller, &event, 1, 5000, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);

        zlink_completion_t completion;
        init_empty_completion (&completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (router, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_ids[1],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[1], completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, completion.send_result);
        TEST_ASSERT_EQUAL_INT (ENOENT, completion.send_terminal_errno);
        TEST_ASSERT_EQUAL_UINT (target_rids[1].size, completion.peer_rid.size);
        TEST_ASSERT_EQUAL_MEMORY (target_rids[1].data,
                                  completion.peer_rid.data,
                                  target_rids[1].size);
        zlink_completion_close (&completion);
    }
    assert_no_completion (router);

    destroy_writable_completion_poller (&poller, router);
    test_context_socket_close_zero_linger (dealers[0]);
    test_context_socket_close_zero_linger (dealers[1]);
    test_context_socket_close_zero_linger (router);
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

    TEST_ASSERT_TRUE (
      fill_routed_until_backpressured (client, &target_rid) != 0);

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
      test_socket_close_terminalizes_and_reclaims_wait_tokens);
    RUN_PHASE3_COMPLETION_TEST (
      test_completion_recv_rejects_dirty_zero_size_routing_id);
    RUN_PHASE3_COMPLETION_TEST (
      test_completion_recv_timeout_and_context_termination_keep_output_empty);
    RUN_PHASE3_COMPLETION_TEST (
      test_blocking_completion_recv_wakes_as_socket_shutdown);
    RUN_PHASE3_COMPLETION_TEST (
      test_blocking_none_send_distinguishes_socket_and_context_shutdown);
    RUN_PHASE3_COMPLETION_TEST (
      test_dontwait_connect_before_bind_is_not_retained_and_can_be_retried);
    RUN_PHASE3_COMPLETION_TEST (
      test_dealer_writable_completion_uses_any_open_candidate);
    RUN_PHASE3_COMPLETION_TEST (
      test_dontwait_hwm_is_immediate_atomic_and_pending_options_do_not_apply);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_directed_multipart_hwm_rollback_pollout_retry);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_writable_completion_is_scoped_to_the_drained_rid);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_none_wait_keeps_logical_rid_across_physical_reconnect);
    RUN_PHASE3_COMPLETION_TEST (
      test_router_none_wait_explicit_rid_removal_is_synchronous_not_found);
#undef RUN_PHASE3_COMPLETION_TEST
    return UNITY_END ();
}
