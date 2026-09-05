/* SPDX-License-Identifier: MPL-2.0 */

//  Byte-HWM contract tests for per-frame charge, low water marks and credit
//  wakeups. Arithmetic and pipe mailbox behavior are checked independently
//  of transport timing.

#include "../testutil_unity.hpp"

#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "core/command.hpp"
#include "api/socket/socket_api_internal.hpp"

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
const uint64_t k_metadata_bytes = static_cast<uint64_t> (sizeof (zlink::msg_t));

uint64_t charge_of_sized_frame (size_t payload_bytes_)
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init_size (payload_bytes_));
    const uint64_t charge = zlink::pipe_t::test_frame_accounted_bytes (&msg);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
    return charge;
}

/* ------------------------------------------------------------------ */
/*  §3.1 frame charge                                                 */
/* ------------------------------------------------------------------ */

//  normal frame charge = payload bytes + sizeof (msg_t), for every size
//  including an empty payload.
void test_normal_frame_charges_payload_plus_metadata ()
{
    const size_t sizes[] = {0, 1, 7, 100, 4096};
    for (size_t i = 0; i < sizeof (sizes) / sizeof (sizes[0]); ++i) {
        const uint64_t expected =
          static_cast<uint64_t> (sizes[i]) + k_metadata_bytes;
        TEST_ASSERT_EQUAL_UINT64 (expected, charge_of_sized_frame (sizes[i]));
    }
}

//  Delimiter, join and leave add no payload: metadata only, whatever the
//  frame would otherwise have carried.
void test_delimiter_join_and_leave_charge_metadata_only ()
{
    zlink::msg_t delimiter;
    TEST_ASSERT_EQUAL_INT (0, delimiter.init_delimiter ());
    TEST_ASSERT_TRUE (delimiter.is_delimiter ());
    TEST_ASSERT_EQUAL_UINT64 (
      k_metadata_bytes, zlink::pipe_t::test_frame_accounted_bytes (&delimiter));
    TEST_ASSERT_EQUAL_INT (0, delimiter.close ());

    zlink::msg_t join;
    TEST_ASSERT_EQUAL_INT (0, join.init_join ());
    TEST_ASSERT_TRUE (join.is_join ());
    TEST_ASSERT_EQUAL_UINT64 (
      k_metadata_bytes, zlink::pipe_t::test_frame_accounted_bytes (&join));
    TEST_ASSERT_EQUAL_INT (0, join.close ());

    zlink::msg_t leave;
    TEST_ASSERT_EQUAL_INT (0, leave.init_leave ());
    TEST_ASSERT_TRUE (leave.is_leave ());
    TEST_ASSERT_EQUAL_UINT64 (
      k_metadata_bytes, zlink::pipe_t::test_frame_accounted_bytes (&leave));
    TEST_ASSERT_EQUAL_INT (0, leave.close ());
}

//  Routing ID and credential frames are ordinary charged frames: §3.1 keeps
//  their bytes in the accounting even though they are not application
//  messages.
void test_routing_id_and_credential_frames_charge_their_payload ()
{
    zlink::msg_t routing_id;
    TEST_ASSERT_EQUAL_INT (0, routing_id.init_size (5));
    routing_id.set_flags (zlink::msg_t::routing_id);
    TEST_ASSERT_EQUAL_UINT64 (
      5 + k_metadata_bytes,
      zlink::pipe_t::test_frame_accounted_bytes (&routing_id));
    TEST_ASSERT_EQUAL_INT (0, routing_id.close ());

    zlink::msg_t credential;
    TEST_ASSERT_EQUAL_INT (0, credential.init_size (9));
    credential.set_flags (zlink::msg_t::credential);
    TEST_ASSERT_EQUAL_UINT64 (
      9 + k_metadata_bytes,
      zlink::pipe_t::test_frame_accounted_bytes (&credential));
    TEST_ASSERT_EQUAL_INT (0, credential.close ());
}

/* ------------------------------------------------------------------ */
/*  §3.2 default LWM and hint                                         */
/* ------------------------------------------------------------------ */

//  default byte LWM = ceil (HWM / 2), so an odd HWM rounds up rather than
//  truncating.
void test_default_lwm_is_ceil_half_of_hwm ()
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink::pipe_t::test_compute_lwm (0));
    TEST_ASSERT_EQUAL_UINT64 (1, zlink::pipe_t::test_compute_lwm (1));
    TEST_ASSERT_EQUAL_UINT64 (1, zlink::pipe_t::test_compute_lwm (2));
    TEST_ASSERT_EQUAL_UINT64 (2, zlink::pipe_t::test_compute_lwm (3));
    TEST_ASSERT_EQUAL_UINT64 (3, zlink::pipe_t::test_compute_lwm (5));
    TEST_ASSERT_EQUAL_UINT64 (512, zlink::pipe_t::test_compute_lwm (1023));
    TEST_ASSERT_EQUAL_UINT64 (512, zlink::pipe_t::test_compute_lwm (1024));
    TEST_ASSERT_EQUAL_UINT64 (513, zlink::pipe_t::test_compute_lwm (1025));
}

//  A positive hint below the default becomes the actual LWM; a hint at or
//  above the default leaves the default in place, because the actual LWM is
//  the smaller of the two.
void test_positive_hint_below_default_is_used ()
{
    const uint64_t hwm = 1000;
    const uint64_t lwm = zlink::pipe_t::test_compute_lwm (hwm);
    TEST_ASSERT_EQUAL_UINT64 (500, lwm);

    TEST_ASSERT_EQUAL_UINT64 (
      64, zlink::pipe_t::test_apply_lwm_hint (hwm, lwm, 64));
    TEST_ASSERT_EQUAL_UINT64 (
      499, zlink::pipe_t::test_apply_lwm_hint (hwm, lwm, 499));
    //  Equal to the default: the default stands.
    TEST_ASSERT_EQUAL_UINT64 (
      500, zlink::pipe_t::test_apply_lwm_hint (hwm, lwm, 500));
    //  Above the default but below the HWM: still the default.
    TEST_ASSERT_EQUAL_UINT64 (
      500, zlink::pipe_t::test_apply_lwm_hint (hwm, lwm, 900));
}

//  A hint of zero means "no hint" and leaves the default untouched, and an
//  unlimited HWM has no LWM to constrain.
void test_absent_hint_and_unlimited_hwm_leave_the_default ()
{
    const uint64_t hwm = 1000;
    const uint64_t lwm = zlink::pipe_t::test_compute_lwm (hwm);
    TEST_ASSERT_EQUAL_UINT64 (lwm,
                              zlink::pipe_t::test_apply_lwm_hint (hwm, lwm, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink::pipe_t::test_apply_lwm_hint (0, 0, 64));
}

//  A hint at or above the HWM is clamped to HWM - 1, and the clamp never
//  produces a zero LWM: the floor is one byte.
void test_hint_at_or_above_hwm_clamps_and_keeps_a_floor_of_one ()
{
    //  HWM 4, default LWM 2. A hint of 4 or 9 clamps to 3, which is above the
    //  default, so the default still wins.
    TEST_ASSERT_EQUAL_UINT64 (
      2, zlink::pipe_t::test_apply_lwm_hint (4, 2, 4));
    TEST_ASSERT_EQUAL_UINT64 (
      2, zlink::pipe_t::test_apply_lwm_hint (4, 2, 9));

    //  HWM 2, default LWM 1: the clamp lands on 1 and the result is 1.
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::pipe_t::test_apply_lwm_hint (2, 1, 2));

    //  HWM 1: the clamp would land on 0, so the floor of one byte applies.
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::pipe_t::test_apply_lwm_hint (1, 1, 1));
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::pipe_t::test_apply_lwm_hint (1, 1, 50));
}

struct credit_pipe_sink_t : zlink::i_pipe_events
{
    int writes = 0;
    int credit_recoveries = 0;
    int terminated = 0;
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE
    {
        ++writes;
        // The socket owner consumes this marker to publish send recovery.
        if (pipe_->take_hwm_credit_recovery ())
            ++credit_recoveries;
    }
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *, bool) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE { ++terminated; }
};

int drain_credit_commands (zlink::socket_base_t *socket_)
{
    int activations = 0;
    zlink::command_t command;
    while (socket_->get_mailbox ()->recv (&command, 0) == 0) {
        if (command.type == zlink::command_t::activate_write)
            ++activations;
        command.destination->process_command (command);
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    return activations;
}

void test_dequeue_notifies_only_a_writer_waiting_for_credit ()
{
    setup_test_context ();
    void *writer = test_context_socket (ZLINK_SOCKET_PAIR);
    void *reader = test_context_socket (ZLINK_SOCKET_PAIR);
    zlink::socket_base_t *writer_core = as_socket_handle (writer).socket;
    zlink::socket_base_t *reader_core = as_socket_handle (reader).socket;
    // Distinct socket mailboxes make every credit command observable before
    // its owner processes it. No transport timing is involved.
    zlink::object_t *parents[] = {writer_core, reader_core};
    zlink::pipe_t *pipes[2];
    const uint64_t charge = sizeof (zlink::msg_t) + 64;
    const uint64_t hwms[] = {2 * charge, 2 * charge};
    const bool conflates[] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));
    credit_pipe_sink_t sink;
    pipes[0]->set_event_sink (&sink);
    pipes[1]->set_event_sink (&sink);

    const auto write_frame = [&] () {
        zlink::msg_t frame;
        TEST_ASSERT_SUCCESS_ERRNO (frame.init_size (64));
        TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&frame));
        drain_credit_commands (reader_core);
    };
    const auto read_frame = [&] () {
        zlink::msg_t frame;
        TEST_ASSERT_SUCCESS_ERRNO (frame.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&frame));
        TEST_ASSERT_SUCCESS_ERRNO (frame.close ());
    };

    int active_writer_commands = 0;
    // Cross several LWM and cached-HWM boundaries without ever parking.
    for (int i = 0; i != 8; ++i) {
        write_frame ();
        read_frame ();
        active_writer_commands += drain_credit_commands (writer_core);
    }

    write_frame ();
    write_frame ();
    TEST_ASSERT_FALSE (pipes[0]->check_write ());
    read_frame ();
    const int waiting_writer_commands = drain_credit_commands (writer_core);
    const int resumed_writers = sink.writes;
    const int credit_recoveries = sink.credit_recoveries;
    TEST_ASSERT_TRUE (pipes[0]->check_write ());
    read_frame ();
    const int resumed_writer_commands = drain_credit_commands (writer_core);

    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    drain_credit_commands (reader_core);
    drain_credit_commands (writer_core);
    drain_credit_commands (reader_core);
    TEST_ASSERT_EQUAL_INT (2, sink.terminated);
    test_context_socket_close_zero_linger (reader);
    test_context_socket_close_zero_linger (writer);
    teardown_test_context ();

    TEST_ASSERT_EQUAL_INT (0, active_writer_commands);
    TEST_ASSERT_EQUAL_INT (1, waiting_writer_commands);
    TEST_ASSERT_EQUAL_INT (1, resumed_writers);
    TEST_ASSERT_EQUAL_INT (1, credit_recoveries);
    TEST_ASSERT_EQUAL_INT (0, resumed_writer_commands);
}
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_normal_frame_charges_payload_plus_metadata);
    RUN_TEST (test_delimiter_join_and_leave_charge_metadata_only);
    RUN_TEST (test_routing_id_and_credential_frames_charge_their_payload);
    RUN_TEST (test_default_lwm_is_ceil_half_of_hwm);
    RUN_TEST (test_positive_hint_below_default_is_used);
    RUN_TEST (test_absent_hint_and_unlimited_hwm_leave_the_default);
    RUN_TEST (test_hint_at_or_above_hwm_clamps_and_keeps_a_floor_of_one);
    RUN_TEST (test_dequeue_notifies_only_a_writer_waiting_for_credit);
    return UNITY_END ();
}
