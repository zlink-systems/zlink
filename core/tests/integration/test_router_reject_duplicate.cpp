/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace { std::vector<void *> monitors; void *poller = NULL; }

void setUp () { setup_test_context (); }
void tearDown ()
{
    if (poller)
        (void) zlink_poller_destroy (&poller);
    for (size_t i = 0; i != monitors.size (); ++i)
        (void) zlink_close (monitors[i]);
    monitors.clear ();
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int wait_ms = 2000;
const int rejected_request_limit_ms = 100;

void *new_socket (zlink_socket_type_t type_)
{
    void *socket = test_context_socket (type_);
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
      socket, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
      socket, ZLINK_OPT_RCVTIMEO, &wait_ms, sizeof wait_ms));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_routing_id (
      socket, type_ == ZLINK_SOCKET_ROUTER ? "server" : "same-client",
      type_ == ZLINK_SOCKET_ROUTER ? 6 : 11));
    return socket;
}

void *open_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED
                     | ZLINK_EVENT_CLOSED | ZLINK_EVENT_SEND_FLOW_PAUSED;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    monitors.push_back (monitor);
    return monitor;
}

zlink_monitor_event_t wait_event (void *monitor_, uint64_t event_,
                                  uint64_t connection_ = 0)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (wait_ms);
    while (clock_type::now () < deadline) {
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (poller) {
                zlink_poller_event_t progress = {};
                zlink_config_result_t error = ZLINK_CONFIG_OK;
                TEST_ASSERT_TRUE (zlink_poller_wait (
                  poller, &progress, 1, 0, &error) >= 0);
                TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
            } else
                std::this_thread::yield ();
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event == event_
            && (!connection_ || event.connection_id == connection_)
            && (event_ != ZLINK_EVENT_CONNECTION_READY
                || (event.flags
                    & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)))
            return event;
    }
    TEST_FAIL_MESSAGE ("expected endpoint monitor transition did not arrive");
    return zlink_monitor_event_t ();
}

void bind_server (void *server_, bool tcp_, char *endpoint_)
{
    if (tcp_)
        bind_loopback_ipv4 (server_, endpoint_, MAX_SOCKET_STRING);
    else {
        strcpy (endpoint_, "inproc://reject-duplicate");
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server_, endpoint_));
    }
}

zlink_completion_id_t submit_request (void *dealer_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (0, zlink_msg_init_size (&part, 4));
    memcpy (zlink_msg_data (&part), "ping", 4);
    zlink_completion_id_t id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer_, NULL, &part, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 1000, dealer_, &id));
    TEST_ASSERT_EQUAL_INT (0, zlink_msg_close (&part));
    TEST_ASSERT_NOT_EQUAL (0, id);
    return id;
}

void receive_and_reply (void *server_)
{
    zlink_msg_t received;
    TEST_ASSERT_EQUAL_INT (0, zlink_msg_init (&received));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, zlink_router_recv_part (
        server_, &source, &token, &received, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_EQUAL (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_INT (4, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&received), 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_reply_part (
        server_, source, token, &received, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (0, zlink_msg_close (&received));
}

void expect_completion (void *dealer_, zlink_completion_id_t id_,
                        zlink_request_result_t result_, int limit_ms_)
{
    zlink_completion_t completion = {};
    completion.struct_size = sizeof completion;
    const clock_type::time_point started = clock_type::now ();
    const clock_type::time_point deadline =
      started + std::chrono::milliseconds (limit_ms_);
    zlink_recv_result_t rc = ZLINK_RECV_NO_DATA;
    while (rc == ZLINK_RECV_NO_DATA && clock_type::now () < deadline) {
        rc = zlink_completion_recv (
          dealer_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (poller) {
                zlink_poller_event_t event = {};
                zlink_config_result_t error = ZLINK_CONFIG_OK;
                const long remaining = static_cast<long> (
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    deadline - clock_type::now ()).count ());
                TEST_ASSERT_TRUE (zlink_poller_wait (
                  poller, &event, 1, remaining > 0 ? remaining : 0, &error) >= 0);
                TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
            } else
                std::this_thread::yield ();
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      ZLINK_RECV_OK, rc, "request did not complete within its terminal bound");
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (result_, completion.request_result);
    TEST_ASSERT_EQUAL_PTR (dealer_, completion.user_context);
    if (result_ == ZLINK_REQUEST_NOT_CONNECTED) {
        TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    }
    zlink_completion_close (&completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_completion_recv (
      dealer_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
}

void expect_no_completion (void *socket_, int duration_ms_)
{
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (
      poller, &event, 1, duration_ms_, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    zlink_completion_t completion = {};
    completion.struct_size = sizeof completion;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_completion_recv (
      socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
}

void run_duplicate_close (bool tcp_)
{
    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    void *server_monitor = open_monitor (server);
    char endpoint[MAX_SOCKET_STRING];
    bind_server (server, tcp_, endpoint);
    void *first = new_socket (ZLINK_SOCKET_DEALER);
    void *first_monitor = open_monitor (first);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (first, endpoint));
    wait_event (first_monitor, ZLINK_EVENT_CONNECTION_READY);
    const zlink_monitor_event_t admitted =
      wait_event (server_monitor, ZLINK_EVENT_CONNECTION_READY);
    const zlink_completion_id_t first_id = submit_request (first);
    receive_and_reply (server);
    expect_completion (first, first_id, ZLINK_REQUEST_OK, wait_ms);

    void *duplicate = new_socket (ZLINK_SOCKET_DEALER);
    void *duplicate_monitor = open_monitor (duplicate);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (duplicate, endpoint));
    const zlink_monitor_event_t rejected =
      wait_event (duplicate_monitor, ZLINK_EVENT_DISCONNECTED);
    TEST_ASSERT_NOT_EQUAL (0, rejected.connection_id);

    // A rejected duplicate must leave the existing RID owner usable.
    const zlink_completion_id_t retained_id = submit_request (first);
    receive_and_reply (server);
    expect_completion (first, retained_id, ZLINK_REQUEST_OK, wait_ms);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (first, endpoint));
    wait_event (first_monitor, ZLINK_EVENT_DISCONNECTED);
    wait_event (server_monitor, ZLINK_EVENT_DISCONNECTED, admitted.connection_id);
    // The server READY edge proves RID admission of the connector's automatic
    // next attempt. No extra connect call is needed on the duplicate socket.
    wait_event (server_monitor, ZLINK_EVENT_CONNECTION_READY);
    const zlink_completion_id_t recovered_id = submit_request (duplicate);
    receive_and_reply (server);
    expect_completion (duplicate, recovered_id, ZLINK_REQUEST_OK, wait_ms);

    test_context_socket_close_zero_linger (duplicate);
    test_context_socket_close_zero_linger (first);
    test_context_socket_close_zero_linger (server);
}

void run_disconnect_reconnect (bool tcp_)
{
    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_server (server, tcp_, endpoint);
    void *dealer = new_socket (ZLINK_SOCKET_DEALER);
    void *monitor = open_monitor (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    const zlink_monitor_event_t initial =
      wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
    const zlink_completion_id_t first_id = submit_request (dealer);
    receive_and_reply (server);
    expect_completion (dealer, first_id, ZLINK_REQUEST_OK, wait_ms);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (dealer, endpoint));
    wait_event (monitor, ZLINK_EVENT_DISCONNECTED, initial.connection_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
    const clock_type::time_point started = clock_type::now ();
    const zlink_completion_id_t next_id = submit_request (dealer);
    poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_poller_add (poller, server, server, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, dealer, ZLINK_POLLCOMPLETION));
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      1,
      zlink_poller_wait (poller, &event, 1, rejected_request_limit_ms, &error),
      "post-READY request was neither received nor completed within 100 ms");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    if (event.socket == server) {
        receive_and_reply (server);
        expect_completion (dealer, next_id, ZLINK_REQUEST_OK, wait_ms);
    } else {
        expect_completion (dealer, next_id, ZLINK_REQUEST_NOT_CONNECTED, 1);
        const int elapsed = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            clock_type::now () - started).count ());
        printf ("rejected_request transport=%s completion_ms=%d\n",
                tcp_ ? "tcp" : "inproc", elapsed);
        TEST_ASSERT_LESS_OR_EQUAL_INT (rejected_request_limit_ms, elapsed);
        wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
        const zlink_completion_id_t recovered_id = submit_request (dealer);
        receive_and_reply (server);
        expect_completion (dealer, recovered_id, ZLINK_REQUEST_OK, wait_ms);
    }

    expect_no_completion (dealer, 1000);
    if (poller)
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (server);
}

void run_transient_disconnect (bool tcp_)
{
    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_server (server, tcp_, endpoint);
    void *dealer = new_socket (ZLINK_SOCKET_DEALER);
    void *monitor = open_monitor (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
    const zlink_completion_id_t pending = submit_request (dealer);

    zlink_msg_t request;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&request));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv_part (
      server, &source, &token, &request, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_EQUAL (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));

    // A public flow transition pins both wait tokens before the disconnect.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (server, ZLINK_RECEIVE_FLOW_PAUSED));
    wait_event (monitor, ZLINK_EVENT_SEND_FLOW_PAUSED);
    zlink_completion_id_t writable_ids[2] = {};
    for (int i = 0; i != 2; ++i) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&request, 4));
        memcpy (zlink_msg_data (&request), "ping", 4);
        const zlink_submit_result_t result = i == 0
          ? zlink_send_part (dealer, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                             ZLINK_PART_FINAL, dealer, &writable_ids[i])
          : zlink_request_part (dealer, NULL, &request,
                                ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                                1000, dealer, &writable_ids[i]);
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        TEST_ASSERT_NOT_EQUAL (0, writable_ids[i]);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));
    }
    TEST_ASSERT_NOT_EQUAL (writable_ids[0], writable_ids[1]);
    // The request terminal must not wait for the application's DATA drain.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&request, 4));
    memcpy (zlink_msg_data (&request), "data", 4);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_send_part_rid (
      server, source, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
      NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));
    zlink_pollitem_t readable = {dealer, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&readable, 1, wait_ms, NULL));

    poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (
      poller, dealer, dealer, ZLINK_POLLCOMPLETION));

    const clock_type::time_point started = clock_type::now ();
    test_context_socket_close_zero_linger (server);
    expect_completion (dealer, pending, ZLINK_REQUEST_NOT_CONNECTED,
                       rejected_request_limit_ms);
    const int elapsed = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - started).count ());
    TEST_ASSERT_LESS_OR_EQUAL_INT (rejected_request_limit_ms, elapsed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_recv_part (
      dealer, NULL, &request, &more, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (4, zlink_msg_size (&request));
    TEST_ASSERT_EQUAL_MEMORY ("data", zlink_msg_data (&request), 4);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_recv_part (
      dealer, NULL, &request, &more, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&request));
    wait_event (monitor, ZLINK_EVENT_DISCONNECTED);
    expect_no_completion (dealer, 0);

    server = new_socket (ZLINK_SOCKET_ROUTER);
    void *server_monitor = open_monitor (server);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    wait_event (server_monitor, ZLINK_EVENT_CONNECTION_READY);
    bool seen[2] = {};
    for (int i = 0; i != 2; ++i) {
        zlink_completion_t completion = {};
        completion.struct_size = sizeof completion;
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (
          poller, &event, 1, wait_ms, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
          dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
        const int index = completion.completion_id == writable_ids[0] ? 0 : 1;
        TEST_ASSERT_EQUAL_UINT64 (writable_ids[index], completion.completion_id);
        TEST_ASSERT_FALSE (seen[index]);
        seen[index] = true;
        TEST_ASSERT_EQUAL_PTR (dealer, completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
        zlink_completion_close (&completion);
    }
    const zlink_completion_id_t recovered = submit_request (dealer);
    receive_and_reply (server);
    expect_completion (dealer, recovered, ZLINK_REQUEST_OK, wait_ms);
    // Advance past the original request deadline through the same public
    // poller: neither its timer nor reconnection can complete it twice.
    expect_no_completion (dealer, 1000);
    printf ("transient_request transport=%s completion_ms=%d writable=2 duplicate=0\n",
            tcp_ ? "tcp" : "inproc", elapsed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (server);
}

void test_rejected_pending_request_inproc ()
{
    // With no I/O threads, the public server poller owns command progress.
    // This makes submission precede its duplicate-admission decision.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 0));
    void *server = new_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_server (server, false, endpoint);
    void *first = new_socket (ZLINK_SOCKET_DEALER);
    poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (
      poller, server, server, ZLINK_POLLIN | ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (
      poller, first, first, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (first, endpoint));
    const zlink_completion_id_t first_id = submit_request (first);
    receive_and_reply (server);
    expect_completion (first, first_id, ZLINK_REQUEST_OK, wait_ms);

    // A server-side wait token follows the logical RID across physical close.
    // Its recovery proves ROUTER admission, unlike the connector's READY edge.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_set_receive_flow_state (
      first, ZLINK_RECEIVE_FLOW_PAUSED));
    zlink_routing_id_t target = {};
    target.size = 11;
    memcpy (target.data, "same-client", target.size);
    zlink_msg_t blocked;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&blocked, 4));
    memcpy (zlink_msg_data (&blocked), "data", 4);
    zlink_completion_id_t writable_id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, zlink_send_part_rid (
      server, &target, &blocked, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      server, &writable_id));
    TEST_ASSERT_NOT_EQUAL (0, writable_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&blocked));

    void *duplicate = new_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (duplicate, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add (
      poller, duplicate, duplicate, ZLINK_POLLCOMPLETION));
    const zlink_completion_id_t rejected_id = submit_request (duplicate);
    const clock_type::time_point started = clock_type::now ();
    expect_completion (duplicate, rejected_id, ZLINK_REQUEST_NOT_CONNECTED,
                       rejected_request_limit_ms);
    const int elapsed = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - started).count ());

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (first, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, first));
    test_context_socket_close_zero_linger (first);
    zlink_poller_event_t ready = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (
      poller, &ready, 1, wait_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_PTR (server, ready.socket);
    zlink_completion_t writable = {};
    writable.struct_size = sizeof writable;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
      server, &writable, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, writable.kind);
    TEST_ASSERT_EQUAL_UINT64 (writable_id, writable.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, writable.send_result);
    TEST_ASSERT_EQUAL_INT (0, writable.send_terminal_errno);
    TEST_ASSERT_EQUAL_PTR (server, writable.user_context);
    TEST_ASSERT_EQUAL_MEMORY (&target, &writable.peer_rid, sizeof target);
    zlink_completion_close (&writable);

    const zlink_completion_id_t recovered_id = submit_request (duplicate);
    receive_and_reply (server);
    expect_completion (duplicate, recovered_id, ZLINK_REQUEST_OK, wait_ms);
    expect_no_completion (duplicate, 1000);
    printf ("rejected_pending transport=inproc completion_ms=%d "
            "old_terminated=1 next_admitted=1 request_ok=1 duplicate=0\n",
            elapsed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (duplicate);
    test_context_socket_close_zero_linger (server);
}

void test_transient_request_tcp () { run_transient_disconnect (true); }
void test_transient_request_inproc () { run_transient_disconnect (false); }

void test_reject_duplicate_close_tcp () { run_duplicate_close (true); }
void test_reject_duplicate_close_inproc () { run_duplicate_close (false); }
void test_reject_disconnect_reconnect_tcp () { run_disconnect_reconnect (true); }
void test_reject_disconnect_reconnect_inproc () { run_disconnect_reconnect (false); }
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define RUN_SELECTED(test_) \
    do { \
        const char *selected = getenv ("ZLINK_TEST_CASE"); \
        if (!selected || !*selected || strcmp (selected, #test_) == 0) \
            RUN_TEST (test_); \
    } while (false)
    RUN_SELECTED (test_rejected_pending_request_inproc);
    RUN_SELECTED (test_transient_request_tcp);
    RUN_SELECTED (test_transient_request_inproc);
    RUN_SELECTED (test_reject_duplicate_close_tcp);
    RUN_SELECTED (test_reject_duplicate_close_inproc);
    RUN_SELECTED (test_reject_disconnect_reconnect_tcp);
    RUN_SELECTED (test_reject_disconnect_reconnect_inproc);
#undef RUN_SELECTED
    return UNITY_END ();
}
