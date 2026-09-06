/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "core/msg.hpp"
#include "utils/ip.hpp"

#include <cstdlib>
#include <cstring>
#include <unity.h>

void setUp ()
{
}

void tearDown ()
{
}

void test_init_view_lmsg_shares_storage ()
{
    zlink::msg_t src;
    zlink::msg_t view;
    TEST_ASSERT_EQUAL_INT (0, src.init_size (128));
    TEST_ASSERT_EQUAL_INT (0, view.init ());

    unsigned char *src_data = static_cast<unsigned char *> (src.data ());
    for (size_t i = 0; i < src.size (); ++i)
        src_data[i] = static_cast<unsigned char> (i & 0xFF);

    TEST_ASSERT_EQUAL_INT (0, view.init_view (src, 8, 64));
    TEST_ASSERT_TRUE (std::memcmp (view.data (), src_data + 8, 64) == 0);

    src_data[10] = 0xAB;
    TEST_ASSERT_EQUAL_UINT8 (0xAB, static_cast<unsigned char *> (view.data ())[2]);

    TEST_ASSERT_EQUAL_INT (0, src.close ());
    TEST_ASSERT_EQUAL_UINT8 (0xAB, static_cast<unsigned char *> (view.data ())[2]);
    TEST_ASSERT_EQUAL_INT (0, view.close ());
}

void test_init_view_vsm_fallback_copy ()
{
    zlink::msg_t src;
    zlink::msg_t view;
    TEST_ASSERT_EQUAL_INT (0, src.init_size (8));
    TEST_ASSERT_EQUAL_INT (0, view.init ());

    unsigned char *src_data = static_cast<unsigned char *> (src.data ());
    for (size_t i = 0; i < src.size (); ++i)
        src_data[i] = static_cast<unsigned char> (0x10 + i);

    TEST_ASSERT_EQUAL_INT (0, view.init_view (src, 2, 4));
    TEST_ASSERT_TRUE (std::memcmp (view.data (), src_data + 2, 4) == 0);

    src_data[3] = 0xEE;
    TEST_ASSERT_TRUE (static_cast<unsigned char *> (view.data ())[1] != 0xEE);

    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (0, src.close ());
}

void test_init_view_invalid_range ()
{
    zlink::msg_t src;
    zlink::msg_t view;
    TEST_ASSERT_EQUAL_INT (0, src.init_size (16));
    TEST_ASSERT_EQUAL_INT (0, view.init ());

    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, view.init_view (src, 10, 7));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (0, src.close ());
}

void on_msg_view_test_free (void *data_, void *hint_)
{
    if (hint_) {
        int *counter = static_cast<int *> (hint_);
        ++(*counter);
    }
    std::free (data_);
}

void test_init_view_from_zcmsg ()
{
    zlink::msg_t src;
    zlink::msg_t view;
    zlink::msg_t::content_t content;
    int free_counter = 0;

    unsigned char *payload = static_cast<unsigned char *> (std::malloc (96));
    TEST_ASSERT_TRUE (payload != NULL);
    std::memset (payload, 0x42, 96);

    TEST_ASSERT_EQUAL_INT (0, src.init ());
    TEST_ASSERT_EQUAL_INT (0,
                           src.init (payload, 96, on_msg_view_test_free, &free_counter, &content));
    TEST_ASSERT_TRUE (src.is_zcmsg ());

    TEST_ASSERT_EQUAL_INT (0, view.init ());
    TEST_ASSERT_EQUAL_INT (0, view.init_view (src, 4, 32));
    TEST_ASSERT_EQUAL_UINT (32, (unsigned int) view.size ());
    TEST_ASSERT_TRUE (std::memcmp (view.data (), payload + 4, 32) == 0);

    TEST_ASSERT_EQUAL_INT (0, src.close ());
    TEST_ASSERT_EQUAL_INT (0, free_counter);
    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (1, free_counter);
}

void test_nested_views_preserve_extent_through_copy_and_move ()
{
    unsigned char *payload = static_cast<unsigned char *> (std::malloc (128));
    TEST_ASSERT_NOT_NULL (payload);
    for (size_t i = 0; i < 128; ++i)
        payload[i] = static_cast<unsigned char> (i);
    int free_counter = 0;
    zlink::msg_t source;
    zlink::msg_t view;
    zlink::msg_t nested;
    zlink::msg_t copied;
    zlink::msg_t moved;
    TEST_ASSERT_EQUAL_INT (
      0, source.init_data (payload, 128, on_msg_view_test_free, &free_counter));
    TEST_ASSERT_EQUAL_INT (0, view.init ());
    TEST_ASSERT_EQUAL_INT (0, nested.init ());
    TEST_ASSERT_EQUAL_INT (0, copied.init ());
    TEST_ASSERT_EQUAL_INT (0, moved.init ());

    TEST_ASSERT_EQUAL_INT (0, view.init_view (source, 8, 64));
    TEST_ASSERT_EQUAL_INT (0, nested.init_view (view, 12, 32));
    TEST_ASSERT_EQUAL_INT (0, copied.copy (nested));
    TEST_ASSERT_EQUAL_INT (0, moved.move (nested));
    TEST_ASSERT_EQUAL_UINT64 (128, source.size ());
    TEST_ASSERT_EQUAL_UINT64 (64, view.size ());
    TEST_ASSERT_EQUAL_UINT64 (0, nested.size ());
    TEST_ASSERT_EQUAL_INT (0, source.close ());
    TEST_ASSERT_EQUAL_INT (0, view.close ());
    TEST_ASSERT_EQUAL_INT (0, nested.close ());
    TEST_ASSERT_EQUAL_INT (0, free_counter);

    TEST_ASSERT_EQUAL_UINT64 (32, moved.size ());
    TEST_ASSERT_EQUAL_UINT64 (32, copied.size ());
    TEST_ASSERT_EQUAL_PTR (payload + 20, moved.data ());
    TEST_ASSERT_EQUAL_PTR (moved.data (), copied.data ());
    TEST_ASSERT_EQUAL_INT (0, moved.close ());
    TEST_ASSERT_EQUAL_INT (0, free_counter);
    TEST_ASSERT_EQUAL_UINT8 (20, static_cast<unsigned char *> (copied.data ())[0]);
    TEST_ASSERT_EQUAL_UINT8 (51, static_cast<unsigned char *> (copied.data ())[31]);
    TEST_ASSERT_EQUAL_INT (0, copied.close ());
    TEST_ASSERT_EQUAL_INT (1, free_counter);
}

static void init_command_msg (zlink::msg_t &msg_, const size_t size_, const unsigned char flags_)
{
    TEST_ASSERT_EQUAL_INT (0, msg_.init_size (size_));
    msg_.set_flags (flags_);
}

void test_command_body_size_for_well_formed_commands ()
{
    zlink::msg_t ping;
    zlink::msg_t pong;
    zlink::msg_t subscribe;
    zlink::msg_t cancel;

    init_command_msg (ping, 7, zlink::msg_t::command | zlink::msg_t::ping);
    init_command_msg (pong, 5, zlink::msg_t::command | zlink::msg_t::pong);
    init_command_msg (subscribe, 13, zlink::msg_t::command | zlink::msg_t::subscribe);
    init_command_msg (cancel, 9, zlink::msg_t::command | zlink::msg_t::cancel);

    TEST_ASSERT_EQUAL_UINT (2, (unsigned int) ping.command_body_size ());
    TEST_ASSERT_EQUAL_PTR (static_cast<unsigned char *> (ping.data ()) + 5, ping.command_body ());
    TEST_ASSERT_EQUAL_UINT (0, (unsigned int) pong.command_body_size ());
    TEST_ASSERT_EQUAL_PTR (static_cast<unsigned char *> (pong.data ()) + 5, pong.command_body ());
    TEST_ASSERT_EQUAL_UINT (3, (unsigned int) subscribe.command_body_size ());
    TEST_ASSERT_EQUAL_PTR (static_cast<unsigned char *> (subscribe.data ()) + 10,
                           subscribe.command_body ());
    TEST_ASSERT_EQUAL_UINT (2, (unsigned int) cancel.command_body_size ());
    TEST_ASSERT_EQUAL_PTR (static_cast<unsigned char *> (cancel.data ()) + 7, cancel.command_body ());

    TEST_ASSERT_EQUAL_INT (0, cancel.close ());
    TEST_ASSERT_EQUAL_INT (0, subscribe.close ());
    TEST_ASSERT_EQUAL_INT (0, pong.close ());
    TEST_ASSERT_EQUAL_INT (0, ping.close ());
}

void test_command_body_size_clamps_malformed_commands ()
{
    zlink::msg_t ping;
    zlink::msg_t subscribe;
    zlink::msg_t cancel;
    zlink::msg_t inproc_subscribe;

    init_command_msg (ping, 4, zlink::msg_t::command | zlink::msg_t::ping);
    init_command_msg (subscribe, 9, zlink::msg_t::command | zlink::msg_t::subscribe);
    init_command_msg (cancel, 6, zlink::msg_t::command | zlink::msg_t::cancel);
    init_command_msg (inproc_subscribe, 3, zlink::msg_t::subscribe);

    TEST_ASSERT_EQUAL_UINT (0, (unsigned int) ping.command_body_size ());
    TEST_ASSERT_NULL (ping.command_body ());
    TEST_ASSERT_EQUAL_UINT (0, (unsigned int) subscribe.command_body_size ());
    TEST_ASSERT_NULL (subscribe.command_body ());
    TEST_ASSERT_EQUAL_UINT (0, (unsigned int) cancel.command_body_size ());
    TEST_ASSERT_NULL (cancel.command_body ());
    TEST_ASSERT_EQUAL_UINT (3, (unsigned int) inproc_subscribe.command_body_size ());
    TEST_ASSERT_EQUAL_PTR (inproc_subscribe.data (), inproc_subscribe.command_body ());

    TEST_ASSERT_EQUAL_INT (0, inproc_subscribe.close ());
    TEST_ASSERT_EQUAL_INT (0, cancel.close ());
    TEST_ASSERT_EQUAL_INT (0, subscribe.close ());
    TEST_ASSERT_EQUAL_INT (0, ping.close ());
}

int main (void)
{
    UNITY_BEGIN ();

    zlink::initialize_network ();
    setup_test_environment ();

    RUN_TEST (test_init_view_lmsg_shares_storage);
    RUN_TEST (test_init_view_vsm_fallback_copy);
    RUN_TEST (test_init_view_invalid_range);
    RUN_TEST (test_init_view_from_zcmsg);
    RUN_TEST (test_nested_views_preserve_extent_through_copy_and_move);
    RUN_TEST (test_command_body_size_for_well_formed_commands);
    RUN_TEST (test_command_body_size_clamps_malformed_commands);

    zlink::shutdown_network ();
    return UNITY_END ();
}
