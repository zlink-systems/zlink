/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
typedef std::chrono::steady_clock clock_type;
const int progress_timeout_ms = 2000;

double elapsed_ms (clock_type::time_point start_)
{
    return std::chrono::duration<double, std::milli> (clock_type::now () - start_)
      .count ();
}

void configure (void *socket_)
{
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &progress_timeout_ms,
                        sizeof (progress_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &progress_timeout_ms,
                        sizeof (progress_timeout_ms)));
}

void *open_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED
                     | ZLINK_EVENT_CLOSED;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

bool next_event (void *monitor_, clock_type::time_point deadline_,
                 zlink_socket_monitor_event_t *event_)
{
    while (clock_type::now () < deadline_) {
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline_ - clock_type::now ()).count ());
        // Only the monitor handle is polled; the DEALER stays idle or inside
        // its one application wait for the entire measured interval.
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        if (zlink_poll (&item, 1, remaining > 0 ? remaining : 1, NULL) != 1)
            return false;
        if (zlink_socket_monitor_recv (monitor_, event_,
                                      ZLINK_RECV_FLAGS_DONTWAIT)
            == ZLINK_RECV_OK)
            return true;
    }
    return false;
}

uint64_t wait_ready (void *monitor_)
{
    const clock_type::time_point deadline = clock_type::now ()
      + std::chrono::milliseconds (progress_timeout_ms);
    zlink_socket_monitor_event_t event = {};
    while (next_event (monitor_, deadline, &event)) {
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))
            return event.connection_id;
    }
    TEST_FAIL_MESSAGE ("initial CONNECTION_READY missing");
    return 0;
}

void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

struct fixture_t
{
    explicit fixture_t (const char *transport_) :
        router (test_context_socket (ZLINK_SOCKET_ROUTER)),
        dealer (test_context_socket (ZLINK_SOCKET_DEALER))
    {
        configure (router);
        configure (dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
          zlink_set_routing_id (dealer, "progress-client", 15));
        const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY,
                            &handover, sizeof (handover)));
        dealer_monitor = open_monitor (dealer);
        router_monitor = open_monitor (router);
        if (strcmp (transport_, "inproc") == 0) {
            strcpy (endpoint, "inproc://disconnect-progress");
            TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
        } else
            test_bind (router, "tcp://127.0.0.1:*", endpoint, sizeof (endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
        old_connection = wait_ready (dealer_monitor);
        old_router_connection = wait_ready (router_monitor);

        zlink_msg_t part;
        init_part (&part, "admitted");
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, NULL));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
          zlink_router_recv_part (router, &source, &token, &part, &more,
                                  ZLINK_RECV_FLAGS_NONE));
        client_rid = *source;
        TEST_ASSERT_EQUAL_UINT64 (0, token);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }

    void close ()
    {
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_monitor_close (&dealer_monitor));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_monitor_close (&router_monitor));
        test_context_socket_close_zero_linger (dealer);
        test_context_socket_close_zero_linger (router);
    }

    void *router;
    void *dealer;
    void *dealer_monitor;
    void *router_monitor;
    char endpoint[MAX_SOCKET_STRING];
    uint64_t old_connection;
    uint64_t old_router_connection;
    zlink_routing_id_t client_rid;
};

enum operation_t { remote_disconnect, explicit_disconnect, reconnect_request };

void run_case (const char *transport_, bool waiting_, bool completion_,
                operation_t operation_)
{
    fixture_t fixture (transport_);
    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.dealer, NULL,
        ZLINK_POLLIN | (completion_ ? ZLINK_POLLCOMPLETION : 0)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add_timer (poller, timer, NULL));
    int wait_result = 0;
    zlink_poller_event_t wait_event = {};
    std::thread waiter;
    bool armed = !waiting_;
    if (waiting_) {
        waiter = std::thread ([&] {
            wait_result = zlink_poller_wait (poller, &wait_event, 1,
                                            progress_timeout_ms * 2, NULL);
        });
        const clock_type::time_point arm_start = clock_type::now ();
        while (elapsed_ms (arm_start) < progress_timeout_ms) {
            zlink_config_result_t error = ZLINK_CONFIG_OK;
            if (zlink_poller_size (poller, &error) == -1
                && error == ZLINK_CONFIG_BUSY) {
                armed = true;
                break;
            }
            std::this_thread::yield ();
        }
    }

    const clock_type::time_point call_start = clock_type::now ();
    const zlink_connect_result_t disconnect_result =
      operation_ == remote_disconnect
        ? zlink_disconnect_rid (fixture.router, &fixture.client_rid)
        : zlink_disconnect (fixture.dealer, fixture.endpoint);
    const clock_type::time_point returned = clock_type::now ();
    zlink_connect_result_t connect_result = ZLINK_CONNECT_OK;
    zlink_submit_result_t submit_result = ZLINK_SUBMIT_OK;
    zlink_completion_id_t request_id = 0;
    if (operation_ == reconnect_request) {
        connect_result = zlink_connect (fixture.dealer, fixture.endpoint);
        zlink_msg_t request;
        init_part (&request, "new-pipe-request");
        submit_result = zlink_request_part (
          fixture.dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
          ZLINK_PART_FINAL, progress_timeout_ms, NULL, &request_id);
        zlink_msg_close (&request);
    }

    double terminal_ms = -1;
    double ready_ms = -1;
    const clock_type::time_point deadline = returned
      + std::chrono::milliseconds (progress_timeout_ms);
    zlink_socket_monitor_event_t event = {};
    while (next_event (fixture.dealer_monitor, deadline, &event)) {
        if (getenv ("ZLINK_PROGRESS_TRACE"))
            printf ("MONITOR event=%llu connection=%llu old=%llu value=%llu flags=%u\n",
                    static_cast<unsigned long long> (event.event),
                    static_cast<unsigned long long> (event.connection_id),
                    static_cast<unsigned long long> (fixture.old_connection),
                    static_cast<unsigned long long> (event.value), event.flags);
        if ((event.event == ZLINK_EVENT_DISCONNECTED
             || event.event == ZLINK_EVENT_CLOSED)
            && event.connection_id == fixture.old_connection)
            terminal_ms = elapsed_ms (returned);
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
            && event.connection_id != fixture.old_connection)
            ready_ms = elapsed_ms (returned);
        if (terminal_ms >= 0
            && (operation_ == explicit_disconnect || ready_ms >= 0))
            break;
    }

    if (waiting_) {
        zlink_timer_start (timer, 1, 1);
        waiter.join ();
    }
    void *const wait_timer = timer;
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_destroy (&timer));

    const char *operation = operation_ == remote_disconnect ? "automatic"
      : operation_ == explicit_disconnect ? "disconnect" : "immediate-request";
    printf ("PROGRESS case=%s transport=%s poll=%s mask=%s call_ms=%.3f "
            "terminal_ms=%.3f ready_ms=%.3f submit=%d\n",
            operation, transport_, waiting_ ? "blocked" : "idle",
            completion_ ? "IN+COMPLETION" : "IN",
            std::chrono::duration<double, std::milli> (returned - call_start).count (),
            terminal_ms, ready_ms, static_cast<int> (submit_result));
    fflush (stdout);

    if (operation_ == reconnect_request && terminal_ms >= 0 && ready_ms >= 0) {
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
        TEST_ASSERT_NOT_EQUAL (0, request_id);
        bool old_router_terminal = false;
        bool new_router_ready = false;
        const clock_type::time_point router_deadline = clock_type::now ()
          + std::chrono::milliseconds (progress_timeout_ms);
        while (next_event (fixture.router_monitor, router_deadline, &event)) {
            if (event.event == ZLINK_EVENT_DISCONNECTED
                && event.connection_id == fixture.old_router_connection)
                old_router_terminal = true;
            if (event.event == ZLINK_EVENT_CONNECTION_READY
                && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                && event.connection_id != fixture.old_router_connection)
                new_router_ready = true;
            if (old_router_terminal && new_router_ready)
                break;
        }
        TEST_ASSERT_TRUE_MESSAGE (old_router_terminal && new_router_ready,
                                  "ROUTER did not retire old pipe and admit new pipe");
        // Receive only after the old ROUTER pipe is terminal. A request
        // admitted to that retiring pipe cannot satisfy this exchange.
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
          zlink_router_recv_part (fixture.router, &source, &token, &part, &more,
                                  ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_UINT (16, zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY ("new-pipe-request", zlink_msg_data (&part), 16);
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
        TEST_ASSERT_NOT_EQUAL (0, token);
        zlink_msg_close (&part);
        init_part (&part, "reply");
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
          zlink_reply_part (fixture.router, source, token, &part, ZLINK_PART_FINAL));
        zlink_msg_close (&part);
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
          zlink_completion_recv (fixture.dealer, &completion, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT (1, completion.reply_part_count);
        TEST_ASSERT_EQUAL_UINT (5, zlink_msg_size (&completion.reply_parts[0]));
        TEST_ASSERT_EQUAL_MEMORY ("reply", zlink_msg_data (&completion.reply_parts[0]), 5);
        zlink_completion_close (&completion);
    }
    fixture.close ();
    TEST_ASSERT_TRUE_MESSAGE (armed, "application poller did not enter wait");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, disconnect_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, connect_result);
    TEST_ASSERT_TRUE_MESSAGE (terminal_ms >= 0,
                              "old connection terminal edge stalled");
    if (operation_ != explicit_disconnect)
        TEST_ASSERT_TRUE_MESSAGE (ready_ms >= 0, "reconnect READY stalled");
    if (waiting_) {
        TEST_ASSERT_EQUAL_INT (1, wait_result);
        TEST_ASSERT_EQUAL_PTR (wait_timer, wait_event.timer);
    }
}

#define PROGRESS_CASE(name, transport, waiting, completion, operation) \
    void name () { run_case (transport, waiting, completion, operation); }
#define TRANSPORT_CASES(prefix, transport, completion) \
    PROGRESS_CASE (prefix##_automatic_idle, transport, false, completion, remote_disconnect) \
    PROGRESS_CASE (prefix##_automatic_blocked, transport, true, completion, remote_disconnect) \
    PROGRESS_CASE (prefix##_disconnect_idle, transport, false, completion, explicit_disconnect) \
    PROGRESS_CASE (prefix##_disconnect_blocked, transport, true, completion, explicit_disconnect) \
    PROGRESS_CASE (prefix##_request_idle, transport, false, completion, reconnect_request) \
    PROGRESS_CASE (prefix##_request_blocked, transport, true, completion, reconnect_request)

TRANSPORT_CASES (tcp_in, "tcp", false)
TRANSPORT_CASES (tcp_completion, "tcp", true)
TRANSPORT_CASES (inproc_in, "inproc", false)
TRANSPORT_CASES (inproc_completion, "inproc", true)
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define SELECTED_TEST(name) \
    if (!getenv ("ZLINK_TEST_CASE") || strcmp (getenv ("ZLINK_TEST_CASE"), #name) == 0) \
        RUN_TEST (name)
#define RUN_TRANSPORT(prefix) \
    SELECTED_TEST (prefix##_automatic_idle); \
    SELECTED_TEST (prefix##_automatic_blocked); \
    SELECTED_TEST (prefix##_disconnect_idle); \
    SELECTED_TEST (prefix##_disconnect_blocked); \
    SELECTED_TEST (prefix##_request_idle); \
    SELECTED_TEST (prefix##_request_blocked)
    RUN_TRANSPORT (tcp_in);
    RUN_TRANSPORT (tcp_completion);
    RUN_TRANSPORT (inproc_in);
    RUN_TRANSPORT (inproc_completion);
    return UNITY_END ();
}
