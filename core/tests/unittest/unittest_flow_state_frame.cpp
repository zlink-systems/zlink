/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include <string.h>

#include "core/flow_state_frame.hpp"
#include "core/msg.hpp"

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
zlink::flow_state::frame_t make_frame (uint8_t state_,
                                       uint64_t pair_id_,
                                       uint64_t generation_,
                                       uint64_t epoch_)
{
    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = state_;
    frame.pair_id = pair_id_;
    frame.generation = generation_;
    frame.epoch = epoch_;
    return frame;
}

void test_state_values_match_the_contract ()
{
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::receive_flow_running);
    TEST_ASSERT_EQUAL_INT (1, zlink::flow_state::receive_flow_paused);
}

void test_round_trip_preserves_every_field ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    const zlink::flow_state::frame_t sent = make_frame (
      zlink::flow_state::receive_flow_paused, 0x0123456789abcdefULL,
      0xfedcba9876543210ULL, 0x00000000deadbeefULL);
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, sent));

    //  The frame must be a command frame: that is what keeps it out of every
    //  application receive path.
    TEST_ASSERT_TRUE ((msg.flags () & zlink::msg_t::command) != 0);
    TEST_ASSERT_EQUAL_UINT64 (zlink::flow_state::frame_size, msg.size ());

    zlink::flow_state::frame_t decoded;
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_ok,
                           zlink::flow_state::decode_frame (msg, &decoded));
    TEST_ASSERT_EQUAL_UINT8 (zlink::flow_state::frame_protocol_version,
                             decoded.version);
    TEST_ASSERT_EQUAL_UINT8 (sent.state, decoded.state);
    TEST_ASSERT_EQUAL_UINT64 (sent.pair_id, decoded.pair_id);
    TEST_ASSERT_EQUAL_UINT64 (sent.generation, decoded.generation);
    TEST_ASSERT_EQUAL_UINT64 (sent.epoch, decoded.epoch);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_running_state_round_trips ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink::flow_state::init_frame (
           &msg, make_frame (zlink::flow_state::receive_flow_running, 7, 3, 9)));
    zlink::flow_state::frame_t decoded;
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_ok,
                           zlink::flow_state::decode_frame (msg, &decoded));
    TEST_ASSERT_EQUAL_UINT8 (zlink::flow_state::receive_flow_running,
                             decoded.state);
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_non_command_frame_is_not_a_flow_frame ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink::flow_state::init_frame (
           &msg, make_frame (zlink::flow_state::receive_flow_paused, 7, 3, 9)));
    msg.reset_flags (zlink::msg_t::command);
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_not_flow_frame,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_foreign_command_frame_is_not_a_flow_frame ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init_size (6));
    memcpy (msg.data (), "WEIGHT", 6);
    msg.set_flags (zlink::msg_t::command);
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_not_flow_frame,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_unsupported_version_is_rejected_but_consumed ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink::flow_state::init_frame (
           &msg, make_frame (zlink::flow_state::receive_flow_paused, 7, 3, 9)));
    static_cast<unsigned char *> (
      msg.data ())[zlink::flow_state::frame_name_size] = 99;
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_unsupported_version,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_FALSE (zlink::flow_state::version_supported (99));
    TEST_ASSERT_TRUE (zlink::flow_state::version_supported (
      zlink::flow_state::frame_protocol_version));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_out_of_range_state_is_malformed ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink::flow_state::init_frame (
           &msg, make_frame (zlink::flow_state::receive_flow_paused, 7, 3, 9)));
    static_cast<unsigned char *> (
      msg.data ())[zlink::flow_state::frame_name_size + 1] = 2;
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_malformed,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_truncated_frame_is_malformed ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (
      0, msg.init_size (zlink::flow_state::frame_name_size + 2));
    memcpy (msg.data (), zlink::flow_state::frame_name,
            zlink::flow_state::frame_name_size);
    static_cast<unsigned char *> (
      msg.data ())[zlink::flow_state::frame_name_size] =
      zlink::flow_state::frame_protocol_version;
    static_cast<unsigned char *> (
      msg.data ())[zlink::flow_state::frame_name_size + 1] =
      zlink::flow_state::receive_flow_paused;
    msg.set_flags (zlink::msg_t::command);
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_malformed,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}

void test_zero_pair_identity_is_malformed ()
{
    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (
      0, zlink::flow_state::init_frame (
           &msg, make_frame (zlink::flow_state::receive_flow_paused, 0, 3, 9)));
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_malformed,
                           zlink::flow_state::decode_frame (msg, NULL));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());
}
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_state_values_match_the_contract);
    RUN_TEST (test_round_trip_preserves_every_field);
    RUN_TEST (test_running_state_round_trips);
    RUN_TEST (test_non_command_frame_is_not_a_flow_frame);
    RUN_TEST (test_foreign_command_frame_is_not_a_flow_frame);
    RUN_TEST (test_unsupported_version_is_rejected_but_consumed);
    RUN_TEST (test_out_of_range_state_is_malformed);
    RUN_TEST (test_truncated_frame_is_malformed);
    RUN_TEST (test_zero_pair_identity_is_malformed);
    return UNITY_END ();
}
