/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <string>
#include <string.h>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
zlink_auto_hwm_budget_snapshot_t read_budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}

void init_payload (zlink_msg_t *message_, size_t size_, unsigned char fill_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (message_, size_));
    memset (zlink_msg_data (message_), fill_, size_);
}

void set_hwm (void *socket_, zlink_option_t option_, uint64_t bytes_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, option_, &bytes_, sizeof (bytes_)));
}

bool send_one_nonblocking (void *socket_, size_t size_)
{
    zlink_msg_t message;
    init_payload (&message, size_, 'r');
    const int rc = zlink_send (socket_, &message, 1, ZLINK_DONTWAIT);
    if (rc == 0)
        return true;
    const int saved_errno = errno;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&message));
    errno = saved_errno;
    return false;
}

}

void test_retained_no_data_does_not_require_credit_worker ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);

    zlink_msg_t message;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&message));
    zlink_hwm_budget_lease_t *lease =
      reinterpret_cast<zlink_hwm_budget_lease_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv_with_hwm_budget_lease (
            receiver, &message, &lease, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&message));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&message));
    lease = reinterpret_cast<zlink_hwm_budget_lease_t *> (0x1);
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part_with_hwm_budget_lease (
        receiver, NULL, &message, &lease, &more,
        ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&message));

    test_context_socket_close_zero_linger (receiver);
}

void test_retained_pair_preserves_total_and_releases_credit_cross_thread ()
{
    const char *endpoint = "inproc://retained-hwm-pair-credit";
    const uint64_t hwm = 128;
    const size_t first_payload_size = 1024;

    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    set_hwm (receiver, ZLINK_OPT_RCVHWM, hwm);
    set_hwm (sender, ZLINK_OPT_SNDHWM, hwm);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));

    TEST_ASSERT_TRUE (send_one_nonblocking (sender, first_payload_size));
    const zlink_auto_hwm_budget_snapshot_t queued = read_budget_snapshot ();
    TEST_ASSERT_GREATER_THAN_UINT64 (0, queued.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, queued.application_accounted_bytes);

    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_hwm_budget_lease_t *lease = NULL;
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (first_payload_size),
      zlink_recv_with_hwm_budget_lease (receiver, &received, &lease, 0));
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_EQUAL_UINT64 (first_payload_size, zlink_msg_size (&received));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    const zlink_auto_hwm_budget_snapshot_t retained = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (queued.current_accounted_bytes,
                              retained.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, retained.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (queued.core_queue_accounted_bytes,
                              retained.application_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              retained.outstanding_application_lease_count);
    TEST_ASSERT_EQUAL_UINT64 (retained.application_accounted_bytes,
                              retained.deferred_origin_credit_bytes);

    TEST_ASSERT_FALSE (send_one_nonblocking (sender, 1));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    std::thread releaser ([&lease] () {
        zlink_hwm_budget_lease_release (&lease);
    });
    releaser.join ();
    TEST_ASSERT_NULL (lease);
    zlink_hwm_budget_lease_release (&lease);

    const zlink_auto_hwm_budget_snapshot_t released = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, released.application_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      0, released.outstanding_application_lease_count);
    TEST_ASSERT_EQUAL_UINT64 (0, released.deferred_origin_credit_bytes);

    //  The registry releases the lease exactly once and synchronously, but the
    //  origin credit it hands back is published to the owning pipe by command,
    //  not inside the release call. The snapshot contract is intra-snapshot
    //  coherence, not synchronous visibility of that publication, so the
    //  aggregate is polled until it settles.
    bool credit_published = false;
    const std::chrono::steady_clock::time_point credit_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (std::chrono::steady_clock::now () < credit_deadline) {
        if (read_budget_snapshot ().current_accounted_bytes == 0) {
            credit_published = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (credit_published);

    bool resumed = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (std::chrono::steady_clock::now () < deadline) {
        if (send_one_nonblocking (sender, 1)) {
            resumed = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        msleep (1);
    }
    TEST_ASSERT_TRUE (resumed);

    // Retained receive starts only command processing. Without a registered
    // receive handler the runtime worker must leave caller-visible data queued.
    msleep (20);
    zlink_msg_t *drained = NULL;
    size_t drained_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &drained, &drained_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, drained_count);
    zlink_multipart_close (drained, drained_count);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_retained_part_lease_is_per_frame_and_tombstone_drains_last ()
{
    const char *endpoint = "inproc://retained-hwm-part-tombstone";
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));

    zlink_msg_t sent[2];
    init_payload (&sent[0], 4, 'a');
    init_payload (&sent[1], 6, 'b');
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (sender, sent, 2, 0));
    const zlink_auto_hwm_budget_snapshot_t queued = read_budget_snapshot ();

    zlink_hwm_budget_lease_t *leases[2] = {NULL, NULL};
    for (size_t i = 0; i != 2; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_recv_part_with_hwm_budget_lease (
            receiver, NULL, &part, &leases[i], &more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (leases[i]);
        TEST_ASSERT_EQUAL_INT (i == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
                               more);
        TEST_ASSERT_EQUAL_UINT64 (i == 0 ? 4 : 6,
                                  zlink_msg_size (&part));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    const zlink_auto_hwm_budget_snapshot_t retained = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (queued.current_accounted_bytes,
                              retained.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (2,
                              retained.outstanding_application_lease_count);
    TEST_ASSERT_EQUAL_UINT64 (0, retained.core_queue_accounted_bytes);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
    zlink_auto_hwm_budget_snapshot_t detached = read_budget_snapshot ();
    const std::chrono::steady_clock::time_point retire_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (detached.retired_queue_count == 0
           && std::chrono::steady_clock::now () < retire_deadline) {
        msleep (1);
        detached = read_budget_snapshot ();
    }
    TEST_ASSERT_EQUAL_UINT64 (1, detached.retired_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (2,
                              detached.outstanding_application_lease_count);

    zlink_hwm_budget_lease_release (&leases[1]);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              read_budget_snapshot ().retired_queue_count);
    zlink_hwm_budget_lease_release (&leases[0]);
    const zlink_auto_hwm_budget_snapshot_t drained = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, drained.retired_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              drained.outstanding_application_lease_count);
    TEST_ASSERT_EQUAL_UINT64 (0, drained.current_accounted_bytes);
}

void test_router_synthetic_frame_is_unleased_and_typed_payloads_are_leased ()
{
    const char *endpoint = "inproc://retained-hwm-router";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    zlink_msg_t raw;
    init_payload (&raw, 3, 'g');
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &raw, 1, 0));

    zlink_msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&frame));
    zlink_hwm_budget_lease_t *lease =
      reinterpret_cast<zlink_hwm_budget_lease_t *> (0x1);
    TEST_ASSERT_GREATER_THAN_INT (
      -1, zlink_recv_with_hwm_budget_lease (router, &frame, &lease, 0));
    TEST_ASSERT_NULL (lease);
    TEST_ASSERT_TRUE (test_msg_has_more (&frame));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&frame));
    TEST_ASSERT_EQUAL_INT (
      3, zlink_recv_with_hwm_budget_lease (router, &frame, &lease, 0));
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_FALSE (test_msg_has_more (&frame));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));
    zlink_hwm_budget_lease_release (&lease);

    zlink_msg_t sent[2];
    init_payload (&sent[0], 3, 'a');
    init_payload (&sent[1], 5, 'b');
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, sent, 2, 0));

    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    for (size_t i = 0; i != 2; ++i) {
        const zlink_routing_id_t *source = NULL;
        uint64_t request_seq = UINT64_MAX;
        uint64_t pair_id = 0;
        uint64_t pair_generation = 0;
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&frame));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part_v2_with_hwm_budget_lease (
            router, &source, &request_seq, &pair_id, &pair_generation,
            &frame, &lease, &more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source);
        TEST_ASSERT_NOT_NULL (lease);
        TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
        TEST_ASSERT_GREATER_THAN_UINT64 (0, pair_id);
        TEST_ASSERT_GREATER_THAN_UINT64 (0, pair_generation);
        TEST_ASSERT_EQUAL_INT (i == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
                               more);
        if (i == 0)
            source_rid = *source;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));
        zlink_hwm_budget_lease_release (&lease);
    }

    zlink_msg_t reply;
    init_payload (&reply, 5, 'd');
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_rid (router, &source_rid, &reply, 1, 0));

    uint8_t message_type = 0xff;
    uint64_t request_seq = UINT64_MAX;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&frame));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_dealer_recv_part_with_hwm_budget_lease (
        dealer, &message_type, &request_seq, &frame, &lease, &more,
        ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_DEALER_MESSAGE_RAW, message_type);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));
    zlink_hwm_budget_lease_release (&lease);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_subscribe_retained_part_preserves_topic_and_payload_lease ()
{
    const char *endpoint = "inproc://retained-hwm-subscribe";
    const char *topic = "orders";
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    msleep (SETTLE_TIME);

    zlink_msg_t payload;
    init_payload (&payload, 7, 'p');
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (pub, topic, &payload, 1, 0));

    char topic_out[16];
    size_t topic_len = 0;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_hwm_budget_lease_t *lease = NULL;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_subscribe_part_with_hwm_budget_lease (
        sub, &source, topic_out, sizeof (topic_out), &topic_len, &received,
        &lease, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (topic, topic_out, topic_len);
    TEST_ASSERT_EQUAL_UINT64 (7, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    zlink_hwm_budget_lease_release (&lease);

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_subscribe_retained_topic_buffer_retry_keeps_first_part ()
{
    const char *endpoint = "inproc://retained-hwm-subscribe-long-topic";
    const std::string topic (300, 't');
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    const int recv_timeout_ms = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));
    msleep (SETTLE_TIME);

    zlink_msg_t sent[2];
    init_payload (&sent[0], 5, 'a');
    init_payload (&sent[1], 6, 'b');
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_publish (pub, topic.c_str (), sent, 2, 0));

    char too_small[16];
    memset (too_small, 'x', sizeof (too_small));
    size_t topic_len = 0;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_hwm_budget_lease_t *lease =
      reinterpret_cast<zlink_hwm_budget_lease_t *> (0x1);
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      zlink_subscribe_part_with_hwm_budget_lease (
        sub, &source, too_small, sizeof (too_small), &topic_len, &received,
        &lease, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
    TEST_ASSERT_EQUAL_UINT64 (topic.size (), topic_len);
    TEST_ASSERT_EQUAL_MEMORY ("xxxxxxxxxxxxxxxx", too_small,
                              sizeof (too_small));
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), source);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&received));
    TEST_ASSERT_NULL (lease);
    TEST_ASSERT_EQUAL_UINT64 (
      0, read_budget_snapshot ().outstanding_application_lease_count);

    std::vector<char> topic_out (topic.size ());
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_subscribe_part_with_hwm_budget_lease (
        sub, &source, &topic_out[0], topic_out.size (), &topic_len, &received,
        &lease, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (topic.size (), topic_len);
    TEST_ASSERT_EQUAL_MEMORY (topic.data (), &topic_out[0], topic.size ());
    TEST_ASSERT_EQUAL_UINT64 (5, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY ("aaaaa", zlink_msg_data (&received), 5);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, more);
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    zlink_hwm_budget_lease_release (&lease);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_subscribe_part_with_hwm_budget_lease (
        sub, &source, &topic_out[0], topic_out.size (), &topic_len, &received,
        &lease, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (6, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY ("bbbbbb", zlink_msg_data (&received), 6);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    zlink_hwm_budget_lease_release (&lease);

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_context_shutdown_invalidates_retained_credit_once ()
{
    const char *endpoint = "inproc://retained-hwm-context-shutdown";
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *receiver = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *sender = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_NOT_NULL (sender);

    const int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &linger,
                        sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));

    zlink_msg_t sent[2];
    init_payload (&sent[0], 8, 'x');
    init_payload (&sent[1], 9, 'y');
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (sender, sent, 2, 0));

    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_hwm_budget_lease_t *lease = NULL;
    TEST_ASSERT_EQUAL_INT (
      8, zlink_recv_with_hwm_budget_lease (receiver, &received, &lease, 0));
    TEST_ASSERT_NOT_NULL (lease);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_hwm_budget_lease_t *rejected =
      reinterpret_cast<zlink_hwm_budget_lease_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      -1,
      zlink_recv_with_hwm_budget_lease (receiver, &received, &rejected,
                                        ZLINK_DONTWAIT));
    TEST_ASSERT_NULL (rejected);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sender));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (receiver));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));

    zlink_hwm_budget_lease_release (&lease);
    TEST_ASSERT_NULL (lease);
    zlink_hwm_budget_lease_release (&lease);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_retained_no_data_does_not_require_credit_worker);
    RUN_TEST (
      test_retained_pair_preserves_total_and_releases_credit_cross_thread);
    RUN_TEST (
      test_retained_part_lease_is_per_frame_and_tombstone_drains_last);
    RUN_TEST (
      test_router_synthetic_frame_is_unleased_and_typed_payloads_are_leased);
    RUN_TEST (
      test_subscribe_retained_part_preserves_topic_and_payload_lease);
    RUN_TEST (
      test_subscribe_retained_topic_buffer_retry_keeps_first_part);
    RUN_TEST (test_context_shutdown_invalidates_retained_credit_once);
    return UNITY_END ();
}
