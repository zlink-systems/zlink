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
    for (size_t i = 0; i != monitors.size (); ++i)
        (void) zlink_monitor_close (&monitors[i]);
    monitors.clear ();
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int setup_timeout_ms = 3000;
// Same command-progress bound as test_socket_disconnect_progress_without_app_poll.
const int disconnect_limit_ms = 200;

void *new_socket (int type_, const char *rid_, int reconnect_ = -1)
{
    void *socket = test_context_socket (type_);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_LINGER, &zero, sizeof zero));
    // Isolate the terminated connection from subsequent automatic attempts.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_RECONNECT_IVL, &reconnect_, sizeof reconnect_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (socket, rid_, strlen (rid_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket, ZLINK_OPT_RCVTIMEO, &setup_timeout_ms, sizeof setup_timeout_ms));
    return socket;
}

void *open_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY
                     | ZLINK_EVENT_DISCONNECTED | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    monitors.push_back (monitor);
    return monitor;
}

zlink_monitor_event_t wait_event (void *monitor_, uint64_t kind_,
                                  uint64_t connection_, int timeout_ms_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (timeout_ms_);
    while (clock_type::now () < deadline) {
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL, event.event);
            if (event.event == kind_
                && (!connection_ || event.connection_id == connection_)
                && event.transport_lane == ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION
                && (kind_ != ZLINK_EVENT_CONNECTION_READY
                    || (event.flags
                        & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))) {
                TEST_ASSERT_NOT_EQUAL (0, event.connection_id);
                return event;
            }
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - clock_type::now ()).count ());
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        TEST_ASSERT_TRUE (zlink_poll (
          &item, 1, remaining > 0 ? remaining : 0, &error) >= 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    }
    fprintf (stderr, "missing event=%llu connection=%llu timeout_ms=%d\n",
             static_cast<unsigned long long> (kind_),
             static_cast<unsigned long long> (connection_), timeout_ms_);
    TEST_FAIL_MESSAGE ("monitor transition exceeded command-progress bound");
    return zlink_monitor_event_t ();
}

void assert_no_disconnect (void *monitor_, uint64_t connection_ = 0)
{
    for (;;) {
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (!connection_ || event.connection_id == connection_)
            TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_DISCONNECTED, event.event);
        TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL, event.event);
    }
}

zlink_completion_id_t submit_request (void *server_)
{
    zlink_routing_id_t target = {};
    target.size = 11;
    memcpy (target.data, "same-client", target.size);
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
    *static_cast<char *> (zlink_msg_data (&part)) = 'Q';
    zlink_completion_id_t id = 0;
    const zlink_submit_result_t rc = zlink_request_part (
      server_, &target, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      setup_timeout_ms, NULL, &id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, rc);
    TEST_ASSERT_NOT_EQUAL (0, id);
    return id;
}

void answer_request (void *client_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv_part (
      client_, &source, &token, &part, &flag, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_EQUAL (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY ("Q", zlink_msg_data (&part), 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_reply_part (
      client_, source, token, &part, ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void assert_completion (void *server_, zlink_completion_id_t id_,
                         zlink_request_result_t expected_, int timeout_ms_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_poller_add (poller, server_, NULL, ZLINK_POLLCOMPLETION));
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller, &event, 1, timeout_ms_, &error);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (1, rc);
    zlink_completion_t completion = {};
    completion.struct_size = sizeof completion;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
      server_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_INT (expected_, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (expected_ == ZLINK_REQUEST_OK ? 1 : 0,
                              completion.reply_part_count);
    zlink_completion_close (&completion);
    completion = zlink_completion_t ();
    completion.struct_size = sizeof completion;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_completion_recv (
      server_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
}

void run_duplicate (bool tcp_, int connector_type_, int policy_,
                    bool reconnect_ = false)
{
    void *server = new_socket (ZLINK_SOCKET_ROUTER, "server");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_RID_DUPLICATE_POLICY, &policy_, sizeof policy_));
    void *server_monitor = open_monitor (server);
    char endpoint[MAX_SOCKET_STRING];
    if (tcp_)
        bind_loopback_ipv4 (server, endpoint, sizeof endpoint);
    else {
        strcpy (endpoint, "inproc://reject-disconnected-without-app-recv");
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));
    }

    void *first = new_socket (connector_type_, "same-client");
    void *first_monitor = open_monitor (first);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (first, endpoint));
    const zlink_monitor_event_t first_ready = wait_event (
      first_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const zlink_monitor_event_t first_accepted = wait_event (
      server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const bool request_contract = connector_type_ == ZLINK_SOCKET_ROUTER;
    const bool reject = policy_ == ZLINK_RID_DUPLICATE_REJECT;
    const zlink_completion_id_t pending =
      request_contract && !reject ? submit_request (server) : 0;

    void *duplicate = new_socket (connector_type_, "same-client",
                                  reconnect_ ? 50 : -1);
    void *duplicate_monitor = open_monitor (duplicate);
    clock_type::time_point started = clock_type::now ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (duplicate, endpoint));
    const zlink_monitor_event_t duplicate_ready = wait_event (
      duplicate_monitor, tcp_ ? ZLINK_EVENT_CONNECTED : ZLINK_EVENT_CONNECTION_READY,
      0, setup_timeout_ms);
    if (!reject) {
        const zlink_monitor_event_t admitted = wait_event (
          server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
        TEST_ASSERT_TRUE (admitted.connection_id != first_accepted.connection_id);
        if (request_contract) {
            assert_completion (server, pending, ZLINK_REQUEST_NOT_CONNECTED,
                               disconnect_limit_ms);
            const zlink_completion_id_t fresh = submit_request (server);
            answer_request (duplicate);
            assert_completion (server, fresh, ZLINK_REQUEST_OK, setup_timeout_ms);
        }
        assert_no_disconnect (server_monitor);
        assert_no_disconnect (first_monitor);
        // Same-direction HANDOVER retains the old physical lane as standby.
        // Closing the binder supplies a physical termination for both peers.
        started = clock_type::now ();
        test_context_socket_close_zero_linger (server);
        server = NULL;
    }
    const zlink_monitor_event_t disconnected = wait_event (
      reject ? duplicate_monitor : first_monitor, ZLINK_EVENT_DISCONNECTED,
      reject ? duplicate_ready.connection_id : first_ready.connection_id,
      disconnect_limit_ms);
    TEST_ASSERT_EQUAL_UINT64 (
      reject ? duplicate_ready.connection_id : first_ready.connection_id,
      disconnected.connection_id);
    if (!reject)
        wait_event (duplicate_monitor, ZLINK_EVENT_DISCONNECTED,
                    duplicate_ready.connection_id, disconnect_limit_ms);
    const int elapsed = static_cast<int> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - started).count ());
    TEST_ASSERT_LESS_OR_EQUAL_INT (disconnect_limit_ms, elapsed);
    printf ("termination transport=%s connector=%s policy=%s elapsed_ms=%d\n",
            tcp_ ? "tcp" : "inproc",
            connector_type_ == ZLINK_SOCKET_ROUTER ? "ROUTER" : "DEALER",
            reject ? "REJECT" : "HANDOVER",
            elapsed);

    if (reject) {
        // REJECT must leave the first admitted route usable. A pair-table
        // collision would terminate it too and report a protocol failure.
        assert_no_disconnect (server_monitor, first_accepted.connection_id);
        assert_no_disconnect (first_monitor);
        if (request_contract) {
            const zlink_completion_id_t kept = submit_request (server);
            answer_request (first);
            assert_completion (server, kept, ZLINK_REQUEST_OK, setup_timeout_ms);
        }
    }

    if (reconnect_) {
        // Only the binder's READY edge proves admission of the automatic
        // attempt after the old RID owner leaves. No connector recv/poll or
        // second connect call advances that attempt.
        test_context_socket_close_zero_linger (first);
        first = NULL;
        const zlink_monitor_event_t recovered = wait_event (
          server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
        wait_event (duplicate_monitor, ZLINK_EVENT_CONNECTION_READY,
                    recovered.connection_id, setup_timeout_ms);
    }
    test_context_socket_close_zero_linger (duplicate);
    if (first)
        test_context_socket_close_zero_linger (first);
    if (server)
        test_context_socket_close_zero_linger (server);
}

#define CASE(name_, tcp_, type_, policy_) \
    void name_ () { run_duplicate (tcp_, type_, policy_); }
CASE (test_router_reject_tcp, true, ZLINK_SOCKET_ROUTER, ZLINK_RID_DUPLICATE_REJECT)
CASE (test_router_reject_inproc, false, ZLINK_SOCKET_ROUTER, ZLINK_RID_DUPLICATE_REJECT)
CASE (test_dealer_reject_tcp, true, ZLINK_SOCKET_DEALER, ZLINK_RID_DUPLICATE_REJECT)
CASE (test_dealer_reject_inproc, false, ZLINK_SOCKET_DEALER, ZLINK_RID_DUPLICATE_REJECT)
CASE (test_router_handover_tcp, true, ZLINK_SOCKET_ROUTER, ZLINK_RID_DUPLICATE_HANDOVER)
CASE (test_router_handover_inproc, false, ZLINK_SOCKET_ROUTER, ZLINK_RID_DUPLICATE_HANDOVER)
CASE (test_dealer_handover_tcp, true, ZLINK_SOCKET_DEALER, ZLINK_RID_DUPLICATE_HANDOVER)
CASE (test_dealer_handover_inproc, false, ZLINK_SOCKET_DEALER, ZLINK_RID_DUPLICATE_HANDOVER)
#undef CASE
void test_router_reject_inproc_reconnect ()
{
    run_duplicate (false, ZLINK_SOCKET_ROUTER, ZLINK_RID_DUPLICATE_REJECT, true);
}

void test_router_handover_tcp_reconnect_different_endpoint ()
{
    void *server = new_socket (ZLINK_SOCKET_ROUTER, "server");
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof handover));
    void *server_monitor = open_monitor (server);
    char dropped_endpoint[MAX_SOCKET_STRING];
    char other_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, dropped_endpoint, sizeof dropped_endpoint);
    bind_loopback_ipv4 (server, other_endpoint, sizeof other_endpoint);

    void *client = new_socket (ZLINK_SOCKET_ROUTER, "same-client", 50);
    void *client_monitor = open_monitor (client);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, dropped_endpoint));
    const zlink_monitor_event_t before = wait_event (
      client_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const zlink_monitor_event_t accepted_before = wait_event (
      server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);

    // Removing the remote listener drops the transport without removing the
    // client's connect intent. Rebinding later drives its automatic reconnect.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unbind (server, dropped_endpoint));
    wait_event (client_monitor, ZLINK_EVENT_DISCONNECTED,
                before.connection_id, disconnect_limit_ms);
    wait_event (server_monitor, ZLINK_EVENT_DISCONNECTED,
                accepted_before.connection_id, disconnect_limit_ms);

    void *other = new_socket (ZLINK_SOCKET_ROUTER, "same-client");
    void *other_monitor = open_monitor (other);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (other, other_endpoint));
    const zlink_monitor_event_t other_ready = wait_event (
      other_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const zlink_monitor_event_t other_accepted = wait_event (
      server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const zlink_completion_id_t pending = submit_request (server);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, dropped_endpoint));
    const zlink_monitor_event_t recovered = wait_event (
      server_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    const zlink_monitor_event_t after = wait_event (
      client_monitor, ZLINK_EVENT_CONNECTION_READY, 0, setup_timeout_ms);
    TEST_ASSERT_TRUE (after.connection_id != before.connection_id);
    TEST_ASSERT_TRUE (recovered.connection_id != accepted_before.connection_id);
    TEST_ASSERT_TRUE (recovered.connection_id != other_accepted.connection_id);
    TEST_ASSERT_TRUE (strcmp (recovered.local_addr, other_accepted.local_addr) != 0);
    assert_completion (server, pending, ZLINK_REQUEST_NOT_CONNECTED,
                       disconnect_limit_ms);
    const zlink_completion_id_t fresh = submit_request (server);
    answer_request (client);
    assert_completion (server, fresh, ZLINK_REQUEST_OK, setup_timeout_ms);
    assert_no_disconnect (other_monitor);
    assert_no_disconnect (server_monitor);

    // Supersession retains the old physical lane set as standby. Only this
    // subsequent physical close may disconnect it.
    test_context_socket_close_zero_linger (server);
    wait_event (other_monitor, ZLINK_EVENT_DISCONNECTED,
                other_ready.connection_id, disconnect_limit_ms);
    wait_event (client_monitor, ZLINK_EVENT_DISCONNECTED,
                after.connection_id, disconnect_limit_ms);
    test_context_socket_close_zero_linger (other);
    test_context_socket_close_zero_linger (client);
}
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
    RUN_SELECTED (test_router_reject_tcp);
    RUN_SELECTED (test_router_reject_inproc);
    RUN_SELECTED (test_dealer_reject_tcp);
    RUN_SELECTED (test_dealer_reject_inproc);
    RUN_SELECTED (test_router_handover_tcp);
    RUN_SELECTED (test_router_handover_inproc);
    RUN_SELECTED (test_dealer_handover_tcp);
    RUN_SELECTED (test_dealer_handover_inproc);
    RUN_SELECTED (test_router_reject_inproc_reconnect);
    RUN_SELECTED (test_router_handover_tcp_reconnect_different_endpoint);
#undef RUN_SELECTED
    return UNITY_END ();
}
