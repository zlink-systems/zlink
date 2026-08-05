/* SPDX-License-Identifier: MPL-2.0 */

#ifdef ZLINK_USE_FUZZING_ENGINE
#include <fuzzer/FuzzedDataProvider.h>
#endif

#include "testutil.hpp"
#include "testutil_unity.hpp"

extern "C" int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    const zlink_option_t options[] = {ZLINK_OPT_AFFINITY,
                                      ZLINK_OPT_RATE,
                                      ZLINK_OPT_RECOVERY_IVL,
                                      ZLINK_OPT_SNDBUF,
                                      ZLINK_OPT_RCVBUF,
                                      ZLINK_OPT_LINGER,
                                      ZLINK_OPT_RECONNECT_IVL,
                                      ZLINK_OPT_BACKLOG,
                                      ZLINK_OPT_RECONNECT_IVL_MAX,
                                      ZLINK_OPT_MAXMSGSIZE,
                                      ZLINK_OPT_SNDHWM,
                                      ZLINK_OPT_RCVHWM,
                                      ZLINK_OPT_MULTICAST_HOPS,
                                      ZLINK_OPT_RCVTIMEO,
                                      ZLINK_OPT_SNDTIMEO,
                                      ZLINK_OPT_TCP_KEEPALIVE,
                                      ZLINK_OPT_TCP_KEEPALIVE_CNT,
                                      ZLINK_OPT_TCP_KEEPALIVE_IDLE,
                                      ZLINK_OPT_TCP_KEEPALIVE_INTVL,
                                      ZLINK_OPT_IMMEDIATE,
                                      ZLINK_OPT_IPV6,
                                      ZLINK_OPT_CONFLATE,
                                      ZLINK_OPT_TOS,
                                      ZLINK_OPT_HANDSHAKE_IVL,
                                      ZLINK_OPT_BLOCKY,
                                      ZLINK_OPT_INVERT_MATCHING,
                                      ZLINK_OPT_CONNECT_TIMEOUT,
                                      ZLINK_OPT_TCP_MAXRT,
                                      ZLINK_OPT_MULTICAST_MAXTPDU,
                                      ZLINK_OPT_BINDTODEVICE,
                                      ZLINK_OPT_ZMP_METADATA,
                                      ZLINK_OPT_TCP_NODELAY};
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *server = zlink_socket (ctx, ZLINK_SOCKET_XPUB);
    TEST_ASSERT_NOT_NULL (server);

    if (!size)
        return 0;

    for (size_t i = 0; i < sizeof (options) / sizeof (options[0]); ++i) {
        uint8_t out[8192];
        size_t out_size = 8192;

        zlink_set_option (server, options[i], data, size);
        zlink_get_option (server, options[i], out, &out_size);
    }

    zlink_close (server);
    zlink_ctx_term (ctx);

    return 0;
}

#ifndef ZLINK_USE_FUZZING_ENGINE
void test_socket_options_fuzzer ()
{
    uint8_t **data;
    size_t *len, num_cases = 0;
    if (fuzzer_corpus_encode ("tests/libzlink-fuzz-corpora/test_socket_options_fuzzer_seed_corpus",
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
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_socket_options_fuzzer);

    return UNITY_END ();
}
#endif
