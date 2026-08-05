/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "protocol/raw_decoder.hpp"
#include "utils/ip.hpp"

#include <unity.h>
#include <vector>

void setUp ()
{
}

void tearDown ()
{
}

void test_decode_passthrough_single_buffer ()
{
    zlink::raw_decoder_t decoder (32, -1);

    const unsigned char buf[] = {'a', 'b', 'c'};
    size_t processed = 0;
    const int rc = decoder.decode (buf, sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT (1, rc);
    TEST_ASSERT_EQUAL_UINT (sizeof (buf), processed);
    TEST_ASSERT_EQUAL_INT ((int) sizeof (buf), decoder.msg ()->size ());
    TEST_ASSERT_EQUAL_STRING_LEN ("abc", static_cast<const char *> (decoder.msg ()->data ()),
                                  decoder.msg ()->size ());
}

void test_decode_zero_length ()
{
    zlink::raw_decoder_t decoder (64, -1);

    size_t processed = 0;
    const int rc = decoder.decode (NULL, 0, processed);
    TEST_ASSERT_EQUAL_INT (1, rc);
    TEST_ASSERT_EQUAL_INT (0, processed);
    TEST_ASSERT_EQUAL_INT (0, decoder.msg ()->size ());
}

void test_raw_payload_too_large ()
{
    zlink::raw_decoder_t decoder (64, 4);

    const unsigned char payload[8] = {0};
    size_t processed = 0;
    const int rc = decoder.decode (payload, sizeof (payload), processed);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
}

int main (void)
{
    UNITY_BEGIN ();

    zlink::initialize_network ();
    setup_test_environment ();

    RUN_TEST (test_decode_passthrough_single_buffer);
    RUN_TEST (test_decode_zero_length);
    RUN_TEST (test_raw_payload_too_large);

    zlink::shutdown_network ();

    return UNITY_END ();
}
