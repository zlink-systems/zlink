/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

void test_system_max ()
{
    // Keep allocating sockets until we run out of system resources
    const int requested_sockets = 2 * 65536;
    int no_of_sockets = requested_sockets;

    struct rlimit rl;
    if (getrlimit (RLIMIT_NOFILE, &rl) == 0) {
        // Reserve some fds for stdio and runtime, and assume each socket may
        // consume multiple fds (signaler, pipes, etc.). Use a conservative factor.
        const unsigned long reserve_fds = 256;
        const unsigned long fds_per_socket = 4;
        const unsigned long max_fds =
          (rl.rlim_cur == RLIM_INFINITY) ? 1048576UL : static_cast<unsigned long> (rl.rlim_cur);

        if (max_fds > reserve_fds) {
            unsigned long max_sockets = (max_fds - reserve_fds) / fds_per_socket;
            if (max_sockets < static_cast<unsigned long> (no_of_sockets))
                no_of_sockets = static_cast<int> (max_sockets);
        }
    }

    if (no_of_sockets < 1)
        no_of_sockets = 1;

    zlink_ctx_set (get_test_context (), ZLINK_MAX_SOCKETS, no_of_sockets);
    std::vector<void *> sockets;

    while (true) {
        void *socket = zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
        if (!socket)
            break;
        sockets.push_back (socket);
    }
    TEST_ASSERT_LESS_OR_EQUAL (no_of_sockets, static_cast<int> (sockets.size ()));
    printf ("Socket creation failed after %i sockets\n", static_cast<int> (sockets.size ()));

    //  System is out of resources, further calls to zlink_socket should return NULL
    for (unsigned int i = 0; i < 10; ++i) {
        TEST_ASSERT_NULL (zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR));
    }
    // Clean up.
    for (unsigned int i = 0; i < sockets.size (); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
}

void test_zlink_default_max ()
{
    //  Keep allocating sockets until we hit the default limit
    std::vector<void *> sockets;

    while (true) {
        void *socket = zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
        if (!socket)
            break;
        sockets.push_back (socket);
    }
    //  We may stop sooner if system has fewer available sockets
    TEST_ASSERT_LESS_OR_EQUAL (ZLINK_MAX_SOCKETS_DFLT, sockets.size ());

    //  Further calls to zlink_socket should return NULL
    for (unsigned int i = 0; i < 10; ++i) {
        TEST_ASSERT_NULL (zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR));
    }

    //  Clean up
    for (unsigned int i = 0; i < sockets.size (); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_system_max);
    RUN_TEST (test_zlink_default_max);
    return UNITY_END ();
}
