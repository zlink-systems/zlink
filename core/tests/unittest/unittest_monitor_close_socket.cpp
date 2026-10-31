/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

void setUp ()
{
}
void tearDown ()
{
}

void test_monitor_close_then_socket_close ()
{
    for (int iteration = 0; iteration != 1000; ++iteration) {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *left = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        void *right = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_NOT_NULL (left);
        TEST_ASSERT_NOT_NULL (right);
        const int linger = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (left, ZLINK_OPT_LINGER, &linger, sizeof linger));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (right, ZLINK_OPT_LINGER, &linger, sizeof linger));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (left, "left", 4));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (right, "right", 5));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "tcp://127.0.0.1:*"));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (right, "tcp://127.0.0.1:*"));
        char left_endpoint[256], right_endpoint[256];
        size_t left_size = sizeof left_endpoint, right_size = sizeof right_endpoint;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (left, ZLINK_OPT_LAST_ENDPOINT, left_endpoint, &left_size));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (right, ZLINK_OPT_LAST_ENDPOINT, right_endpoint, &right_size));
        const zlink_socket_monitor_open_options_t options = {
          ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED, 0};
        void *left_monitor = zlink_socket_monitor_open (left, &options);
        void *right_monitor = zlink_socket_monitor_open (right, &options);
        TEST_ASSERT_NOT_NULL (left_monitor);
        TEST_ASSERT_NOT_NULL (right_monitor);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (right, left_endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (left, right_endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (left, right_endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&right_monitor));
        TEST_ASSERT_NULL (right_monitor);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (right));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&left_monitor));
        TEST_ASSERT_NULL (left_monitor);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (left));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_monitor_close_then_socket_close);
    return UNITY_END ();
}
