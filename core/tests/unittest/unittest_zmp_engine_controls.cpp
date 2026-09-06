/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "contract_zmp_engine_fixture.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/dealer/dealer.hpp"
#include "sockets/router/router.hpp"

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
using test_zmp_wire::append_wire_frame;
using test_zmp_wire::make_zmp_wire_frame;

uint64_t request_sequence (contract_zmp_engine_t &engine_)
{
    engine_.pump ();
    const std::vector<contract_zmp_wire_frame_t> frames = contract_zmp_take_output (*engine_.state);
    for (size_t i = 0; i != frames.size (); ++i) {
        const contract_zmp_wire_frame_t &frame = frames[i];
        if (frame.kind == test_zmp_wire::zmp_kind_request
            && !(frame.flags
                 & (test_zmp_wire::zmp_flag_control | test_zmp_wire::zmp_flag_identity))) {
            TEST_ASSERT_TRUE (frame.sequence != 0);
            return frame.sequence;
        }
    }
    TEST_FAIL_MESSAGE ("request metadata was not emitted");
    return 0;
}

void assert_ready_reply (contract_zmp_engine_t &engine_)
{
    engine_.pump ();
    const std::vector<contract_zmp_wire_frame_t> frames = contract_zmp_take_output (*engine_.state);
    bool ready = false;
    for (size_t i = 0; i != frames.size (); ++i)
        if ((frames[i].flags & test_zmp_wire::zmp_flag_control) && !frames[i].body.empty ()
            && frames[i].body[0] == test_zmp_wire::zmp_control_ready)
            ready = true;
    TEST_ASSERT_TRUE (ready);
}

zlink::pipe_t *application_pipe (contract_zmp_engine_t &engine_, bool ready_)
{
    TEST_ASSERT_TRUE (engine_.alive);
    const uint64_t pair = engine_.session->transport_pair_id ();
    const uint64_t generation = engine_.session->transport_pair_generation ();
    TEST_ASSERT_TRUE (pair != 0);
    TEST_ASSERT_TRUE (generation != 0);
    TEST_ASSERT_EQUAL_INT (ready_, engine_.core->test_pair_is_ready (pair, generation));
    zlink::pipe_t *pipe = engine_.core->test_pair_pipe (pair, generation, false);
    TEST_ASSERT_NOT_NULL (pipe);
    return pipe;
}

void assert_no_payload (void *socket_)
{
    unsigned char data[16];
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (socket_, data, sizeof (data), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}

zlink_completion_t completion_now (void *socket_)
{
    zlink::socket_base_t *core = as_socket_handle (socket_).socket;
    zlink::completion_drain_scope_t owner (core);
    core->process_ready_completion_pipes ();
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    return completion;
}

void assert_no_completion (void *socket_)
{
    zlink::socket_base_t *core = as_socket_handle (socket_).socket;
    zlink::completion_drain_scope_t owner (core);
    core->process_ready_completion_pipes ();
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    zlink_completion_close (&completion);
}
}

void test_raw_wire_peer_weight_bypasses_application_limit_and_consumes_malformed ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const int64_t max_message_size = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_MAXMSGSIZE, &max_message_size,
                                                 sizeof (max_message_size)));
    {
        contract_zmp_engine_t application (dealer);
        application.handshake (test_zmp_wire::socket_router, "raw-weight-limit-router");
        assert_ready_reply (application);
        zlink::pipe_t *pipe = application_pipe (application, true);
        zlink::dealer_t *owner = static_cast<zlink::dealer_t *> (application.core);
        TEST_ASSERT_EQUAL_UINT32 (100, owner->test_peer_weight (pipe));
        const unsigned char malformed[] = {'W', 'E', 'I', 'G', 'H', 'T'};
        application.feed (test_zmp_wire::control_frame (
          std::vector<unsigned char> (malformed, malformed + sizeof (malformed))));
        const unsigned char barrier[] = {'x'};
        application.feed (
          make_zmp_wire_frame (0, test_zmp_wire::zmp_kind_data, 0, barrier, sizeof (barrier)));
        unsigned char received[4];
        TEST_ASSERT_EQUAL_INT (1, zlink_recv (dealer, received, sizeof (received), ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT8 ('x', received[0]);
        TEST_ASSERT_EQUAL_UINT32 (100, owner->test_peer_weight (pipe));
        assert_no_payload (dealer);
        const unsigned char valid[] = {'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 7};
        application.feed (test_zmp_wire::control_frame (
          std::vector<unsigned char> (valid, valid + sizeof (valid))));
        application.feed (
          make_zmp_wire_frame (0, test_zmp_wire::zmp_kind_data, 0, barrier, sizeof (barrier)));
        TEST_ASSERT_EQUAL_INT (1, zlink_recv (dealer, received, sizeof (received), ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT8 ('x', received[0]);
        TEST_ASSERT_EQUAL_UINT32 (7, owner->test_peer_weight (pipe));
        assert_no_payload (dealer);
    }
    test_context_socket_close_zero_linger (dealer);
}

void test_raw_wire_peer_weight_waits_for_exact_pair_readiness ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    {
        contract_zmp_monitor_t monitor (router, ZLINK_EVENT_PEER_WEIGHT_CHANGED);
        contract_zmp_engine_t application (router);
        application.handshake (test_zmp_wire::socket_router, "raw-weight-gated-router", 2, 0);
        assert_ready_reply (application);
        const unsigned char valid[] = {'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 41};
        application.feed (test_zmp_wire::control_frame (
          std::vector<unsigned char> (valid, valid + sizeof (valid))));
        zlink::pipe_t *pipe = application_pipe (application, false);
        uint32_t cached_weight = 0;
        TEST_ASSERT_TRUE (pipe->peer_weight (&cached_weight));
        TEST_ASSERT_EQUAL_UINT32 (41, cached_weight);
        zlink::router_t *owner = static_cast<zlink::router_t *> (application.core);
        TEST_ASSERT_EQUAL_UINT32 (0, owner->test_peer_weight (pipe));
        zlink::socket_monitor_event_record_t record;
        TEST_ASSERT_FALSE (monitor.next (&record));
        contract_zmp_engine_t completion (router);
        completion.handshake (test_zmp_wire::socket_router, "raw-weight-gated-router", 2, 1);
        assert_ready_reply (completion);
        application.pump ();
        TEST_ASSERT_EQUAL_PTR (pipe, application_pipe (application, true));
        TEST_ASSERT_EQUAL_UINT32 (41, owner->test_peer_weight (pipe));
        TEST_ASSERT_FALSE (monitor.next (&record));
        const zlink_routing_id_t *rid = NULL;
        uint64_t sequence = 0;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                               zlink_router_recv_part (router, &rid, &sequence, &part, &more,
                                                       ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }
    test_context_socket_close_zero_linger (router);
}

void test_stale_application_connection_cannot_complete_reconnected_request ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    {
        contract_zmp_engine_t old_application (server);
        old_application.handshake (test_zmp_wire::socket_router, "raw-reconnect-peer");
        assert_ready_reply (old_application);
        TEST_ASSERT_TRUE (old_application.core->acquire_completion_poller (&old_application));
        zlink::pipe_t *old_pipe = application_pipe (old_application, true);
        const uint64_t old_pair = old_application.session->transport_pair_id ();
        const uint64_t old_generation = old_application.session->transport_pair_generation ();
        zlink_msg_t first_request;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&first_request, 1));
        zlink_completion_id_t first_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, zlink_request_part (server, NULL, &first_request, ZLINK_SEND_FLAGS_NONE,
                                               ZLINK_PART_FINAL, 250, NULL, &first_id));
        TEST_ASSERT_TRUE (first_id != 0);
        TEST_ASSERT_TRUE (request_sequence (old_application) != 0);
        // Keep the already installed read callback to deliver the stale result
        // after the real engine and transport have been retired.
        zlink::i_asio_transport::completion_handler_t stale_read =
          old_application.state->read_handler;
        TEST_ASSERT_TRUE (bool (stale_read));
        old_pipe->terminate (false);
        old_application.pump ();
        zlink_completion_t first = completion_now (server);
        TEST_ASSERT_EQUAL_UINT64 (first_id, first.completion_id);
        zlink_completion_close (&first);
        TEST_ASSERT_FALSE (old_application.state->opened);
        contract_zmp_engine_t application (server);
        application.handshake (test_zmp_wire::socket_router, "raw-reconnect-peer");
        assert_ready_reply (application);
        application_pipe (application, true);
        TEST_ASSERT_TRUE (application.session->transport_pair_id () != old_pair
                          || application.session->transport_pair_generation () != old_generation);
        zlink_msg_t second_request;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&second_request, 1));
        zlink_completion_id_t second_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, zlink_request_part (server, NULL, &second_request, ZLINK_SEND_FLAGS_NONE,
                                               ZLINK_PART_FINAL, 5000, NULL, &second_id));
        TEST_ASSERT_TRUE (second_id != 0);
        const uint64_t sequence = request_sequence (application);
        // The weak engine lifetime guard must reject even a successful old
        // read completion. Its old input buffer has already been released.
        stale_read (boost::system::error_code (), 16);
        old_application.pump ();
        assert_no_completion (server);
        application.feed (
          make_zmp_wire_frame (0, test_zmp_wire::zmp_kind_reply, sequence, NULL, 0));
        zlink_completion_t second = completion_now (server);
        TEST_ASSERT_EQUAL_UINT64 (second_id, second.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, second.request_result);
        zlink_completion_close (&second);
        old_application.core->release_completion_poller (&old_application);
    }
    test_context_socket_close_zero_linger (server);
}

void test_error_reply_payload_export_allocation_failure_is_payloadless ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    {
        contract_zmp_engine_t application (server);
        application.handshake (test_zmp_wire::socket_router, "raw-error-reply");
        assert_ready_reply (application);
        application_pipe (application, true);
        TEST_ASSERT_TRUE (application.core->acquire_completion_poller (&application));
        zlink_msg_t request;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 1));
        *static_cast<unsigned char *> (zlink_msg_data (&request)) = 'q';
        int user_context = 17;
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                               zlink_request_part (server, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                                                   ZLINK_PART_FINAL, 5000, &user_context, &id));
        TEST_ASSERT_TRUE (id != 0);
        const uint64_t sequence = request_sequence (application);
        unsigned char error[4];
        test_zmp_wire::put_uint32 (error, EACCES);
        std::vector<unsigned char> record =
          make_zmp_wire_frame (test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_error_reply,
                               sequence, error, sizeof (error));
        append_wire_frame (
          &record, make_zmp_wire_frame (test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_data,
                                        0, reinterpret_cast<const unsigned char *> ("detail"), 6));
        append_wire_frame (
          &record, make_zmp_wire_frame (0, test_zmp_wire::zmp_kind_data, 0,
                                        reinterpret_cast<const unsigned char *> ("context"), 7));
        zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
          zlink::socket_reqrep_internal::request_reply_allocation_payload_export);
        application.feed (record);
        zlink_completion_t result = completion_now (server);
        TEST_ASSERT_EQUAL_UINT64 (id, result.completion_id);
        TEST_ASSERT_EQUAL_PTR (&user_context, result.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_INTERNAL_ERROR, result.request_result);
        TEST_ASSERT_EQUAL_UINT64 (0, result.reply_part_count);
        TEST_ASSERT_NULL (result.reply_parts);
        zlink_completion_close (&result);
        zlink_completion_close (&result);
        assert_no_completion (server);
        application.core->release_completion_poller (&application);
    }
    test_context_socket_close_zero_linger (server);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_raw_wire_peer_weight_bypasses_application_limit_and_consumes_malformed);
    RUN_TEST (test_raw_wire_peer_weight_waits_for_exact_pair_readiness);
    RUN_TEST (test_stale_application_connection_cannot_complete_reconnected_request);
    RUN_TEST (test_error_reply_payload_export_allocation_failure_is_payloadless);
    return UNITY_END ();
}
