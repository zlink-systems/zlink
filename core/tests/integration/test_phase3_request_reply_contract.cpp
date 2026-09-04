/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/socket_request_reply_internal.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"

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

zlink::socket_base_t *as_socket (void *socket_)
{
    return as_socket_handle (socket_).socket;
}

struct completion_budget_barrier_t
{
    completion_budget_barrier_t () :
        socket (NULL),
        pair_id (0),
        generation (0),
        entered (false),
        released (false)
    {
    }

    bool wait_until_entered (int timeout_ms_)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (
          lock, std::chrono::milliseconds (timeout_ms_),
          [this] { return entered; });
    }

    void release ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        released = true;
        changed.notify_all ();
    }

    zlink::socket_base_t *socket;
    uint64_t pair_id;
    uint64_t generation;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered;
    bool released;
};

void completion_budget_barrier_hook (zlink::socket_base_t *socket_,
                                     zlink::pipe_t *pipe_, void *userdata_)
{
    completion_budget_barrier_t *const barrier =
      static_cast<completion_budget_barrier_t *> (userdata_);
    if (!barrier || socket_ != barrier->socket || !pipe_
        || pipe_->get_transport_pair_id () != barrier->pair_id
        || pipe_->get_transport_pair_generation () != barrier->generation)
        return;

    //  One-shot before blocking: no later drain can retain stack userdata if a
    //  test assertion aborts after this exact owner turn is released.
    zlink::socket_reqrep_internal::
      test_set_completion_pipe_budget_exhausted_hook (NULL, NULL);
    std::unique_lock<std::mutex> lock (barrier->mutex);
    barrier->entered = true;
    barrier->changed.notify_all ();
    barrier->changed.wait (lock, [barrier] { return barrier->released; });
}

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
      zlink_request_part (dealer_, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, timeout_ms_, NULL,
                          &completion_id));
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
    zlink_part_flag_t part_flag;
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
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        errno = 0;
        const zlink_recv_result_t result = zlink_router_recv_part (
          router_, &source_rid, &reply_token, &part, &part_flag,
          ZLINK_RECV_FLAGS_DONTWAIT);
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

void wait_for_ready_pair (void *socket_, void *first_peer_progress_socket_,
                          void *second_peer_progress_socket_,
                          const zlink_routing_id_t &peer_rid_,
                          uint64_t *pair_id_out_, uint64_t *generation_out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        if (first_peer_progress_socket_)
            process_socket_commands_through_public_api (
              first_peer_progress_socket_);
        if (second_peer_progress_socket_)
            process_socket_commands_through_public_api (
              second_peer_progress_socket_);
        process_socket_commands_through_public_api (socket_);
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        if (as_socket (socket_)->select_routed_submit_target (&peer_rid_,
                                                              &target)
              == 0
            && target.transport_pair_id != 0
            && target.transport_pair_generation != 0
            && as_socket (socket_)->test_pair_pipe (
                 target.transport_pair_id,
                 target.transport_pair_generation, false)
            && as_socket (socket_)->test_pair_pipe (
                 target.transport_pair_id,
                 target.transport_pair_generation, true)) {
            *pair_id_out_ = target.transport_pair_id;
            *generation_out_ = target.transport_pair_generation;
            return;
        }
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ready transport pair");
}

void wait_for_completion_pair_queued (void *socket_, uint64_t pair_id_,
                                      uint64_t generation_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        process_socket_commands_through_public_api (socket_);
        if (as_socket (socket_)->test_completion_pair_queued (
              pair_id_, generation_))
            return;
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for queued completion pair");
}

void wait_for_completion_pair_queued_without_drain (
  void *socket_, uint64_t pair_id_, uint64_t generation_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        // A registered poller is the sole completion owner. Process only the
        // socket's mailbox commands here so this assertion observes the
        // physical ready queue before POLLCOMPLETION drains it.
        TEST_ASSERT_EQUAL_INT (
          0, as_socket (socket_)->test_process_commands_only ());
        if (as_socket (socket_)->test_completion_pair_queued (
              pair_id_, generation_))
            return;
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for undrained completion pair");
}

bool wait_for_completion_lane_detached (void *socket_, uint64_t pair_id_,
                                        uint64_t generation_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        //  This helper runs while the poller owner is paused at the budget
        //  barrier. Process only mailbox lifecycle commands: querying EVENTS
        //  here would itself enter completion draining and consume the source
        //  whose stale requeue fence the test is trying to observe.
        (void) as_socket (socket_)->test_process_commands_only ();
        if (!as_socket (socket_)->test_pair_pipe (pair_id_, generation_, true))
            return true;
        msleep (1);
    }
    return false;
}

zlink_completion_id_t send_router_request_to (
  void *router_, const zlink_routing_id_t &target_, const char *payload_,
  void *user_context_)
{
    zlink_msg_t request;
    init_part (&request, payload_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (router_, &target_, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 120000, user_context_,
                          &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_part_consumed (&request);
    return completion_id;
}

void receive_dealer_data_eventually (void *dealer_, const char *expected_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        errno = 0;
        const zlink_recv_result_t result = zlink_recv_part (
          dealer_, NULL, &part, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);
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
      zlink_send_part (dealer_, &prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&prime);

    const router_part_t received = receive_router_part_eventually (router_);
    TEST_ASSERT_EQUAL_UINT64 (0, received.reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received.part_flag);
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
      zlink_request_part (NULL, NULL, &invalid_handle,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 50, NULL,
                          &completion_id));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&invalid_handle);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    zlink_msg_t invalid_more;
    init_part (&invalid_more, "invalid-more");
    completion_id = UINT64_MAX;
    int context = 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_request_part (dealer, NULL, &invalid_more,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, 1,
                          &context, &completion_id));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_part_consumed (&invalid_more);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const zlink_routing_id_t rid = make_rid ("source");
    zlink_msg_t invalid_reply;
    init_part (&invalid_reply, "invalid-token");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_reply_part (router, &rid, 0, &invalid_reply, ZLINK_PART_FINAL));
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

    zlink_msg_t request_more;
    init_part (&request_more, "request-head");
    zlink_completion_id_t more_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request_more,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, 0, NULL,
                          &more_id));
    TEST_ASSERT_EQUAL_UINT64 (0, more_id);
    assert_part_consumed (&request_more);

    int request_context = 42;
    zlink_msg_t request_final;
    init_part (&request_final, "request-tail");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request_final,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 2000,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request_final);

    const router_part_t request_head =
      receive_router_part_eventually (router);
    TEST_ASSERT_EQUAL_STRING ("dealer-public",
                              std::string (
                                reinterpret_cast<const char *> (
                                  request_head.source_rid.data),
                                request_head.source_rid.size)
                                .c_str ());
    TEST_ASSERT_NOT_EQUAL (0, request_head.reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, request_head.part_flag);
    TEST_ASSERT_EQUAL_STRING ("request-head", request_head.payload.c_str ());

    const router_part_t request_tail =
      receive_router_part_eventually (router);
    TEST_ASSERT_EQUAL_UINT (request_head.source_rid.size,
                            request_tail.source_rid.size);
    TEST_ASSERT_EQUAL_MEMORY (request_head.source_rid.data,
                              request_tail.source_rid.data,
                              request_head.source_rid.size);
    TEST_ASSERT_EQUAL_UINT64 (request_head.reply_token,
                              request_tail.reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, request_tail.part_flag);
    TEST_ASSERT_EQUAL_STRING ("request-tail", request_tail.payload.c_str ());

    zlink_msg_t reply_more;
    init_part (&reply_more, "reply-head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request_tail.source_rid,
                        request_tail.reply_token, &reply_more,
                        ZLINK_PART_MORE));
    assert_part_consumed (&reply_more);

    zlink_msg_t reply_final;
    init_part (&reply_final, "reply-tail");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request_tail.source_rid,
                        request_tail.reply_token, &reply_final,
                        ZLINK_PART_FINAL));
    assert_part_consumed (&reply_final);

    zlink_msg_t duplicate_reply;
    init_part (&duplicate_reply, "duplicate");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply_part (router, &request_tail.source_rid,
                        request_tail.reply_token, &duplicate_reply,
                        ZLINK_PART_FINAL));
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
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, request.part_flag);
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
    TEST_ASSERT_EQUAL_INT (
      request.source_rid.size,
      zlink_send (router, request.source_rid.data, request.source_rid.size,
                  ZLINK_SNDMORE));
    TEST_ASSERT_EQUAL_INT (sizeof (data) - 1,
                           zlink_send (router, data, sizeof (data) - 1, 0));

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
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
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
      zlink_request_part (router, &dealer_rid, &request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 100, NULL,
                          &completion_id));
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
      zlink_send_part (requester, &prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &prime_id));
    TEST_ASSERT_EQUAL_UINT64 (0, prime_id);
    assert_part_consumed (&prime);
    receive_dealer_data_eventually (peer, "prime");

    zlink_msg_t request;
    init_part (&request, "no-router");
    zlink_completion_id_t completion_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_request_part (requester, NULL, &request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 100, NULL,
                          &completion_id));
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
      zlink_request_part (dealer, NULL, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 25,
                          &request_context, &completion_id));
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
          zlink_request_part (dealer, NULL, &request,
                              ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_FINAL, timeout_ms,
                              &request_context, &completion_id));
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
          zlink_reply_part (router, &received.source_rid,
                            received.reply_token, &reply,
                            ZLINK_PART_FINAL));
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
        submit_result = zlink_request_part (
          dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
          3000, &request_context, &request_id);
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
      zlink_reply_part (router, &received.source_rid, received.reply_token,
                        &reply, ZLINK_PART_FINAL));
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
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 1000, NULL, &request_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_admitted_request_survives_physical_detach_and_same_rid_reconnect_without_replay ()
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
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 600, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);

    const router_part_t admitted =
      receive_router_part_eventually (first_router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);
    TEST_ASSERT_EQUAL_STRING ("admitted-before-physical-detach",
                              admitted.payload.c_str ());

    test_context_socket_close_zero_linger (first_router);
    first_router = NULL;
    // Physical paired-pipe termination is not a REQUEST terminal owner.
    assert_no_completion_for (dealer, 40);

    void *replacement_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_router);
    set_routing_id_text (replacement_router, "request-reconnect-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_router, endpoint));
    msleep (SETTLE_TIME);
    // A DATA marker on the replacement route must be its first record. The
    // admitted REQUEST payload is never replayed after reconnect.
    prime_router_dealer_route (dealer, replacement_router);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                           completion.request_result);
    TEST_ASSERT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    zlink_completion_close (&completion);
    assert_empty_completion (completion);
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
          zlink_request_part (dealer, NULL, &request_part,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              2000, &request_contexts[i], &request_ids[i]));
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
          zlink_reply_part (router, &request.source_rid,
                            request.reply_token, &reply, ZLINK_PART_FINAL));
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
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received.part_flag);
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
      zlink_send_part (first_dealer, &behind_blocked_request,
                       ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&behind_blocked_request);

    // Readiness itself must discover and pause both capacity-blocked heads.
    // A receive call has deliberately not run since they were enqueued.
    zlink_pollitem_t item = {router, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 0, NULL));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t no_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&no_part));
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (router, &source_rid, &reply_token, &no_part,
                              &part_flag, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&no_part));

    zlink_msg_t data;
    init_part (&data, "fair-data");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (data_dealer, &data, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
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
      zlink_reply_part (router, &first_request.source_rid,
                        first_request.reply_token, &reply,
                        ZLINK_PART_FINAL));
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
      zlink_reply_part (router, &redriven_a.source_rid,
                        redriven_a.reply_token, &reply,
                        ZLINK_PART_FINAL));
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
      zlink_reply_part (router, &redriven_b.source_rid,
                        redriven_b.reply_token, &reply,
                        ZLINK_PART_FINAL));
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
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
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
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);

    init_part (&reply, "duplicate-after-reconnect");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (replacement);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_final_oom_releases_checkout_and_retains_token ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-final-oom-peer");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-reply-final-oom"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-reply-final-oom"));
    msleep (SETTLE_TIME);

    const zlink_completion_id_t request_id =
      send_public_request (dealer, "reply-final-oom-request");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);

    zlink_msg_t prefix;
    init_part (&prefix, "discarded-oom-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &prefix, ZLINK_PART_MORE));
    assert_part_consumed (&prefix);

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_reply_key);
    zlink_msg_t failed_final;
    init_part (&failed_final, "oom-final");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &failed_final, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, zlink_errno ());
    assert_part_consumed (&failed_final);

    // A different thread must be able to start a fresh sequence. This catches
    // an abandoned helper prefix whose token checkout was accidentally kept.
    zlink_msg_t retry;
    init_part (&retry, "fresh-reply-after-oom");
    zlink_submit_result_t retry_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread retry_thread ([&] () {
        errno = 0;
        retry_result = zlink_reply_part (
          router, &request.source_rid, request.reply_token, &retry,
          ZLINK_PART_FINAL);
    });
    retry_thread.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, retry_result);
    assert_part_consumed (&retry);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "fresh-reply-after-oom",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_final_runtime_failure_releases_checkout_and_retains_token ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-final-eio-peer");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-reply-final-eio"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-reply-final-eio"));
    msleep (SETTLE_TIME);

    const zlink_completion_id_t request_id =
      send_public_request (dealer, "reply-final-eio-request");
    const router_part_t request = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);

    zlink_msg_t prefix;
    init_part (&prefix, "discarded-eio-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &prefix, ZLINK_PART_MORE));
    assert_part_consumed (&prefix);

    zlink_msg_t failed_final;
    init_part (&failed_final, "eio-final");
    zlink::socket_reqrep_internal::
      test_set_request_reply_write_failure_after_prefix (true);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INTERNAL_ERROR,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &failed_final, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EIO, zlink_errno ());
    assert_part_consumed (&failed_final);

    // Rollback emptied the completion pipe; a fresh reply proves that EIO
    // released the checkout while retaining the live token.
    zlink_msg_t retry;
    init_part (&retry, "fresh-reply-after-eio");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &retry, ZLINK_PART_FINAL));
    assert_part_consumed (&retry);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "fresh-reply-after-eio",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (dealer);
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
        zlink_msg_t prefix;
        init_part (&prefix, "discarded-context-prefix");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (router, &request.source_rid,
                            request.reply_token, &prefix, ZLINK_PART_MORE));
        assert_part_consumed (&prefix);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
        zlink_msg_t final;
        init_part (&final, "context-final");
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_TERMINATED,
          zlink_reply_part (router, &request.source_rid,
                            request.reply_token, &final, ZLINK_PART_FINAL));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_part_consumed (&final);

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
        zlink_msg_t prefix;
        init_part (&prefix, "discarded-socket-prefix");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (router, &request.source_rid,
                            request.reply_token, &prefix, ZLINK_PART_MORE));
        assert_part_consumed (&prefix);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
        zlink_msg_t final;
        init_part (&final, "socket-final");
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_TERMINATED,
          zlink_reply_part (router, &request.source_rid,
                            request.reply_token, &final, ZLINK_PART_FINAL));
        TEST_ASSERT_EQUAL_INT (ESHUTDOWN, zlink_errno ());
        assert_part_consumed (&final);

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

    zlink_msg_t timed_out_prefix;
    init_part (&timed_out_prefix, "timed-out-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &timed_out_prefix, ZLINK_PART_MORE));
    assert_part_consumed (&timed_out_prefix);

    zlink_msg_t timed_out_reply;
    init_part (&timed_out_reply, "timed-out-reply");
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &timed_out_reply, ZLINK_PART_FINAL));
    const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started)
        .count ();
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_TRUE (elapsed_ms >= 15);
    TEST_ASSERT_TRUE (elapsed_ms < kWaitMilliseconds);
    assert_part_consumed (&timed_out_reply);

    // A zero-budget retry proves timeout released the checkout while the
    // token itself remained live.
    sndtimeo = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &sndtimeo,
                        sizeof (sndtimeo)));
    init_part (&timed_out_reply, "zero-budget-retry");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &timed_out_reply, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_part_consumed (&timed_out_reply);

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

    zlink_msg_t retry;
    init_part (&retry, "retry-prefix-after-reconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &retry, ZLINK_PART_MORE));
    assert_part_consumed (&retry);
    init_part (&retry, "retry-final-after-reconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &retry, ZLINK_PART_FINAL));
    assert_part_consumed (&retry);

    init_part (&retry, "consumed-token");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &retry, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&retry);

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
        result = zlink_reply_part (router, &request.source_rid,
                                   request.reply_token, &reply,
                                   ZLINK_PART_FINAL);
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
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOENT, zlink_errno ());
    assert_part_consumed (&reply);

    test_context_socket_close_zero_linger (replacement);
    test_context_socket_close_zero_linger (router);
}

void test_router_reply_checkout_second_sequence_and_mismatch_preserve_owner ()
{
    const char *const endpoint =
      "inproc://phase3-reply-checkout-mismatch";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *first_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *second_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (first_dealer);
    TEST_ASSERT_NOT_NULL (second_dealer);
    set_routing_id_text (first_dealer, "reply-checkout-first");
    set_routing_id_text (second_dealer, "reply-checkout-second");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (first_dealer, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (second_dealer, endpoint));
    msleep (SETTLE_TIME);

    (void) send_public_request (first_dealer, "checkout-first");
    (void) send_public_request (second_dealer, "checkout-second");
    const router_part_t first = receive_router_part_eventually (router);
    const router_part_t second = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, first.reply_token);
    TEST_ASSERT_NOT_EQUAL (0, second.reply_token);

    zlink_msg_t first_more;
    init_part (&first_more, "first-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &first.source_rid, first.reply_token,
                        &first_more, ZLINK_PART_MORE));
    assert_part_consumed (&first_more);

    zlink_msg_t concurrent_final;
    init_part (&concurrent_final, "concurrent-second-sequence");
    zlink_submit_result_t concurrent_result = ZLINK_SUBMIT_OK;
    int concurrent_errno = 0;
    size_t concurrent_size = UINT64_MAX;
    std::thread concurrent ([&] () {
        errno = 0;
        concurrent_result = zlink_reply_part (
          router, &first.source_rid, first.reply_token, &concurrent_final,
          ZLINK_PART_FINAL);
        concurrent_errno = zlink_errno ();
        concurrent_size = zlink_msg_size (&concurrent_final);
    });
    concurrent.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_STATE, concurrent_result);
    TEST_ASSERT_EQUAL_INT (EBUSY, concurrent_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, concurrent_size);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&concurrent_final));

    // The rejected second owner must not disturb the original staging or
    // checkout; its FINAL still consumes exactly the first token.
    zlink_msg_t first_final;
    init_part (&first_final, "first-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &first.source_rid, first.reply_token,
                        &first_final, ZLINK_PART_FINAL));
    assert_part_consumed (&first_final);

    zlink_msg_t second_more;
    init_part (&second_more, "second-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &second.source_rid, second.reply_token,
                        &second_more, ZLINK_PART_MORE));
    assert_part_consumed (&second_more);

    // A different RID/token on the continuation aborts only the active
    // sequence and releases its checkout. The original second token remains
    // available for a complete retry from its first part.
    zlink_msg_t mismatch;
    init_part (&mismatch, "mismatched-final");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_reply_part (router, &first.source_rid, first.reply_token,
                        &mismatch, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    assert_part_consumed (&mismatch);

    zlink_msg_t second_retry;
    init_part (&second_retry, "second-retry");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &second.source_rid, second.reply_token,
                        &second_retry, ZLINK_PART_FINAL));
    assert_part_consumed (&second_retry);

    test_context_socket_close_zero_linger (second_dealer);
    test_context_socket_close_zero_linger (first_dealer);
    test_context_socket_close_zero_linger (router);
}

struct cyclic_reply_worker_result_t
{
    cyclic_reply_worker_result_t () :
        received (0),
        replied (0),
        recv_result (ZLINK_RECV_OK),
        recv_errno (0),
        reply_result (ZLINK_SUBMIT_OK),
        reply_errno (0)
    {
    }

    size_t received;
    size_t replied;
    zlink_recv_result_t recv_result;
    int recv_errno;
    zlink_submit_result_t reply_result;
    int reply_errno;
};

struct cyclic_reply_worker_sync_t
{
    cyclic_reply_worker_sync_t () : reply_attempts (0) {}

    void note_reply_attempt ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        ++reply_attempts;
        changed.notify_all ();
    }

    bool wait_for_reply_attempt (size_t expected_, int timeout_ms_)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (
          lock, std::chrono::milliseconds (timeout_ms_),
          [this, expected_] { return reply_attempts >= expected_; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    size_t reply_attempts;
};

struct cyclic_request_submit_result_t
{
    cyclic_request_submit_result_t () :
        init_result (ZLINK_CONFIG_INTERNAL_ERROR),
        submit_result (ZLINK_SUBMIT_INTERNAL_ERROR),
        submit_errno (0),
        completion_id (0),
        consumed (false),
        close_result (ZLINK_CONFIG_INTERNAL_ERROR)
    {
    }

    zlink_config_result_t init_result;
    zlink_submit_result_t submit_result;
    int submit_errno;
    zlink_completion_id_t completion_id;
    bool consumed;
    zlink_config_result_t close_result;
};

cyclic_request_submit_result_t submit_cyclic_request (
  void *dealer_, size_t payload_size_, uint32_t request_timeout_ms_)
{
    cyclic_request_submit_result_t result;
    zlink_msg_t request;
    result.init_result = zlink_msg_init_size (&request, payload_size_);
    if (result.init_result != ZLINK_CONFIG_OK) {
        result.submit_errno = zlink_errno ();
        return result;
    }
    memset (zlink_msg_data (&request), 'q', payload_size_);
    errno = 0;
    result.submit_result = zlink_request_part (
      dealer_, NULL, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
      request_timeout_ms_, NULL, &result.completion_id);
    result.submit_errno = zlink_errno ();
    result.consumed = zlink_msg_size (&request) == 0;
    result.close_result = zlink_msg_close (&request);
    return result;
}

bool wait_for_hwm_credit_waiter (zlink::pipe_t *pipe_, int timeout_ms_,
                                 uint64_t *in_flight_bytes_out_)
{
    if (in_flight_bytes_out_)
        *in_flight_bytes_out_ = 0;
    if (!pipe_)
        return false;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    do {
        bool active = true;
        bool hwm_full = false;
        bool byte_credit_waiter = false;
        uint64_t in_flight_bytes = 0;
        pipe_->test_flow_probe (&active, &hwm_full, NULL,
                                &byte_credit_waiter, &in_flight_bytes);
        if (!active && hwm_full && byte_credit_waiter) {
            if (in_flight_bytes_out_)
                *in_flight_bytes_out_ = in_flight_bytes;
            return true;
        }
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < deadline);
    return false;
}

void run_cyclic_reply_worker (void *router_, size_t request_count_,
                              size_t payload_size_,
                              cyclic_reply_worker_sync_t *sync_,
                              cyclic_reply_worker_result_t *result_)
{
    for (size_t i = 0; i != request_count_; ++i) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t request;
        if (zlink_msg_init (&request) != ZLINK_CONFIG_OK) {
            result_->recv_result = ZLINK_RECV_INTERNAL_ERROR;
            result_->recv_errno = zlink_errno ();
            return;
        }
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        result_->recv_result = zlink_router_recv_part (
          router_, &source_rid, &reply_token, &request, &part_flag,
          ZLINK_RECV_FLAGS_NONE);
        result_->recv_errno = zlink_errno ();
        if (result_->recv_result != ZLINK_RECV_OK || !source_rid
            || reply_token == 0 || part_flag != ZLINK_PART_FINAL) {
            zlink_msg_close (&request);
            return;
        }
        ++result_->received;
        zlink_msg_close (&request);

        zlink_msg_t reply;
        if (zlink_msg_init_size (&reply, payload_size_) != ZLINK_CONFIG_OK) {
            result_->reply_result = ZLINK_SUBMIT_INTERNAL_ERROR;
            result_->reply_errno = zlink_errno ();
            return;
        }
        memset (zlink_msg_data (&reply), 'r', payload_size_);
        sync_->note_reply_attempt ();
        result_->reply_result = zlink_reply_part (
          router_, source_rid, reply_token, &reply, ZLINK_PART_FINAL);
        result_->reply_errno = zlink_errno ();
        zlink_msg_close (&reply);
        if (result_->reply_result != ZLINK_SUBMIT_OK)
            return;
        ++result_->replied;
    }
}

void test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle ()
{
    const char *const endpoint =
      "inproc://phase3-blocking-request-completion-hwm-cycle";
    const char *const dealer_rid_text = "completion-hwm-cycle-dealer";
    const char *const router_rid_text = "completion-hwm-cycle-router";
    const size_t worker_request_count = 3;
    const size_t payload_size = 8192;
    const uint64_t hwm = 4096;
    const int initial_send_timeout_ms = 5000;
    const int dealer_send_timeout_ms = 1000;
    const int router_send_timeout_ms = 4000;
    const int router_receive_timeout_ms = 2000;
    const uint32_t request_timeout_ms = 5000;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, router_rid_text);
    set_routing_id_text (dealer, dealer_rid_text);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO,
                        &initial_send_timeout_ms,
                        sizeof (initial_send_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO,
                        &router_send_timeout_ms,
                        sizeof (router_send_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVTIMEO,
                        &router_receive_timeout_ms,
                        sizeof (router_receive_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    // Complete the pair handshake through blocking public operations. This is
    // a state transition, not a settle delay, and leaves both physical queues
    // empty before completion ownership changes.
    zlink_msg_t prime;
    init_part (&prime, "completion-cycle-prime");
    zlink_completion_id_t prime_completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, &prime_completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, prime_completion_id);
    assert_part_consumed (&prime);
    const zlink_routing_id_t *prime_source_rid = NULL;
    zlink_reply_token_t prime_reply_token = UINT64_MAX;
    zlink_msg_t prime_received;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init (&prime_received));
    zlink_part_flag_t prime_part_flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &prime_source_rid, &prime_reply_token,
                              &prime_received, &prime_part_flag,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (prime_source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, prime_reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, prime_part_flag);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&prime_received));

    // Register the sole completion owner before the first request. The call
    // does not return until the async mailbox owner has quiesced, so no hidden
    // completion consumer can drain the physical reply pipe below.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, dealer, ZLINK_POLLCOMPLETION));

    const std::chrono::steady_clock::time_point first_request_started =
      std::chrono::steady_clock::now ();
    const cyclic_request_submit_result_t first =
      submit_cyclic_request (dealer, payload_size, request_timeout_ms);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, first.init_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, first.submit_result);
    TEST_ASSERT_EQUAL_INT (0, first.submit_errno);
    TEST_ASSERT_NOT_EQUAL (0, first.completion_id);
    TEST_ASSERT_TRUE (first.consumed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, first.close_result);

    const zlink_routing_id_t *first_source_rid = NULL;
    zlink_reply_token_t first_reply_token = 0;
    zlink_msg_t first_request;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init (&first_request));
    zlink_part_flag_t first_part_flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &first_source_rid, &first_reply_token,
                              &first_request, &first_part_flag,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (first_source_rid);
    const zlink_routing_id_t first_source_rid_copy = *first_source_rid;
    TEST_ASSERT_NOT_EQUAL (0, first_reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, first_part_flag);
    TEST_ASSERT_EQUAL_UINT64 (payload_size,
                              zlink_msg_size (&first_request));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_close (&first_request));

    zlink_routed_submit_target_t dealer_target;
    memset (&dealer_target, 0, sizeof (dealer_target));
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (dealer)->select_routed_submit_target (
           NULL, &dealer_target));
    const uint64_t dealer_pair_id = dealer_target.transport_pair_id;
    const uint64_t dealer_pair_generation =
      dealer_target.transport_pair_generation;
    TEST_ASSERT_NOT_EQUAL (0, dealer_pair_id);
    TEST_ASSERT_NOT_EQUAL (0, dealer_pair_generation);

    // Inproc creates both socket-owned pipe halves under one pair fence. D/R
    // is a single-lane topology, so the same id selects the router's reply
    // output and the dealer's request output on their respective halves.
    const uint64_t router_pair_id = dealer_pair_id;
    const uint64_t router_pair_generation = dealer_pair_generation;
    TEST_ASSERT_NOT_NULL (as_socket (router)->test_pair_pipe (
      router_pair_id, router_pair_generation, false));

    zlink_msg_t first_reply;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (&first_reply, payload_size));
    memset (zlink_msg_data (&first_reply), 'r', payload_size);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &first_source_rid_copy, first_reply_token,
                        &first_reply, ZLINK_PART_FINAL));
    assert_part_consumed (&first_reply);

    // One oversize reply is admitted to an empty byte-HWM pipe. Its retained
    // byte charge deterministically makes the next reply hit physical HWM;
    // this is observed without probing or draining the dealer completion side.
    zlink::pipe_t *router_reply_pipe = as_socket (router)->test_pair_pipe (
      router_pair_id, router_pair_generation, true);
    if (!router_reply_pipe)
        router_reply_pipe = as_socket (router)->test_pair_pipe (
          router_pair_id, router_pair_generation, false);
    TEST_ASSERT_NOT_NULL (router_reply_pipe);
    bool first_reply_hwm_full = false;
    uint64_t first_reply_in_flight_bytes = 0;
    router_reply_pipe->test_flow_probe (
      NULL, &first_reply_hwm_full, NULL, NULL,
      &first_reply_in_flight_bytes);
    TEST_ASSERT_TRUE (first_reply_hwm_full);
    TEST_ASSERT_TRUE (first_reply_in_flight_bytes != 0);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO,
                        &dealer_send_timeout_ms,
                        sizeof (dealer_send_timeout_ms)));

    const cyclic_request_submit_result_t second =
      submit_cyclic_request (dealer, payload_size, request_timeout_ms);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, second.init_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, second.submit_result);
    TEST_ASSERT_EQUAL_INT (0, second.submit_errno);
    TEST_ASSERT_NOT_EQUAL (0, second.completion_id);
    TEST_ASSERT_TRUE (second.consumed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, second.close_result);

    // The worker consumes request 2, then blocks its reply behind reply 1.
    // A condition variable fixes the attempt ordering; the pipe's non-mutating
    // test probe fixes the exact EAGAIN/HWM state before this thread proceeds.
    cyclic_reply_worker_sync_t worker_sync;
    cyclic_reply_worker_result_t worker_result;
    std::thread worker (run_cyclic_reply_worker, router,
                        worker_request_count, payload_size, &worker_sync,
                        &worker_result);
    const bool second_reply_attempted =
      worker_sync.wait_for_reply_attempt (1, kWaitMilliseconds);
    uint64_t blocked_reply_in_flight_bytes = 0;
    const bool second_reply_waiting_for_hwm =
      second_reply_attempted
      && wait_for_hwm_credit_waiter (
        router_reply_pipe, kWaitMilliseconds,
        &blocked_reply_in_flight_bytes);

    const cyclic_request_submit_result_t third =
      submit_cyclic_request (dealer, payload_size, request_timeout_ms);

    // With the worker blocked in reply 2, request 3 is the one oversize record
    // admitted to the empty Application pipe. This cached probe proves that
    // request 4's first physical admission must report HWM backpressure.
    bool application_active = false;
    bool application_hwm_full = false;
    bool application_remote_paused = false;
    bool application_credit_waiter = false;
    uint64_t application_in_flight_bytes = 0;
    const bool application_pipe_probed =
      as_socket (dealer)->test_application_pipe_flow_probe (
        dealer_pair_id, dealer_pair_generation, &application_active,
        &application_hwm_full, &application_remote_paused,
        &application_credit_waiter, &application_in_flight_bytes);

    const std::chrono::steady_clock::time_point blocking_request_started =
      std::chrono::steady_clock::now ();
    const cyclic_request_submit_result_t fourth =
      submit_cyclic_request (dealer, payload_size, request_timeout_ms);

    // Do not call completion_recv first: the public contract under regression
    // is the registered POLLCOMPLETION event. The blocking fourth request must
    // borrow this poller's owner gate, drain already-arrived reply 1, and
    // return before SNDTIMEO; only then do we sample the poller and dequeue.
    zlink_poller_event_t first_event;
    memset (&first_event, 0, sizeof (first_event));
    zlink_config_result_t first_poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    const int first_poll_count = zlink_poller_wait (
      poller, &first_event, 1, request_timeout_ms, &first_poll_error);
    const std::chrono::steady_clock::time_point first_event_observed =
      std::chrono::steady_clock::now ();
    const int64_t blocking_to_event_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        first_event_observed - blocking_request_started)
        .count ();
    const int64_t first_request_to_event_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        first_event_observed - first_request_started)
        .count ();

    std::vector<zlink_completion_id_t> completion_ids;
    completion_ids.reserve (4);
    completion_ids.push_back (first.completion_id);
    completion_ids.push_back (second.completion_id);
    if (third.submit_result == ZLINK_SUBMIT_OK && third.completion_id != 0)
        completion_ids.push_back (third.completion_id);
    if (fourth.submit_result == ZLINK_SUBMIT_OK && fourth.completion_id != 0)
        completion_ids.push_back (fourth.completion_id);
    std::vector<bool> completion_seen (completion_ids.size (), false);
    size_t completion_count = 0;
    bool completion_shape_ok = true;
    const std::chrono::steady_clock::time_point completion_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (request_timeout_ms);
    bool have_completion_event = first_poll_count == 1;
    while (completion_count != completion_ids.size ()
           && std::chrono::steady_clock::now () < completion_deadline) {
        if (!have_completion_event) {
            zlink_poller_event_t event;
            memset (&event, 0, sizeof (event));
            zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
            have_completion_event =
              zlink_poller_wait (poller, &event, 1, kWaitMilliseconds,
                                 &poll_error)
                == 1
              && poll_error == ZLINK_CONFIG_OK
              && event.socket == dealer
              && (event.events & ZLINK_POLLCOMPLETION) != 0;
            if (!have_completion_event)
                break;
        }
        have_completion_event = false;

        zlink_completion_t completion;
        init_empty_completion (&completion);
        const zlink_recv_result_t result = zlink_completion_recv (
          dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result != ZLINK_RECV_OK) {
            completion_shape_ok = false;
            break;
        }
        size_t matched = completion_ids.size ();
        for (size_t i = 0; i != completion_ids.size (); ++i) {
            if (completion_ids[i] == completion.completion_id) {
                matched = i;
                break;
            }
        }
        completion_shape_ok =
          completion_shape_ok && matched != completion_ids.size ()
          && !completion_seen[matched]
          && completion.kind == ZLINK_COMPLETION_REQUEST
          && completion.request_result == ZLINK_REQUEST_OK
          && completion.reply_part_count == 1
          && zlink_msg_size (&completion.reply_parts[0]) == payload_size;
        if (matched != completion_ids.size () && !completion_seen[matched]) {
            completion_seen[matched] = true;
            ++completion_count;
        }
        zlink_completion_close (&completion);
    }

    worker.join ();
    const zlink_config_result_t remove_result =
      zlink_poller_remove (poller, dealer);
    const zlink_close_result_t destroy_result = zlink_poller_destroy (&poller);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);

    TEST_ASSERT_TRUE (second_reply_attempted);
    TEST_ASSERT_TRUE (second_reply_waiting_for_hwm);
    TEST_ASSERT_TRUE (blocked_reply_in_flight_bytes != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, third.init_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, third.submit_result);
    TEST_ASSERT_EQUAL_INT (0, third.submit_errno);
    TEST_ASSERT_NOT_EQUAL (0, third.completion_id);
    TEST_ASSERT_TRUE (third.consumed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, third.close_result);
    TEST_ASSERT_TRUE (application_pipe_probed);
    TEST_ASSERT_TRUE (application_hwm_full);
    TEST_ASSERT_FALSE (application_remote_paused);
    TEST_ASSERT_TRUE (application_in_flight_bytes != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, fourth.init_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, fourth.submit_result);
    TEST_ASSERT_EQUAL_INT (0, fourth.submit_errno);
    TEST_ASSERT_NOT_EQUAL (0, fourth.completion_id);
    TEST_ASSERT_TRUE (fourth.consumed);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, fourth.close_result);
    TEST_ASSERT_EQUAL_INT (1, first_poll_count);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, first_poll_error);
    TEST_ASSERT_EQUAL_PTR (dealer, first_event.socket);
    TEST_ASSERT_EQUAL_PTR (dealer, first_event.user_data);
    TEST_ASSERT_TRUE (
      (first_event.events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_TRUE (blocking_to_event_ms >= 0);
    TEST_ASSERT_TRUE (blocking_to_event_ms < dealer_send_timeout_ms);
    TEST_ASSERT_TRUE (first_request_to_event_ms >= 0);
    TEST_ASSERT_TRUE (first_request_to_event_ms < request_timeout_ms);
    TEST_ASSERT_EQUAL_UINT64 (worker_request_count, worker_result.received);
    TEST_ASSERT_EQUAL_UINT64 (worker_request_count, worker_result.replied);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, worker_result.recv_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, worker_result.reply_result);
    TEST_ASSERT_TRUE (completion_shape_ok);
    TEST_ASSERT_EQUAL_UINT64 (4, completion_ids.size ());
    TEST_ASSERT_EQUAL_UINT64 (completion_ids.size (), completion_count);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, remove_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, destroy_result);
}

void test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced ()
{
    const char *const endpoint_a =
      "inproc://phase3-completion-budget-fairness-a";
    const char *const endpoint_b =
      "inproc://phase3-completion-budget-fairness-b";
    const char *const requester_rid_text = "completion-budget-requester";
    const char *const responder_a_rid_text = "completion-budget-a";
    const char *const responder_b_rid_text = "completion-budget-b";
    const size_t budget =
      zlink::socket_reqrep_internal::completion_pipe_record_budget;
    const size_t a_record_count = budget * 4;

    void *requester = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *responder_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *responder_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (requester);
    TEST_ASSERT_NOT_NULL (responder_a);
    TEST_ASSERT_NOT_NULL (responder_b);
    set_routing_id_text (requester, requester_rid_text);
    set_routing_id_text (responder_a, responder_a_rid_text);
    set_routing_id_text (responder_b, responder_b_rid_text);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (responder_a,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               requester_rid_text,
                               strlen (requester_rid_text)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (responder_b,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               requester_rid_text,
                               strlen (requester_rid_text)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (requester, endpoint_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (requester, endpoint_b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (responder_a, endpoint_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (responder_b, endpoint_b));

    const zlink_routing_id_t responder_a_rid =
      make_rid (responder_a_rid_text);
    const zlink_routing_id_t responder_b_rid =
      make_rid (responder_b_rid_text);
    uint64_t pair_a_id = 0;
    uint64_t pair_a_generation = 0;
    uint64_t pair_b_id = 0;
    uint64_t pair_b_generation = 0;

    std::vector<int> contexts_a (a_record_count);
    std::vector<zlink_completion_id_t> completion_ids_a;
    std::vector<router_part_t> requests_a;
    completion_ids_a.reserve (a_record_count);
    requests_a.reserve (a_record_count);
    for (size_t i = 0; i != a_record_count; ++i) {
        contexts_a[i] = static_cast<int> (i);
        completion_ids_a.push_back (send_router_request_to (
          requester, responder_a_rid, "fairness-request-a",
          &contexts_a[i]));
        requests_a.push_back (receive_router_part_eventually (responder_a));
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL,
                               requests_a.back ().part_flag);
    }

    int context_b = 0x5b;
    const zlink_completion_id_t completion_id_b = send_router_request_to (
      requester, responder_b_rid, "fairness-request-b", &context_b);
    const router_part_t request_b =
      receive_router_part_eventually (responder_b);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, request_b.part_flag);

    //  The first routed request on each connection also proves that both
    //  inproc owners have adopted their Application and Completion halves.
    //  Resolve the exact pair keys only after that owner progress has occurred.
    wait_for_ready_pair (requester, responder_a, responder_b, responder_a_rid,
                         &pair_a_id, &pair_a_generation);
    wait_for_ready_pair (requester, responder_a, responder_b, responder_b_rid,
                         &pair_b_id, &pair_b_generation);

    //  Register the sole completion owner before any reply is emitted. This
    //  lets the test establish A then B in the ready-pair deque without an
    //  asynchronous owner consuming either source in between.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, requester, requester,
                        ZLINK_POLLCOMPLETION));

    //  Every A reply is multipart. The 64-unit budget therefore proves both
    //  that a record is never split and that parts are not counted as records.
    for (size_t i = 0; i != requests_a.size (); ++i) {
        zlink_msg_t prefix;
        init_part (&prefix, "fairness-a-prefix");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (responder_a, &requests_a[i].source_rid,
                            requests_a[i].reply_token, &prefix,
                            ZLINK_PART_MORE));
        assert_part_consumed (&prefix);

        zlink_msg_t final;
        init_part (&final, "fairness-a-final");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (responder_a, &requests_a[i].source_rid,
                            requests_a[i].reply_token, &final,
                            ZLINK_PART_FINAL));
        assert_part_consumed (&final);
    }
    wait_for_completion_pair_queued (requester, pair_a_id,
                                     pair_a_generation);

    zlink_msg_t reply_b;
    init_part (&reply_b, "fairness-b-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (responder_b, &request_b.source_rid,
                        request_b.reply_token, &reply_b, ZLINK_PART_FINAL));
    assert_part_consumed (&reply_b);
    wait_for_completion_pair_queued (requester, pair_b_id,
                                     pair_b_generation);

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, kWaitMilliseconds,
                            &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_PTR (requester, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLCOMPLETION) != 0);

    for (size_t i = 0; i != budget; ++i) {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (requester, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_UINT64 (completion_ids_a[i],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts_a[i], completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
        TEST_ASSERT_EQUAL_STRING (
          "fairness-a-prefix",
          part_string (&completion.reply_parts[0]).c_str ());
        TEST_ASSERT_EQUAL_STRING (
          "fairness-a-final",
          part_string (&completion.reply_parts[1]).c_str ());
        zlink_completion_close (&completion);
    }

    //  A consumed exactly one 64-record turn. B must therefore be record 65,
    //  while A's remaining three budgets sit at the ready-queue tail.
    zlink_completion_t completion_b;
    init_empty_completion (&completion_b);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (requester, &completion_b,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (completion_id_b,
                              completion_b.completion_id);
    TEST_ASSERT_EQUAL_PTR (&context_b, completion_b.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                           completion_b.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion_b.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "fairness-b-final",
      part_string (&completion_b.reply_parts[0]).c_str ());
    zlink_completion_close (&completion_b);
    TEST_ASSERT_TRUE (as_socket (requester)->test_completion_pair_queued (
      pair_a_id, pair_a_generation));

    //  A poller implementation may observe another mailbox edge before its
    //  first wait returns. Remove every already-published A record before
    //  installing the barrier; completion pulls do not own physical draining.
    size_t next_a_completion = budget;
    while (true) {
        zlink_completion_t already_published;
        init_empty_completion (&already_published);
        errno = 0;
        const zlink_recv_result_t result = zlink_completion_recv (
          requester, &already_published, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            assert_empty_completion (already_published);
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
        TEST_ASSERT_LESS_THAN_UINT64 (a_record_count, next_a_completion);
        TEST_ASSERT_EQUAL_UINT64 (completion_ids_a[next_a_completion],
                                  already_published.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts_a[next_a_completion],
                               already_published.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                               already_published.request_result);
        TEST_ASSERT_EQUAL_UINT64 (2,
                                  already_published.reply_part_count);
        zlink_completion_close (&already_published);
        ++next_a_completion;
    }
    TEST_ASSERT_TRUE (next_a_completion + budget <= a_record_count);
    TEST_ASSERT_TRUE (as_socket (requester)->test_completion_pair_queued (
      pair_a_id, pair_a_generation));

    //  Pause the next A owner turn after its 64th whole record but before the
    //  budget result can requeue the source. Detaching in that exact window
    //  makes the stale generation fence deterministic.
    completion_budget_barrier_t barrier;
    barrier.socket = as_socket (requester);
    barrier.pair_id = pair_a_id;
    barrier.generation = pair_a_generation;
    zlink::socket_reqrep_internal::
      test_set_completion_pipe_budget_exhausted_hook (
        completion_budget_barrier_hook, &barrier);

    zlink_poller_event_t owner_event;
    memset (&owner_event, 0, sizeof (owner_event));
    zlink_config_result_t owner_poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    int owner_poll_result = -1;
    std::thread owner_turn ([&] {
        owner_poll_result = zlink_poller_wait (
          poller, &owner_event, 1, kWaitMilliseconds, &owner_poll_error);
    });
    const bool barrier_entered =
      barrier.wait_until_entered (kWaitMilliseconds);
    if (!barrier_entered) {
        zlink::socket_reqrep_internal::
          test_set_completion_pipe_budget_exhausted_hook (NULL, NULL);
        barrier.release ();
        owner_turn.join ();
        TEST_FAIL_MESSAGE ("completion owner did not reach the budget barrier");
    }

    test_context_socket_close_zero_linger (responder_a);
    responder_a = NULL;
    const bool old_completion_detached = wait_for_completion_lane_detached (
      requester, pair_a_id, pair_a_generation);
    barrier.release ();
    owner_turn.join ();
    TEST_ASSERT_TRUE (old_completion_detached);
    TEST_ASSERT_EQUAL_INT (1, owner_poll_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, owner_poll_error);
    TEST_ASSERT_EQUAL_PTR (requester, owner_event.socket);
    TEST_ASSERT_TRUE (
      (owner_event.events & ZLINK_POLLCOMPLETION) != 0);

    //  The in-flight owner may publish exactly the 64 records it completed
    //  before the barrier. The detached source's remaining records must never
    //  be consumed by a stale tail requeue.
    const size_t after_barrier_completion = next_a_completion + budget;
    for (size_t i = next_a_completion; i != after_barrier_completion; ++i) {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (requester, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT64 (completion_ids_a[i],
                                  completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts_a[i], completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
        zlink_completion_close (&completion);
    }
    std::vector<bool> detached_request_seen (a_record_count, false);
    while (true) {
        zlink_completion_t detached_completion;
        init_empty_completion (&detached_completion);
        errno = 0;
        const zlink_recv_result_t detached_result = zlink_completion_recv (
          requester, &detached_completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (detached_result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            assert_empty_completion (detached_completion);
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, detached_result);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                               detached_completion.kind);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_FOUND,
                               detached_completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (0,
                                  detached_completion.reply_part_count);
        size_t matched = a_record_count;
        for (size_t i = after_barrier_completion; i != a_record_count; ++i) {
            if (completion_ids_a[i] == detached_completion.completion_id) {
                matched = i;
                break;
            }
        }
        TEST_ASSERT_LESS_THAN_UINT64 (a_record_count, matched);
        TEST_ASSERT_FALSE (detached_request_seen[matched]);
        TEST_ASSERT_EQUAL_PTR (&contexts_a[matched],
                               detached_completion.user_context);
        detached_request_seen[matched] = true;
        zlink_completion_close (&detached_completion);
    }
    TEST_ASSERT_FALSE (as_socket (requester)->test_completion_pair_queued (
      pair_a_id, pair_a_generation));

    void *replacement_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_a);
    set_routing_id_text (replacement_a, responder_a_rid_text);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (replacement_a,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               requester_rid_text,
                               strlen (requester_rid_text)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_a, endpoint_a));

    uint64_t replacement_pair_id = 0;
    uint64_t replacement_generation = 0;
    int replacement_context = 0x6a;
    const zlink_completion_id_t replacement_completion_id =
      send_router_request_to (requester, responder_a_rid,
                              "replacement-request", &replacement_context);
    const router_part_t replacement_request =
      receive_router_part_eventually (replacement_a);
    wait_for_ready_pair (requester, replacement_a, responder_b,
                         responder_a_rid, &replacement_pair_id,
                         &replacement_generation);
    TEST_ASSERT_TRUE (replacement_pair_id != pair_a_id
                      || replacement_generation != pair_a_generation);
    zlink_msg_t replacement_reply;
    init_part (&replacement_reply, "replacement-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (replacement_a, &replacement_request.source_rid,
                        replacement_request.reply_token, &replacement_reply,
                        ZLINK_PART_FINAL));
    assert_part_consumed (&replacement_reply);
    wait_for_completion_pair_queued (requester, replacement_pair_id,
                                     replacement_generation);

    memset (&event, 0, sizeof (event));
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, kWaitMilliseconds,
                            &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_FALSE (as_socket (requester)->test_completion_pair_queued (
      pair_a_id, pair_a_generation));

    zlink_completion_t replacement_completion;
    init_empty_completion (&replacement_completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (requester, &replacement_completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (replacement_completion_id,
                              replacement_completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&replacement_context,
                           replacement_completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                           replacement_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              replacement_completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "replacement-reply",
      part_string (&replacement_completion.reply_parts[0]).c_str ());
    zlink_completion_close (&replacement_completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, requester));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (replacement_a);
    test_context_socket_close_zero_linger (responder_b);
    test_context_socket_close_zero_linger (requester);
}

void test_count1_completion_ready_queue_preserves_fifo_and_reuses_pipe_node ()
{
    const char *const endpoint =
      "inproc://phase3-count1-completion-ready-queue";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "count1-ready-router");
    set_routing_id_text (dealer, "count1-ready-dealer");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    // A blocking public round trip completes pair admission without a fixed
    // settle interval and leaves the shared Application FIFO empty.
    prime_router_dealer_route (dealer, router);
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (dealer)->select_routed_submit_target (NULL, &target));
    TEST_ASSERT_NOT_EQUAL (0, target.transport_pair_id);
    TEST_ASSERT_NOT_EQUAL (0, target.transport_pair_generation);

    // Registration quiesces the asynchronous owner. Until poller_wait below,
    // mailbox progress may classify a private head but cannot consume it.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, dealer, ZLINK_POLLCOMPLETION));

    int contexts[3] = {0x31, 0x32, 0x33};
    const char *const request_payloads[3] = {
      "count1-request-a", "count1-request-b", "count1-request-c"};
    const char *const reply_payloads[3] = {
      "count1-reply-a", "count1-reply-b", "count1-reply-c"};
    zlink_completion_id_t completion_ids[3] = {0, 0, 0};
    router_part_t requests[3];
    for (size_t i = 0; i != 2; ++i) {
        zlink_msg_t request;
        init_part (&request, request_payloads[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &request,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              5000, &contexts[i], &completion_ids[i]));
        TEST_ASSERT_NOT_EQUAL (0, completion_ids[i]);
        assert_part_consumed (&request);
        requests[i] = receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, requests[i].reply_token);
    }

    // Put one public DATA record ahead of both private replies on the same
    // physical FIFO. Classification must leave that pipe public and must not
    // publish its private-ready node until DATA has been consumed.
    const char public_payload[] = "count1-public-before-replies";
    TEST_ASSERT_EQUAL_INT (
      requests[0].source_rid.size,
      zlink_send (router, requests[0].source_rid.data,
                  requests[0].source_rid.size, ZLINK_SNDMORE));
    TEST_ASSERT_EQUAL_INT (
      sizeof (public_payload) - 1,
      zlink_send (router, public_payload, sizeof (public_payload) - 1, 0));
    for (size_t i = 0; i != 2; ++i) {
        zlink_msg_t reply;
        init_part (&reply, reply_payloads[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (router, &requests[i].source_rid,
                            requests[i].reply_token, &reply,
                            ZLINK_PART_FINAL));
        assert_part_consumed (&reply);
    }

    (void) as_socket (dealer)->test_process_commands_only ();
    TEST_ASSERT_FALSE (as_socket (dealer)->test_completion_pair_queued (
      target.transport_pair_id, target.transport_pair_generation));
    receive_dealer_data_eventually (dealer, public_payload);
    wait_for_completion_pair_queued_without_drain (
      dealer, target.transport_pair_id, target.transport_pair_generation);

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, kWaitMilliseconds,
                            &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_PTR (dealer, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_FALSE (as_socket (dealer)->test_completion_pair_queued (
      target.transport_pair_id, target.transport_pair_generation));

    for (size_t i = 0; i != 2; ++i) {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (dealer, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT64 (completion_ids[i], completion.completion_id);
        TEST_ASSERT_EQUAL_PTR (&contexts[i], completion.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
        TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
        TEST_ASSERT_EQUAL_STRING (
          reply_payloads[i],
          part_string (&completion.reply_parts[0]).c_str ());
        zlink_completion_close (&completion);
    }

    // A second idle->queued->draining->idle cycle reuses the same intrusive
    // pipe node and proves that neither the atomic dedupe state nor its link is
    // left stale by the first finite owner batch.
    zlink_msg_t request;
    init_part (&request, request_payloads[2]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 5000, &contexts[2],
                          &completion_ids[2]));
    TEST_ASSERT_NOT_EQUAL (0, completion_ids[2]);
    assert_part_consumed (&request);
    requests[2] = receive_router_part_eventually (router);

    zlink_msg_t reply;
    init_part (&reply, reply_payloads[2]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &requests[2].source_rid,
                        requests[2].reply_token, &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);
    wait_for_completion_pair_queued_without_drain (
      dealer, target.transport_pair_id, target.transport_pair_generation);

    memset (&event, 0, sizeof (event));
    poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, kWaitMilliseconds,
                            &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_FALSE (as_socket (dealer)->test_completion_pair_queued (
      target.transport_pair_id, target.transport_pair_generation));

    zlink_completion_t completion;
    init_empty_completion (&completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (dealer, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (completion_ids[2], completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&contexts[2], completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      reply_payloads[2], part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);

    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (dealer, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
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
      test_admitted_request_survives_physical_detach_and_same_rid_reconnect_without_replay);
    RUN_PHASE3_REQUEST_TEST (test_request_completions_are_drained_once_by_id);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_registry_capacity_fair_pollin_and_round_robin_redrive);
    RUN_PHASE3_REQUEST_TEST (
      test_router_explicit_logical_rid_removal_invalidates_reply_token);
    RUN_PHASE3_REQUEST_TEST (
      test_router_physical_disconnect_preserves_token_for_same_rid_reconnect);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_oom_releases_checkout_and_retains_token);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_runtime_failure_releases_checkout_and_retains_token);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_distinguishes_context_and_socket_shutdown);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_timeout_retains_token_for_full_retry);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_final_waits_for_same_rid_reconnect);
    RUN_PHASE3_REQUEST_TEST (
      test_router_reply_checkout_second_sequence_and_mismatch_preserve_owner);
    RUN_PHASE3_REQUEST_TEST (
      test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle);
    RUN_PHASE3_REQUEST_TEST (
      test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced);
    RUN_PHASE3_REQUEST_TEST (
      test_count1_completion_ready_queue_preserves_fifo_and_reuses_pipe_node);

#undef RUN_PHASE3_REQUEST_TEST

    return UNITY_END ();
}
