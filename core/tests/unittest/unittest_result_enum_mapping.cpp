/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "../../src/api/message/bind_result_internal.hpp"
#include "../../src/api/core/close_result_internal.hpp"
#include "../../src/api/core/config_result_internal.hpp"
#include "../../src/api/message/connect_result_internal.hpp"
#include "../../src/api/message/handler_result_internal.hpp"
#include "../../src/api/message/recv_result_internal.hpp"
#include "../../src/api/message/request_result_internal.hpp"
#include "../../src/api/message/submit_result_internal.hpp"

#include <unity.h>

void setUp ()
{
}

void tearDown ()
{
}

void test_recv_unknown_errno_maps_to_internal_error ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INTERNAL_ERROR,
                           zlink::recv_result_internal::from_errno (EPROTO));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INTERNAL_ERROR,
                           zlink::recv_result_internal::from_errno (ENOMEM));
}

void test_request_unknown_errno_maps_to_internal_error ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_INTERNAL_ERROR,
                           zlink::request_result_internal::from_errno (ENOMEM));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_PROTOCOL_ERROR,
                           zlink::request_result_internal::from_errno (EPROTO));
}

void test_request_result_maps_to_canonical_errno ()
{
    TEST_ASSERT_EQUAL_INT (0, zlink::request_result_internal::to_errno (ZLINK_REQUEST_OK));
    TEST_ASSERT_EQUAL_INT (ETIMEDOUT,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_TIMED_OUT));
    TEST_ASSERT_EQUAL_INT (ENOENT,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_NOT_FOUND));
    TEST_ASSERT_EQUAL_INT (ETERM,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_TERMINATED));
    TEST_ASSERT_EQUAL_INT (EPROTO,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_PROTOCOL_ERROR));
    TEST_ASSERT_EQUAL_INT (EIO,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_INTERNAL_ERROR));
    TEST_ASSERT_EQUAL_INT (EACCES,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_REJECTED));
    TEST_ASSERT_EQUAL_INT (ESTALE,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_CONFLICT));
    TEST_ASSERT_EQUAL_INT (EBUSY,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_BUSY));
    TEST_ASSERT_EQUAL_INT (ENOTCONN,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_NOT_CONNECTED));
    TEST_ASSERT_EQUAL_INT (
      EINVAL, zlink::request_result_internal::to_errno (ZLINK_REQUEST_INVALID_ARGUMENT));
    TEST_ASSERT_EQUAL_INT (EFSM,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_INVALID_STATE));
    TEST_ASSERT_EQUAL_INT (ENOTSUP,
                           zlink::request_result_internal::to_errno (ZLINK_REQUEST_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_INT (
      EAGAIN, zlink::request_result_internal::to_errno (ZLINK_REQUEST_BACKPRESSURED));
}

void test_request_errno_contract_matrix ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_REJECTED,
                           zlink::request_result_internal::from_errno (ECANCELED));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_CONFLICT,
                           zlink::request_result_internal::from_errno (EEXIST));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_PROTOCOL_ERROR,
                           zlink::request_result_internal::from_errno (ENOCOMPATPROTO));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TERMINATED,
                           zlink::request_result_internal::from_errno (ESHUTDOWN));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_INVALID_STATE,
                           zlink::request_result_internal::from_errno (EALREADY));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_BACKPRESSURED,
                           zlink::request_result_internal::from_errno (EAGAIN));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_BACKPRESSURED,
                           zlink::request_result_internal::from_errno (ENOBUFS));
}

void test_config_unknown_errno_maps_to_internal_error ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR,
                           zlink::config_result_internal::from_errno (ENOMEM));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (EBUSY));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_FOUND,
                           zlink::config_result_internal::from_errno (ENOENT));
}

void test_config_errno_contract_matrix ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (ESTALE));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (EALREADY));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (ENOTCONN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (ETIMEDOUT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE,
                           zlink::config_result_internal::from_errno (EPROTO));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_CONFLICT,
                           zlink::config_result_internal::from_errno (EEXIST));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_BUFFER_TOO_SMALL,
                           zlink::config_result_internal::from_errno (ENOBUFS));
}

void test_handler_unknown_errno_maps_to_internal_error ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_INTERNAL_ERROR,
                           zlink::handler_result_internal::from_errno (ENOMEM));
}

void test_connect_bind_close_unknown_errno_map_to_internal_error ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INTERNAL_ERROR,
                           zlink::connect_result_internal::from_errno (EIO));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_INTERNAL_ERROR,
                           zlink::bind_result_internal::from_errno (EADDRNOTAVAIL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INTERNAL_ERROR,
                           zlink::close_result_internal::from_errno (EINTR));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_NOT_SUPPORTED,
                           zlink::bind_result_internal::from_errno (EPROTONOSUPPORT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE,
                           zlink::close_result_internal::from_errno (ESTALE));
}

void test_connect_result_maps_peer_disconnect_errnos ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_NOT_FOUND,
                           zlink::connect_result_internal::from_errno (ENOENT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_CONFLICT,
                           zlink::connect_result_internal::from_errno (EADDRINUSE));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_BUSY, zlink::connect_result_internal::from_errno (EBUSY));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_CONFLICT,
                           zlink::connect_result_internal::from_errno (EEXIST));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_CONFLICT,
                           zlink::connect_result_internal::from_errno (ESTALE));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_BUSY,
                           zlink::connect_result_internal::from_errno (ESHUTDOWN));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_AUTH_FAILED,
                           zlink::connect_result_internal::from_errno (EACCES));
}

void test_submit_unknown_errno_is_normalized ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INTERNAL_ERROR,
                           zlink::submit_result_internal::from_errno (EINTR));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OUT_OF_MEMORY,
                           zlink::submit_result_internal::from_errno (ENOMEM));
}

void test_submit_errno_contract_matrix ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED,
                           zlink::submit_result_internal::from_errno (ETIMEDOUT));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED,
                           zlink::submit_result_internal::from_errno (ENOBUFS));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_ADMITTED,
                           zlink::submit_result_internal::from_errno (EACCES));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT,
                           zlink::submit_result_internal::from_errno (EMSGSIZE));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_STATE,
                           zlink::submit_result_internal::from_errno (ESTALE));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_STATE,
                           zlink::submit_result_internal::from_errno (EALREADY));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION,
                           zlink::submit_result_internal::from_errno (EDEADLK));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION,
                           zlink::submit_result_internal::from_errno (EPERM));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_SEQ_EXHAUSTED,
                           zlink::submit_result_internal::from_request_submit_errno (EOVERFLOW));
}

void test_recv_errno_contract_matrix ()
{
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink::recv_result_internal::from_errno (ETIMEDOUT));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_BUFFER_TOO_SMALL,
                           zlink::recv_result_internal::from_errno (ENOBUFS));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INVALID_STATE,
                           zlink::recv_result_internal::from_errno (EINVAL));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INVALID_STATE,
                           zlink::recv_result_internal::from_errno (ESTALE));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_INVALID_STATE,
                           zlink::recv_result_internal::from_errno (ESHUTDOWN));
}

int main (int argc, char *argv[])
{
    UNITY_BEGIN ();
    RUN_TEST (test_recv_unknown_errno_maps_to_internal_error);
    RUN_TEST (test_request_unknown_errno_maps_to_internal_error);
    RUN_TEST (test_request_result_maps_to_canonical_errno);
    RUN_TEST (test_request_errno_contract_matrix);
    RUN_TEST (test_config_unknown_errno_maps_to_internal_error);
    RUN_TEST (test_config_errno_contract_matrix);
    RUN_TEST (test_handler_unknown_errno_maps_to_internal_error);
    RUN_TEST (test_connect_bind_close_unknown_errno_map_to_internal_error);
    RUN_TEST (test_connect_result_maps_peer_disconnect_errnos);
    RUN_TEST (test_submit_unknown_errno_is_normalized);
    RUN_TEST (test_submit_errno_contract_matrix);
    RUN_TEST (test_recv_errno_contract_matrix);
    return UNITY_END ();
}
