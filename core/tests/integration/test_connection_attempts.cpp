/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include <chrono>
#include <cstring>
#include <cstddef>
#include <thread>
#include <atomic>
#ifndef ZLINK_HAVE_WINDOWS
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
SETUP_TEARDOWN_TESTCONTEXT
namespace
{
zlink_connection_attempt_t snapshot (void *s)
{
    zlink_connection_attempt_t row = {};
    char bytes[512];
    size_t count = 0, required = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              s, &row, 1, &count, bytes, sizeof bytes, &required));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (bytes), row.endpoint_length);
    TEST_ASSERT_EQUAL_UINT64 (0, row.endpoint_offset);
    return row;
}
zlink_connection_attempt_t wait_state (void *s, uint32_t state)
{
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    do {
        auto row = snapshot (s);
        if (row.state == state)
            return row;
        zlink_pollitem_t item = {s, 0, ZLINK_POLLCONNECT, 0};
        zlink_config_result_t error;
        TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
    } while (std::chrono::steady_clock::now () < end);
    TEST_FAIL_MESSAGE ("connection state did not converge");
    return {};
}
int ready (void *s)
{
    zlink_pollitem_t item = {s, 0, ZLINK_POLLCONNECT, 0};
    zlink_config_result_t error;
    TEST_ASSERT_TRUE (zlink_poll (&item, 1, 0, &error) >= 0);
    return item.revents;
}
void test_abi ()
{
    TEST_ASSERT_EQUAL_UINT64 (0, offsetof (zlink_connection_attempt_t, endpoint_offset));
    TEST_ASSERT_EQUAL_UINT64 (8, offsetof (zlink_connection_attempt_t, endpoint_length));
    TEST_ASSERT_EQUAL_UINT64 (0, offsetof (zlink_connection_failure_t, kind));
    TEST_ASSERT_EQUAL_UINT64 (4, offsetof (zlink_connection_failure_t, stage));
    TEST_ASSERT_EQUAL_UINT64 (8, offsetof (zlink_connection_failure_t, normalized_errno));
    TEST_ASSERT_EQUAL_UINT64 (12, offsetof (zlink_connection_failure_t, disconnect_reason));
    TEST_ASSERT_EQUAL_UINT64 (16, sizeof (zlink_connection_failure_t));
    TEST_ASSERT_EQUAL_UINT64 (64, sizeof (zlink_connection_attempt_t));
    TEST_ASSERT_EQUAL_UINT64 (16, offsetof (zlink_connection_attempt_t, intent_id));
    TEST_ASSERT_EQUAL_UINT64 (24, offsetof (zlink_connection_attempt_t, revision));
    TEST_ASSERT_EQUAL_UINT64 (32, offsetof (zlink_connection_attempt_t, last_failure_id));
    TEST_ASSERT_EQUAL_UINT64 (40, offsetof (zlink_connection_attempt_t, state));
    TEST_ASSERT_EQUAL_UINT64 (44, offsetof (zlink_connection_attempt_t, last_failure));
}
void test_refused_recovery_lifetime ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[256];
    size_t len = sizeof endpoint;
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, "tcp://127.0.0.1:*"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_unbind (server, endpoint));
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    TEST_ASSERT_TRUE (ready (client) & ZLINK_POLLCONNECT);
    size_t count = 0, bytes = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_BUFFER_TOO_SMALL,
      zlink_socket_connection_attempts_snapshot (client, NULL, 0, &count, NULL, 0, &bytes));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (endpoint) + 1, bytes);
    TEST_ASSERT_TRUE (ready (client) & ZLINK_POLLCONNECT);
    auto failed = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_REFUSED, failed.last_failure.kind);
    TEST_ASSERT_EQUAL_INT (ECONNREFUSED, failed.last_failure.normalized_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_STAGE_TRANSPORT, failed.last_failure.stage);
    TEST_ASSERT_NOT_EQUAL (0, failed.last_failure_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    auto connected = wait_state (client, ZLINK_CONNECTION_CONNECTED);
    TEST_ASSERT_EQUAL_UINT64 (failed.intent_id, connected.intent_id);
    TEST_ASSERT_NOT_EQUAL (failed.revision, connected.revision);
    TEST_ASSERT_NOT_EQUAL (0, connected.last_failure_id);
    TEST_ASSERT_EQUAL_INT (0, ready (client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_unbind (server, endpoint));
    auto lost = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_CONNECTION_LOST, lost.last_failure.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_STAGE_ESTABLISHED, lost.last_failure.stage);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (client, endpoint));
    TEST_ASSERT_TRUE (ready (client) & ZLINK_POLLCONNECT);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              client, NULL, 0, &count, NULL, 0, &bytes));
    TEST_ASSERT_EQUAL_UINT64 (0, count);
    TEST_ASSERT_EQUAL_INT (0, ready (client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    auto next = snapshot (client);
    TEST_ASSERT_NOT_EQUAL (failed.intent_id, next.intent_id);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}
void test_arguments_and_ipc ()
{
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    size_t n = 0, b = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, zlink_socket_connection_attempts_snapshot (
                                                          client, NULL, 0, NULL, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, zlink_socket_connection_attempts_snapshot (
                                                          client, NULL, 1, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, zlink_socket_connection_attempts_snapshot (
                                                          client, NULL, 0, &n, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, zlink_socket_connection_attempts_snapshot (
                                                          client, NULL, 0, &n, NULL, 1, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, zlink_socket_connection_attempts_snapshot (
                                                          NULL, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              client, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_ARGUMENT,
                           zlink_connect (client, "invalid-endpoint"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              client, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_UINT64 (0, n);
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED, zlink_socket_connection_attempts_snapshot (
                                                         stream, NULL, 0, &n, NULL, 0, &b));
    test_context_socket_close_zero_linger (stream);
    if (zlink_has ("ipc")) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK, zlink_connect (client, "ipc:///tmp/zlink-1710-nonexistent/endpoint"));
        auto row = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_ENDPOINT_MISSING, row.last_failure.kind);
    }
    test_context_socket_close_zero_linger (client);
}
void test_connecting_and_inproc ()
{
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    const char *endpoint = "inproc://connection-attempt-lifecycle";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    auto initial = snapshot (client);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_CONNECTING, initial.state);
    TEST_ASSERT_EQUAL_UINT64 (0, initial.last_failure_id);
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    auto connected = wait_state (client, ZLINK_CONNECTION_CONNECTED);
    TEST_ASSERT_EQUAL_UINT64 (initial.intent_id, connected.intent_id);
    test_context_socket_close_zero_linger (server);
    auto lost = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_CONNECTION_LOST, lost.last_failure.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (client, endpoint));
    size_t n = 1, b = 1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              client, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_UINT64 (0, n);
    test_context_socket_close_zero_linger (client);
}
void test_poller_and_full_monitor ()
{
    if (!zlink_has ("ipc")) {
        TEST_IGNORE_MESSAGE ("IPC is not available on this platform");
    }
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_ALL;
    options.monitor_hwm_bytes = 1;
    void *monitor = zlink_socket_monitor_open (client, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, client, client, ZLINK_POLLCONNECT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (client, "ipc:///tmp/zlink-1710-nonexistent/monitor"));
    zlink_poller_event_t event = {};
    zlink_config_result_t error;
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 1000, &error));
    TEST_ASSERT_TRUE (event.events & ZLINK_POLLCONNECT);
    size_t count, bytes;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_BUFFER_TOO_SMALL,
      zlink_socket_connection_attempts_snapshot (client, NULL, 0, &count, NULL, 0, &bytes));
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 0, &error));
    const auto failure = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_ENDPOINT_MISSING, failure.last_failure.kind);
    // Repeated changes continue to reach the retained row while the monitor is unread.
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    zlink_connection_attempt_t next = {};
    do {
        TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 1000, &error));
        next = snapshot (client);
    } while (next.last_failure_id == failure.last_failure_id
             && std::chrono::steady_clock::now () < end);
    TEST_ASSERT_NOT_EQUAL (failure.last_failure_id, next.last_failure_id);
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
}
void test_snapshot_races_changes ()
{
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    std::atomic<bool> start (false), done (false);
    std::atomic<int> writer_error (0);
    std::thread writer ([&] {
        while (!start.load ())
            std::this_thread::yield ();
        for (int i = 0; i < 32; ++i) {
            const auto connected = zlink_connect (client, "inproc://snapshot-racing");
            if (connected != ZLINK_CONNECT_OK) {
                writer_error.store (connected);
                break;
            }
            if (i != 31) {
                const auto disconnected = zlink_disconnect (client, "inproc://snapshot-racing");
                if (disconnected != ZLINK_CONNECT_OK) {
                    writer_error.store (disconnected);
                    break;
                }
            }
        }
        done.store (true);
    });
    start.store (true);
    size_t n = 0, b = 0;
    zlink_connection_attempt_t row = {};
    char endpoint[128];
    while (!done.load ()) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (client, &row, 1, &n, endpoint,
                                                                      sizeof endpoint, &b));
    }
    writer.join ();
    TEST_ASSERT_EQUAL_INT (0, writer_error.load ());
    // A snapshot racing the last change either observes that intent or leaves its level set.
    const int pending = ready (client);
    const auto final_row = snapshot (client);
    if (n != 1 || row.intent_id != final_row.intent_id) {
        TEST_ASSERT_TRUE (pending & ZLINK_POLLCONNECT);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_CONNECTING, final_row.state);
    TEST_ASSERT_EQUAL_INT (0, ready (client));
    test_context_socket_close_zero_linger (client);
}
void test_tls_auth_failure ()
{
#ifdef ZLINK_HAVE_TLS
    const tls_test_files_t files = make_tls_test_files ();
    void *server = test_context_socket (ZLINK_SOCKET_PUB);
    void *client = test_context_socket (ZLINK_SOCKET_SUB);
    const int zero = 0, one = 1, no_reconnect = -1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL,
                                                              &no_reconnect, sizeof no_reconnect));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &zero, sizeof zero));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (client, ZLINK_OPT_TLS_VERIFY, &one, sizeof one));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (),
                                             files.server_cert.size ()));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (),
                                             files.server_key.size ()));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));
    const char *hostname = "wrong-host.invalid";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME,
                                                              hostname, strlen (hostname)));
    char endpoint[256];
    test_bind (server, "tls://127.0.0.1:*", endpoint, sizeof endpoint);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    auto failed = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_AUTH_FAILED, failed.last_failure.kind);
    TEST_ASSERT_EQUAL_INT (EACCES, failed.last_failure.normalized_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_STAGE_TLS, failed.last_failure.stage);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
    cleanup_tls_test_files (files);
#endif
}
void test_handshake_timeout ()
{
#ifndef ZLINK_HAVE_WINDOWS
    int listener = ::socket (AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE (listener >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT (
      0, ::bind (listener, reinterpret_cast<sockaddr *> (&address), sizeof address));
    TEST_ASSERT_EQUAL_INT (0, ::listen (listener, 1));
    socklen_t length = sizeof address;
    TEST_ASSERT_EQUAL_INT (
      0, getsockname (listener, reinterpret_cast<sockaddr *> (&address), &length));
    char endpoint[64];
    snprintf (endpoint, sizeof endpoint, "tcp://127.0.0.1:%u", ntohs (address.sin_port));
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    const int interval = 50, no_reconnect = -1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (client, ZLINK_OPT_HANDSHAKE_IVL,
                                                              &interval, sizeof interval));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL,
                                                              &no_reconnect, sizeof no_reconnect));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    auto failed = wait_state (client, ZLINK_CONNECTION_ATTEMPT_FAILED);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_FAILURE_TIMED_OUT, failed.last_failure.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECTION_STAGE_CORE_HANDSHAKE, failed.last_failure.stage);
    ::close (listener);
    test_context_socket_close_zero_linger (client);
#endif
}
void test_arena_and_registration_after_change ()
{
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, "inproc://arena-one"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, "inproc://arena-two"));
    size_t n = 0, b = 0;
    zlink_connection_attempt_t rows[2] = {};
    char bytes[128];
    rows[0].intent_id = 123;
    bytes[0] = 'x';
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_BUFFER_TOO_SMALL,
      zlink_socket_connection_attempts_snapshot (client, rows, 2, &n, bytes, 1, &b));
    TEST_ASSERT_EQUAL_UINT64 (2, n);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("inproc://arena-one") + strlen ("inproc://arena-two") + 2, b);
    TEST_ASSERT_EQUAL_UINT64 (123, rows[0].intent_id);
    TEST_ASSERT_EQUAL_INT ('x', bytes[0]);
    TEST_ASSERT_TRUE (ready (client) & ZLINK_POLLCONNECT);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              client, rows, 2, &n, bytes, sizeof bytes, &b));
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE (strcmp (bytes + rows[i].endpoint_offset, "inproc://arena-one") == 0
                          || strcmp (bytes + rows[i].endpoint_offset, "inproc://arena-two") == 0);
        TEST_ASSERT_EQUAL_UINT64 (strlen (bytes + rows[i].endpoint_offset),
                                  rows[i].endpoint_length);
    }
    TEST_ASSERT_NOT_EQUAL (rows[0].intent_id, rows[1].intent_id);
    TEST_ASSERT_EQUAL_INT (0, ready (client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (client, "inproc://arena-one"));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, client, NULL, ZLINK_POLLCONNECT));
    zlink_poller_event_t event = {};
    zlink_config_result_t error;
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 0, &error));
    TEST_ASSERT_TRUE (event.events & ZLINK_POLLCONNECT);
    snapshot (client);
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (client);
}

void rows_for_endpoint (void *socket_, const char *endpoint_, uint32_t state_, uint64_t *ids_)
{
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    do {
        zlink_connection_attempt_t rows[2] = {};
        char bytes[512];
        size_t count = 0, required = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_socket_connection_attempts_snapshot (
                                 socket_, rows, 2, &count, bytes, sizeof bytes, &required));
        TEST_ASSERT_EQUAL_UINT64 (2, count);
        TEST_ASSERT_NOT_EQUAL (rows[0].intent_id, rows[1].intent_id);
        for (size_t i = 0; i != count; ++i)
            TEST_ASSERT_EQUAL_STRING (endpoint_, bytes + rows[i].endpoint_offset);
        if (rows[0].state == state_ && rows[1].state == state_) {
            ids_[0] = rows[0].intent_id;
            ids_[1] = rows[1].intent_id;
            if (ids_[0] > ids_[1])
                std::swap (ids_[0], ids_[1]);
            return;
        }
        zlink_pollitem_t item = {socket_, 0, ZLINK_POLLCONNECT, 0};
        zlink_config_result_t error;
        TEST_ASSERT_TRUE (zlink_poll (&item, 1, 10, &error) >= 0);
    } while (std::chrono::steady_clock::now () < end);
    TEST_FAIL_MESSAGE ("Both same-endpoint intents did not reach the expected state");
}
void assert_no_attempts (void *socket_)
{
    size_t count = 1, bytes = 1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_socket_connection_attempts_snapshot (
                                              socket_, NULL, 0, &count, NULL, 0, &bytes));
    TEST_ASSERT_EQUAL_UINT64 (0, count);
    TEST_ASSERT_EQUAL_UINT64 (0, bytes);
}
void test_same_endpoint_tcp_calls ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PUB);
    void *client = test_context_socket (ZLINK_SOCKET_SUB);
    char endpoint[256];
    test_bind (server, "tcp://127.0.0.1:*", endpoint, sizeof endpoint);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    uint64_t ids[2];
    rows_for_endpoint (client, endpoint, ZLINK_CONNECTION_CONNECTED, ids);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (client, endpoint));
    assert_no_attempts (client);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}
void test_same_endpoint_inproc_reconnects ()
{
    const char *endpoint = "inproc://two-independent-intents";
    void *client = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    uint64_t initial[2];
    rows_for_endpoint (client, endpoint, ZLINK_CONNECTION_CONNECTING, initial);
    void *server = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    uint64_t connected[2];
    rows_for_endpoint (client, endpoint, ZLINK_CONNECTION_CONNECTED, connected);
    TEST_ASSERT_EQUAL_UINT64_ARRAY (initial, connected, 2);
    test_context_socket_close_zero_linger (server);
    uint64_t failed[2];
    rows_for_endpoint (client, endpoint, ZLINK_CONNECTION_ATTEMPT_FAILED, failed);
    server = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    uint64_t recovered[2];
    rows_for_endpoint (client, endpoint, ZLINK_CONNECTION_CONNECTED, recovered);
    TEST_ASSERT_EQUAL_UINT64_ARRAY (initial, recovered, 2);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (client, endpoint));
    assert_no_attempts (client);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_context_shutdown ()
{
    void *context = zlink_ctx_new ();
    void *client = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, "inproc://shutdown-attempt"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
    size_t n, b;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, zlink_socket_connection_attempts_snapshot (
                                                          client, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INTERNAL_ERROR,
                           zlink_connect (client, "inproc://shutdown-attempt"));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

void test_closed_handle ()
{
    void *context = zlink_ctx_new ();
    void *client = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_add (poller, client, NULL, ZLINK_POLLCONNECT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (client));
    size_t n, b;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE, zlink_socket_connection_attempts_snapshot (
                                                         client, NULL, 0, &n, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, client));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
}

}
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_abi);
    RUN_TEST (test_refused_recovery_lifetime);
    RUN_TEST (test_arguments_and_ipc);
    RUN_TEST (test_connecting_and_inproc);
    RUN_TEST (test_poller_and_full_monitor);
    RUN_TEST (test_snapshot_races_changes);
    RUN_TEST (test_tls_auth_failure);
    RUN_TEST (test_handshake_timeout);
    RUN_TEST (test_closed_handle);
    RUN_TEST (test_context_shutdown);
    RUN_TEST (test_same_endpoint_tcp_calls);
    RUN_TEST (test_same_endpoint_inproc_reconnects);
    RUN_TEST (test_arena_and_registration_after_change);
    return UNITY_END ();
}
