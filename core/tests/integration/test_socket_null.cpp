/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"

#include <cstring>
#include <unity.h>

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
void discard_monitor_event (const zlink_monitor_event_t *, void *)
{
}
}

//  tests all socket-related functions with a NULL socket argument
void test_zlink_socket_null_context ()
{
    TEST_ASSERT_NULL (zlink_socket (NULL, ZLINK_SOCKET_PAIR));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_close_null_socket ()
{
    zlink_close_result_t rc = zlink_close (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_setsockopt_null_socket ()
{
    int hwm = 100;
    size_t hwm_size = sizeof hwm;
    zlink_config_result_t rc = zlink_set_option (NULL, ZLINK_OPT_SNDHWM, &hwm, hwm_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_getsockopt_null_socket ()
{
    int hwm;
    size_t hwm_size = sizeof hwm;
    zlink_config_result_t rc = zlink_get_option (NULL, ZLINK_OPT_SNDHWM, &hwm, &hwm_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_socket_monitor_open_null_socket ()
{
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_ALL;
    void *monitor = zlink_socket_monitor_open (NULL, &opts);
    TEST_ASSERT_NULL (monitor);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}


void test_zlink_bind_null_socket ()
{
    zlink_bind_result_t rc = zlink_bind (NULL, "inproc://socket");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_connect_null_socket ()
{
    zlink_connect_result_t rc = zlink_connect (NULL, "inproc://socket");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_unbind_null_socket ()
{
    zlink_connect_result_t rc = zlink_unbind (NULL, "inproc://socket");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_disconnect_null_socket ()
{
    zlink_connect_result_t rc = zlink_disconnect (NULL, "inproc://socket");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_msg_null_handle ()
{
    zlink_config_result_t rc = zlink_msg_init (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_init_size (NULL, 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_init_data (NULL, NULL, 0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_close (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    zlink_msg_t msg;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&msg));

    rc = zlink_msg_move (NULL, &msg);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_copy (NULL, &msg);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_move (&msg, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    rc = zlink_msg_copy (&msg, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&msg));

    errno = 0;
    TEST_ASSERT_NULL (zlink_msg_data (NULL));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    errno = 0;
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (NULL));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_msg_closed_handle_read_is_invalid ()
{
    zlink_msg_t msg;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&msg));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&msg));

    zlink_config_result_t rc = zlink_msg_close (&msg);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    errno = 0;
    TEST_ASSERT_NULL (zlink_msg_data (&msg));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    errno = 0;
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&msg));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);

    zlink_config_result_t refcnt_error = ZLINK_CONFIG_OK;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_msg_refcnt (&msg, &refcnt_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_HANDLE, refcnt_error);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

int main (void)
{
    UNITY_BEGIN ();
    RUN_TEST (test_zlink_socket_null_context);
    RUN_TEST (test_zlink_close_null_socket);
    RUN_TEST (test_zlink_setsockopt_null_socket);
    RUN_TEST (test_zlink_getsockopt_null_socket);
    RUN_TEST (test_zlink_socket_monitor_open_null_socket);
    RUN_TEST (test_zlink_bind_null_socket);
    RUN_TEST (test_zlink_connect_null_socket);
    RUN_TEST (test_zlink_unbind_null_socket);
    RUN_TEST (test_zlink_disconnect_null_socket);
    RUN_TEST (test_zlink_msg_null_handle);
    RUN_TEST (test_zlink_msg_closed_handle_read_is_invalid);


    return UNITY_END ();
}
