/* SPDX-License-Identifier: MPL-2.0 */

#include "../integration/zmp_request_reply_fixture.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/msg.hpp"
#include "protocol/zmp_protocol.hpp"
#include "contract_zmp_engine_fixture.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"

SETUP_TEARDOWN_TESTCONTEXT

#if defined ZLINK_HAVE_WS
namespace
{
void assert_request_reply_metadata_hidden (zlink_msg_t *part_)
{
    unsigned char retained_kind = 0;
    uint64_t retained_sequence = 0;
    TEST_ASSERT_FALSE (reinterpret_cast<zlink::msg_t *> (part_)->get_request_reply_metadata (
      &retained_kind, &retained_sequence));
}
struct message_engine_pair_t
{
    message_engine_pair_t (void *client_, void *server_, bool paired_, bool encrypted_) :
        client (client_, true, encrypted_), server (server_, true, encrypted_)
    {
        client.handshake (paired_ ? test_zmp_wire::socket_router : test_zmp_wire::socket_pair,
                          paired_ ? "zmp-ws-router" : "");
        server.handshake (paired_ ? test_zmp_wire::socket_dealer : test_zmp_wire::socket_pair,
                          paired_ ? "zmp-ws-dealer" : "");
        client.state->outgoing.clear ();
        server.state->outgoing.clear ();
        TEST_ASSERT_TRUE (client.core->acquire_completion_poller (this));
    }
    ~message_engine_pair_t () { client.core->release_completion_poller (this); }
    static void progress (void *data_)
    {
        message_engine_pair_t *pair = static_cast<message_engine_pair_t *> (data_);
        bool work;
        do {
            work = pair->client.transfer_to (pair->server);
            work = pair->server.transfer_to (pair->client) || work;
        } while (work);
        zlink::socket_reqrep_internal::completion_discard_t discard;
        {
            zlink::completion_drain_scope_t owner (pair->client.core,
                                                    &discard);
            pair->client.core->process_ready_completion_pipes ();
        }
        zlink::socket_reqrep_internal::release_completion_discard (&discard);
    }
    contract_zmp_engine_t client, server;
};

void assert_pair_has_no_message (void *server_)
{
    unsigned char received[64];
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (server_, received, sizeof (received), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}

void send_invalid_middle_metadata_record (void *sender_,
                                          void *receiver_,
                                          message_engine_pair_t &pair_)
{
    zlink_msg_t parts[4];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], 7));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[2], 7));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[3], 5));
    memcpy (zlink_msg_data (&parts[0]), "first", 5);
    memcpy (zlink_msg_data (&parts[1]), "invalid", 7);
    memcpy (zlink_msg_data (&parts[2]), "invalid", 7);
    memcpy (zlink_msg_data (&parts[3]), "final", 5);
    TEST_ASSERT_SUCCESS_ERRNO (reinterpret_cast<zlink::msg_t *> (&parts[1])
                                 ->set_request_reply_metadata (zlink::zmp_kind_request, 91));
    TEST_ASSERT_SUCCESS_ERRNO (reinterpret_cast<zlink::msg_t *> (&parts[2])
                                 ->set_request_reply_metadata (zlink::zmp_kind_reply, 92));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_socket_send_internal (
      sender_, parts, 4, ZLINK_SEND_FLAGS_NONE));
    zlink_multipart_close (parts, 4);

    // The first frame has already established encoder multipart state. A
    // batching fallback could discard both invalid continuations, encode the
    // ordinary final frame, and publish a spliced [first, final] record. The
    // message transport must instead close at the first invalid header.
    message_engine_pair_t::progress (&pair_);
    TEST_ASSERT_FALSE (pair_.client.state->opened);
    TEST_ASSERT_TRUE (pair_.client.state->outgoing.empty ());
    assert_pair_has_no_message (receiver_);
}

}

void run_pair_message_metadata (bool encrypted_, const char *payload_)
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    {
        message_engine_pair_t pair (client, server, false, encrypted_);
        send_string_expect_success (client, payload_, 0);
        message_engine_pair_t::progress (&pair);
        recv_string_expect_success (server, payload_, ZLINK_DONTWAIT);
        send_invalid_middle_metadata_record (client, server, pair);
    }
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void run_request_reply_metadata (bool encrypted_, const char *request_, const char *reply_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "zmp-ws-dealer", 13));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "zmp-ws-router", 13));
    {
        message_engine_pair_t pair (client, server, true, encrypted_);
        exercise_request_reply (server, client, NULL, request_, reply_,
                                &assert_request_reply_metadata_hidden,
                                &message_engine_pair_t::progress, &pair);
    }
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_zmp_ws_pair_message_metadata ()
{
    run_pair_message_metadata (false, "ws-zmp");
}
void test_zmp_ws_request_reply_metadata ()
{
    run_request_reply_metadata (false, "ws-request", "ws-reply");
}
#if defined ZLINK_HAVE_WSS
void test_zmp_wss_pair_message_metadata ()
{
    run_pair_message_metadata (true, "wss-zmp");
}
void test_zmp_wss_request_reply_metadata ()
{
    run_request_reply_metadata (true, "wss-request", "wss-reply");
}
#endif
#endif

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#if defined ZLINK_HAVE_WS
    RUN_TEST (test_zmp_ws_pair_message_metadata);
    RUN_TEST (test_zmp_ws_request_reply_metadata);
#if defined ZLINK_HAVE_WSS
    RUN_TEST (test_zmp_wss_pair_message_metadata);
    RUN_TEST (test_zmp_wss_request_reply_metadata);
#endif
#endif
    return UNITY_END ();
}
