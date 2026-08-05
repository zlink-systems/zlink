/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

void setUp ()
{
}

void tearDown ()
{
}

#define THREAD_COUNT 100

struct thread_data
{
    char endpoint[MAX_SOCKET_STRING];
};

extern "C" {
static void worker (void *data_)
{
    const thread_data *const tdata = static_cast<const thread_data *> (data_);

    void *socket = zlink_socket (get_test_context (), ZLINK_SOCKET_SUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (socket, tdata->endpoint));

    //  Start closing the socket while the connecting process is underway.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));
}
}

void test_shutdown_stress ()
{
    void *threads[THREAD_COUNT];

    for (int j = 0; j != 10; j++) {
        //  Check the shutdown with many parallel I/O threads.
        struct thread_data tdata;
        setup_test_context ();
        zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 7);

        void *socket = test_context_socket (ZLINK_SOCKET_PUB);

        bind_loopback_ipv4 (socket, tdata.endpoint, sizeof (tdata.endpoint));

        for (int i = 0; i != THREAD_COUNT; i++) {
            threads[i] = zlink_thread_start (&worker, &tdata);
        }

        for (int i = 0; i != THREAD_COUNT; i++) {
            zlink_thread_join (threads[i]);
        }

        test_context_socket_close (socket);

        teardown_test_context ();
    }
}

int main ()
{
    setup_test_environment (180);

    UNITY_BEGIN ();
    RUN_TEST (test_shutdown_stress);
    return UNITY_END ();
}
