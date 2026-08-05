/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstring>
#include <future>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kSendTimeoutMs = 2000;
const size_t kMsgSize = 64;
const uint64_t kPipeHwm = 100u * (kMsgSize + sizeof (zlink_msg_t));
const uint64_t kSocketHwm = kPipeHwm / 2;
const int kLwmMessages = 50;
const char *kEndpoint = "inproc://pair_send_blocking_wakeup";
const char *kTimeoutEndpoint = "inproc://pair_send_blocking_timeout";

void configure_pair_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &kSocketHwm, sizeof (kSocketHwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &kSocketHwm, sizeof (kSocketHwm)));
}

void fill_until_hwm (void *sender_, const char *buffer_)
{
    int sent = 0;
    while (zlink_send (sender_, buffer_, kMsgSize, ZLINK_DONTWAIT) == static_cast<int> (kMsgSize))
        ++sent;

    TEST_ASSERT_GREATER_THAN_INT (0, sent);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}
} // namespace

void test_pair_blocking_send_wakes_only_after_lwm_reads ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    configure_pair_socket (receiver);
    configure_pair_socket (sender);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDTIMEO, &kSendTimeoutMs, sizeof (kSendTimeoutMs)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, kEndpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, kEndpoint));

    char send_buf[kMsgSize];
    char recv_buf[kMsgSize];
    memset (send_buf, 'a', sizeof (send_buf));
    memset (recv_buf, 0, sizeof (recv_buf));

    fill_until_hwm (sender, send_buf);

    auto send_future = std::async (std::launch::async,
                                   [&] () { return zlink_send (sender, send_buf, kMsgSize, 0); });

    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    for (int i = 0; i < kLwmMessages - 1; ++i) {
        const int rc = zlink_recv (receiver, recv_buf, kMsgSize, 0);
        TEST_ASSERT_EQUAL_INT (static_cast<int> (kMsgSize), rc);
    }

    const std::future_status state_before_lwm =
      send_future.wait_for (std::chrono::milliseconds (100));

    const int lwm_rc = zlink_recv (receiver, recv_buf, kMsgSize, 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (kMsgSize), lwm_rc);

    const std::future_status state_after_lwm =
      send_future.wait_for (std::chrono::milliseconds (1500));
    if (state_after_lwm != std::future_status::ready)
        send_future.wait ();
    const int send_rc = send_future.get ();

    TEST_ASSERT_EQUAL_INT (std::future_status::timeout, state_before_lwm);
    TEST_ASSERT_EQUAL_INT (std::future_status::ready, state_after_lwm);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (kMsgSize), send_rc);

    test_context_socket_close (sender);
    test_context_socket_close (receiver);
}

void test_pair_blocking_send_times_out_without_recv ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    configure_pair_socket (receiver);
    configure_pair_socket (sender);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDTIMEO, &kSendTimeoutMs, sizeof (kSendTimeoutMs)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, kTimeoutEndpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, kTimeoutEndpoint));

    char send_buf[kMsgSize];
    memset (send_buf, 'b', sizeof (send_buf));

    fill_until_hwm (sender, send_buf);

    void *stopwatch = zlink_stopwatch_start ();
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_send (sender, send_buf, kMsgSize, 0));
    const unsigned int elapsed_ms = zlink_stopwatch_stop (stopwatch) / 1000;

    TEST_ASSERT_TRUE (elapsed_ms >= static_cast<unsigned int> (kSendTimeoutMs - 250));

    test_context_socket_close (sender);
    test_context_socket_close (receiver);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pair_blocking_send_wakes_only_after_lwm_reads);
    RUN_TEST (test_pair_blocking_send_times_out_without_recv);
    return UNITY_END ();
}
