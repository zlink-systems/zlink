/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <stdio.h>

SETUP_TEARDOWN_TESTCONTEXT

static bool recv_byte_with_deadline (void *socket_, char *out_, int timeout_ms_, int *attempts_out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    int attempts = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        ++attempts;
        const int n = zlink_recv (socket_, out_, 1, ZLINK_DONTWAIT);
        if (n == 1) {
            if (attempts_out_)
                *attempts_out_ = attempts;
            return true;
        }
        TEST_ASSERT_EQUAL_INT (-1, n);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        msleep (1);
    }
    if (attempts_out_)
        *attempts_out_ = attempts;
    return false;
}

void test_inproc_pubsub_alternating_does_not_timeout ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    const int linger = 0;
    const int timeout_ms = 1000;
    const uint64_t hwm = 1000u * (1u + sizeof (zlink_msg_t));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://pubsub-alternating"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://pubsub-alternating"));

    msleep (SETTLE_TIME);

    const char send_ch = 'a';
    char recv_ch = 0;
    int attempts = 0;
    msleep (SETTLE_TIME * 4);
    TEST_ASSERT_EQUAL_INT (1, zlink_send (pub, &send_ch, 1, 0));
    TEST_ASSERT_TRUE_MESSAGE (recv_byte_with_deadline (sub, &recv_ch, timeout_ms, &attempts),
                              "subscriber never became ready");
    TEST_ASSERT_EQUAL_UINT8 ((uint8_t) send_ch, (uint8_t) recv_ch);

    int received_count = 0;
    int consecutive_timeouts = 0;
    for (int i = 0; i < 5000; ++i) {
        TEST_ASSERT_EQUAL_INT (1, zlink_send (pub, &send_ch, 1, 0));
        if (!recv_byte_with_deadline (sub, &recv_ch, timeout_ms, &attempts)) {
            consecutive_timeouts++;
            if (consecutive_timeouts > 3) {
                char msg[128];
                snprintf (msg, sizeof (msg),
                          "recv stalled at iter=%d errno=%d attempts=%d "
                          "received=%d consecutive_timeouts=%d",
                          i, errno, attempts, received_count, consecutive_timeouts);
                TEST_FAIL_MESSAGE (msg);
            }
            continue;
        }

        consecutive_timeouts = 0;
        received_count++;
        TEST_ASSERT_EQUAL_UINT8 ((uint8_t) send_ch, (uint8_t) recv_ch);
    }

    TEST_ASSERT_TRUE_MESSAGE (received_count >= 4500,
                              "alternating pub/sub lost too many messages while checking progress");

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_inproc_pubsub_alternating_does_not_timeout);
    return UNITY_END ();
}
