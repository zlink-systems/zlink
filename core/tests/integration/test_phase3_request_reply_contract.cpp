/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kWaitMilliseconds = 3000;
const size_t kReplyTokenCapacity = 65536;

bool should_run_phase3_request_test (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

void init_part (zlink_msg_t *part_, const char *payload_)
{
    const size_t size = strlen (payload_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size));
    if (size != 0)
        memcpy (zlink_msg_data (part_), payload_, size);
}

std::string part_string (zlink_msg_t *part_)
{
    return std::string (static_cast<const char *> (zlink_msg_data (part_)),
                        zlink_msg_size (part_));
}

void assert_part_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

void set_routing_id_text (void *socket_, const char *value_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (socket_, value_, strlen (value_)));
}

zlink_completion_id_t send_public_request (void *dealer_, const char *payload_,
                                           uint32_t timeout_ms_ = 120000)
{
    zlink_msg_t request;
    init_part (&request, payload_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer_, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms_, NULL, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_part_consumed (&request);
    return completion_id;
}

void process_socket_commands_through_public_api (void *socket_)
{
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (socket_, ZLINK_OPT_EVENTS, &events, &events_size));
}

zlink_auto_hwm_budget_snapshot_t read_auto_hwm_budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}

zlink_routing_id_t make_rid (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    TEST_ASSERT_TRUE (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    if (size != 0)
        memcpy (rid.data, value_, size);
    return rid;
}

void init_empty_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

void assert_empty_completion (const zlink_completion_t &completion_)
{
    TEST_ASSERT_EQUAL_UINT32 (sizeof (zlink_completion_t),
                              completion_.struct_size);
    TEST_ASSERT_EQUAL_INT (0, completion_.kind);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_.completion_id);
    TEST_ASSERT_NULL (completion_.user_context);
    TEST_ASSERT_EQUAL_UINT (0, completion_.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (0, completion_.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion_.send_terminal_errno);
    TEST_ASSERT_EQUAL_INT (0, completion_.request_result);
    TEST_ASSERT_NULL (completion_.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_.reply_part_count);
}

zlink_completion_t receive_completion_eventually (void *socket_)
{
    zlink_completion_t completion;
    init_empty_completion (&completion);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        errno = 0;
        const zlink_recv_result_t result = zlink_completion_recv (
          socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK)
            return completion;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        assert_empty_completion (completion);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for REQUEST completion");
    return completion;
}

void assert_no_completion_for (void *socket_, int duration_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (duration_ms_);
    do {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_completion_recv (socket_, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        assert_empty_completion (completion);
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
}

struct router_part_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    size_t part_flag;
    std::string payload;
};

router_part_t receive_router_part_eventually (void *router_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        size_t part_flag = 1;
        errno = 0;
        const zlink_recv_result_t result = zlink_router_recv (router_, &source_rid, &reply_token, &part, 1, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source_rid);
            router_part_t received;
            received.source_rid = *source_rid;
            received.reply_token = reply_token;
            received.part_flag = part_flag;
            received.payload = part_string (&part);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return received;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ROUTER request part");
    return router_part_t ();
}

void receive_dealer_data_eventually (void *dealer_, const char *expected_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        size_t part_flag = 1;
        errno = 0;
        const zlink_recv_result_t result = zlink_recv (dealer_, NULL, &part, 1, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_INT (1, part_flag);
            TEST_ASSERT_EQUAL_STRING (expected_, part_string (&part).c_str ());
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for DEALER data");
}

void prime_router_dealer_route (void *dealer_, void *router_)
{
    zlink_msg_t prime;
    init_part (&prime, "prime");
    zlink_completion_id_t completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (dealer_, &prime, 1, ZLINK_SEND_FLAGS_NONE, NULL, &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&prime);

    const router_part_t received = receive_router_part_eventually (router_);
    TEST_ASSERT_EQUAL_UINT64 (0, received.reply_token);
    TEST_ASSERT_EQUAL_INT (1, received.part_flag);
    TEST_ASSERT_EQUAL_STRING ("prime", received.payload.c_str ());
}

void test_request_outputs_are_zeroed_and_parts_are_always_consumed ()
{
    zlink_msg_t invalid_handle;
    init_part (&invalid_handle, "invalid-handle");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_request (NULL, NULL, &invalid_handle, 1, ZLINK_SEND_FLAGS_NONE, 50, NULL, &completion_id));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&invalid_handle);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const zlink_routing_id_t rid = make_rid ("source");
    zlink_msg_t invalid_reply;
    init_part (&invalid_reply, "invalid-token");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_reply (router, &rid, 0, &invalid_reply, 1));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    assert_part_consumed (&invalid_reply);

    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_router_public_request_reply_completion_and_token_consumption ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (router, "router-public", 13));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (dealer, "dealer-public", 13));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-public-request-roundtrip"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-public-request-roundtrip"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    int request_context = 42;
    zlink_msg_t request[2];
    init_part (&request[0], "request-head");
    init_part (&request[1], "request-tail");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer, NULL, request, 2, ZLINK_SEND_FLAGS_NONE, 2000,
                     &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request[0]);
    assert_part_consumed (&request[1]);

    const zlink_routing_id_t *request_source = NULL;
    zlink_reply_token_t request_token = 0;
    zlink_msg_t received_request[2];
    size_t received_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &request_source, &request_token,
                         received_request, 2, &received_count,
                         ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (request_source);
    TEST_ASSERT_EQUAL_UINT64 (2, received_count);
    TEST_ASSERT_EQUAL_STRING ("dealer-public",
                              std::string (
                                reinterpret_cast<const char *> (
                                  request_source->data),
                                request_source->size)
                                .c_str ());
    TEST_ASSERT_NOT_EQUAL (0, request_token);
    TEST_ASSERT_EQUAL_STRING (
      "request-head", part_string (&received_request[0]).c_str ());
    TEST_ASSERT_EQUAL_STRING (
      "request-tail", part_string (&received_request[1]).c_str ());
    const zlink_routing_id_t request_rid = *request_source;
    zlink_multipart_close (received_request, received_count);

    zlink_msg_t reply[2];
    init_part (&reply[0], "reply-head");
    init_part (&reply[1], "reply-tail");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &request_rid, request_token, reply, 2));
    assert_part_consumed (&reply[0]);
    assert_part_consumed (&reply[1]);

    zlink_msg_t duplicate_reply;
    init_part (&duplicate_reply, "duplicate");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply (router, &request_rid, request_token, &duplicate_reply, 1));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&duplicate_reply);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (0, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_NOT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
    TEST_ASSERT_EQUAL_PTR (completion.reply_parts + 1,
                           &completion.reply_parts[1]);
    TEST_ASSERT_EQUAL_STRING (
      "reply-head", part_string (&completion.reply_parts[0]).c_str ());
    TEST_ASSERT_EQUAL_STRING (
      "reply-tail", part_string (&completion.reply_parts[1]).c_str ());

    zlink_completion_close (&completion);
    assert_empty_completion (completion);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_router_reply_remains_on_application_fifo_and_accounting ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "router-single-lane-accounting");
    set_routing_id_text (dealer, "dealer-single-lane-accounting");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router,
                  "inproc://phase3-single-lane-reply-accounting"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer,
                     "inproc://phase3-single-lane-reply-accounting"));
    msleep (SETTLE_TIME);

    const zlink_completion_id_t request_id =
      send_public_request (dealer, "single-lane-accounting-request");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
    TEST_ASSERT_EQUAL_INT (1, request.part_flag);
    TEST_ASSERT_EQUAL_STRING ("single-lane-accounting-request",
                              request.payload.c_str ());

    zlink_auto_hwm_budget_snapshot_t baseline;
    TEST_ASSERT_TRUE (zlink_test_wait_until (kWaitMilliseconds, [&] {
        baseline = read_auto_hwm_budget_snapshot ();
        return baseline.current_accounted_bytes == 0
               && baseline.active_directional_queue_count == 2
               && baseline.active_completion_directional_queue_count == 0;
    }));
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1,
                              baseline.abi_version);
    TEST_ASSERT_EQUAL_UINT32 (sizeof (baseline), baseline.struct_size);
    TEST_ASSERT_EQUAL_UINT64 (0, baseline.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, baseline.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, baseline.completion_pending_message_count);

    const char data[] = "data-before-single-lane-reply";
    zlink_msg_t data_part;
    init_part (&data_part, data);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_rid (router, &request.source_rid, &data_part, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    assert_part_consumed (&data_part);

    zlink_auto_hwm_budget_snapshot_t data_queued;
    TEST_ASSERT_TRUE (zlink_test_wait_until (kWaitMilliseconds, [&] {
        data_queued = read_auto_hwm_budget_snapshot ();
        return data_queued.current_accounted_bytes
               > baseline.current_accounted_bytes;
    }));

    const char reply_payload[] = "single-lane-accounting-reply";
    zlink_msg_t reply;
    init_part (&reply, reply_payload);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    zlink_auto_hwm_budget_snapshot_t reply_queued;
    TEST_ASSERT_TRUE (zlink_test_wait_until (kWaitMilliseconds, [&] {
        reply_queued = read_auto_hwm_budget_snapshot ();
        return reply_queued.current_accounted_bytes
               > data_queued.current_accounted_bytes;
    }));
    TEST_ASSERT_EQUAL_UINT64 (2,
                              reply_queued.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      0, reply_queued.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (reply_queued.current_accounted_bytes,
                              reply_queued.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, reply_queued.application_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              reply_queued.provisional_accounted_bytes);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64 (reply_queued.current_accounted_bytes,
                                         reply_queued.peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      0, reply_queued.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              reply_queued.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              reply_queued.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (reply_queued.current_accounted_bytes,
                              reply_queued.total_messaging_accounted_bytes);

    // DATA and REPLY share the Application FIFO. A completion receive cannot
    // skip the DATA head, so the reply charge remains visible until the public
    // data receive advances that FIFO.
    assert_no_completion_for (dealer, 20);
    receive_dealer_data_eventually (dealer, data);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      reply_payload, part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);

    zlink_auto_hwm_budget_snapshot_t drained;
    TEST_ASSERT_TRUE (zlink_test_wait_until (kWaitMilliseconds, [&] {
        drained = read_auto_hwm_budget_snapshot ();
        return drained.current_accounted_bytes
                 == baseline.current_accounted_bytes
               && drained.completion_current_accounted_bytes == 0;
    }));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_request_to_dealer_is_rejected_as_peer_type ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (router, "requester", 9));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, "dealer-only", 11));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-router-request-dealer"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-router-request-dealer"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    const zlink_routing_id_t dealer_rid = make_rid ("dealer-only");
    zlink_msg_t request;
    init_part (&request, "wrong-peer-type");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_ADMITTED,
      zlink_request (router, &dealer_rid, &request, 1, ZLINK_SEND_FLAGS_NONE, 100, NULL, &completion_id));
    TEST_ASSERT_EQUAL_INT (EPROTOTYPE, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&request);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_request_with_only_dealer_peer_is_not_connected ()
{
    void *requester = test_context_socket (ZLINK_SOCKET_DEALER);
    void *peer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (requester);
    TEST_ASSERT_NOT_NULL (peer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (requester, "requester", 9));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (peer, "dealer-peer", 11));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (peer, "inproc://phase3-dealer-only-request"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (requester, "inproc://phase3-dealer-only-request"));
    msleep (SETTLE_TIME);

    zlink_msg_t prime;
    init_part (&prime, "prime");
    zlink_completion_id_t prime_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (requester, &prime, 1, ZLINK_SEND_FLAGS_NONE, NULL, &prime_id));
    TEST_ASSERT_EQUAL_UINT64 (0, prime_id);
    assert_part_consumed (&prime);
    receive_dealer_data_eventually (peer, "prime");

    zlink_msg_t request;
    init_part (&request, "no-router");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_request (requester, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE, 100, NULL, &completion_id));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&request);

    test_context_socket_close_zero_linger (requester);
    test_context_socket_close_zero_linger (peer);
}

void test_dealer_dontwait_request_times_out_with_one_request_completion ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, "timeout-dealer", 14));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-public-request-timeout"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-public-request-timeout"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    int request_context = 73;
    zlink_msg_t request;
    init_part (&request, "no-reply");
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT, 25, &request_context, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_part_consumed (&request);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           completion.request_result);
    TEST_ASSERT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);

    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (dealer, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_reply_timeout_resolution_is_exactly_once_under_race ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "timeout-race-dealer");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-request-reply-timeout-race"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-request-reply-timeout-race"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    size_t reply_wins = 0;
    size_t timeout_wins = 0;
    const int iterations = 20;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const std::string request_payload =
          "timeout-race-request-" + std::to_string (iteration);
        const std::string reply_payload =
          "timeout-race-reply-" + std::to_string (iteration);
        int request_context = iteration;
        const uint32_t timeout_ms = iteration % 4 == 0 ? 100 : 15;

        zlink_msg_t request;
        init_part (&request, request_payload.c_str ());
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms, &request_context, &completion_id));
        TEST_ASSERT_NOT_EQUAL (0, completion_id);
        assert_part_consumed (&request);

        const router_part_t received = receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
        TEST_ASSERT_EQUAL_STRING (request_payload.c_str (),
                                  received.payload.c_str ());

        if (iteration % 4 == 1)
            msleep (30);
        else if (iteration % 4 == 2)
            msleep (14);
        else if (iteration % 4 == 3)
            msleep (16);

        zlink_msg_t reply;
        init_part (&reply, reply_payload.c_str ());
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply (router, &received.source_rid, received.reply_token, &reply, 1));
        assert_part_consumed (&reply);

        zlink_completion_t completion = receive_completion_eventually (dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
        if (completion.request_result == ZLINK_REQUEST_OK) {
            ++reply_wins;
            TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
            TEST_ASSERT_EQUAL_STRING (
              reply_payload.c_str (),
              part_string (&completion.reply_parts[0]).c_str ());
        } else {
            ++timeout_wins;
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                                   completion.request_result);
            TEST_ASSERT_NULL (completion.reply_parts);
            TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
        }
        zlink_completion_close (&completion);
        assert_no_completion_for (dealer, static_cast<int> (timeout_ms) + 10);
    }

    TEST_ASSERT_TRUE (reply_wins > 0);
    TEST_ASSERT_TRUE (timeout_wins > 0);
    TEST_ASSERT_EQUAL_UINT64 (iterations, reply_wins + timeout_wins);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_none_request_waits_for_never_handshaken_router ()
{
    const char *const endpoint = endpoint_3 ();
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    const int send_timeout_ms = 3000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    int request_context = 174;
    zlink_msg_t request;
    init_part (&request, "wait-for-first-router");
    std::atomic<bool> submit_started (false);
    std::atomic<bool> submit_done (false);
    zlink_submit_result_t submit_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_completion_id_t request_id = 0;
    std::thread submit_thread ([&] () {
        submit_started.store (true, std::memory_order_release);
        submit_result = zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE, 3000, &request_context, &request_id);
        submit_done.store (true, std::memory_order_release);
    });
    while (!submit_started.load (std::memory_order_acquire))
        msleep (1);
    msleep (40);
    TEST_ASSERT_FALSE (submit_done.load (std::memory_order_acquire));

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    set_routing_id_text (router, "first-handshake-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    submit_thread.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);

    const router_part_t received = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
    TEST_ASSERT_EQUAL_STRING ("wait-for-first-router",
                              received.payload.c_str ());
    zlink_msg_t reply;
    init_part (&reply, "first-router-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &received.source_rid, received.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "first-router-reply",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_request_with_only_zero_weight_router_gets_wait_token ()
{
    const char *const endpoint =
      "inproc://phase3-request-zero-weight-router";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "zero-weight-router");
    const int zero_weight = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT,
                               &zero_weight, sizeof (zero_weight)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    const std::chrono::steady_clock::time_point weight_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    do {
        process_socket_commands_through_public_api (router);
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    } while (std::chrono::steady_clock::now () < weight_deadline);

    zlink_msg_t request;
    init_part (&request, "must-not-select-zero-weight");
    zlink_completion_id_t request_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_DONTWAIT, 1000, NULL, &request_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_admitted_request_completes_not_connected_on_physical_detach_without_replay_after_same_rid_reconnect ()
{
    const char *const endpoint =
      "inproc://phase3-admitted-request-physical-reconnect";
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *first_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (first_router);
    set_routing_id_text (first_router, "request-reconnect-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (dealer, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (first_router, endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, first_router);

    int request_context = 181;
    zlink_msg_t request;
    init_part (&request, "admitted-before-physical-detach");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE, 600, &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);

    const router_part_t admitted =
      receive_router_part_eventually (first_router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);
    TEST_ASSERT_EQUAL_STRING ("admitted-before-physical-detach",
                              admitted.payload.c_str ());

    test_context_socket_close_zero_linger (first_router);
    first_router = NULL;
    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED,
                           completion.request_result);
    TEST_ASSERT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);

    void *replacement_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_router);
    set_routing_id_text (replacement_router, "request-reconnect-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_router, endpoint));
    msleep (SETTLE_TIME);
    // A DATA marker on the replacement route must be its first record. The
    // admitted REQUEST payload is never replayed after reconnect.
    prime_router_dealer_route (dealer, replacement_router);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (replacement_router);
    test_context_socket_close_zero_linger (dealer);
}

void test_request_completions_are_drained_once_by_id ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (dealer, "request-completion", 18));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-request-completion-drain"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-request-completion-drain"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    int request_contexts[2] = {121, 122};
    zlink_completion_id_t request_ids[2] = {0, 0};
    const char *const request_payloads[2] = {"first-request",
                                             "second-request"};
    const char *const reply_payloads[2] = {"first-reply", "second-reply"};
    for (size_t i = 0; i != 2; ++i) {
        zlink_msg_t request_part;
        init_part (&request_part, request_payloads[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request (dealer, NULL, &request_part, 1, ZLINK_SEND_FLAGS_DONTWAIT, 2000, &request_contexts[i], &request_ids[i]));
        TEST_ASSERT_NOT_EQUAL (0, request_ids[i]);
        assert_part_consumed (&request_part);
    }
    TEST_ASSERT_NOT_EQUAL (request_ids[0], request_ids[1]);

    for (size_t i = 0; i != 2; ++i) {
        const router_part_t request =
          receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
        TEST_ASSERT_EQUAL_STRING (request_payloads[i],
                                  request.payload.c_str ());

        zlink_msg_t reply;
        init_part (&reply, reply_payloads[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
        assert_part_consumed (&reply);
    }

    bool saw_request[2] = {false, false};
    for (size_t completion_index = 0; completion_index != 2;
         ++completion_index) {
        zlink_completion_t completion =
          receive_completion_eventually (dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);

        size_t request_index = 2;
        if (completion.completion_id == request_ids[0])
            request_index = 0;
        else if (completion.completion_id == request_ids[1])
            request_index = 1;
        TEST_ASSERT_TRUE_MESSAGE (
          request_index != 2,
          "REQUEST completion queue returned an unknown completion id");
        TEST_ASSERT_FALSE (saw_request[request_index]);
        saw_request[request_index] = true;
        TEST_ASSERT_EQUAL_PTR (&request_contexts[request_index],
                               completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                               completion.request_result);
        TEST_ASSERT_NOT_NULL (completion.reply_parts);
        TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
        TEST_ASSERT_EQUAL_STRING (
          reply_payloads[request_index],
          part_string (&completion.reply_parts[0]).c_str ());
        zlink_completion_close (&completion);
    }
    TEST_ASSERT_TRUE (saw_request[0]);
    TEST_ASSERT_TRUE (saw_request[1]);

    zlink_completion_t empty;
    init_empty_completion (&empty);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (dealer, &empty, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (empty);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_registry_capacity_fair_pollin_and_round_robin_redrive ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *fill_dealers[4] = {
      test_context_socket (ZLINK_SOCKET_DEALER),
      test_context_socket (ZLINK_SOCKET_DEALER),
      test_context_socket (ZLINK_SOCKET_DEALER),
      test_context_socket (ZLINK_SOCKET_DEALER)};
    void *first_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *second_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *data_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    static const char *const fill_rids[4] = {
      "reply-cap-fill-0", "reply-cap-fill-1", "reply-cap-fill-2",
      "reply-cap-fill-3"};
    for (size_t i = 0; i != 4; ++i) {
        TEST_ASSERT_NOT_NULL (fill_dealers[i]);
        set_routing_id_text (fill_dealers[i], fill_rids[i]);
    }
    TEST_ASSERT_NOT_NULL (first_dealer);
    TEST_ASSERT_NOT_NULL (second_dealer);
    TEST_ASSERT_NOT_NULL (data_dealer);
    set_routing_id_text (first_dealer, "reply-cap-first");
    set_routing_id_text (second_dealer, "reply-cap-second");
    set_routing_id_text (data_dealer, "reply-cap-data");

    const uint64_t large_hwm = 128u * 1024u * 1024u;
    for (size_t i = 0; i != 4; ++i)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (fill_dealers[i], ZLINK_OPT_SNDHWM, &large_hwm,
                            sizeof (large_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &large_hwm,
                        sizeof (large_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-reply-token-capacity"));
    for (size_t i = 0; i != 4; ++i)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (fill_dealers[i],
                         "inproc://phase3-reply-token-capacity"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (first_dealer,
                     "inproc://phase3-reply-token-capacity"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (second_dealer,
                     "inproc://phase3-reply-token-capacity"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (data_dealer,
                     "inproc://phase3-reply-token-capacity"));
    msleep (SETTLE_TIME);
    for (size_t i = 0; i != 4; ++i)
        prime_router_dealer_route (fill_dealers[i], router);
    prime_router_dealer_route (first_dealer, router);
    prime_router_dealer_route (second_dealer, router);
    prime_router_dealer_route (data_dealer, router);

    router_part_t first_request;
    const size_t requests_per_fill_pipe = kReplyTokenCapacity / 4;
    for (size_t i = 0; i != kReplyTokenCapacity; ++i) {
        (void) send_public_request (
          fill_dealers[i / requests_per_fill_pipe], "fill");
        const router_part_t received =
          receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
        TEST_ASSERT_EQUAL_INT (1, received.part_flag);
        TEST_ASSERT_EQUAL_STRING ("fill", received.payload.c_str ());
        if (i == 0)
            first_request = received;
    }

    (void) send_public_request (first_dealer, "blocked-first");
    (void) send_public_request (second_dealer, "blocked-second");

    // This DATA is physically behind blocked-first on the same pipe. Registry
    // pressure may skip the pipe, but must never let the DATA overtake REQUEST.
    zlink_msg_t behind_blocked_request;
    init_part (&behind_blocked_request, "same-pipe-after-blocked-request");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (first_dealer, &behind_blocked_request, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    assert_part_consumed (&behind_blocked_request);

    // Readiness itself must discover and pause both capacity-blocked heads.
    // A receive call has deliberately not run since they were enqueued.
    zlink_pollitem_t item = {router, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 0, NULL));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t no_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&no_part));
    size_t part_flag = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv (router, &source_rid, &reply_token, &no_part, 1, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&no_part));

    zlink_msg_t data;
    init_part (&data, "fair-data");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (data_dealer, &data, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    assert_part_consumed (&data);
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, kWaitMilliseconds, NULL));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
    const router_part_t fair_data = receive_router_part_eventually (router);
    TEST_ASSERT_EQUAL_UINT64 (0, fair_data.reply_token);
    TEST_ASSERT_EQUAL_STRING ("fair-data", fair_data.payload.c_str ());
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 0, NULL));

    zlink_msg_t reply;
    init_part (&reply, "release-capacity");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &first_request.source_rid, first_request.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    const router_part_t redriven_a = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, redriven_a.reply_token);
    TEST_ASSERT_TRUE (redriven_a.payload == "blocked-first"
                      || redriven_a.payload == "blocked-second");

    bool received_same_pipe_data = false;
    if (redriven_a.payload == "blocked-first") {
        // The DATA behind that REQUEST is visible only after the REQUEST itself
        // was admitted to the token registry. It needs no token and may now
        // progress even while the registry is full again.
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poll (&item, 1, kWaitMilliseconds, NULL));
        TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
        const router_part_t same_pipe_data =
          receive_router_part_eventually (router);
        TEST_ASSERT_EQUAL_UINT64 (0, same_pipe_data.reply_token);
        TEST_ASSERT_EQUAL_STRING ("same-pipe-after-blocked-request",
                                  same_pipe_data.payload.c_str ());
        received_same_pipe_data = true;
    }

    // Receiving one redriven REQUEST fills the registry again. The other
    // blocked source stays paused until this newly occupied slot is released.
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 0, NULL));
    init_part (&reply, "release-first-round-robin-redrive");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &redriven_a.source_rid, redriven_a.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, kWaitMilliseconds, NULL));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
    const router_part_t redriven_b = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, redriven_b.reply_token);
    TEST_ASSERT_TRUE (redriven_b.payload == "blocked-first"
                      || redriven_b.payload == "blocked-second");
    TEST_ASSERT_TRUE (redriven_b.payload != redriven_a.payload);

    if (redriven_b.payload == "blocked-first") {
        TEST_ASSERT_FALSE (received_same_pipe_data);
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poll (&item, 1, kWaitMilliseconds, NULL));
        TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);
        const router_part_t same_pipe_data =
          receive_router_part_eventually (router);
        TEST_ASSERT_EQUAL_UINT64 (0, same_pipe_data.reply_token);
        TEST_ASSERT_EQUAL_STRING ("same-pipe-after-blocked-request",
                                  same_pipe_data.payload.c_str ());
        received_same_pipe_data = true;
    }
    TEST_ASSERT_TRUE (received_same_pipe_data);

    init_part (&reply, "release-second-round-robin-redrive");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &redriven_b.source_rid, redriven_b.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (data_dealer);
    test_context_socket_close_zero_linger (second_dealer);
    test_context_socket_close_zero_linger (first_dealer);
    for (size_t i = 0; i != 4; ++i)
        test_context_socket_close_zero_linger (fill_dealers[i]);
    test_context_socket_close_zero_linger (router);
}

void test_router_explicit_logical_rid_removal_invalidates_reply_token ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-token-removed-rid");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-reply-token-rid-removal"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-reply-token-rid-removal"));
    msleep (SETTLE_TIME);

    (void) send_public_request (dealer, "remove-rid");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
    TEST_ASSERT_EQUAL_STRING ("remove-rid", request.payload.c_str ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_disconnect_rid (router, &request.source_rid));

    zlink_msg_t reply;
    init_part (&reply, "must-not-send");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_physical_disconnect_preserves_token_for_same_rid_reconnect ()
{
    const char *const endpoint =
      "inproc://phase3-reply-token-same-rid-reconnect";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-token-reconnect");
    const int sndtimeo = kWaitMilliseconds;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    (void) send_public_request (dealer, "retain-token");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
    TEST_ASSERT_EQUAL_STRING ("retain-token", request.payload.c_str ());

    test_context_socket_close_zero_linger (dealer);
    for (int attempt = 0; attempt != 20; ++attempt) {
        process_socket_commands_through_public_api (router);
        msleep (1);
    }

    void *replacement = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (replacement);
    set_routing_id_text (replacement, "reply-token-reconnect");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement, endpoint));
    msleep (SETTLE_TIME);

    zlink_msg_t reply;
    init_part (&reply, "reply-after-reconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
    assert_part_consumed (&reply);

    init_part (&reply, "duplicate-after-reconnect");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (replacement);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_final_distinguishes_context_and_socket_shutdown ()
{
    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);
        set_routing_id_text (dealer, "reply-context-peer");
        const int zero_linger = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_BIND_OK,
          zlink_bind (router, "inproc://phase3-reply-context-term"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (dealer, "inproc://phase3-reply-context-term"));
        msleep (SETTLE_TIME);

        (void) send_public_request (dealer, "reply-context-request");
        const router_part_t request = receive_router_part_eventually (router);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
        zlink_msg_t reply[2];
        init_part (&reply[0], "discarded-context-prefix");
        init_part (&reply[1], "context-final");
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_TERMINATED,
          zlink_reply (router, &request.source_rid, request.reply_token, reply,
                       2));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_part_consumed (&reply[0]);
        assert_part_consumed (&reply[1]);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }

    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);
        set_routing_id_text (dealer, "reply-socket-peer");
        const int zero_linger = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_BIND_OK,
          zlink_bind (router, "inproc://phase3-reply-socket-close"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (dealer, "inproc://phase3-reply-socket-close"));
        msleep (SETTLE_TIME);

        (void) send_public_request (dealer, "reply-socket-request");
        const router_part_t request = receive_router_part_eventually (router);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
        zlink_msg_t reply[2];
        init_part (&reply[0], "discarded-socket-prefix");
        init_part (&reply[1], "socket-final");
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_TERMINATED,
          zlink_reply (router, &request.source_rid, request.reply_token, reply,
                       2));
        TEST_ASSERT_EQUAL_INT (ESHUTDOWN, zlink_errno ());
        assert_part_consumed (&reply[0]);
        assert_part_consumed (&reply[1]);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}

void test_router_reply_final_timeout_retains_token_for_full_retry ()
{
    const char *const endpoint =
      "inproc://phase3-reply-final-timeout-retry";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-timeout-peer");
    int sndtimeo = 25;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    (void) send_public_request (dealer, "timeout-request");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);

    test_context_socket_close_zero_linger (dealer);
    for (int attempt = 0; attempt != 20; ++attempt) {
        process_socket_commands_through_public_api (router);
        msleep (1);
    }

    zlink_msg_t timed_out_reply[2];
    init_part (&timed_out_reply[0], "timed-out-prefix");
    init_part (&timed_out_reply[1], "timed-out-reply");
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_reply (router, &request.source_rid, request.reply_token,
                   timed_out_reply, 2));
    const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_TRUE (elapsed_ms >= 15);
    TEST_ASSERT_TRUE (elapsed_ms < kWaitMilliseconds);
    assert_part_consumed (&timed_out_reply[0]);
    assert_part_consumed (&timed_out_reply[1]);

    // A zero-budget retry proves timeout released the checkout while the
    // token itself remained live.
    sndtimeo = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));
    zlink_msg_t zero_budget_retry;
    init_part (&zero_budget_retry, "zero-budget-retry");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_reply (router, &request.source_rid, request.reply_token,
                   &zero_budget_retry, 1));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_part_consumed (&zero_budget_retry);

    void *replacement = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (replacement);
    set_routing_id_text (replacement, "reply-timeout-peer");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement, endpoint));
    sndtimeo = kWaitMilliseconds;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));

    zlink_msg_t retry[2];
    init_part (&retry[0], "retry-prefix-after-reconnect");
    init_part (&retry[1], "retry-final-after-reconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router, &request.source_rid, request.reply_token, retry, 2));
    assert_part_consumed (&retry[0]);
    assert_part_consumed (&retry[1]);

    zlink_msg_t consumed_token;
    init_part (&consumed_token, "consumed-token");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply (router, &request.source_rid, request.reply_token,
                   &consumed_token, 1));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&consumed_token);

    test_context_socket_close_zero_linger (replacement);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_final_waits_for_same_rid_reconnect ()
{
    const char *const endpoint =
      "inproc://phase3-reply-final-waits-reconnect";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-wait-peer");
    const int sndtimeo = kWaitMilliseconds;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    (void) send_public_request (dealer, "wait-reconnect-request");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);

    test_context_socket_close_zero_linger (dealer);
    for (int attempt = 0; attempt != 20; ++attempt) {
        process_socket_commands_through_public_api (router);
        msleep (1);
    }

    zlink_msg_t reply;
    init_part (&reply, "reply-during-reconnect");
    void *replacement = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (replacement);
    set_routing_id_text (replacement, "reply-wait-peer");
    std::atomic<bool> entered (false);
    std::atomic<bool> finished (false);
    zlink_submit_result_t result = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread reply_thread ([&] () {
        entered.store (true, std::memory_order_release);
        errno = 0;
        result = zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1);
        finished.store (true, std::memory_order_release);
    });
    while (!entered.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (20);
    const bool was_waiting = !finished.load (std::memory_order_acquire);
    const zlink_connect_result_t connect_result =
      zlink_connect (replacement, endpoint);
    reply_thread.join ();

    TEST_ASSERT_TRUE (was_waiting);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, connect_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    assert_part_consumed (&reply);

    init_part (&reply, "already-consumed");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply (router, &request.source_rid, request.reply_token, &reply, 1));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (replacement);
    test_context_socket_close_zero_linger (router);
}

struct concurrent_reqrep_payload_t
{
    uint32_t caller;
    uint32_t sequence;
    uint32_t part;
};

class reqrep_barrier_t
{
  public:
    explicit reqrep_barrier_t (int participants_) :
        _participants (participants_), _arrived (0), _generation (0)
    {
    }

    void wait ()
    {
        std::unique_lock<std::mutex> lock (_mutex);
        const int generation = _generation;
        if (++_arrived == _participants) {
            _arrived = 0;
            ++_generation;
            _cv.notify_all ();
            return;
        }
        _cv.wait (lock, [&] { return _generation != generation; });
    }

  private:
    const int _participants;
    int _arrived;
    int _generation;
    std::mutex _mutex;
    std::condition_variable _cv;
};

struct concurrent_reply_target_t
{
    zlink_routing_id_t rid;
    zlink_reply_token_t token;
};

void test_router_reply_distinct_tokens_submit_concurrently ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    set_routing_id_text (dealer, "distinct-token-requester");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-distinct-reply-tokens"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-distinct-reply-tokens"));
    msleep (SETTLE_TIME);

    (void) send_public_request (dealer, "distinct-token-a");
    (void) send_public_request (dealer, "distinct-token-b");
    concurrent_reply_target_t targets[2];
    for (int i = 0; i != 2; ++i) {
        const router_part_t request = receive_router_part_eventually (router);
        targets[i].rid = request.source_rid;
        targets[i].token = request.reply_token;
        TEST_ASSERT_NOT_EQUAL (0, targets[i].token);
    }
    TEST_ASSERT_NOT_EQUAL (targets[0].token, targets[1].token);

    reqrep_barrier_t more_barrier (2);
    std::atomic<int> failures (0);
    std::thread workers[2];
    for (int i = 0; i != 2; ++i) {
        workers[i] = std::thread ([&, i] {
            zlink_msg_t reply[2];
            init_part (&reply[0],
                       i == 0 ? "distinct-a-more" : "distinct-b-more");
            init_part (&reply[1],
                       i == 0 ? "distinct-a-final" : "distinct-b-final");
            more_barrier.wait ();
            if (zlink_reply (router, &targets[i].rid, targets[i].token, reply, 2)
                != ZLINK_SUBMIT_OK)
                failures.fetch_add (1, std::memory_order_relaxed);
            zlink_multipart_close (reply, 2);
        });
    }
    workers[0].join ();
    workers[1].join ();
    TEST_ASSERT_EQUAL_INT (0, failures.load (std::memory_order_relaxed));

    for (int i = 0; i != 2; ++i) {
        zlink_completion_t completion = receive_completion_eventually (dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
        zlink_completion_close (&completion);
    }
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_and_reply_four_callers_complete_independently ()
{
    const int caller_count = 4;
    const int rounds = 20;
    const int total = caller_count * rounds;
    zlink_completion_id_t request_ids[caller_count][rounds] = {};
    uintptr_t request_contexts[caller_count][rounds] = {};
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    set_routing_id_text (dealer, "concurrent-requester");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-concurrent-request-reply"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-concurrent-request-reply"));
    msleep (SETTLE_TIME);

    reqrep_barrier_t request_more_barrier (caller_count);
    reqrep_barrier_t request_final_barrier (caller_count);
    std::atomic<int> request_failures (0);
    std::vector<std::thread> requesters;
    for (int caller = 0; caller < caller_count; ++caller) {
        requesters.emplace_back ([&, caller] {
            for (int sequence = 0; sequence < rounds; ++sequence) {
                concurrent_reqrep_payload_t payload = {
                  static_cast<uint32_t> (caller),
                  static_cast<uint32_t> (sequence), 0};
                zlink_msg_t parts[2];
                if (zlink_msg_init_size (&parts[0], sizeof (payload)) != 0) {
                    request_failures.fetch_add (1, std::memory_order_relaxed);
                    request_more_barrier.wait ();
                    request_final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&parts[0]), &payload, sizeof (payload));
                payload.part = 1;
                if (zlink_msg_init_size (&parts[1], sizeof (payload)) != 0) {
                    request_failures.fetch_add (1, std::memory_order_relaxed);
                    zlink_msg_close (&parts[0]);
                    request_more_barrier.wait ();
                    request_final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&parts[1]), &payload,
                        sizeof (payload));
                request_more_barrier.wait ();
                zlink_completion_id_t completion_id = 0;
                request_contexts[caller][sequence] =
                  static_cast<uintptr_t> (caller * rounds + sequence + 1);
                if (zlink_request (dealer, NULL, parts, 2,
                                   ZLINK_SEND_FLAGS_NONE, 120000,
                                   &request_contexts[caller][sequence],
                                   &completion_id)
                      != ZLINK_SUBMIT_OK
                    || completion_id == 0)
                    request_failures.fetch_add (1, std::memory_order_relaxed);
                request_ids[caller][sequence] = completion_id;
                zlink_multipart_close (parts, 2);
                request_final_barrier.wait ();
            }
        });
    }

    std::vector<concurrent_reply_target_t> targets (total);
    bool seen[caller_count][rounds] = {};
    for (int record = 0; record < total; ++record) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t token = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (router, &source_rid, &token, &parts, &part_count,
                             ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_NOT_EQUAL (0, token);
        TEST_ASSERT_EQUAL_UINT64 (2, part_count);
        concurrent_reqrep_payload_t first = {};
        concurrent_reqrep_payload_t second = {};
        TEST_ASSERT_EQUAL_UINT64 (sizeof (first), zlink_msg_size (&parts[0]));
        TEST_ASSERT_EQUAL_UINT64 (sizeof (second), zlink_msg_size (&parts[1]));
        memcpy (&first, zlink_msg_data (&parts[0]), sizeof (first));
        memcpy (&second, zlink_msg_data (&parts[1]), sizeof (second));
        TEST_ASSERT_EQUAL_UINT32 (first.caller, second.caller);
        TEST_ASSERT_EQUAL_UINT32 (first.sequence, second.sequence);
        TEST_ASSERT_EQUAL_UINT32 (0, first.part);
        TEST_ASSERT_EQUAL_UINT32 (1, second.part);
        TEST_ASSERT_TRUE (first.caller < static_cast<uint32_t> (caller_count));
        TEST_ASSERT_TRUE (first.sequence < static_cast<uint32_t> (rounds));
        const int index = static_cast<int> (first.caller) * rounds
                          + static_cast<int> (first.sequence);
        TEST_ASSERT_FALSE (seen[first.caller][first.sequence]);
        seen[first.caller][first.sequence] = true;
        targets[index].rid = *source_rid;
        targets[index].token = token;
        zlink_multipart_close (parts, part_count);
    }
    for (std::vector<std::thread>::iterator it = requesters.begin ();
         it != requesters.end (); ++it)
        it->join ();
    TEST_ASSERT_EQUAL_INT (0,
                           request_failures.load (std::memory_order_relaxed));
    for (int caller = 0; caller != caller_count; ++caller)
        for (int sequence = 0; sequence != rounds; ++sequence) {
            TEST_ASSERT_TRUE (seen[caller][sequence]);
            TEST_ASSERT_NOT_EQUAL (0, request_ids[caller][sequence]);
        }

    reqrep_barrier_t reply_more_barrier (caller_count);
    reqrep_barrier_t reply_final_barrier (caller_count);
    std::atomic<int> reply_failures (0);
    std::vector<std::thread> repliers;
    for (int caller = 0; caller < caller_count; ++caller) {
        repliers.emplace_back ([&, caller] {
            for (int sequence = 0; sequence < rounds; ++sequence) {
                const concurrent_reply_target_t &target =
                  targets[caller * rounds + sequence];
                concurrent_reqrep_payload_t payload = {
                  static_cast<uint32_t> (caller),
                  static_cast<uint32_t> (sequence), 0};
                zlink_msg_t parts[2];
                if (zlink_msg_init_size (&parts[0], sizeof (payload)) != 0) {
                    reply_failures.fetch_add (1, std::memory_order_relaxed);
                    reply_more_barrier.wait ();
                    reply_final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&parts[0]), &payload, sizeof (payload));
                payload.part = 1;
                if (zlink_msg_init_size (&parts[1], sizeof (payload)) != 0) {
                    reply_failures.fetch_add (1, std::memory_order_relaxed);
                    zlink_msg_close (&parts[0]);
                    reply_more_barrier.wait ();
                    reply_final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&parts[1]), &payload,
                        sizeof (payload));
                reply_more_barrier.wait ();
                if (zlink_reply (router, &target.rid, target.token, parts, 2)
                    != ZLINK_SUBMIT_OK)
                    reply_failures.fetch_add (1, std::memory_order_relaxed);
                zlink_multipart_close (parts, 2);
                reply_final_barrier.wait ();
            }
        });
    }

    bool completed[caller_count][rounds] = {};
    for (int i = 0; i < total; ++i) {
        zlink_completion_t completion = receive_completion_eventually (dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
        concurrent_reqrep_payload_t first = {};
        concurrent_reqrep_payload_t second = {};
        TEST_ASSERT_EQUAL_UINT64 (
          sizeof (first), zlink_msg_size (&completion.reply_parts[0]));
        TEST_ASSERT_EQUAL_UINT64 (
          sizeof (second), zlink_msg_size (&completion.reply_parts[1]));
        memcpy (&first, zlink_msg_data (&completion.reply_parts[0]),
                sizeof (first));
        memcpy (&second, zlink_msg_data (&completion.reply_parts[1]),
                sizeof (second));
        TEST_ASSERT_EQUAL_UINT32 (first.caller, second.caller);
        TEST_ASSERT_EQUAL_UINT32 (first.sequence, second.sequence);
        TEST_ASSERT_EQUAL_UINT32 (0, first.part);
        TEST_ASSERT_EQUAL_UINT32 (1, second.part);
        TEST_ASSERT_TRUE (first.caller
                          < static_cast<uint32_t> (caller_count));
        TEST_ASSERT_TRUE (first.sequence
                          < static_cast<uint32_t> (rounds));
        TEST_ASSERT_EQUAL_UINT64 (
          request_ids[first.caller][first.sequence], completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (
          &request_contexts[first.caller][first.sequence],
          completion.user_context);
        TEST_ASSERT_FALSE (completed[first.caller][first.sequence]);
        completed[first.caller][first.sequence] = true;
        zlink_completion_close (&completion);
    }
    for (std::vector<std::thread>::iterator it = repliers.begin ();
         it != repliers.end (); ++it)
        it->join ();
    TEST_ASSERT_EQUAL_INT (0,
                           reply_failures.load (std::memory_order_relaxed));
    for (int caller = 0; caller != caller_count; ++caller)
        for (int sequence = 0; sequence != rounds; ++sequence)
            TEST_ASSERT_TRUE (completed[caller][sequence]);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

}

int main ()
{
    setup_test_environment (180);
    UNITY_BEGIN ();

#define RUN_PHASE3_REQUEST_TEST(test_)                                      \
    do {                                                                    \
        if (should_run_phase3_request_test (#test_))                        \
            RUN_TEST (test_);                                               \
    } while (false)

    RUN_PHASE3_REQUEST_TEST (
      test_request_outputs_are_zeroed_and_parts_are_always_consumed);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_router_public_request_reply_completion_and_token_consumption);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_router_reply_remains_on_application_fifo_and_accounting);
    RUN_PHASE3_REQUEST_TEST (
      test_router_request_to_dealer_is_rejected_as_peer_type);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_request_with_only_dealer_peer_is_not_connected);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_dontwait_request_times_out_with_one_request_completion);
    RUN_PHASE3_REQUEST_TEST (
      test_request_reply_timeout_resolution_is_exactly_once_under_race);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_none_request_waits_for_never_handshaken_router);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_request_with_only_zero_weight_router_gets_wait_token);
    RUN_PHASE3_REQUEST_TEST (
      test_admitted_request_completes_not_connected_on_physical_detach_without_replay_after_same_rid_reconnect);
    RUN_PHASE3_REQUEST_TEST (test_request_completions_are_drained_once_by_id);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_registry_capacity_fair_pollin_and_round_robin_redrive);
    RUN_PHASE3_REQUEST_TEST (
      test_router_explicit_logical_rid_removal_invalidates_reply_token);
    RUN_PHASE3_REQUEST_TEST (
      test_router_physical_disconnect_preserves_token_for_same_rid_reconnect);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_distinguishes_context_and_socket_shutdown);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_timeout_retains_token_for_full_retry);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_waits_for_same_rid_reconnect);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_distinct_tokens_submit_concurrently);
    RUN_PHASE3_REQUEST_TEST (
      test_request_and_reply_four_callers_complete_independently);

#undef RUN_PHASE3_REQUEST_TEST

    return UNITY_END ();
}
