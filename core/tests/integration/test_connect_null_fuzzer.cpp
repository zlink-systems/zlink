/* SPDX-License-Identifier: MPL-2.0 */

#ifdef ZLINK_USE_FUZZING_ENGINE
#include <fuzzer/FuzzedDataProvider.h>
#endif

#include "testutil.hpp"
#include "testutil_unity.hpp"

// Test that the ZMTP engine handles invalid handshake when connecting
// https://rfc.zlink.org/spec/37/
extern "C" int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    setup_test_context ();
    char my_endpoint[MAX_SOCKET_STRING];
    fd_t server = bind_socket_resolve_port ("127.0.0.1", "0", my_endpoint);

    void *client = test_context_socket (ZLINK_SOCKET_SUB);
    //  As per API by default there's no limit to the size of a message,
    //  but the sanitizer allocator will barf over a gig or so
    int64_t max_msg_size = 64 * 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_MAXMSGSIZE, &max_msg_size, sizeof (int64_t)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (client, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, my_endpoint));

    fd_t server_accept = TEST_ASSERT_SUCCESS_RAW_ERRNO (accept (server, NULL));

    //  If there is not enough data for a full greeting, just send what we can
    //  Otherwise send greeting first, as expected by the protocol
    uint8_t buf[64];
    if (size >= 64) {
        send (server_accept, (void *) data, 64, MSG_NOSIGNAL);
        data += 64;
        size -= 64;
    }
    recv (server_accept, buf, 64, 0);
    msleep (250);
    for (ssize_t sent = 0; size > 0 && (sent != -1 || errno == EINTR);
         size -= sent > 0 ? sent : 0, data += sent > 0 ? sent : 0)
        sent = send (server_accept, (const char *) data, size, MSG_NOSIGNAL);
    msleep (250);

    zlink_msg_t msg;
    zlink_msg_init (&msg);
    while (-1 != test_recv_single_msg (&msg, client, ZLINK_DONTWAIT)) {
        zlink_msg_close (&msg);
        zlink_msg_init (&msg);
    }

    close (server_accept);
    close (server);

    test_context_socket_close_zero_linger (client);
    teardown_test_context ();

    return 0;
}

#ifndef ZLINK_USE_FUZZING_ENGINE
void test_connect_null_fuzzer ()
{
    uint8_t **data;
    size_t *len, num_cases = 0;
    if (fuzzer_corpus_encode ("tests/libzlink-fuzz-corpora/test_connect_null_fuzzer_seed_corpus",
                              &data, &len, &num_cases)
        != 0)
        exit (77);

    while (num_cases-- > 0) {
        TEST_ASSERT_SUCCESS_ERRNO (LLVMFuzzerTestOneInput (data[num_cases], len[num_cases]));
        free (data[num_cases]);
    }

    free (data);
    free (len);
}

int main (int argc, char **argv)
{
    LIBZLINK_UNUSED (argc);
    LIBZLINK_UNUSED (argv);

    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_connect_null_fuzzer);

    return UNITY_END ();
}
#endif
