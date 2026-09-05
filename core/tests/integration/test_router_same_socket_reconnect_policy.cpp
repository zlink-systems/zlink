/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace { std::vector<void *> monitors; }

void setUp () { setup_test_context (); }
void tearDown ()
{
    for (size_t i = 0; i < monitors.size (); ++i)
        (void) zlink_monitor_close (&monitors[i]);
    monitors.clear ();
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int setup_ms = 3000;
const int progress_ms = 200;

zlink_routing_id_t routing_id (const char *text_)
{
    zlink_routing_id_t rid = {};
    rid.size = static_cast<uint8_t> (strlen (text_));
    memcpy (rid.data, text_, rid.size);
    return rid;
}

void *new_router (const char *rid_, int policy_, bool retry_)
{
    void *socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int zero = 0;
    const int reconnect = retry_ ? 50 : -1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (socket, rid_, strlen (rid_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_RECONNECT_IVL, &reconnect, sizeof reconnect));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_RID_DUPLICATE_POLICY, &policy_, sizeof policy_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_RCVTIMEO, &setup_ms, sizeof setup_ms));
    return socket;
}

struct observation_t
{
    void *monitor;
    std::vector<zlink_monitor_event_t> events;

    explicit observation_t (void *socket_)
    {
        zlink_socket_monitor_open_options_t options = {};
        options.events = ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY
                         | ZLINK_EVENT_DISCONNECTED
                         | ZLINK_EVENT_SEND_FLOW_PAUSED
                         | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL;
        monitor = zlink_socket_monitor_open (socket_, &options);
        TEST_ASSERT_NOT_NULL (monitor);
        monitors.push_back (monitor);
    }

    void drain ()
    {
        for (;;) {
            zlink_monitor_event_t event = {};
            const zlink_recv_result_t rc = zlink_socket_monitor_recv (
              monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_NO_DATA)
                return;
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
            TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL, event.event);
            events.push_back (event);
        }
    }

    uint64_t wait (uint64_t kind_, uint64_t excluded_, int timeout_ms_,
                   bool match_ = false)
    {
        const clock_type::time_point deadline =
          clock_type::now () + std::chrono::milliseconds (timeout_ms_);
        do {
            drain ();
            for (size_t i = 0; i < events.size (); ++i) {
                const zlink_monitor_event_t &event = events[i];
                if (event.event == kind_
                    && (match_ ? event.connection_id == excluded_
                               : event.connection_id != excluded_)
                    && event.transport_lane == ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION
                    && (kind_ != ZLINK_EVENT_CONNECTION_READY
                        || (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))) {
                    TEST_ASSERT_NOT_EQUAL (0, event.connection_id);
                    return event.connection_id;
                }
            }
            const long remaining = static_cast<long> (
              std::chrono::duration_cast<std::chrono::milliseconds> (
                deadline - clock_type::now ()).count ());
            zlink_pollitem_t item = {monitor, 0, ZLINK_POLLIN, 0};
            zlink_config_result_t error = ZLINK_CONFIG_OK;
            TEST_ASSERT_TRUE (zlink_poll (
              &item, 1, remaining > 0 ? remaining : 0, &error) >= 0);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        } while (clock_type::now () < deadline);
        fprintf (stderr, "missing event=%llu excluded=%llu timeout_ms=%d\n",
                 static_cast<unsigned long long> (kind_),
                 static_cast<unsigned long long> (excluded_), timeout_ms_);
        TEST_FAIL_MESSAGE ("monitor transition did not progress");
        return 0;
    }

    void assert_kept (uint64_t connection_)
    {
        drain ();
        for (size_t i = 0; i < events.size (); ++i)
            if (events[i].connection_id == connection_)
                TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_DISCONNECTED, events[i].event);
    }
};

zlink_completion_id_t request (void *socket_, const char *rid_)
{
    const zlink_routing_id_t target = routing_id (rid_);
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
    *static_cast<char *> (zlink_msg_data (&part)) = 'Q';
    zlink_completion_id_t id = 0;
    const zlink_submit_result_t rc = zlink_request_part (
      socket_, &target, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      setup_ms, NULL, &id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, rc);
    TEST_ASSERT_NOT_EQUAL (0, id);
    return id;
}

void receive_request (void *socket_, bool reply_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv_part (
      socket_, &source, &token, &part, &flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_EQUAL (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY ("Q", zlink_msg_data (&part), 1);
    if (reply_)
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_reply_part (
          socket_, source, token, &part, ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void completion (void *socket_, zlink_completion_id_t id_,
                  zlink_request_result_t expected_, int timeout_ms_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_poller_add (poller, socket_, NULL, ZLINK_POLLCOMPLETION));
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller, &event, 1, timeout_ms_, &error);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (1, rc);
    zlink_completion_t result = {};
    result.struct_size = sizeof result;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
      socket_, &result, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (id_, result.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, result.kind);
    TEST_ASSERT_EQUAL_INT (expected_, result.request_result);
    TEST_ASSERT_EQUAL_UINT64 (expected_ == ZLINK_REQUEST_OK ? 1 : 0,
                              result.reply_part_count);
    if (expected_ == ZLINK_REQUEST_OK) {
        TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&result.reply_parts[0]));
        TEST_ASSERT_EQUAL_MEMORY ("Q", zlink_msg_data (&result.reply_parts[0]), 1);
    }
    zlink_completion_close (&result);
    result = zlink_completion_t ();
    result.struct_size = sizeof result;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_completion_recv (
      socket_, &result, ZLINK_RECV_FLAGS_DONTWAIT));
}

void roundtrip (void *from_, void *to_, const char *rid_)
{
    const zlink_completion_id_t id = request (from_, rid_);
    receive_request (to_, true);
    completion (from_, id, ZLINK_REQUEST_OK, setup_ms);
}

void await_reconnected_request (void *client_, void *server_)
{
    // READY and WRITABLE can race another rejected attempt. D-090 requires
    // the caller to resubmit after NOT_CONNECTED; it does not reconnect the
    // socket. Keep client application receive out of this recovery loop so
    // it cannot drain an old preamble to make Core's reconnect work.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (
      poller, client_, NULL, ZLINK_POLLCOMPLETION | ZLINK_POLLOUT));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (
      poller, server_, NULL, ZLINK_POLLIN));
    const zlink_routing_id_t target = routing_id ("server");
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (setup_ms);
    zlink_completion_id_t pending = 0;
    bool done = false;
    unsigned attempts = 0;
    while (!done && clock_type::now () < deadline) {
        if (!pending) {
            zlink_msg_t part;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
            *static_cast<char *> (zlink_msg_data (&part)) = 'Q';
            const zlink_submit_result_t rc = zlink_request_part (
              client_, &target, &part, ZLINK_SEND_FLAGS_DONTWAIT,
              ZLINK_PART_FINAL, setup_ms, NULL, &pending);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
            TEST_ASSERT_TRUE (rc == ZLINK_SUBMIT_OK
                              || rc == ZLINK_SUBMIT_BACKPRESSURED
                              || rc == ZLINK_SUBMIT_NOT_CONNECTED
                              || rc == ZLINK_SUBMIT_NOT_FOUND);
            if (rc == ZLINK_SUBMIT_OK || rc == ZLINK_SUBMIT_BACKPRESSURED) {
                TEST_ASSERT_NOT_EQUAL (0, pending);
            } else {
                TEST_ASSERT_EQUAL_UINT64 (0, pending);
            }
            ++attempts;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_modify (
              poller, client_, ZLINK_POLLCOMPLETION
                                  | (pending ? 0 : ZLINK_POLLOUT)));
        }
        zlink_poller_event_t events[2] = {};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - clock_type::now ()).count ());
        const int count = zlink_poller_wait (
          poller, events, 2, remaining > 0 ? remaining : 0, &error);
        TEST_ASSERT_TRUE (count >= 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        for (int i = 0; i < count; ++i) {
            if (events[i].socket == server_ && (events[i].events & ZLINK_POLLIN))
                receive_request (server_, true);
            if (events[i].socket != client_
                || !(events[i].events & ZLINK_POLLCOMPLETION))
                continue;
            zlink_completion_t result = {};
            result.struct_size = sizeof result;
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
              client_, &result, ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_UINT64 (pending, result.completion_id);
            if (result.kind == ZLINK_COMPLETION_WRITABLE) {
                TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, result.send_result);
            } else {
                TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, result.kind);
                TEST_ASSERT_TRUE (result.request_result == ZLINK_REQUEST_OK
                                  || result.request_result == ZLINK_REQUEST_NOT_CONNECTED);
                done = result.request_result == ZLINK_REQUEST_OK;
                if (done) {
                    TEST_ASSERT_EQUAL_UINT64 (1, result.reply_part_count);
                    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&result.reply_parts[0]));
                    TEST_ASSERT_EQUAL_MEMORY ("Q", zlink_msg_data (&result.reply_parts[0]), 1);
                }
            }
            zlink_completion_close (&result);
            pending = 0;
        }
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    printf ("automatic reconnect request attempts=%u completed=%d\n", attempts, done);
    TEST_ASSERT_TRUE_MESSAGE (done, "connect intent did not recover without client app recv");
}

void run_same_socket (bool tcp_, int policy_, bool retry_ = false)
{
    void *server = new_router ("server", policy_, false);
    void *client = new_router ("client", policy_, retry_);
    observation_t server_events (server), client_events (client);
    char endpoint[MAX_SOCKET_STRING];
    if (tcp_)
        bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    else {
        strcpy (endpoint, "inproc://same-socket-reconnect-policy");
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    const uint64_t old_client = client_events.wait (
      ZLINK_EVENT_CONNECTION_READY, 0, setup_ms);
    const uint64_t old_server = server_events.wait (
      ZLINK_EVENT_CONNECTION_READY, 0, setup_ms);
    roundtrip (client, server, "server");
    roundtrip (server, client, "client");

    char duplicate_endpoint[MAX_SOCKET_STRING];
    strcpy (duplicate_endpoint, endpoint);
    if (tcp_) {
        // TCP deduplicates an identical endpoint string before admission.
        // Verify that call, then use a hostname alias of the same listener
        // to exercise admission of a second physical connection.
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
        roundtrip (client, server, "server");
        roundtrip (server, client, "client");
        client_events.assert_kept (old_client);
        server_events.assert_kept (old_server);
        snprintf (duplicate_endpoint, sizeof duplicate_endpoint,
                  "tcp://localhost:%s", strrchr (endpoint, ':') + 1);
    }

    const bool handover = policy_ == ZLINK_RID_DUPLICATE_HANDOVER;
    zlink_completion_id_t client_pending = 0, server_pending = 0;
    if (handover) {
        client_pending = request (client, "server");
        receive_request (server, false);
        server_pending = request (server, "client");
        receive_request (client, false);
    }

    // Deliberately retain the admitted connection while the same socket
    // connects to the same listener again. No disconnect precedes it.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, duplicate_endpoint));
    uint64_t new_client = 0, new_server = 0;
    if (handover) {
        new_client = client_events.wait (
          ZLINK_EVENT_CONNECTION_READY, old_client, setup_ms);
        new_server = server_events.wait (
          ZLINK_EVENT_CONNECTION_READY, old_server, setup_ms);
        completion (client, client_pending, ZLINK_REQUEST_NOT_CONNECTED, progress_ms);
        completion (server, server_pending, ZLINK_REQUEST_NOT_CONNECTED, progress_ms);
    } else {
        // Only monitor handles are polled: no application receive advances
        // the rejected pipe's termination (D-092).
        const uint64_t rejected = client_events.wait (
          ZLINK_EVENT_DISCONNECTED, old_client, progress_ms);
        TEST_ASSERT_TRUE (rejected != old_client);
    }
    client_events.assert_kept (old_client);
    server_events.assert_kept (old_server);
    roundtrip (client, server, "server");
    roundtrip (server, client, "client");
    client_events.assert_kept (old_client);
    server_events.assert_kept (old_server);

    if (retry_) {
        // The duplicate was rejected while the old RID owner still existed,
        // before its terminate command could possibly have been processed.
        // Ask only the admitted peer to terminate; retain both connect intents.
        // No further connect call or application receive drives the retry.
        const zlink_routing_id_t peer = routing_id ("client");
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect_rid (server, &peer));
        client_events.wait (ZLINK_EVENT_DISCONNECTED, old_client, progress_ms, true);
        server_events.wait (ZLINK_EVENT_DISCONNECTED, old_server, progress_ms, true);
        new_client = client_events.wait (
          ZLINK_EVENT_CONNECTION_READY, old_client, setup_ms);
        new_server = server_events.wait (
          ZLINK_EVENT_CONNECTION_READY, old_server, setup_ms);
        await_reconnected_request (client, server);
        roundtrip (server, client, "client");
        // READY is transport readiness, not a promise that this first retry
        // wins admission. The bidirectional completions prove convergence.
    }
    printf ("same_socket transport=%s policy=%s retry=%d old=%llu new=%llu\n",
            tcp_ ? "tcp" : "inproc", handover ? "HANDOVER" : "REJECT", retry_,
            static_cast<unsigned long long> (old_client),
            static_cast<unsigned long long> (new_client));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_reject_tcp () { run_same_socket (true, ZLINK_RID_DUPLICATE_REJECT); }
void test_reject_inproc () { run_same_socket (false, ZLINK_RID_DUPLICATE_REJECT); }
void test_handover_tcp () { run_same_socket (true, ZLINK_RID_DUPLICATE_HANDOVER); }
void test_handover_inproc () { run_same_socket (false, ZLINK_RID_DUPLICATE_HANDOVER); }
// No tcp retry case: the tcp duplicate above needs a second connect intent
// (hostname alias) on the same RID, and two intents of one RID retrying
// against one listener collide per ZMP section 4.1 (D-096). The retry
// ordering rule is covered by the inproc case, where one intent suffices.
void test_reject_retry_inproc () { run_same_socket (false, ZLINK_RID_DUPLICATE_REJECT, true); }
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
    RUN_SELECTED (test_reject_tcp);
    RUN_SELECTED (test_reject_inproc);
    RUN_SELECTED (test_handover_tcp);
    RUN_SELECTED (test_handover_inproc);
    RUN_SELECTED (test_reject_retry_inproc);
#undef RUN_SELECTED
    return UNITY_END ();
}
