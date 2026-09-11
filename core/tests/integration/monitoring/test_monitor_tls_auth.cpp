/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
#if defined ZLINK_HAVE_TLS || defined ZLINK_HAVE_WSS
void run_tls_auth_failure (const char *bind_address_, bool missing_client_cert_)
{
    const tls_test_files_t files = make_tls_test_files ();
    void *server = test_context_socket (ZLINK_SOCKET_PUB);
    void *client = test_context_socket (ZLINK_SOCKET_SUB);

    const int zero = 0;
    const int one = 1;
    const int no_reconnect = -1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &no_reconnect, sizeof (no_reconnect)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_VERIFY, &one, sizeof (one)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (), files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));
    const char *hostname = missing_client_cert_ ? "localhost" : "wrong-host.invalid";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
    if (missing_client_cert_) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_TLS_TRUST_SYSTEM, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          server, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          server, ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT, &one, sizeof (one)));
    }

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    monitor_options.events = ZLINK_EVENT_HANDSHAKE_FAILED_AUTH
                             | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                             | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                             | ZLINK_EVENT_DISCONNECTED;
    // Observe the endpoint that performs the failing certificate check.
    void *monitor = zlink_socket_monitor_open (
      missing_client_cert_ ? server : client, &monitor_options);
    TEST_ASSERT_NOT_NULL (monitor);

    char endpoint[MAX_SOCKET_STRING];
    test_bind (server, bind_address_, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    unsigned int auth_count = 0;
    unsigned int no_detail_count = 0;
    unsigned int protocol_count = 0;
    uint64_t auth_value = 0;
    uint64_t disconnect_value = 0;
    bool disconnected = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_monitor_event_t event;
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        switch (event.event) {
            case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
                ++auth_count;
                auth_value = event.value;
                break;
            case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
                ++no_detail_count;
                break;
            case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
                ++protocol_count;
                break;
            case ZLINK_EVENT_DISCONNECTED:
                disconnected = true;
                disconnect_value = event.value;
                break;
            default:
                TEST_FAIL_MESSAGE ("Unexpected event in TLS authentication monitor");
        }
    }

    // Observe the complete, bounded window with reconnect disabled. An
    // outgoing pre-handshake pipe can still lack a transport connection id,
    // which independently suppresses DISCONNECTED; A7 tests AUTH emission.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
    cleanup_tls_test_files (files);

    TEST_ASSERT_EQUAL_UINT (1, auth_count);
    TEST_ASSERT_EQUAL_UINT64 (EACCES, auth_value);
    TEST_ASSERT_EQUAL_UINT (0, no_detail_count);
    TEST_ASSERT_EQUAL_UINT (0, protocol_count);
    if (disconnected)
        TEST_ASSERT_EQUAL_UINT64 (ZLINK_DISCONNECT_REASON_HANDSHAKE_FAILED, disconnect_value);
}
#endif

void test_tls_hostname_verify_emits_auth_once ()
{
#if defined ZLINK_HAVE_TLS
    run_tls_auth_failure ("tls://127.0.0.1:*", false);
#else
    TEST_IGNORE_MESSAGE ("TLS not available");
#endif
}

void test_tls_missing_client_cert_emits_auth_once ()
{
#if defined ZLINK_HAVE_TLS
    run_tls_auth_failure ("tls://127.0.0.1:*", true);
#else
    TEST_IGNORE_MESSAGE ("TLS not available");
#endif
}

void test_wss_hostname_verify_emits_auth_once ()
{
#if defined ZLINK_HAVE_WSS
    run_tls_auth_failure ("wss://127.0.0.1:*", false);
#else
    TEST_IGNORE_MESSAGE ("WSS not available");
#endif
}

void test_wss_missing_client_cert_emits_auth_once ()
{
#if defined ZLINK_HAVE_WSS
    run_tls_auth_failure ("wss://127.0.0.1:*", true);
#else
    TEST_IGNORE_MESSAGE ("WSS not available");
#endif
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_tls_hostname_verify_emits_auth_once);
    RUN_TEST (test_tls_missing_client_cert_emits_auth_once);
    RUN_TEST (test_wss_hostname_verify_emits_auth_once);
    RUN_TEST (test_wss_missing_client_cert_emits_auth_once);
    return UNITY_END ();
}
