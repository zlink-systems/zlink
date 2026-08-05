/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "utils/ip.hpp"
#include "core/msg.hpp"
#include "protocol/decoder_allocators.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_decoder.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"

#include <limits>
#include <unity.h>
#include <vector>

void setUp ()
{
}

void tearDown ()
{
}

static void build_header (unsigned char *buf_, unsigned char flags_, uint32_t body_len_)
{
    buf_[0] = zlink::zmp_magic;
    buf_[1] = zlink::zmp_version;
    buf_[2] = flags_;
    buf_[3] = 0;
    zlink::put_uint32 (buf_ + 4, body_len_);
}

void test_invalid_magic ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 0);
    buf[0] = 0x00;
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_invalid_magic, decoder.error_code ());
}

void test_version_mismatch ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 0);
    buf[1] = static_cast<unsigned char> (zlink::zmp_version + 1);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_version_mismatch, decoder.error_code ());
}

void test_flags_invalid ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_control | zlink::zmp_flag_more, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_flags_invalid, decoder.error_code ());
}

void test_subscribe_cancel_invalid ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_subscribe | zlink::zmp_flag_cancel, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_flags_invalid, decoder.error_code ());
}

void test_body_too_large ()
{
    zlink::zmp_decoder_t decoder (64, 16);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, 0, 32);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_body_too_large, decoder.error_code ());
}

void test_more_identity_allowed ()
{
    zlink::zmp_decoder_t decoder (64, -1);
    unsigned char buf[zlink::zmp_header_size];
    build_header (buf, zlink::zmp_flag_more | zlink::zmp_flag_identity, 0);
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (1, rc);
    const unsigned char flags = decoder.msg ()->flags ();
    TEST_ASSERT_TRUE (flags & zlink::msg_t::more);
    TEST_ASSERT_TRUE (flags & zlink::msg_t::routing_id);
}

void test_metadata_parse_valid ()
{
    std::vector<unsigned char> buf;
    zlink::zmp_metadata::append_property (buf, "Socket-Type", "PAIR", 4);

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_EQUAL_STRING ("PAIR", out["Socket-Type"].c_str ());
}

void test_metadata_parse_invalid ()
{
    std::vector<unsigned char> buf;
    zlink::zmp_metadata::append_property (buf, "Socket-Type", "PAIR", 4);
    if (!buf.empty ())
        buf.pop_back ();

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
}

void test_metadata_add_basic_properties ()
{
    zlink::options_t options;
    options.type = ZLINK_CORE_SOCKET_ROUTER;
    const char *routing_id = "RID";
    memcpy (options.routing_id, routing_id, 3);
    options.routing_id_size = 3;

    std::vector<unsigned char> buf;
    zlink::zmp_metadata::add_basic_properties (options, buf);

    zlink::metadata_t::dict_t out;
    const int rc = zlink::zmp_metadata::parse (&buf[0], buf.size (), out);
    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_EQUAL_STRING ("ROUTER", out["Socket-Type"].c_str ());
    TEST_ASSERT_EQUAL_STRING ("RID", out["Routing-Id"].c_str ());
}

void test_shared_message_allocator_size_checks_overflow ()
{
    std::size_t allocation_size = 0;
    TEST_ASSERT_TRUE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      64, 2, &allocation_size));
    TEST_ASSERT_EQUAL_UINT64 (
      64 + sizeof (zlink::atomic_counter_t) + 2 * sizeof (zlink::msg_t::content_t),
      allocation_size);

    const std::size_t max_size = std::numeric_limits<std::size_t>::max ();
    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      64, max_size / sizeof (zlink::msg_t::content_t) + 1, &allocation_size));

    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      max_size - sizeof (zlink::atomic_counter_t) + 1, 0, &allocation_size));

    TEST_ASSERT_FALSE (zlink::shared_message_memory_allocator::allocation_size_for_test (
      max_size - sizeof (zlink::atomic_counter_t), 1, &allocation_size));
}

int main (void)
{
    UNITY_BEGIN ();

    zlink::initialize_network ();
    setup_test_environment ();

    RUN_TEST (test_invalid_magic);
    RUN_TEST (test_version_mismatch);
    RUN_TEST (test_flags_invalid);
    RUN_TEST (test_subscribe_cancel_invalid);
    RUN_TEST (test_body_too_large);
    RUN_TEST (test_more_identity_allowed);
    RUN_TEST (test_metadata_parse_valid);
    RUN_TEST (test_metadata_parse_invalid);
    RUN_TEST (test_metadata_add_basic_properties);
    RUN_TEST (test_shared_message_allocator_size_checks_overflow);

    zlink::shutdown_network ();

    return UNITY_END ();
}
