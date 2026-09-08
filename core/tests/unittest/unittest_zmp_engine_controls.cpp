/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "contract_zmp_engine_fixture.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/dealer/dealer.hpp"
#include "sockets/router/router.hpp"
#include "transports/ws/ws_batch_policy.hpp"

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
    zlink::socket_reqrep_internal::completion_discard_t discard;
    {
        zlink::completion_drain_scope_t owner (core, &discard);
        core->process_ready_completion_pipes ();
    }
    zlink::socket_reqrep_internal::release_completion_discard (&discard);
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
    zlink::socket_reqrep_internal::completion_discard_t discard;
    {
        zlink::completion_drain_scope_t owner (core, &discard);
        core->process_ready_completion_pipes ();
    }
    zlink::socket_reqrep_internal::release_completion_discard (&discard);
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    zlink_completion_close (&completion);
}

void assert_ws_batch_operations (const std::vector<size_t> &body_sizes_,
                                 const std::vector<size_t> &buffer_counts_)
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    {
        contract_zmp_engine_t application (
          dealer, true, false, zlink::ws_batch_policy::zmp_send_batch_size ());
        application.handshake (test_zmp_wire::socket_router, "bounded-batch-router");
        assert_ready_reply (application);
        application_pipe (application, true);
        application.session->test_output_enabled = true;

        application.engine->test_set_stream_encoder_write_target_size (
          zlink::ws_batch_policy::zmp_send_batch_max_size ());
        TEST_ASSERT_EQUAL_UINT64 (
          128 * 1024,
          application.engine->stream_encoder_write_target_size ());
        application.state->outgoing.clear ();
        application.state->outgoing_buffer_counts.clear ();
        application.state->hold_writes = true;

        for (size_t i = 0; i != body_sizes_.size (); ++i) {
            zlink::msg_t message;
            TEST_ASSERT_SUCCESS_ERRNO (message.init_size (body_sizes_[i]));
            if (body_sizes_[i] != 0)
                memset (message.data (), static_cast<int> ('a' + i), body_sizes_[i]);
            if (i + 1 != body_sizes_.size ())
                message.set_flags (zlink::msg_t::more);
            application.session->queue_test_output (message);
            TEST_ASSERT_SUCCESS_ERRNO (message.close ());
        }
        application.engine->restart_output ();
        application.pump ();

        size_t completion_rounds = 0;
        while (!application.state->writes.empty ()) {
            TEST_ASSERT_LESS_THAN_UINT (body_sizes_.size () + 3,
                                        ++completion_rounds);
            application.state->drain_writes ();
            application.pump ();
        }

        TEST_ASSERT_EQUAL_UINT (buffer_counts_.size (),
                                application.state->outgoing.size ());
        TEST_ASSERT_EQUAL_UINT (buffer_counts_.size (),
                                application.state->outgoing_buffer_counts.size ());
        for (size_t i = 0; i != buffer_counts_.size (); ++i)
            TEST_ASSERT_EQUAL_UINT (buffer_counts_[i],
                                    application.state->outgoing_buffer_counts[i]);

        const std::vector<contract_zmp_wire_frame_t> frames =
          contract_zmp_take_output (*application.state);
        TEST_ASSERT_EQUAL_UINT (body_sizes_.size (), frames.size ());
        for (size_t i = 0; i != body_sizes_.size (); ++i) {
            TEST_ASSERT_EQUAL_UINT (body_sizes_[i], frames[i].body.size ());
            if (body_sizes_[i] != 0) {
                TEST_ASSERT_EQUAL_UINT8 (static_cast<unsigned char> ('a' + i),
                                         frames[i].body.front ());
                TEST_ASSERT_EQUAL_UINT8 (static_cast<unsigned char> ('a' + i),
                                         frames[i].body.back ());
            }
        }
    }
    test_context_socket_close_zero_linger (dealer);
}
}

void test_ws_batch_64k_then_small_is_one_three_buffer_write ()
{
    assert_ws_batch_operations ({64 * 1024, 64}, {3});
}

void test_ws_batch_small_64k_small_is_one_three_buffer_write ()
{
    assert_ws_batch_operations ({64, 64 * 1024, 64}, {3});
}

void test_ws_batch_over_max_uses_two_encoder_writes ()
{
    assert_ws_batch_operations (
      {64, zlink::ws_batch_policy::zmp_send_batch_max_size ()}, {1, 1});
}

void test_ws_batch_two_pointer_bodies_use_two_writes ()
{
    assert_ws_batch_operations ({64 * 1024, 64 * 1024}, {2, 2});
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
        //  Applying the cached value at readiness is the actual change
        //  (ROUTER §5 rule 3, D-137): exactly one event, then nothing more.
        TEST_ASSERT_TRUE (monitor.next (&record));
        TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_PEER_WEIGHT_CHANGED, record.event);
        TEST_ASSERT_EQUAL_UINT64 (41, record.values[0]);
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

void test_stamped_records_are_dropped_after_transport_replacement ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    {
        contract_zmp_engine_t application (dealer);
        application.handshake (test_zmp_wire::socket_router, "stamped-record-replacement");
        assert_ready_reply (application);
        zlink::pipe_t *const pipe = application_pipe (application, true);
        std::optional<zlink::socket_public_send_scope_t> turn;
        TEST_ASSERT_TRUE (application.core->begin_complete_send_scope (&turn));
        const uint64_t old_connection = pipe->get_transport_connection_id ();
        TEST_ASSERT_TRUE (old_connection != 0);
        const uint64_t new_connection =
          old_connection == UINT64_MAX ? 1 : old_connection + 1;

        zlink::msg_t stale_single;
        TEST_ASSERT_SUCCESS_ERRNO (stale_single.init_buffer ("stale-single", 12));
        stale_single.set_transport_connection_id (old_connection);
        TEST_ASSERT_TRUE (pipe->write (&stale_single));
        TEST_ASSERT_SUCCESS_ERRNO (stale_single.close ());

        //  The record is staged with the old stamp. Replacing the shared
        //  connection identity before publication must make session pull drop it.
        pipe->set_transport_connection_id (new_connection);
        pipe->flush ();
        turn.reset ();
        application.pump ();
        TEST_ASSERT_TRUE (contract_zmp_take_output (*application.state).empty ());

        TEST_ASSERT_TRUE (application.core->begin_complete_send_scope (&turn));
        zlink::msg_t stale_prefix;
        TEST_ASSERT_SUCCESS_ERRNO (stale_prefix.init_buffer ("stale-prefix", 12));
        stale_prefix.set_flags (zlink::msg_t::more);
        stale_prefix.set_transport_connection_id (old_connection);
        TEST_ASSERT_TRUE (pipe->write (&stale_prefix));
        TEST_ASSERT_SUCCESS_ERRNO (stale_prefix.close ());

        //  Let the final part carry the replacement stamp. Once the prefix is
        //  recognized as stale, the existing multipart drop window consumes this
        //  final part too, so no fragment of the old record reaches the wire.
        zlink::msg_t stale_final;
        TEST_ASSERT_SUCCESS_ERRNO (stale_final.init_buffer ("stale-final", 11));
        stale_final.set_transport_connection_id (new_connection);
        TEST_ASSERT_TRUE (pipe->write (&stale_final));
        TEST_ASSERT_SUCCESS_ERRNO (stale_final.close ());
        pipe->flush ();
        turn.reset ();
        application.pump ();
        TEST_ASSERT_TRUE (contract_zmp_take_output (*application.state).empty ());

        TEST_ASSERT_TRUE (application.core->begin_complete_send_scope (&turn));
        zlink::msg_t fresh;
        TEST_ASSERT_SUCCESS_ERRNO (fresh.init_buffer ("fresh", 5));
        fresh.set_transport_connection_id (new_connection);
        TEST_ASSERT_TRUE (pipe->write (&fresh));
        TEST_ASSERT_SUCCESS_ERRNO (fresh.close ());
        pipe->flush ();
        turn.reset ();
        application.pump ();

        const std::vector<contract_zmp_wire_frame_t> frames =
          contract_zmp_take_output (*application.state);
        TEST_ASSERT_EQUAL_UINT (1, frames.size ());
        TEST_ASSERT_EQUAL_UINT8 (test_zmp_wire::zmp_kind_data, frames[0].kind);
        TEST_ASSERT_EQUAL_UINT (5, frames[0].body.size ());
        TEST_ASSERT_EQUAL_MEMORY ("fresh", frames[0].body.data (), 5);
    }
    test_context_socket_close_zero_linger (dealer);
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
    RUN_TEST (test_stamped_records_are_dropped_after_transport_replacement);
    RUN_TEST (test_error_reply_payload_export_allocation_failure_is_payloadless);
    RUN_TEST (test_ws_batch_64k_then_small_is_one_three_buffer_write);
    RUN_TEST (test_ws_batch_small_64k_small_is_one_three_buffer_write);
    RUN_TEST (test_ws_batch_over_max_uses_two_encoder_writes);
    RUN_TEST (test_ws_batch_two_pointer_bodies_use_two_writes);
    return UNITY_END ();
}
