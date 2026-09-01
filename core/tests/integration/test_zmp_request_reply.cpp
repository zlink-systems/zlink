/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>
#include <map>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
bool selected (const char *name_)
{
    const char *value = getenv ("ZLINK_TEST_CASE");
    return !value || !*value || strcmp (value, name_) == 0;
}

struct received_request_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    std::string payload;
};

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

received_request_t receive_request (void *router_)
{
    received_request_t result;
    memset (&result.source_rid, 0, sizeof (result.source_rid));
    result.reply_token = 0;

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source_rid, &result.reply_token,
                              &part, &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_NOT_EQUAL (0, result.reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    result.source_rid = *source_rid;
    result.payload.assign (
      static_cast<const char *> (zlink_msg_data (&part)),
      zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    return result;
}

zlink_completion_t receive_completion (void *socket_)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    zlink_recv_result_t rc = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < 5000 && rc == ZLINK_RECV_NO_DATA;
         ++attempt) {
        rc = zlink_completion_recv (socket_, &completion,
                                    ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    return completion;
}

void configure_tls (void *router_, void *dealer_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system,
      sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (),
      files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (),
      files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (),
      files_.ca_cert.size ()));
    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer_, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
}

void run_transport_round_trip (const char *transport_)
{
    const bool is_tls = strcmp (transport_, "tls") == 0;
    if (is_tls && zlink_has ("tls") == 0)
        TEST_IGNORE_MESSAGE ("tls transport not available");

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, "transport-dealer", 16));

    tls_test_files_t tls_files;
    if (is_tls) {
        tls_files = make_tls_test_files ();
        configure_tls (router, dealer, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    std::string bind_endpoint = std::string (transport_) + "://127.0.0.1:*";
    test_bind (router, bind_endpoint.c_str (), endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * (is_tls ? 5 : 2));

    int request_context = 33;
    zlink_msg_t request;
    init_part (&request, std::string (transport_) + "-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 5000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);

    const received_request_t received = receive_request (router);
    TEST_ASSERT_EQUAL_STRING (
      (std::string (transport_) + "-request").c_str (),
      received.payload.c_str ());
    TEST_ASSERT_EQUAL_UINT (16, received.source_rid.size);
    TEST_ASSERT_EQUAL_MEMORY ("transport-dealer", received.source_rid.data,
                              received.source_rid.size);

    zlink_msg_t reply;
    init_part (&reply, std::string (transport_) + "-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &received.source_rid, received.reply_token,
                        &reply, ZLINK_PART_FINAL));

    zlink_completion_t completion = receive_completion (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      (std::string (transport_) + "-reply").c_str (),
      static_cast<const char *> (zlink_msg_data (&completion.reply_parts[0])),
      zlink_msg_size (&completion.reply_parts[0]));
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
    if (is_tls)
        cleanup_tls_test_files (tls_files);
}
}

void test_request_reply_over_tcp_with_explicit_routing_id ()
{
    run_transport_round_trip ("tcp");
}

void test_request_reply_over_tls_with_explicit_routing_id ()
{
    run_transport_round_trip ("tls");
}

void test_out_of_order_replies_match_completion_ids ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://request-out-of-order-completions"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://request-out-of-order-completions"));
    msleep (SETTLE_TIME);

    std::map<std::string, zlink_completion_id_t> ids;
    for (int i = 0; i < 2; ++i) {
        const std::string payload = i == 0 ? "first" : "second";
        zlink_msg_t request;
        init_part (&request, payload);
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                              ZLINK_PART_FINAL, 3000, NULL, &id));
        ids[payload] = id;
    }

    const received_request_t first = receive_request (router);
    const received_request_t second = receive_request (router);
    TEST_ASSERT_EQUAL_STRING ("first", first.payload.c_str ());
    TEST_ASSERT_EQUAL_STRING ("second", second.payload.c_str ());

    zlink_msg_t second_reply;
    init_part (&second_reply, "reply-second");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &second.source_rid, second.reply_token,
                        &second_reply, ZLINK_PART_FINAL));
    zlink_msg_t first_reply;
    init_part (&first_reply, "reply-first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &first.source_rid, first.reply_token,
                        &first_reply, ZLINK_PART_FINAL));

    zlink_completion_t completion = receive_completion (dealer);
    TEST_ASSERT_EQUAL_UINT64 (ids["second"], completion.completion_id);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "reply-second",
      static_cast<const char *> (zlink_msg_data (&completion.reply_parts[0])),
      12);
    zlink_completion_close (&completion);
    completion = receive_completion (dealer);
    TEST_ASSERT_EQUAL_UINT64 (ids["first"], completion.completion_id);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "reply-first",
      static_cast<const char *> (zlink_msg_data (&completion.reply_parts[0])),
      11);
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    if (selected ("test_request_reply_over_tcp_with_explicit_routing_id"))
        RUN_TEST (test_request_reply_over_tcp_with_explicit_routing_id);
    if (selected ("test_request_reply_over_tls_with_explicit_routing_id"))
        RUN_TEST (test_request_reply_over_tls_with_explicit_routing_id);
    if (selected ("test_out_of_order_replies_match_completion_ids"))
        RUN_TEST (test_out_of_order_replies_match_completion_ids);
    return UNITY_END ();
}
