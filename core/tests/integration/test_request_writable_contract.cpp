/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wait_ms = 5000;
const size_t payload_size = 64;
const size_t max_fill_attempts = 512;

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

void assert_part_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

void init_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

void set_zero_linger (void *socket_)
{
    const int linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    const int receive_timeout = wait_ms;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &receive_timeout,
                        sizeof (receive_timeout)));
}

void set_small_hwm (void *socket_)
{
    const uint64_t hwm =
      4u * (payload_size + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
}

zlink_routing_id_t make_rid (const char *text_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (text_);
    TEST_ASSERT_TRUE (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    memcpy (rid.data, text_, size);
    return rid;
}

struct received_request_t
{
    zlink_routing_id_t source;
    zlink_reply_token_t token;
    std::string payload;
};

received_request_t receive_request (void *router_)
{
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source, &token, &part, &flag,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
    received_request_t received;
    received.source = *source;
    received.token = token;
    received.payload.assign (
      static_cast<const char *> (zlink_msg_data (&part)),
      zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return received;
}

void reply (void *router_, const received_request_t &request_)
{
    zlink_msg_t part;
    init_part (&part, "reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, &request_.source, request_.token, &part,
                        ZLINK_PART_FINAL));
    assert_part_consumed (&part);
}

void assert_no_completion (void *socket_)
{
    zlink_completion_t completion;
    init_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);
}

zlink_completion_t receive_completion (void *socket_)
{
    zlink_completion_t completion;
    init_completion (&completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_NONE));
    return completion;
}

void assert_writable_poll (void *socket_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (
        poller, socket_, NULL,
        static_cast<short> (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)));
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, wait_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, event.events);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, socket_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}

void assert_writable_completion (
  zlink_completion_t *completion_, zlink_completion_id_t id_, void *context_,
  const zlink_routing_id_t *rid_ = NULL)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion_->kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, completion_->completion_id);
    TEST_ASSERT_EQUAL_PTR (context_, completion_->user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion_->send_result);
    TEST_ASSERT_EQUAL_INT (0, completion_->send_terminal_errno);
    if (rid_) {
        TEST_ASSERT_EQUAL_UINT (rid_->size, completion_->peer_rid.size);
        TEST_ASSERT_EQUAL_MEMORY (rid_->data, completion_->peer_rid.data,
                                  rid_->size);
    }
    zlink_completion_close (completion_);
}

void assert_request_completion (void *socket_, zlink_completion_id_t id_,
                                void *context_)
{
    zlink_completion_t completion = receive_completion (socket_);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    zlink_completion_close (&completion);
}

void prime_route (void *sender_, void *receiver_,
                  const zlink_routing_id_t *target_)
{
    zlink_msg_t part;
    init_part (&part, "prime");
    zlink_completion_id_t id = UINT64_MAX;
    const zlink_submit_result_t result =
      target_ ? zlink_send_part_rid (
                  sender_, target_, &part, ZLINK_SEND_FLAGS_NONE,
                  ZLINK_PART_FINAL, NULL, &id)
              : zlink_send_part (
                  sender_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                  NULL, &id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    assert_part_consumed (&part);
    const received_request_t received = receive_request (receiver_);
    TEST_ASSERT_EQUAL_UINT64 (0, received.token);
    TEST_ASSERT_EQUAL_STRING ("prime", received.payload.c_str ());
}

size_t fill_requests (
  void *sender_, const zlink_routing_id_t *target_, void *wait_context_,
  zlink_completion_id_t *wait_id_out_)
{
    const std::string payload (payload_size, 'f');
    size_t accepted = 0;
    for (; accepted != max_fill_attempts; ++accepted) {
        zlink_msg_t part;
        init_part (&part, payload);
        zlink_completion_id_t id = 0;
        errno = 0;
        const zlink_submit_result_t result = zlink_request_part (
          sender_, target_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, 120000, wait_context_, &id);
        assert_part_consumed (&part);
        if (result == ZLINK_SUBMIT_OK) {
            TEST_ASSERT_NOT_EQUAL (0, id);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_NOT_EQUAL (0, id);
        *wait_id_out_ = id;
        return accepted;
    }
    TEST_FAIL_MESSAGE ("REQUEST did not reach physical HWM");
    return accepted;
}

size_t drain_until_writable (void *sender_, void *receiver_, size_t accepted_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (
        poller, sender_, NULL,
        static_cast<short> (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)));
    size_t drained = 0;
    while (drained != accepted_) {
        const received_request_t received = receive_request (receiver_);
        TEST_ASSERT_NOT_EQUAL (0, received.token);
        ++drained;
        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        const int count =
          zlink_poller_wait (poller, &event, 1, 100, &error);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        if (count == 1) {
            TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, event.events);
            TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK, zlink_poller_remove (poller, sender_));
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
            return drained;
        }
    }
    TEST_FAIL_MESSAGE ("draining admitted REQUESTs did not publish WRITABLE");
    return drained;
}

void run_hwm_request_retry (
  void *sender_, void *receiver_, const zlink_routing_id_t *target_)
{
    int wait_context = 11;
    zlink_completion_id_t wait_id = 0;
    const size_t accepted =
      fill_requests (sender_, target_, &wait_context, &wait_id);
    TEST_ASSERT_TRUE (accepted > 0);
    assert_no_completion (sender_);

    const size_t drained = drain_until_writable (sender_, receiver_, accepted);
    zlink_completion_t writable = receive_completion (sender_);
    assert_writable_completion (
      &writable, wait_id, &wait_context, target_);

    int request_context = 12;
    zlink_msg_t retry;
    init_part (&retry, "retry-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (sender_, target_, &retry,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    TEST_ASSERT_NOT_EQUAL (wait_id, request_id);
    assert_part_consumed (&retry);

    const size_t remaining_fillers = accepted - drained;
    for (size_t i = 0; i != remaining_fillers; ++i) {
        const received_request_t filler = receive_request (receiver_);
        TEST_ASSERT_NOT_EQUAL (0, filler.token);
        TEST_ASSERT_EQUAL_UINT64 (payload_size, filler.payload.size ());
    }
    const received_request_t request = receive_request (receiver_);
    TEST_ASSERT_EQUAL_STRING ("retry-request", request.payload.c_str ());
    reply (receiver_, request);
    assert_request_completion (sender_, request_id, &request_context);
}

void test_dealer_router_hwm_request_uses_writable_retry ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_zero_linger (router);
    set_zero_linger (dealer);
    set_small_hwm (router);
    set_small_hwm (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://request-writable-dealer-router"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://request-writable-dealer-router"));
    prime_route (dealer, router, NULL);
    run_hwm_request_retry (dealer, router, NULL);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_router_hwm_request_preserves_rid ()
{
    const char *const server_id = "request-writable-server";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    set_zero_linger (server);
    set_zero_linger (client);
    set_small_hwm (server);
    set_small_hwm (client);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (server, server_id, strlen (server_id)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               server_id, strlen (server_id)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (server, "inproc://request-writable-router-router"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (client, "inproc://request-writable-router-router"));
    const zlink_routing_id_t target = make_rid (server_id);
    prime_route (client, server, &target);
    run_hwm_request_retry (client, server, &target);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_connect_before_bind_and_mixed_tokens_are_independent ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);
    set_zero_linger (dealer);
    set_zero_linger (router);
    const int immediate = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &immediate,
                        sizeof (immediate)));
    const uint64_t one = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one,
                        sizeof (one)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_BYTES, &one,
                        sizeof (one)));

    char endpoint[MAX_SOCKET_STRING];
    fd_t reserved = bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    close (reserved);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    int request_context = 21;
    zlink_msg_t request;
    init_part (&request, "connect-later-request");
    zlink_completion_id_t request_wait_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1,
                          &request_context, &request_wait_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, request_wait_id);
    assert_part_consumed (&request);

    int send_context = 22;
    zlink_msg_t send;
    init_part (&send, "connect-later-send");
    zlink_completion_id_t send_wait_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (dealer, &send, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, &send_context, &send_wait_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, send_wait_id);
    TEST_ASSERT_NOT_EQUAL (request_wait_id, send_wait_id);
    assert_part_consumed (&send);
    assert_no_completion (dealer);

    // The rejected REQUEST owns only a WRITABLE token. Its one-millisecond
    // reply timeout is not armed before the retry is admitted.
    const int short_receive_timeout = 20;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_RCVTIMEO, &short_receive_timeout,
                        sizeof (short_receive_timeout)));
    zlink_completion_t before_admission;
    init_completion (&before_admission);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (dealer, &before_admission,
                             ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&before_admission);
    const int normal_receive_timeout = wait_ms;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_RCVTIMEO, &normal_receive_timeout,
                        sizeof (normal_receive_timeout)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    assert_writable_poll (dealer);
    bool saw_request = false;
    bool saw_send = false;
    for (size_t i = 0; i != 2; ++i) {
        zlink_completion_t completion = receive_completion (dealer);
        if (completion.completion_id == request_wait_id) {
            assert_writable_completion (
              &completion, request_wait_id, &request_context);
            saw_request = true;
        } else {
            assert_writable_completion (
              &completion, send_wait_id, &send_context);
            saw_send = true;
        }
    }
    TEST_ASSERT_TRUE (saw_request);
    TEST_ASSERT_TRUE (saw_send);

    zlink_msg_t retry;
    init_part (&retry, "connect-later-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &retry,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 40,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&retry);
    const received_request_t received = receive_request (router);
    TEST_ASSERT_EQUAL_STRING ("connect-later-request",
                              received.payload.c_str ());
    reply (router, received);
    assert_request_completion (dealer, request_id, &request_context);

    zlink_msg_t timeout_request;
    init_part (&timeout_request, "timeout-after-admission");
    zlink_completion_id_t timeout_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &timeout_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 20,
                          &request_context, &timeout_id));
    TEST_ASSERT_NOT_EQUAL (0, timeout_id);
    assert_part_consumed (&timeout_request);
    const received_request_t timeout_received = receive_request (router);
    TEST_ASSERT_EQUAL_STRING ("timeout-after-admission",
                              timeout_received.payload.c_str ());
    zlink_completion_t timed_out = receive_completion (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, timed_out.kind);
    TEST_ASSERT_EQUAL_UINT64 (timeout_id, timed_out.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           timed_out.request_result);
    zlink_completion_close (&timed_out);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_wait_token_is_reclaimed_by_close ()
{
    void *context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    set_zero_linger (dealer);
    const int immediate = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &immediate,
                        sizeof (immediate)));
    char endpoint[MAX_SOCKET_STRING];
    fd_t reserved = bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    close (reserved);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    int context_value = 31;
    zlink_msg_t request;
    init_part (&request, "close-wait-token");
    zlink_completion_id_t wait_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1,
                          &context_value, &wait_id));
    TEST_ASSERT_NOT_EQUAL (0, wait_id);
    assert_part_consumed (&request);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_request_wait_token_target_removal_is_terminal ()
{
    const char *const server_id = "request-terminal-server";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    set_zero_linger (server);
    set_zero_linger (client);
    set_small_hwm (server);
    set_small_hwm (client);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (server, server_id, strlen (server_id)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               server_id, strlen (server_id)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (server, "inproc://request-writable-terminal"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (client, "inproc://request-writable-terminal"));
    const zlink_routing_id_t target = make_rid (server_id);

    prime_route (client, server, &target);
    int wait_context = 41;
    zlink_completion_id_t wait_id = 0;
    TEST_ASSERT_TRUE (
      fill_requests (client, &target, &wait_context, &wait_id) > 0);
    TEST_ASSERT_NOT_EQUAL (0, wait_id);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK, zlink_disconnect_rid (client, &target));

    zlink_completion_t terminal = receive_completion (client);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, terminal.kind);
    TEST_ASSERT_EQUAL_UINT64 (wait_id, terminal.completion_id);
    TEST_ASSERT_EQUAL_PTR (&wait_context, terminal.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, terminal.send_result);
    TEST_ASSERT_EQUAL_INT (ENOENT, terminal.send_terminal_errno);
    TEST_ASSERT_EQUAL_UINT (target.size, terminal.peer_rid.size);
    TEST_ASSERT_EQUAL_MEMORY (target.data, terminal.peer_rid.data,
                              target.size);
    zlink_completion_close (&terminal);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_missing_router_route_has_no_wait_token ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const int mandatory = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY,
                               &mandatory, sizeof (mandatory)));
    const zlink_routing_id_t missing = make_rid ("missing-route");
    zlink_msg_t request;
    init_part (&request, "missing");
    zlink_completion_id_t id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_request_part (router, &missing, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 100,
                          NULL, &id));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    assert_part_consumed (&request);
    assert_no_completion (router);
    test_context_socket_close_zero_linger (router);
}
}

int main ()
{
    setup_test_environment (120);
    UNITY_BEGIN ();
    RUN_TEST (test_dealer_router_hwm_request_uses_writable_retry);
    RUN_TEST (test_router_router_hwm_request_preserves_rid);
    RUN_TEST (test_connect_before_bind_and_mixed_tokens_are_independent);
    RUN_TEST (test_request_wait_token_is_reclaimed_by_close);
    RUN_TEST (test_request_wait_token_target_removal_is_terminal);
    RUN_TEST (test_missing_router_route_has_no_wait_token);
    return UNITY_END ();
}
