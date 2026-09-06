/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "core/recv_internal.hpp"
#include "core/send_internal.hpp"

#include <chrono>
#include <cstring>
#include <future>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

// Raw send owns one physical admission attempt and its blocking retry loop.
// Public MORE stages a record; public FINAL retries at the submission owner.
// Keep the exact physical-attempt metrics on this transport-free pipe pair.
void test_auto_hwm_blocked_ratio_counts_only_first_physical_attempt ()
{
    void *ctx = get_test_context ();
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    const size_t message_size = 64;
    const uint64_t physical_hwm =
      8u * (message_size + sizeof (zlink_msg_t));
    const uint64_t endpoint_hwm = physical_hwm / 2;
    const int send_timeout_ms = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &endpoint_hwm,
                        sizeof (endpoint_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    contract_socket_pair_t pair (receiver, sender, 0, 0, true, physical_hwm);
    const auto raw_send = [&] (const void *data_, size_t size_, int flags_) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, size_));
        memcpy (zlink_msg_data (&part), data_, size_);
        const int rc = zlink::send_msg_internal (pair.cores[1], &part, flags_);
        const int saved_errno = errno;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        errno = saved_errno;
        return rc;
    };

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    std::vector<char> oversized_more (
      static_cast<size_t> (physical_hwm), 'm');
    TEST_ASSERT_FAILURE_ERRNO (
      EAGAIN,
      raw_send (oversized_more.data (), oversized_more.size (),
                  ZLINK_DONTWAIT | ZLINK_SNDMORE));
    TEST_ASSERT_EQUAL_UINT32 (
      1000000, read_auto_hwm_budget_snapshot (ctx).blocked_ratio_ppm);

    char payload[message_size];
    memset (payload, 'b', sizeof (payload));
    int queued = 0;
    while (raw_send (payload, sizeof (payload), ZLINK_DONTWAIT)
           == static_cast<int> (sizeof (payload)))
        ++queued;
    TEST_ASSERT_GREATER_THAN_INT (0, queued);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t before =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT32 (0, before.blocked_ratio_ppm);

    std::future<int> blocked_send =
      std::async (std::launch::async, [&] () {
          return raw_send (payload, sizeof (payload), 0);
      });

    zlink_auto_hwm_budget_snapshot_t blocked = before;
    const std::chrono::steady_clock::time_point observed_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (blocked.blocked_ratio_ppm != 1000000
           && std::chrono::steady_clock::now () < observed_deadline) {
        msleep (1);
        blocked = read_auto_hwm_budget_snapshot (ctx);
    }
    TEST_ASSERT_EQUAL_UINT32 (1000000, blocked.blocked_ratio_ppm);

    char received[message_size];
    for (int i = 0; i < queued
                    && blocked_send.wait_for (std::chrono::milliseconds (0))
                         != std::future_status::ready;
         ++i) {
        TEST_ASSERT_EQUAL_INT (
          static_cast<int> (sizeof (received)),
          zlink::recv_buffer_internal (pair.cores[0], received, sizeof (received), 0));
        (void) blocked_send.wait_for (std::chrono::milliseconds (20));
    }
    const std::future_status resumed =
      blocked_send.wait_for (std::chrono::seconds (1));
    TEST_ASSERT_EQUAL_INT (std::future_status::ready, resumed);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload)),
                           blocked_send.get ());

    const zlink_auto_hwm_budget_snapshot_t after =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT32 (1000000, after.blocked_ratio_ppm);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_reset_auto_hwm_budget_metrics (ctx));
    const zlink_auto_hwm_budget_snapshot_t reset =
      read_auto_hwm_budget_snapshot (ctx);
    TEST_ASSERT_EQUAL_UINT64 (after.measurement_epoch + 1,
                              reset.measurement_epoch);
    TEST_ASSERT_EQUAL_UINT32 (0, reset.blocked_ratio_ppm);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_auto_hwm_blocked_ratio_counts_only_first_physical_attempt);
    return UNITY_END ();
}
