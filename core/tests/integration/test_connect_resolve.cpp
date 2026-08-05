/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"

#include "testutil_unity.hpp"

#include <unity.h>

void *sock;

void setUp ()
{
    setup_test_context ();
    sock = test_context_socket (ZLINK_SOCKET_PUB);
}

void tearDown ()
{
    test_context_socket_close (sock);
    sock = NULL;
    teardown_test_context ();
}

void test_hostname_ipv4 ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sock, "tcp://localhost:1234"));
}

void test_loopback_ipv6 ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sock, "tcp://[::1]:1234"));
}

void test_invalid_service_fails ()
{
    zlink_connect_result_t rc = zlink_connect (sock, "tcp://localhost:invalid");
    TEST_ASSERT_TRUE (rc != ZLINK_CONNECT_OK);
}

void test_hostname_with_spaces_fails ()
{
    zlink_connect_result_t rc = zlink_connect (sock, "tcp://in val id:1234");
    TEST_ASSERT_TRUE (rc != ZLINK_CONNECT_OK);
}

void test_no_hostname_fails ()
{
    zlink_connect_result_t rc = zlink_connect (sock, "tcp://");
    TEST_ASSERT_TRUE (rc != ZLINK_CONNECT_OK);
}

void test_x ()
{
    zlink_connect_result_t rc = zlink_connect (sock, "tcp://192.168.0.200:*");
    TEST_ASSERT_TRUE (rc != ZLINK_CONNECT_OK);
}

void test_invalid_proto_fails ()
{
    zlink_connect_result_t rc = zlink_connect (sock, "invalid://localhost:1234");
    TEST_ASSERT_TRUE (rc != ZLINK_CONNECT_OK);
    TEST_ASSERT_EQUAL_INT (EPROTONOSUPPORT, errno);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_hostname_ipv4);
    RUN_TEST (test_loopback_ipv6);
    RUN_TEST (test_hostname_with_spaces_fails);
    RUN_TEST (test_no_hostname_fails);
    RUN_TEST (test_invalid_service_fails);
    RUN_TEST (test_invalid_proto_fails);
    return UNITY_END ();
}
