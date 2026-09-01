/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

zlink_recv_result_t recv_pair_part_eventually (
  void *socket_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_, zlink_part_flag_t *has_more_out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t result = zlink_recv_part (
          socket_, source_rid_out_, part_out_, has_more_out_,
          static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (result == ZLINK_RECV_OK)
            return result;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for PAIR part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

void send_final (void *socket_, const std::string &payload_)
{
    zlink_msg_t part;
    init_part (&part, payload_);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
}

void expect_final (void *socket_, const std::string &payload_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_pair_part_eventually (socket_, &source_rid, &part, &has_more));
    TEST_ASSERT_NULL (source_rid);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT64 (payload_.size (), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload_.data (), zlink_msg_data (&part),
                              payload_.size ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

zlink_submit_result_t try_send_sized (void *socket_, size_t size_, char fill_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size_));
    if (size_ != 0)
        memset (zlink_msg_data (&part), fill_, size_);
    return zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                            ZLINK_PART_FINAL, NULL, NULL);
}

size_t fill_pair_until_backpressured (void *sender_, size_t payload_size_)
{
    for (size_t admitted = 0; admitted != 256; ++admitted) {
        const zlink_submit_result_t result =
          try_send_sized (sender_, payload_size_, 'f');
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            return admitted;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_FAIL_MESSAGE ("PAIR did not reach configured byte HWM");
    return 0;
}

bool drain_one_pair_part (void *receiver_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const zlink_recv_result_t result = zlink_recv_part (
      receiver_, NULL, &part, &has_more,
      static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
    if (result == ZLINK_RECV_NO_DATA) {
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        return false;
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return true;
}

} // namespace

void test_pair_bidirectional_parts_have_null_source_and_exclusive_peer ()
{
    const char *endpoint = "inproc://gap-h3-pair-exclusive";
    void *bound = test_context_socket (ZLINK_SOCKET_PAIR);
    void *first = test_context_socket (ZLINK_SOCKET_PAIR);
    void *second = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (bound, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (first, endpoint));

    send_final (first, "first-to-bound");
    expect_final (bound, "first-to-bound");
    send_final (bound, "bound-to-first");
    expect_final (first, "bound-to-first");

    //  Establishing another inproc pipe must not replace the already selected
    //  PAIR peer. The existing peer remains the only bidirectional route.
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (second, endpoint));
    msleep (SETTLE_TIME);
    send_final (bound, "still-first");
    expect_final (first, "still-first");

    zlink_msg_t absent;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&absent));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (second, NULL, &absent, &has_more,
                       static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&absent));

    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (first);
    test_context_socket_close_zero_linger (bound);
}

void test_pair_failed_staged_record_is_atomic_and_next_submit_restarts ()
{
    const char *endpoint = "inproc://gap-h3-pair-record-atomic";
    const int64_t max_message_size = 1024;
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (receiver, ZLINK_OPT_MAXMSGSIZE, &max_message_size,
                        sizeof (max_message_size)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (receiver, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (sender, endpoint));

    zlink_msg_t head;
    init_part (&head, std::string (400, 'h'));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &head, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_MORE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&head));

    zlink_msg_t oversized_tail;
    init_part (&oversized_tail, std::string (700, 't'));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (sender, &oversized_tail, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&oversized_tail));

    zlink_msg_t absent;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&absent));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (receiver, NULL, &absent, &has_more,
                       static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&absent));

    send_final (sender, "fresh-record");

    zlink_msg_t observed_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&observed_part));
    const zlink_routing_id_t *source_rid = NULL;
    has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_pair_part_eventually (receiver, &source_rid, &observed_part,
                                 &has_more));
    const zlink_part_flag_t observed_has_more = has_more;
    const std::string observed_payload (
      static_cast<const char *> (zlink_msg_data (&observed_part)),
      zlink_msg_size (&observed_part));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&observed_part));
    while (has_more == ZLINK_PART_MORE) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_msg_init (&observed_part));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          recv_pair_part_eventually (receiver, &source_rid, &observed_part,
                                     &has_more));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_msg_close (&observed_part));
    }

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);

    TEST_ASSERT_EQUAL_INT_MESSAGE (
      ZLINK_PART_FINAL, observed_has_more,
      "failed staged PAIR record leaked into the next record");
    TEST_ASSERT_EQUAL_STRING ("fresh-record", observed_payload.c_str ());
}

void test_pair_receive_flow_is_unsupported_without_monitor_side_effects ()
{
    const char *endpoint = "inproc://gap-h3-pair-no-flow";
    const uint64_t hwm = 4096;
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_bind (receiver, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_connect (sender, endpoint));

    test_monitor_probe_t probe;
    const zlink_socket_monitor_event_mask_t flow_events =
      ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED
      | ZLINK_EVENT_FLOW_STATE_STALE;
    void *monitor = open_test_monitor_probe (sender, flow_events, &probe);
    TEST_ASSERT_NOT_NULL (monitor);

    uint64_t sndhwm_before = 0;
    size_t sndhwm_size = sizeof (sndhwm_before);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (sender, ZLINK_OPT_SNDHWM, &sndhwm_before,
                        &sndhwm_size));
    TEST_ASSERT_EQUAL_UINT64 (hwm, sndhwm_before);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_socket_set_receive_flow_state (sender,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());

    uint64_t sndhwm_after = 0;
    sndhwm_size = sizeof (sndhwm_after);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (sender, ZLINK_OPT_SNDHWM, &sndhwm_after,
                        &sndhwm_size));
    TEST_ASSERT_EQUAL_UINT64 (sndhwm_before, sndhwm_after);

    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor, &status));
    TEST_ASSERT_EQUAL_UINT32 (
      0u, status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_paused_connections);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_pause_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_resume_applied_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_state_stale_total);
    TEST_ASSERT_EQUAL_UINT64 (0, status.flow_pause_duration_ms);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 0, 200));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

int main ()
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (test_pair_bidirectional_parts_have_null_source_and_exclusive_peer);
    RUN_TEST (test_pair_failed_staged_record_is_atomic_and_next_submit_restarts);
    RUN_TEST (test_pair_receive_flow_is_unsupported_without_monitor_side_effects);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
