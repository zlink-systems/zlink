/* SPDX-License-Identifier: MPL-2.0 */

// Exercise first-message admission and credit wakeups through the public C API.
#include "testutil.hpp"
#include "testutil_unity.hpp"

namespace
{
void *sender = NULL;
void *receiver = NULL;
void *sender_monitor = NULL;
void *receiver_monitor = NULL;
void *completion_poller = NULL;
const int wait_ms = 1000;
const char payload[] = "first-data-after-ready";

void configure_socket (void *socket_, const char *rid_)
{
    const uint64_t hwm = 1;
    const int linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &wait_ms, sizeof (wait_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (socket_, rid_, strlen (rid_)));
}

void *open_ready_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

void expect_ready (void *monitor_)
{
    // Opening a monitor after admission does not replay the READY edge.
    zlink_monitor_status_t status = {};
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor_, &status));
    if ((status.state_flags & ZLINK_MONITOR_STATE_READY) != 0)
        return;
    zlink_socket_monitor_event_t event = {};
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_socket_monitor_recv (monitor_, &event, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_CONNECTION_READY, event.event);
    TEST_ASSERT_EQUAL_UINT64 (1, event.value);
}

void expect_no_data ()
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (receiver, &rid, &token, &part, &more,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

zlink_submit_result_t send_data (const zlink_routing_id_t *target_,
                                 zlink_completion_id_t *id_, void *context_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (&part, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&part), payload, sizeof (payload) - 1);
    const zlink_submit_result_t result =
      target_ ? zlink_send_part_rid (
                  sender, target_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                  ZLINK_PART_FINAL, context_, id_)
              : zlink_send_part (
                  sender, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                  ZLINK_PART_FINAL, context_, id_);
    const int send_errno = zlink_errno ();
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    if (result == ZLINK_SUBMIT_BACKPRESSURED) {
        TEST_ASSERT_EQUAL_INT (EAGAIN, send_errno);
        TEST_ASSERT_NOT_EQUAL (0, *id_);
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        TEST_ASSERT_EQUAL_UINT64 (0, *id_);
    }
    return result;
}

void expect_no_completion ()
{
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);
}

void expect_writable (zlink_completion_id_t id_, void *context_,
                      const zlink_routing_id_t *target_)
{
    completion_poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (completion_poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (completion_poller, sender, NULL, ZLINK_POLLCOMPLETION));
    zlink_poller_event_t event = {};
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (completion_poller, &event, 1, wait_ms, NULL));
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLCOMPLETION, event.events);
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (sender, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    TEST_ASSERT_EQUAL_UINT (target_ ? target_->size : 0, completion.peer_rid.size);
    if (target_)
        TEST_ASSERT_EQUAL_MEMORY (target_->data, completion.peer_rid.data,
                                  target_->size);
    zlink_completion_close (&completion);
    expect_no_completion ();
}

void run_first_data (int sender_type_, bool inproc_, bool reader_first_)
{
    sender = test_context_socket (sender_type_);
    receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (sender, "d118-sender-0001");
    configure_socket (receiver, "d118-reader-0001");
    zlink_routing_id_t target = {};
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (receiver, &target));

    char endpoint[MAX_SOCKET_STRING] = "inproc://ready-empty-pipe-first-data";
    if (inproc_ && reader_first_) {
        // With no sender monitor/command owner yet, connect-before-bind lets
        // the bound ROUTER consume the preamble before the sender processes
        // its queued pair admission. Hold release must sample that credit.
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender, endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver, endpoint));
        expect_no_data ();
    } else {
        // Reversing the endpoint roles leaves the inproc ROUTER connector's
        // preamble unread: it adopts the peer RID from connection metadata.
        // Its first public receive below must return credit to a held writer
        // even when that writer has already released its transport hold.
        void *bound = reader_first_ ? receiver : sender;
        void *connected = reader_first_ ? sender : receiver;
        if (inproc_)
            TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint));
        else
            bind_loopback_ipv4 (bound, endpoint, sizeof (endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (connected, endpoint));
    }

    if (reader_first_) {
        receiver_monitor = open_ready_monitor (receiver);
        expect_ready (receiver_monitor);
        sender_monitor = open_ready_monitor (sender);
        expect_ready (sender_monitor);
    } else {
        sender_monitor = open_ready_monitor (sender);
        expect_ready (sender_monitor);
        receiver_monitor = open_ready_monitor (receiver);
        expect_ready (receiver_monitor);
    }

    const zlink_routing_id_t *target_ptr =
      sender_type_ == ZLINK_SOCKET_ROUTER ? &target : NULL;
    int context = 118;
    zlink_completion_id_t id = UINT64_MAX;
    if (send_data (target_ptr, &id, &context) == ZLINK_SUBMIT_BACKPRESSURED) {
        // A rejected send consumed its payload. Only the preamble can be read
        // here; its credit must wake the original token without another send.
        expect_no_data ();
        expect_writable (id, &context, target_ptr);
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                               send_data (target_ptr, &id, &context));
    }

    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (receiver, &source, &token, &part, &more,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    zlink_routing_id_t sender_rid = {};
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (sender, &sender_rid));
    TEST_ASSERT_EQUAL_UINT (sender_rid.size, source->size);
    TEST_ASSERT_EQUAL_MEMORY (sender_rid.data, source->data, source->size);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&part), sizeof (payload) - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    expect_no_data ();
    expect_no_completion ();
    if (completion_poller) {
        zlink_poller_event_t event = {};
        TEST_ASSERT_EQUAL_INT (
          0, zlink_poller_wait (completion_poller, &event, 1, 0, NULL));
    }
}

#define FIRST_DATA_CASE(name, type, inproc, reader_first)                      \
    void name () { run_first_data (type, inproc, reader_first); }

FIRST_DATA_CASE (test_inproc_dealer_router_reader_first, ZLINK_SOCKET_DEALER,
                 true, true)
FIRST_DATA_CASE (test_inproc_dealer_router_writer_first, ZLINK_SOCKET_DEALER,
                 true, false)
FIRST_DATA_CASE (test_inproc_router_router_reader_first, ZLINK_SOCKET_ROUTER,
                 true, true)
FIRST_DATA_CASE (test_inproc_router_router_writer_first, ZLINK_SOCKET_ROUTER,
                 true, false)
FIRST_DATA_CASE (test_tcp_dealer_router_reader_first, ZLINK_SOCKET_DEALER,
                 false, true)
FIRST_DATA_CASE (test_tcp_dealer_router_writer_first, ZLINK_SOCKET_DEALER,
                 false, false)
FIRST_DATA_CASE (test_tcp_router_router_reader_first, ZLINK_SOCKET_ROUTER,
                 false, true)
FIRST_DATA_CASE (test_tcp_router_router_writer_first, ZLINK_SOCKET_ROUTER,
                 false, false)
#undef FIRST_DATA_CASE
}

void setUp ()
{
    setup_test_context ();
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_set (get_test_context (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0));
}

void tearDown ()
{
    if (completion_poller)
        zlink_poller_destroy (&completion_poller);
    if (sender_monitor)
        zlink_monitor_close (&sender_monitor);
    if (receiver_monitor)
        zlink_monitor_close (&receiver_monitor);
    if (sender)
        sender = test_context_socket_close_zero_linger (sender);
    if (receiver)
        receiver = test_context_socket_close_zero_linger (receiver);
    teardown_test_context ();
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_inproc_dealer_router_reader_first);
    RUN_TEST (test_inproc_dealer_router_writer_first);
    RUN_TEST (test_inproc_router_router_reader_first);
    RUN_TEST (test_inproc_router_router_writer_first);
    RUN_TEST (test_tcp_dealer_router_reader_first);
    RUN_TEST (test_tcp_dealer_router_writer_first);
    RUN_TEST (test_tcp_router_router_reader_first);
    RUN_TEST (test_tcp_router_router_writer_first);
    return UNITY_END ();
}
