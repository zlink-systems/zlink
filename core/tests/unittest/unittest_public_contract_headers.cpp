/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include <zlink/core/api.h>
#include <zlink/eventing/api.h>
#include <zlink/message/api.h>
#include <zlink/socket/api.h>

#include <unity.h>

void setUp ()
{
}

void tearDown ()
{
}

void test_grouped_contract_headers_compile ()
{
    TEST_ASSERT_TRUE (ZLINK_VERSION_MAJOR >= 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, 0);
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_ctx_new));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_msg_init));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_socket));
    TEST_ASSERT_NOT_NULL (reinterpret_cast<void *> (&zlink_socket_monitor_open));
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_grouped_contract_headers_compile);
    return UNITY_END ();
}
