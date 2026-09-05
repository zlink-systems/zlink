/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace { std::vector<void *> monitors; }

void setUp () { setup_test_context (); }
void tearDown ()
{
    for (size_t i = 0; i != monitors.size (); ++i)
        zlink_monitor_close (&monitors[i]);
    monitors.clear ();
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;

void *make_socket (zlink_socket_type_t type_)
{
    void *socket = test_context_socket (type_);
    const int linger = 0;
    const int timeout = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));
    return socket;
}

void *open_monitor (void *socket_, uint64_t events_)
{
    zlink_socket_monitor_open_options_t options = {};
    options.events = events_;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    monitors.push_back (monitor);
    return monitor;
}

void close_monitors ()
{
    for (size_t i = 0; i != monitors.size (); ++i)
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitors[i]));
    monitors.clear ();
}

void wait_event (void *monitor_, uint64_t expected_)
{
    const clock_type::time_point deadline = clock_type::now () + std::chrono::seconds (3);
    while (clock_type::now () < deadline) {
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - clock_type::now ()).count ());
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        TEST_ASSERT_GREATER_OR_EQUAL_INT (
          0, zlink_poll (&item, 1, remaining > 0 ? remaining : 0, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            continue;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event == expected_
            && (expected_ != ZLINK_EVENT_CONNECTION_READY
                || (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)))
            return;
    }
    TEST_FAIL_MESSAGE ("expected monitor event did not arrive");
}

void configure_tls (void *server_, void *client_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (), files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (), files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (), files_.ca_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_HOSTNAME, "localhost", 9));
}

void listener_release (const char *transport_, bool unbind_, int iterations_)
{
    if (!zlink_has (transport_))
        TEST_IGNORE_MESSAGE ("transport unavailable");
    const bool secure = strcmp (transport_, "tls") == 0 || strcmp (transport_, "wss") == 0;
    tls_test_files_t files;
    if (secure)
        files = make_tls_test_files ();
    for (int i = 0; i != iterations_; ++i) {
        void *old = make_socket (ZLINK_SOCKET_PAIR);
        void *replacement = make_socket (ZLINK_SOCKET_PAIR);
        void *client = make_socket (ZLINK_SOCKET_PAIR);
        if (secure) {
            configure_tls (old, client, files);
            configure_tls (replacement, client, files);
        }
        void *old_monitor = unbind_
          ? open_monitor (old, ZLINK_EVENT_CLOSED | ZLINK_EVENT_ACCEPTED) : NULL;
        char bind_address[256];
        if (strcmp (transport_, "ipc") == 0)
            snprintf (bind_address, sizeof (bind_address), "ipc://endpoint-release-%d-%d.sock", unbind_, i);
        else
            snprintf (bind_address, sizeof (bind_address), "%s://127.0.0.1:*", transport_);
        char endpoint[256];
        test_bind (old, bind_address, endpoint, sizeof (endpoint));

        // Network listeners have exclusive ownership while bound. In
        // particular, rebind must not be implemented with SO_REUSEPORT.
        if (strcmp (transport_, "ipc") != 0) {
            TEST_ASSERT_NOT_EQUAL (ZLINK_BIND_OK, zlink_bind (replacement, endpoint));
            TEST_ASSERT_EQUAL_INT (EADDRINUSE, zlink_errno ());
        }
        if (unbind_)
            TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_unbind (old, endpoint));
        else
            test_context_socket_close (old);
        // The very next operation binds once: no retry or bind-side wait.
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (replacement, endpoint));

        // Monitor delivery is asynchronous; the exclusive bind above and
        // immediate successful rebind establish the fd-release boundary.
        if (old_monitor)
            wait_event (old_monitor, ZLINK_EVENT_CLOSED);
        zlink_monitor_event_t event = {};
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
        zlink_msg_t part;
        const char payload[] = "new-listener";
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, sizeof (payload)));
        memcpy (zlink_msg_data (&part), payload, sizeof (payload));
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_send_part (
          client, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        const zlink_routing_id_t *source = NULL;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_recv_part (
          replacement, &source, &part, &more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
        TEST_ASSERT_EQUAL_UINT64 (sizeof (payload), zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&part), sizeof (payload));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        // A connection initiated after rebind cannot be accepted by the old fd.
        if (old_monitor)
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_socket_monitor_recv (
              old_monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT));
        close_monitors ();
        test_context_socket_close (client);
        test_context_socket_close (replacement);
        if (unbind_)
            test_context_socket_close (old);
#ifdef ZLINK_HAVE_IPC
        if (strcmp (transport_, "ipc") == 0)
            TEST_ASSERT_EQUAL_INT (0, remove (endpoint + strlen ("ipc://")));
#endif
    }
    if (secure)
        cleanup_tls_test_files (files);
}
}

void test_tcp_close_releases_listener () { listener_release ("tcp", false, 100); }
void test_tcp_unbind_releases_listener () { listener_release ("tcp", true, 100); }
void test_ipc_releases_listener ()
{
    listener_release ("ipc", false, 10);
    listener_release ("ipc", true, 10);
}
void test_ws_releases_listener ()
{
    listener_release ("ws", false, 10);
    listener_release ("ws", true, 10);
}
void test_tls_releases_listener ()
{
    listener_release ("tls", false, 10);
    listener_release ("tls", true, 10);
}
void test_wss_releases_listener ()
{
    listener_release ("wss", false, 10);
    listener_release ("wss", true, 10);
}

void test_inproc_unbind_disconnected_and_not_found ()
{
    long maximum_us = 0;
    for (int i = 0; i != 50; ++i) {
        void *bound = make_socket (ZLINK_SOCKET_ROUTER);
        void *peer = make_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (bound, "bound", 5));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (peer, "peer", 4));
        void *monitor = open_monitor (
          peer, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);
        void *bound_monitor = open_monitor (bound, ZLINK_EVENT_CONNECTION_READY);
        char endpoint[80];
        snprintf (endpoint, sizeof (endpoint), "inproc://two-lane-unbind-%d", i);
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (peer, endpoint));
        wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
        wait_event (bound_monitor, ZLINK_EVENT_CONNECTION_READY);

        zlink_routing_id_t peer_rid = {};
        TEST_ASSERT_SUCCESS_ERRNO (zlink_get_routing_id (peer, &peer_rid));
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
        *static_cast<unsigned char *> (zlink_msg_data (&part)) = 1;
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_request_part (
          bound, &peer_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
          3000, NULL, &id));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv_part (
          peer, &source, &token, &part, &more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_EQUAL (0, token);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

        const clock_type::time_point started = clock_type::now ();
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_unbind (bound, endpoint));
        const long elapsed_us = static_cast<long> (
          std::chrono::duration_cast<std::chrono::microseconds> (
            clock_type::now () - started).count ());
        if (elapsed_us > maximum_us)
            maximum_us = elapsed_us;
        char detail[128];
        snprintf (detail, sizeof (detail), "iteration %d: unbind took %ld us", i, elapsed_us);
        TEST_ASSERT_LESS_THAN_INT_MESSAGE (50000, elapsed_us, detail);
        wait_event (monitor, ZLINK_EVENT_DISCONNECTED);
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_completion_recv (
          bound, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_FOUND, completion.request_result);
        zlink_completion_close (&completion);
        close_monitors ();
        test_context_socket_close (peer);
        test_context_socket_close (bound);
    }
    printf ("two-lane unbind: 50 iterations, maximum %ld us\n", maximum_us);
}

void test_inproc_two_lane_unbind_progress ()
{
    long maximum_us = 0;
    for (int i = 0; i != 50; ++i) {
        void *bound = make_socket (ZLINK_SOCKET_ROUTER);
        void *peer = make_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (bound, "bound", 5));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (peer, "peer", 4));
        // Observe readiness, then close the monitor before measuring. A peer
        // monitor drives peer progress; a caller monitor makes process_commands
        // nonblocking. Either would hide the idle two-lane handshake delay.
        void *monitor = open_monitor (bound, ZLINK_EVENT_CONNECTION_READY);
        char endpoint[80];
        snprintf (endpoint, sizeof (endpoint), "inproc://idle-two-lane-unbind-%d", i);
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (peer, endpoint));
        wait_event (monitor, ZLINK_EVENT_CONNECTION_READY);
        close_monitors ();
        const clock_type::time_point started = clock_type::now ();
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_unbind (bound, endpoint));
        const long elapsed_us = static_cast<long> (
          std::chrono::duration_cast<std::chrono::microseconds> (
            clock_type::now () - started).count ());
        if (elapsed_us > maximum_us)
            maximum_us = elapsed_us;
        char detail[128];
        snprintf (detail, sizeof (detail), "iteration %d: idle unbind took %ld us", i, elapsed_us);
        TEST_ASSERT_LESS_THAN_INT_MESSAGE (50000, elapsed_us, detail);
        close_monitors ();
        test_context_socket_close (peer);
        test_context_socket_close (bound);
    }
    printf ("idle two-lane unbind: 50 iterations, maximum %ld us\n", maximum_us);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_tcp_close_releases_listener);
    RUN_TEST (test_tcp_unbind_releases_listener);
    RUN_TEST (test_ipc_releases_listener);
    RUN_TEST (test_ws_releases_listener);
    RUN_TEST (test_tls_releases_listener);
    RUN_TEST (test_wss_releases_listener);
    RUN_TEST (test_inproc_two_lane_unbind_progress);
    RUN_TEST (test_inproc_unbind_disconnected_and_not_found);
    return UNITY_END ();
}
