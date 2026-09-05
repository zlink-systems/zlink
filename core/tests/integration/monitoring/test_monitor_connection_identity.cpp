/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int event_timeout_ms = 5000;
tls_test_files_t tls_files;

void configure_tls_socket (void *socket_, bool server_)
{
    if (server_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          socket_, ZLINK_OPT_TLS_CERT, tls_files.server_cert.c_str (),
          tls_files.server_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          socket_, ZLINK_OPT_TLS_KEY, tls_files.server_key.c_str (),
          tls_files.server_key.size ()));
    } else {
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          socket_, ZLINK_OPT_TLS_TRUST_SYSTEM, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          socket_, ZLINK_OPT_TLS_CA, tls_files.ca_cert.c_str (),
          tls_files.ca_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          socket_, ZLINK_OPT_TLS_HOSTNAME, "localhost", 9));
    }
}

const uint32_t ready_edge_flag = ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE;

void set_common_options (void *socket_)
{
    const int zero = 0;
    const int reconnect_ivl = 20;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      socket_, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl,
      sizeof (reconnect_ivl)));
}

void *open_identity_monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECT_DELAYED | ZLINK_EVENT_CONNECT_RETRIED
                     | ZLINK_EVENT_CLOSED
                     | ZLINK_EVENT_DISCONNECTED
                     | ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    return monitor;
}

zlink_monitor_event_t await_event (void *monitor_, uint64_t expected_event_,
                                   bool require_ready_edge_ = false,
                                   bool forbid_closed_ = false)
{
    const int iterations = event_timeout_ms;
    for (int i = 0; i < iterations; ++i) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (forbid_closed_)
            TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_CLOSED, event.event);
        if (event.event == expected_event_
            && (!require_ready_edge_
                || (event.flags & ready_edge_flag) != 0))
            return event;
    }

    TEST_FAIL_MESSAGE ("timed out waiting for monitor event");
    zlink_monitor_event_t unreachable;
    memset (&unreachable, 0, sizeof (unreachable));
    return unreachable;
}

void assert_no_queued_closed (void *monitor_)
{
    for (;;) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        TEST_ASSERT_NOT_EQUAL (ZLINK_EVENT_CLOSED, event.event);
    }
}

void assert_ready_record (const zlink_monitor_event_t &event_)
{
    TEST_ASSERT_TRUE (event_.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                              event_.transport_lane);
    TEST_ASSERT_EQUAL_UINT32 (ready_edge_flag, event_.flags);
}

void assert_same_physical_attempt (const zlink_monitor_event_t &expected_,
                                   const zlink_monitor_event_t &actual_)
{
    TEST_ASSERT_TRUE (actual_.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT64 (expected_.connection_id,
                              actual_.connection_id);
    TEST_ASSERT_EQUAL_UINT32 (expected_.transport_lane,
                              actual_.transport_lane);
    TEST_ASSERT_EQUAL_UINT32 (0, actual_.flags);
}

void assert_same_identity_and_lane (const zlink_monitor_event_t &expected_,
                                    const zlink_monitor_event_t &actual_)
{
    TEST_ASSERT_EQUAL_UINT64 (expected_.connection_id,
                              actual_.connection_id);
    TEST_ASSERT_EQUAL_UINT32 (expected_.transport_lane,
                              actual_.transport_lane);
}

void bind_server (void *server_, const std::string &endpoint_)
{
    if (endpoint_.find ("tls://") == 0)
        configure_tls_socket (server_, true);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_, endpoint_.c_str ()));
}

void run_connected_lifecycle (const std::string &endpoint_,
                              void *initial_server_, bool network_transport_)
{
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    set_common_options (client);
    if (endpoint_.find ("tls://") == 0)
        configure_tls_socket (client, false);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, "monitor-client", 14));
    void *monitor = open_identity_monitor (client);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_.c_str ()));
    zlink_monitor_event_t initial_attempt;
    memset (&initial_attempt, 0, sizeof (initial_attempt));
    if (network_transport_)
        initial_attempt = await_event (monitor, ZLINK_EVENT_CONNECT_DELAYED);
    const zlink_monitor_event_t first_ready =
      await_event (monitor, ZLINK_EVENT_CONNECTION_READY, true,
                   !network_transport_);
    assert_ready_record (first_ready);
    if (network_transport_)
        assert_same_identity_and_lane (initial_attempt, first_ready);

    test_context_socket_close_zero_linger (initial_server_);
    const zlink_monitor_event_t first_disconnected =
      await_event (monitor, ZLINK_EVENT_DISCONNECTED, false,
                   !network_transport_);
    assert_same_physical_attempt (first_ready, first_disconnected);
    if (!network_transport_)
        assert_no_queued_closed (monitor);

    void *replacement = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (replacement);
    if (network_transport_) {
        const zlink_monitor_event_t delayed =
          await_event (monitor, ZLINK_EVENT_CONNECT_DELAYED);
        TEST_ASSERT_TRUE (delayed.connection_id != first_ready.connection_id);
        const zlink_monitor_event_t closed =
          await_event (monitor, ZLINK_EVENT_CLOSED);
        assert_same_physical_attempt (delayed, closed);
    }
    bind_server (replacement, endpoint_);
    const zlink_monitor_event_t reconnect_ready =
      await_event (monitor, ZLINK_EVENT_CONNECTION_READY, true,
                   !network_transport_);
    assert_ready_record (reconnect_ready);
    TEST_ASSERT_TRUE (first_ready.connection_id
                      != reconnect_ready.connection_id);

    if (network_transport_) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_disconnect (client, endpoint_.c_str ()));
        const zlink_monitor_event_t explicit_disconnected =
          await_event (monitor, ZLINK_EVENT_DISCONNECTED, false,
                   !network_transport_);
        assert_same_physical_attempt (reconnect_ready,
                                      explicit_disconnected);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (replacement);
}

void run_failed_connect_attempt (const std::string &endpoint_)
{
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    set_common_options (client);
    if (endpoint_.find ("tls://") == 0)
        configure_tls_socket (client, false);
    void *monitor = open_identity_monitor (client);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_.c_str ()));
    const zlink_monitor_event_t delayed =
      await_event (monitor, ZLINK_EVENT_CONNECT_DELAYED);
    TEST_ASSERT_TRUE (delayed.connection_id != 0);
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                              delayed.transport_lane);
    TEST_ASSERT_EQUAL_UINT32 (0, delayed.flags);

    const zlink_monitor_event_t closed =
      await_event (monitor, ZLINK_EVENT_CLOSED);
    assert_same_physical_attempt (delayed, closed);
    const zlink_monitor_event_t retried =
      await_event (monitor, ZLINK_EVENT_CONNECT_RETRIED);
    assert_same_physical_attempt (delayed, retried);
    const zlink_monitor_event_t next_delayed =
      await_event (monitor, ZLINK_EVENT_CONNECT_DELAYED);
    TEST_ASSERT_TRUE (next_delayed.connection_id != delayed.connection_id);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint_.c_str ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
}

void run_explicit_disconnect (const std::string &endpoint_, void *server_,
                              bool network_transport_)
{
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    set_common_options (client);
    if (endpoint_.find ("tls://") == 0)
        configure_tls_socket (client, false);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, "monitor-client", 14));
    void *monitor = open_identity_monitor (client);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_.c_str ()));
    if (network_transport_)
        (void) await_event (monitor, ZLINK_EVENT_CONNECT_DELAYED);
    const zlink_monitor_event_t ready =
      await_event (monitor, ZLINK_EVENT_CONNECTION_READY, true,
                   !network_transport_);
    assert_ready_record (ready);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint_.c_str ()));
    if (!network_transport_) {
        // Inproc endpoint termination completes through the peer command
        // queue. A public poll call supplies that peer-side progress.
        zlink_pollitem_t peer_progress = {server_, 0, ZLINK_POLLIN, 0};
        for (int i = 0; i < 10; ++i) {
            (void) zlink_poll (&peer_progress, 1, 10, NULL);
            msleep (1);
        }
    }
    const zlink_monitor_event_t disconnected =
      await_event (monitor, ZLINK_EVENT_DISCONNECTED, false,
                   !network_transport_);
    assert_same_physical_attempt (ready, disconnected);
    if (!network_transport_)
        assert_no_queued_closed (monitor);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_);
}

void run_client_close (const std::string &endpoint_, void *server_,
                       bool network_transport_)
{
    void *server_monitor = open_identity_monitor (server_);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    set_common_options (client);
    if (endpoint_.find ("tls://") == 0)
        configure_tls_socket (client, false);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, "monitor-client", 14));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_.c_str ()));
    const zlink_monitor_event_t ready =
      await_event (server_monitor, ZLINK_EVENT_CONNECTION_READY, true,
                   !network_transport_);
    assert_ready_record (ready);

    test_context_socket_close_zero_linger (client);
    if (!network_transport_) {
        // Inproc termination completes through the peer command queue.
        zlink_pollitem_t peer_progress = {server_, 0, ZLINK_POLLIN, 0};
        for (int i = 0; i < 10; ++i) {
            (void) zlink_poll (&peer_progress, 1, 10, NULL);
            msleep (1);
        }
    }
    const zlink_monitor_event_t disconnected =
      await_event (server_monitor, ZLINK_EVENT_DISCONNECTED, false,
                   !network_transport_);
    assert_same_physical_attempt (ready, disconnected);
    if (!network_transport_)
        assert_no_queued_closed (server_monitor);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    test_context_socket_close_zero_linger (server_);
}
}

void test_tcp_connection_identity_lifecycle ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    run_connected_lifecycle (endpoint, server, true);
}

#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
void test_ipc_connection_identity_lifecycle ()
{
    const std::string endpoint = "ipc://" + make_random_ipc_path ();
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_connected_lifecycle (endpoint, server, true);
}
#endif

void test_inproc_connection_identity_lifecycle ()
{
    const std::string endpoint = "inproc://monitor-connection-identity";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_connected_lifecycle (endpoint, server, false);
}

void test_tcp_failed_attempt_closed_identity ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *reservation = test_context_socket (ZLINK_SOCKET_ROUTER);
    bind_loopback_ipv4 (reservation, endpoint, sizeof (endpoint));
    test_context_socket_close_zero_linger (reservation);
    run_failed_connect_attempt (endpoint);
}

void test_tcp_explicit_disconnect_identity ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    run_explicit_disconnect (endpoint, server, true);
}

#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
void test_ipc_explicit_disconnect_identity ()
{
    const std::string endpoint = "ipc://" + make_random_ipc_path ();
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_explicit_disconnect (endpoint, server, true);
}
#endif

void test_inproc_explicit_disconnect_identity ()
{
    const std::string endpoint = "inproc://monitor-explicit-disconnect";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_explicit_disconnect (endpoint, server, false);
}

void test_tcp_client_close_identity ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    run_client_close (endpoint, server, true);
}

#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
void test_ipc_client_close_identity ()
{
    const std::string endpoint = "ipc://" + make_random_ipc_path ();
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_client_close (endpoint, server, true);
}
#endif

void test_inproc_client_close_identity ()
{
    const std::string endpoint = "inproc://monitor-client-close-identity";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, endpoint);
    run_client_close (endpoint, server, false);
}

#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
void test_ipc_failed_attempt_closed_identity ()
{
    run_failed_connect_attempt ("ipc://" + make_random_ipc_path ());
}
#endif

namespace
{
void run_extra_transport (const char *transport_, int scenario_)
{
    if (!zlink_has (transport_))
        TEST_IGNORE_MESSAGE ("transport unavailable");
    const bool tls = strcmp (transport_, "tls") == 0;
    if (tls)
        tls_files = make_tls_test_files ();
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_common_options (server);
    bind_server (server, std::string (transport_) + "://127.0.0.1:*");
    char endpoint[MAX_SOCKET_STRING];
    size_t length = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (
      server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &length));
    if (scenario_ == 0)
        run_connected_lifecycle (endpoint, server, true);
    else if (scenario_ == 1) {
        test_context_socket_close_zero_linger (server);
        run_failed_connect_attempt (endpoint);
    } else if (scenario_ == 2)
        run_explicit_disconnect (endpoint, server, true);
    else
        run_client_close (endpoint, server, true);
    if (tls)
        cleanup_tls_test_files (tls_files);
}
}

void test_ws_connection_identity_lifecycle ()
{
    run_extra_transport ("ws", 0);
}

void test_ws_failed_attempt_closed_identity ()
{
    run_extra_transport ("ws", 1);
}

void test_ws_explicit_disconnect_identity ()
{
    run_extra_transport ("ws", 2);
}

void test_ws_client_close_identity ()
{
    run_extra_transport ("ws", 3);
}

void test_tls_connection_identity_lifecycle ()
{
    run_extra_transport ("tls", 0);
}

void test_tls_failed_attempt_closed_identity ()
{
    run_extra_transport ("tls", 1);
}

void test_tls_explicit_disconnect_identity ()
{
    run_extra_transport ("tls", 2);
}

void test_tls_client_close_identity ()
{
    run_extra_transport ("tls", 3);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_tcp_connection_identity_lifecycle);
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_TEST (test_ipc_connection_identity_lifecycle);
#endif
    RUN_TEST (test_inproc_connection_identity_lifecycle);
    RUN_TEST (test_tcp_failed_attempt_closed_identity);
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_TEST (test_ipc_failed_attempt_closed_identity);
#endif
    RUN_TEST (test_tcp_explicit_disconnect_identity);
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_TEST (test_ipc_explicit_disconnect_identity);
#endif
    RUN_TEST (test_inproc_explicit_disconnect_identity);
    RUN_TEST (test_tcp_client_close_identity);
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_TEST (test_ipc_client_close_identity);
#endif
    RUN_TEST (test_inproc_client_close_identity);
    RUN_TEST (test_ws_connection_identity_lifecycle);
    RUN_TEST (test_ws_failed_attempt_closed_identity);
    RUN_TEST (test_ws_explicit_disconnect_identity);
    RUN_TEST (test_ws_client_close_identity);
    RUN_TEST (test_tls_connection_identity_lifecycle);
    RUN_TEST (test_tls_failed_attempt_closed_identity);
    RUN_TEST (test_tls_explicit_disconnect_identity);
    RUN_TEST (test_tls_client_close_identity);
    return UNITY_END ();
}
