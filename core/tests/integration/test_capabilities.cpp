/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

void setUp ()
{
}

void tearDown ()
{
}

void test_capabilities ()
{
#if defined(ZLINK_HAVE_IPC)
    TEST_ASSERT_TRUE (zlink_has ("ipc"));
#else
    TEST_ASSERT_TRUE (!zlink_has ("ipc"));
#endif

#if defined(ZLINK_HAVE_TLS)
    TEST_ASSERT_TRUE (zlink_has ("tls"));
#else
    TEST_ASSERT_TRUE (!zlink_has ("tls"));
#endif

#if defined(ZLINK_HAVE_WS)
    TEST_ASSERT_TRUE (zlink_has ("ws"));
#else
    TEST_ASSERT_TRUE (!zlink_has ("ws"));
#endif

#if defined(ZLINK_HAVE_WSS)
    TEST_ASSERT_TRUE (zlink_has ("wss"));
#else
    TEST_ASSERT_TRUE (!zlink_has ("wss"));
#endif

    TEST_ASSERT_TRUE (!zlink_has ("pgm"));
    TEST_ASSERT_TRUE (!zlink_has ("epgm"));

    // TIPC is removed
    TEST_ASSERT_TRUE (!zlink_has ("tipc"));

    // NORM is removed
    TEST_ASSERT_TRUE (!zlink_has ("norm"));

    // CURVE is removed
    TEST_ASSERT_TRUE (!zlink_has ("curve"));

    // GSSAPI is removed
    TEST_ASSERT_TRUE (!zlink_has ("gssapi"));

    // VMCI is removed
    TEST_ASSERT_TRUE (!zlink_has ("vmci"));

    // Draft API is removed
    TEST_ASSERT_TRUE (!zlink_has ("draft"));
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_capabilities);
    return UNITY_END ();
}
