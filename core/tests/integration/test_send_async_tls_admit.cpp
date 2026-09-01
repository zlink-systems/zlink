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

void run_case (const char *transport_, int settle_ms_)
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

    char endpoint[MAX_SOCKET_STRING];
    test_bind (server, is_tls ? "tls://127.0.0.1:*" : "tcp://127.0.0.1:*",
               endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    if (settle_ms_ > 0)
        msleep (settle_ms_);

    std::string payload (payload_size, 'x');
    zlink_submit_result_t submit = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_completion_id_t completion_id = 0;
    int completion_context = 91;
    for (int i = 0; i != completion_wait_ms / 5; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, payload.size ()));
        memcpy (zlink_msg_data (&part), payload.data (), payload.size ());
        submit = zlink_send_part (client, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                                  ZLINK_PART_FINAL, &completion_context,
                                  &completion_id);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        if (submit == ZLINK_SUBMIT_OK)
            break;
        TEST_ASSERT_TRUE (submit == ZLINK_SUBMIT_NOT_CONNECTED
                          || submit == ZLINK_SUBMIT_BACKPRESSURED);
        msleep (5);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit);

    if (completion_id != 0) {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        zlink_recv_result_t recv_rc = ZLINK_RECV_NO_DATA;
        for (int waited = 0;
             waited < completion_wait_ms && recv_rc == ZLINK_RECV_NO_DATA;
             waited += 5) {
            recv_rc = zlink_completion_recv (
              client, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_rc == ZLINK_RECV_NO_DATA)
                msleep (5);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE (
          ZLINK_RECV_OK, recv_rc,
          "pending send completion never became pull-readable");
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&completion_context, completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        zlink_completion_close (&completion);
    }

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
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
    if (is_tls)
        cleanup_tls_test_files (tls_files);
}
}

void test_pending_send_admits_over_tcp ()
{
    run_case ("tcp", SETTLE_TIME);
}

void test_pending_send_admits_over_tls ()
{
    run_case ("tls", SETTLE_TIME);
}

void test_pending_send_admits_over_tls_during_handshake ()
{
    run_case ("tls", 0);
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    if (selected ("test_pending_send_admits_over_tcp"))
        RUN_TEST (test_pending_send_admits_over_tcp);
    if (selected ("test_pending_send_admits_over_tls"))
        RUN_TEST (test_pending_send_admits_over_tls);
    if (selected ("test_pending_send_admits_over_tls_during_handshake"))
        RUN_TEST (test_pending_send_admits_over_tls_during_handshake);
    return UNITY_END ();
}
