/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "utils/config.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

#if defined ZLINK_HAVE_WS
void test_zmp_ws_pair_message ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "ws://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "ws-zmp", 0);
    recv_string_expect_success (server, "ws-zmp", 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

#if defined ZLINK_HAVE_WSS
void test_zmp_wss_pair_message ()
{
    const tls_test_files_t files = make_tls_test_files ();

    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (), files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));

    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "wss://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "wss-zmp", 0);
    recv_string_expect_success (server, "wss-zmp", 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
    cleanup_tls_test_files (files);
}
#endif // ZLINK_HAVE_WSS
#endif // ZLINK_HAVE_WS

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

#if defined ZLINK_HAVE_WS
    RUN_TEST (test_zmp_ws_pair_message);
#if defined ZLINK_HAVE_WSS
    RUN_TEST (test_zmp_wss_pair_message);
#endif
#else
    TEST_IGNORE_MESSAGE ("WebSocket support not enabled");
#endif

    return UNITY_END ();
}
