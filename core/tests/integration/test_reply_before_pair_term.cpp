/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>

void setUp () {}
void tearDown () {}

namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void reply_before_term (bool registered_, bool completion_first_,
                        bool send_reply_, size_t reply_parts_ = 1,
                        bool data_before_reply_ = false)
{
    void *server_context = zlink_ctx_new ();
    void *client_context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (server_context);
    TEST_ASSERT_NOT_NULL (client_context);
    void *server = zlink_socket (server_context, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (client_context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    void *poller = NULL;
    if (registered_) {
        poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_poller_add (poller, client, NULL, ZLINK_POLLCOMPLETION));
    }
    const int linger = 30000;
    const int receive_timeout = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client, ZLINK_OPT_RCVTIMEO, &receive_timeout, sizeof (receive_timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://127.0.0.1:*"));
    char endpoint[256];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (
      server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    zlink_msg_t request;
    init_part (&request, "request-2981");
    zlink_completion_id_t id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (client, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE,
                     5000, NULL, &id));
    zlink_msg_t incoming;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&incoming));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (server, &source, &token, &incoming, 1, &count,
                         ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT (1, count);
    TEST_ASSERT_NOT_EQUAL (0, token);
    const zlink_routing_id_t target = *source;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&incoming));
    if (data_before_reply_) {
        zlink_msg_t data;
        init_part (&data, "data-before-reply");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_rid (server, &target, &data, 1, ZLINK_SEND_FLAGS_NONE,
                          NULL, NULL));
    }
    if (send_reply_) {
        zlink_msg_t reply[2];
        for (size_t i = 0; i < reply_parts_; ++i)
            init_part (&reply[i], "reply-2981");
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                              zlink_reply (server, &target, token, reply,
                                           reply_parts_));
    }
    if (!completion_first_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (server_context));
    }
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (client, &completion, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
    const bool expect_reply = send_reply_ && !data_before_reply_;
    TEST_ASSERT_EQUAL_INT (expect_reply ? ZLINK_REQUEST_OK
                                     : ZLINK_REQUEST_NOT_CONNECTED,
                           completion.request_result);
    TEST_ASSERT_EQUAL_UINT (expect_reply ? reply_parts_ : 0,
                            completion.reply_part_count);
    for (size_t i = 0; i < completion.reply_part_count; ++i) {
        TEST_ASSERT_EQUAL_UINT (10, zlink_msg_size (&completion.reply_parts[i]));
        TEST_ASSERT_EQUAL_MEMORY ("reply-2981",
                                  zlink_msg_data (&completion.reply_parts[i]), 10);
    }
    zlink_completion_close (&completion);
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (client, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    zlink_completion_close (&completion);
    if (completion_first_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (server_context));
    }
    if (poller)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    const int zero_linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      client, ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (client_context));
}

void test_registered_reply_before_term ()
{
    reply_before_term (true, false, true);
}
void test_completion_before_term ()
{
    reply_before_term (true, true, true);
}
void test_unregistered_reply_before_term ()
{
    reply_before_term (false, false, true);
}
void test_term_without_reply ()
{
    reply_before_term (true, false, false);
}
void test_unregistered_term_without_reply ()
{
    reply_before_term (false, false, false);
}
void test_data_head_blocks_reply_at_term ()
{
    reply_before_term (true, false, true, 1, true);
}
void test_multipart_reply_before_term ()
{
    reply_before_term (true, false, true, 2);
}
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    RUN_TEST (test_registered_reply_before_term);
    RUN_TEST (test_completion_before_term);
    RUN_TEST (test_unregistered_reply_before_term);
    RUN_TEST (test_term_without_reply);
    RUN_TEST (test_unregistered_term_without_reply);
    RUN_TEST (test_multipart_reply_before_term);
    RUN_TEST (test_data_head_blocks_reply_at_term);
    return UNITY_END ();
}
