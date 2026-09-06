/* SPDX-License-Identifier: MPL-2.0 */

// ZLINK_OPT_TYPE reports the public zlink_socket_type_t value (D-134), never
// the internal core enum, for every socket type through the public C API.
#include "testutil.hpp"
#include "testutil_unity.hpp"

namespace
{
void expect_public_type (zlink_socket_type_t type_)
{
    void *socket = test_context_socket (type_);
    int reported = -1;
    size_t size = sizeof (reported);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (socket, ZLINK_OPT_TYPE, &reported, &size));
    TEST_ASSERT_EQUAL_UINT (sizeof (int), size);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (type_), reported);
    test_context_socket_close_zero_linger (socket);
}
}

void test_pair () { expect_public_type (ZLINK_SOCKET_PAIR); }
void test_pub () { expect_public_type (ZLINK_SOCKET_PUB); }
void test_sub () { expect_public_type (ZLINK_SOCKET_SUB); }
void test_dealer () { expect_public_type (ZLINK_SOCKET_DEALER); }
void test_router () { expect_public_type (ZLINK_SOCKET_ROUTER); }
void test_xpub () { expect_public_type (ZLINK_SOCKET_XPUB); }
void test_xsub () { expect_public_type (ZLINK_SOCKET_XSUB); }
void test_stream () { expect_public_type (ZLINK_SOCKET_STREAM); }

void test_short_buffer_is_invalid_argument ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    char small = 0;
    size_t size = sizeof (small);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_get_option (socket, ZLINK_OPT_TYPE, &small, &size));
    test_context_socket_close_zero_linger (socket);
}

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pair);
    RUN_TEST (test_pub);
    RUN_TEST (test_sub);
    RUN_TEST (test_dealer);
    RUN_TEST (test_router);
    RUN_TEST (test_xpub);
    RUN_TEST (test_xsub);
    RUN_TEST (test_stream);
    RUN_TEST (test_short_buffer_is_invalid_argument);
    return UNITY_END ();
}
