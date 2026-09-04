/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const size_t payload_size = 1024;
const int completion_wait_ms = 10000;

bool selected (const char *name_)
{
    const char *value = getenv ("ZLINK_TEST_CASE");
    return !value || !*value || strcmp (value, name_) == 0;
}

bool tls_available ()
{
    return zlink_has ("tls") != 0;
}

void configure_tls (void *server_, void *client_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (),
      files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (),
      files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (), files_.ca_cert.size ()));
    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client_, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
}

void assert_no_completion (void *socket_)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);
}

void receive_writable_completion (void *socket_,
                                  zlink_completion_id_t expected_id_,
                                  void *expected_context_)
{
    zlink_pollitem_t writable = {socket_, 0, ZLINK_POLLOUT, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poll (&writable, 1, completion_wait_ms, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);

    writable.revents = 0;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&writable, 1, 0, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (expected_context_, completion.user_context);
    TEST_ASSERT_EQUAL_UINT8 (0, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    zlink_completion_close (&completion);
}

void run_case (const char *transport_)
{
    const bool is_tls = strcmp (transport_, "tls") == 0;
    if (is_tls && !tls_available ())
        TEST_IGNORE_MESSAGE ("tls transport not available");

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    tls_test_files_t tls_files;
    if (is_tls) {
        tls_files = make_tls_test_files ();
        configure_tls (server, client, tls_files);
    }

    const int immediate = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client, ZLINK_OPT_IMMEDIATE, &immediate, sizeof (immediate)));

    char endpoint[MAX_SOCKET_STRING];
    fd_t reserved = bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    close (reserved);
    if (is_tls)
        memcpy (endpoint, "tls", 3);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    std::string payload (payload_size, 'x');
    int wait_context = 91;
    zlink_msg_t rejected;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&rejected, payload.size ()));
    memcpy (zlink_msg_data (&rejected), payload.data (), payload.size ());
    zlink_completion_id_t wait_token = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_part (client, &rejected, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, &wait_context, &wait_token));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, wait_token);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&rejected));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&rejected));
    TEST_ASSERT_EQUAL_UINT64 (payload_size, payload.size ());
    assert_no_completion (client);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (server, endpoint));
    receive_writable_completion (client, wait_token, &wait_context);

    zlink_msg_t retry;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&retry, payload.size ()));
    memcpy (zlink_msg_data (&retry), payload.data (), payload.size ());
    zlink_completion_id_t retry_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (client, &retry, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &retry_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retry_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&retry));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&retry));
    assert_no_completion (client);

    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (server, &source, &token, &received, &has_more,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT (payload_size, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY (payload.data (), zlink_msg_data (&received),
                              payload.size ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
    if (is_tls)
        cleanup_tls_test_files (tls_files);
}
}

void test_writable_token_retries_over_tcp ()
{
    run_case ("tcp");
}

void test_writable_token_retries_over_tls ()
{
    run_case ("tls");
}

void test_writable_token_retries_over_tls_during_handshake ()
{
    run_case ("tls");
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    if (selected ("test_writable_token_retries_over_tcp"))
        RUN_TEST (test_writable_token_retries_over_tcp);
    if (selected ("test_writable_token_retries_over_tls"))
        RUN_TEST (test_writable_token_retries_over_tls);
    if (selected ("test_writable_token_retries_over_tls_during_handshake"))
        RUN_TEST (test_writable_token_retries_over_tls_during_handshake);
    return UNITY_END ();
}
