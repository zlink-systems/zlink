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

void assert_request_not_found_completion (
  void *socket_, zlink_completion_id_t expected_id_,
  void *expected_context_, const zlink_routing_id_t *expected_peer_rid_)
{
    zlink_completion_t completion = receive_completion_eventually (socket_);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (expected_context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_FOUND,
                           completion.request_result);
    TEST_ASSERT_NULL (completion.reply_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.reply_part_count);
    if (expected_peer_rid_) {
        TEST_ASSERT_EQUAL_UINT (expected_peer_rid_->size,
                                completion.peer_rid.size);
        TEST_ASSERT_EQUAL_MEMORY (expected_peer_rid_->data,
                                  completion.peer_rid.data,
                                  expected_peer_rid_->size);
    } else {
        TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
    }
    zlink_completion_close (&completion);
    assert_empty_completion (completion);
}

void fill_live_dealer_pipe_until_shared_limit_rejects (void *dealer_)
{
    bool rejected = false;
    for (int attempt = 0; attempt != 256; ++attempt) {
        zlink_msg_t filler;
        init_part (&filler,
                   "pending-pool-fill-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        const zlink_submit_result_t result =
          zlink_send_part (dealer_, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id);
        assert_part_consumed (&filler);
        if (result == ZLINK_SUBMIT_OK) {
            // Every pre-limit fill is direct admission. Accepting a pending
            // SEND here would prove that it escaped the shared REQUEST slot.
            TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        rejected = true;
        break;
    }
    TEST_ASSERT_TRUE_MESSAGE (rejected,
                              "live data pipe never reached pending admission");
}

zlink_completion_id_t submit_pending_send_after_shared_limit_release (
  void *dealer_)
{
    zlink_msg_t pending;
    init_part (&pending, "pending-after-shared-limit-release");
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer_, &pending, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL, NULL, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_part_consumed (&pending);
    return completion_id;
}

zlink_completion_id_t fill_live_dealer_pipe_until_pending_is_accepted (
  void *dealer_)
{
    for (int attempt = 0; attempt != 256; ++attempt) {
        zlink_msg_t filler;
        init_part (&filler,
                   "pending-release-fill-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer_, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        assert_part_consumed (&filler);
        if (completion_id != 0)
            return completion_id;
    }
    TEST_FAIL_MESSAGE ("live data pipe never produced an accepted pending SEND");
    return 0;
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

void assert_no_router_part_for (void *router_, int duration_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (duration_ms_);
    do {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_router_recv_part (router_, &source_rid, &reply_token, &part,
                                  &part_flag, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT64 (0, reply_token);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
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

void test_dealer_dontwait_request_pins_known_detached_endpoint_until_reconnect ()
{
    const char *const selected_endpoint =
      "inproc://phase3-request-known-detached-selected";
    const char *const other_endpoint =
      "inproc://phase3-request-known-detached-other";
    void *selected_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (selected_router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (selected_router, "known-detached-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, selected_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (selected_router,
                                          selected_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, selected_router);

    test_context_socket_close_zero_linger (selected_router);
    selected_router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    int request_context = 175;
    zlink_msg_t request;
    init_part (&request, "pending-for-selected-endpoint");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 1000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    assert_no_completion_for (dealer, 20);

    void *other_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (other_router);
    set_routing_id_text (other_router, "other-live-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, other_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (other_router, other_endpoint));
    msleep (SETTLE_TIME);
    assert_no_router_part_for (other_router, 40);

    void *replacement_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_router);
    set_routing_id_text (replacement_router, "known-detached-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_router,
                                          selected_endpoint));
    const router_part_t received =
      receive_router_part_eventually (replacement_router);
    TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
    TEST_ASSERT_EQUAL_STRING ("pending-for-selected-endpoint",
                              received.payload.c_str ());
    zlink_msg_t reply;
    init_part (&reply, "selected-endpoint-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (replacement_router, &received.source_rid,
                        received.reply_token, &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "selected-endpoint-reply",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);
    assert_no_router_part_for (other_router, 20);

    test_context_socket_close_zero_linger (replacement_router);
    test_context_socket_close_zero_linger (other_router);
    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_pending_request_survives_detach_before_admission_without_prior_send ()
{
    const char *const endpoint =
      "inproc://phase3-request-only-pending-physical-reconnect";
    void *first_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (first_router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (first_router, "request-only-reconnect-router");
    set_routing_id_text (dealer, "request-only-reconnect-dealer");
    const uint64_t one_pending = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one_pending,
                        sizeof (one_pending)));
    const uint64_t one_byte_hwm = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &one_byte_hwm,
                        sizeof (one_byte_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (first_router, ZLINK_OPT_RCVHWM, &one_byte_hwm,
                        sizeof (one_byte_hwm)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (dealer, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (first_router, endpoint));
    msleep (SETTLE_TIME);
    for (int i = 0; i != 300; ++i) {
        process_socket_commands_through_public_api (first_router);
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    // The tiny HWM makes this first application operation pending. No raw SEND
    // history participates in route selection or detach handling.
    int request_context = 176;
    zlink_msg_t request;
    init_part (&request, "request-only-pending-before-detach");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 120000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    assert_no_completion_for (dealer, 20);

    zlink_msg_t pending_probe;
    init_part (&pending_probe, "pending-before-detach-probe");
    zlink_completion_id_t pending_probe_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &pending_probe,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          NULL, &pending_probe_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, pending_probe_id);
    assert_part_consumed (&pending_probe);

    // No application SEND has ever run on this socket. Physical detach is not
    // a terminal owner for the accepted REQUEST or its completion budget.
    test_context_socket_close_zero_linger (first_router);
    first_router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }
    assert_no_completion_for (dealer, 20);

    const uint64_t reconnect_hwm = 128u * 1024u;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &reconnect_hwm,
                        sizeof (reconnect_hwm)));
    void *replacement_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_router);
    set_routing_id_text (replacement_router,
                         "request-only-reconnect-router");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (replacement_router, ZLINK_OPT_RCVHWM, &reconnect_hwm,
                        sizeof (reconnect_hwm)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_router, endpoint));
    for (int i = 0; i != 300; ++i) {
        process_socket_commands_through_public_api (replacement_router);
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }
    const router_part_t received =
      receive_router_part_eventually (replacement_router);
    TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
    TEST_ASSERT_EQUAL_STRING ("request-only-pending-before-detach",
                              received.payload.c_str ());
    assert_no_router_part_for (replacement_router, 20);

    zlink_msg_t reply;
    init_part (&reply, "request-only-reconnect-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (replacement_router, &received.source_rid,
                        received.reply_token, &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);

    zlink_completion_t completion = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "request-only-reconnect-reply",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (replacement_router);
    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_request_with_only_zero_weight_known_router_is_not_admitted ()
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
      ZLINK_SUBMIT_NOT_ADMITTED,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 1000, NULL, &request_id));
    TEST_ASSERT_EQUAL_INT (ECONNREFUSED, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, request_id);
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

void test_dealer_explicit_endpoint_removal_completes_admitted_request_not_found_once ()
{
    const char *const endpoint =
      "inproc://phase3-request-explicit-endpoint-removal";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "endpoint-removal-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    int request_context = 182;
    zlink_msg_t request;
    init_part (&request, "endpoint-removal-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 2000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    const router_part_t admitted = receive_router_part_eventually (router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (dealer, endpoint));
    assert_request_not_found_completion (dealer, request_id,
                                         &request_context, NULL);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_explicit_logical_rid_removal_completes_admitted_request_not_found_once ()
{
    const char *const endpoint =
      "inproc://phase3-request-explicit-rid-removal";
    const char *const server_rid_text = "request-rid-server";
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);
    set_routing_id_text (server_router, server_rid_text);
    set_routing_id_text (client_router, "request-rid-client");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (client_router,
                               ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               server_rid_text, strlen (server_rid_text)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (server_router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (client_router, endpoint));
    msleep (SETTLE_TIME);

    const zlink_routing_id_t server_rid = make_rid (server_rid_text);
    int request_context = 183;
    zlink_msg_t request;
    init_part (&request, "rid-removal-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (client_router, &server_rid, &request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 2000,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    const router_part_t admitted =
      receive_router_part_eventually (server_router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK, zlink_disconnect_rid (client_router, &server_rid));
    assert_request_not_found_completion (
      client_router, request_id, &request_context, &server_rid);
    assert_no_completion_for (client_router, 20);

    test_context_socket_close_zero_linger (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_send_and_request_share_pending_record_limit_and_release ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (dealer, "shared-pending", 14));

    const uint64_t hwm =
      4u * (64u + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    const uint64_t one_pending = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one_pending,
                        sizeof (one_pending)));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-shared-pending"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-shared-pending"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    zlink_completion_id_t pending_send_id = 0;
    for (int attempt = 0; attempt != 256 && pending_send_id == 0; ++attempt) {
        zlink_msg_t filler;
        init_part (&filler,
                   "send-pending-fill-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        assert_part_consumed (&filler);
        pending_send_id = completion_id;
    }
    TEST_ASSERT_NOT_EQUAL (0, pending_send_id);

    zlink_msg_t rejected_request;
    init_part (&rejected_request, "request-must-share-limit");
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &rejected_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 100,
                          NULL, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);
    assert_part_consumed (&rejected_request);

    zlink_completion_t send_completion;
    init_empty_completion (&send_completion);
    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < drain_deadline) {
        if (zlink_completion_recv (dealer, &send_completion,
                                  ZLINK_RECV_FLAGS_DONTWAIT)
            == ZLINK_RECV_OK)
            break;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        const router_part_t drained = receive_router_part_eventually (router);
        TEST_ASSERT_EQUAL_UINT64 (0, drained.reply_token);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, send_completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (pending_send_id,
                              send_completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                           send_completion.send_result);
    zlink_completion_close (&send_completion);

    int request_context = 99;
    zlink_msg_t accepted_request;
    init_part (&accepted_request, "request-after-release");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &accepted_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1000,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&accepted_request);

    router_part_t request;
    do {
        request = receive_router_part_eventually (router);
    } while (request.reply_token == 0);
    zlink_msg_t reply;
    init_part (&reply, "shared-limit-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);

    zlink_completion_t request_completion =
      receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                           request_completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id,
                              request_completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&request_context,
                           request_completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                           request_completion.request_result);
    zlink_completion_close (&request_completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_pending_msgs_request_then_send_share_pool ()
{
    const char *const request_endpoint =
      "inproc://phase3-pending-msgs-request-route";
    const char *const data_endpoint =
      "inproc://phase3-pending-msgs-data-route";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *data_peer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (data_peer);
    set_routing_id_text (router, "pending-msgs-router");
    set_routing_id_text (dealer, "pending-msgs-dealer");
    set_routing_id_text (data_peer, "pending-msgs-data-peer");

    const uint64_t hwm =
      4u * (64u + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (data_peer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    const uint64_t one_pending = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one_pending,
                        sizeof (one_pending)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, request_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router, request_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);
    test_context_socket_close_zero_linger (router);
    router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (data_peer, data_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, data_endpoint));
    msleep (SETTLE_TIME);
    zlink_msg_t data_prime;
    init_part (&data_prime, "pending-msgs-data-prime");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &data_prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&data_prime);
    receive_dealer_data_eventually (data_peer, "pending-msgs-data-prime");

    int request_context = 201;
    zlink_msg_t request;
    init_part (&request, "request-owns-shared-msg-slot");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 120000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    assert_no_completion_for (dealer, 20);

    fill_live_dealer_pipe_until_shared_limit_rejects (dealer);

    test_context_socket_close_zero_linger (data_peer);
    test_context_socket_close_zero_linger (dealer);
}

void test_pending_request_terminal_releases_shared_msg_slot ()
{
    const char *const request_endpoint = endpoint_3 ();
    const char *const data_endpoint =
      "inproc://phase3-pending-terminal-data-route";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *data_peer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (data_peer);
    set_routing_id_text (router, "pending-terminal-router");
    set_routing_id_text (dealer, "pending-terminal-dealer");
    set_routing_id_text (data_peer, "pending-terminal-data-peer");

    const uint64_t hwm =
      4u * (64u + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (data_peer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    const uint64_t one_pending = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one_pending,
                        sizeof (one_pending)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (router, request_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, request_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    for (int i = 0; i != 300; ++i) {
        process_socket_commands_through_public_api (router);
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }
    zlink_pollitem_t paused_item = {dealer, 0, ZLINK_POLLOUT, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&paused_item, 1, 0, NULL));

    int request_context = 202;
    zlink_msg_t request;
    init_part (&request, "terminal-releases-shared-msg-slot");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 120000, &request_context,
                          &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (dealer, request_endpoint));
    assert_request_not_found_completion (dealer, request_id,
                                         &request_context, NULL);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (data_peer, data_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, data_endpoint));
    msleep (SETTLE_TIME);
    zlink_msg_t data_prime;
    init_part (&data_prime, "pending-terminal-data-prime");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &data_prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&data_prime);
    receive_dealer_data_eventually (data_peer,
                                    "pending-terminal-data-prime");
    (void) fill_live_dealer_pipe_until_pending_is_accepted (dealer);

    test_context_socket_close_zero_linger (data_peer);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_pending_bytes_request_then_send_share_pool_and_admission_releases ()
{
    const char *const request_endpoint =
      "inproc://phase3-pending-bytes-request-route";
    const char *const data_endpoint =
      "inproc://phase3-pending-bytes-data-route";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *data_peer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (data_peer);
    set_routing_id_text (router, "pending-bytes-router");
    set_routing_id_text (dealer, "pending-bytes-dealer");
    set_routing_id_text (data_peer, "pending-bytes-data-peer");

    const uint64_t hwm =
      4u * (64u + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (data_peer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    const uint64_t one_part_charge = sizeof (zlink_msg_t);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_BYTES,
                        &one_part_charge, sizeof (one_part_charge)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, request_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router, request_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);
    test_context_socket_close_zero_linger (router);
    router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (data_peer, data_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, data_endpoint));
    msleep (SETTLE_TIME);
    zlink_msg_t data_prime;
    init_part (&data_prime, "pending-bytes-data-prime");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &data_prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&data_prime);
    receive_dealer_data_eventually (data_peer, "pending-bytes-data-prime");

    zlink_msg_t request;
    init_part (&request, "r");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, 120000, NULL, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);
    fill_live_dealer_pipe_until_shared_limit_rejects (dealer);

    void *replacement_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (replacement_router);
    set_routing_id_text (replacement_router, "pending-bytes-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (replacement_router,
                                          request_endpoint));
    const router_part_t admitted =
      receive_router_part_eventually (replacement_router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);
    TEST_ASSERT_EQUAL_STRING ("r", admitted.payload.c_str ());

    // Admission, not reply completion, releases the shared byte reservation.
    // Remove only the physical Router so raw SEND has one live data target.
    test_context_socket_close_zero_linger (replacement_router);
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }
    (void) submit_pending_send_after_shared_limit_release (dealer);

    test_context_socket_close_zero_linger (data_peer);
    test_context_socket_close_zero_linger (dealer);
}

void test_pending_byte_sum_saturates_at_uint64_max_without_wrap ()
{
    if (std::numeric_limits<size_t>::max ()
        != std::numeric_limits<uint64_t>::max ())
        TEST_IGNORE_MESSAGE (
          "size_t cannot drive the uint64 pending-byte overflow boundary");

    const char *const request_endpoint =
      "inproc://phase3-pending-bytes-overflow-request";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "pending-overflow-router");
    set_routing_id_text (dealer, "pending-overflow-dealer");

    const uint64_t byte_limit = std::numeric_limits<uint64_t>::max () - 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_BYTES, &byte_limit,
                        sizeof (byte_limit)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, request_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router, request_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);
    test_context_socket_close_zero_linger (router);
    router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    // Constant messages preserve the public size without allocating or
    // reading the dummy byte. Three INT64_MAX-sized parts overflow only the
    // record charge sum. The known-detached route reaches pending admission
    // without a transport accessing their payload.
    unsigned char borrowed = 0;
    const size_t huge_size =
      static_cast<size_t> (std::numeric_limits<int64_t>::max ());
    for (int part_index = 0; part_index != 2; ++part_index) {
        zlink_msg_t huge_more;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_msg_init_data (&huge_more, &borrowed, huge_size, NULL, NULL));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &huge_more,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, 0,
                              NULL, NULL));
        assert_part_consumed (&huge_more);
    }

    zlink_msg_t huge_final;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_msg_init_data (&huge_final, &borrowed, huge_size, NULL, NULL));
    zlink_completion_id_t request_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &huge_final,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          NULL, &request_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, request_id);
    assert_part_consumed (&huge_final);
    assert_no_completion_for (dealer, 20);

    test_context_socket_close_zero_linger (dealer);
}

void test_pending_limit_runtime_shrink_preserves_existing_reservation ()
{
    const zlink_option_t options[] = {ZLINK_OPT_PENDING_MAX_MSGS,
                                     ZLINK_OPT_PENDING_MAX_BYTES};
    const char *const endpoints[] = {
      "inproc://phase3-pending-shrink-msgs",
      "inproc://phase3-pending-shrink-bytes"};

    for (size_t option_index = 0;
         option_index != sizeof (options) / sizeof (options[0]);
         ++option_index) {
        void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);
        set_routing_id_text (router, option_index == 0 ? "shrink-msgs-router"
                                                       : "shrink-bytes-router");
        set_routing_id_text (dealer, option_index == 0 ? "shrink-msgs-dealer"
                                                       : "shrink-bytes-dealer");

        const uint64_t one_reservation =
          option_index == 0 ? 1 : static_cast<uint64_t> (sizeof (zlink_msg_t));
        const uint64_t initial_limit = one_reservation * 2;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, options[option_index], &initial_limit,
                            sizeof (initial_limit)));

        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (dealer, endpoints[option_index]));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (router, endpoints[option_index]));
        msleep (SETTLE_TIME);
        prime_router_dealer_route (dealer, router);
        test_context_socket_close_zero_linger (router);
        router = NULL;
        for (int i = 0; i != 40; ++i) {
            process_socket_commands_through_public_api (dealer);
            msleep (1);
        }

        zlink_msg_t first;
        init_part (&first, "first-shrink-reservation");
        zlink_completion_id_t first_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &first,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              120000, NULL, &first_id));
        TEST_ASSERT_NOT_EQUAL (0, first_id);
        assert_part_consumed (&first);

        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, options[option_index], &one_reservation,
                            sizeof (one_reservation)));
        assert_no_completion_for (dealer, 20);

        zlink_msg_t second;
        init_part (&second, "second-shrink-reservation");
        zlink_completion_id_t second_id = UINT64_MAX;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_BACKPRESSURED,
          zlink_request_part (dealer, NULL, &second,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              120000, NULL, &second_id));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, second_id);
        assert_part_consumed (&second);
        assert_no_completion_for (dealer, 20);

        test_context_socket_close_zero_linger (dealer);
    }
}

void test_request_lifecycle_discards_pending_and_unread_completion ()
{
    {
        void *context = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (context);
        void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
        void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);
        set_routing_id_text (router, "request-close-router");
        set_routing_id_text (dealer, "request-close-dealer");

        const int zero_linger = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero_linger,
                            sizeof (zero_linger)));
        const uint64_t one_byte_hwm = 1;
        const uint64_t one_pending = 1;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &one_byte_hwm,
                            sizeof (one_byte_hwm)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_RCVHWM, &one_byte_hwm,
                            sizeof (one_byte_hwm)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one_pending,
                            sizeof (one_pending)));

        TEST_ASSERT_EQUAL_INT (
          ZLINK_BIND_OK,
          zlink_bind (dealer, "inproc://phase3-request-pending-close"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (router, "inproc://phase3-request-pending-close"));
        msleep (SETTLE_TIME);
        for (int i = 0; i != 300; ++i) {
            process_socket_commands_through_public_api (router);
            process_socket_commands_through_public_api (dealer);
            msleep (1);
        }

        zlink_msg_t request;
        init_part (&request, "pending-request-discarded-by-close");
        zlink_completion_id_t request_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &request,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              120000, NULL, &request_id));
        TEST_ASSERT_NOT_EQUAL (0, request_id);
        assert_part_consumed (&request);

        zlink_msg_t pending_probe;
        init_part (&pending_probe, "pending-close-probe");
        zlink_completion_id_t probe_id = UINT64_MAX;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_BACKPRESSURED,
          zlink_request_part (dealer, NULL, &pending_probe,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              120000, NULL, &probe_id));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, probe_id);
        assert_part_consumed (&pending_probe);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_INVALID_STATE,
          zlink_completion_recv (dealer, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ESHUTDOWN, zlink_errno ());
        assert_empty_completion (completion);

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
        set_routing_id_text (router, "request-context-router");
        set_routing_id_text (dealer, "request-context-dealer");

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
          zlink_bind (dealer, "inproc://phase3-request-unread-context"));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (router, "inproc://phase3-request-unread-context"));
        msleep (SETTLE_TIME);
        for (int i = 0; i != 300; ++i) {
            process_socket_commands_through_public_api (router);
            process_socket_commands_through_public_api (dealer);
            msleep (1);
        }

        zlink_msg_t request;
        init_part (&request, "request-with-unread-completion");
        zlink_completion_id_t request_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (dealer, NULL, &request,
                              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                              120000, NULL, &request_id));
        TEST_ASSERT_NOT_EQUAL (0, request_id);
        assert_part_consumed (&request);

        const router_part_t received = receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
        zlink_msg_t reply;
        init_part (&reply, "unread-request-reply");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_reply_part (router, &received.source_rid,
                            received.reply_token, &reply, ZLINK_PART_FINAL));
        assert_part_consumed (&reply);

        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer, dealer,
                            ZLINK_POLLCOMPLETION));
        zlink_poller_event_t completion_event;
        memset (&completion_event, 0, sizeof (completion_event));
        const std::chrono::steady_clock::time_point ready_deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::milliseconds (kWaitMilliseconds);
        do {
            process_socket_commands_through_public_api (dealer);
            zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
            const int count = zlink_poller_wait (
              poller, &completion_event, 1, 0, &poll_error);
            TEST_ASSERT_TRUE (count == 0 || count == 1);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
            if (count == 1)
                break;
            msleep (1);
        } while (std::chrono::steady_clock::now () < ready_deadline);
        TEST_ASSERT_TRUE ((completion_event.events & ZLINK_POLLCOMPLETION)
                          != 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller, dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                               zlink_poller_destroy (&poller));

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (context));
        zlink_completion_t completion;
        init_empty_completion (&completion);
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_TERMINATED,
          zlink_completion_recv (dealer, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ETERM, zlink_errno ());
        assert_empty_completion (completion);

        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (router));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }
}

void test_mixed_send_and_request_completions_are_drained_once_by_id ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (dealer, "mixed-completion", 16));

    const uint64_t hwm =
      4u * (64u + static_cast<uint64_t> (sizeof (zlink_msg_t)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://phase3-mixed-completion-drain"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://phase3-mixed-completion-drain"));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);

    int send_context = 121;
    zlink_completion_id_t send_id = 0;
    for (int attempt = 0; attempt != 256 && send_id == 0; ++attempt) {
        zlink_msg_t part;
        init_part (&part,
                   "mixed-send-fill-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, &send_context, &completion_id));
        assert_part_consumed (&part);
        send_id = completion_id;
    }
    TEST_ASSERT_NOT_EQUAL (0, send_id);

    int request_context = 122;
    zlink_msg_t request_part;
    init_part (&request_part, "mixed-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request_part,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 2000,
                          &request_context, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    TEST_ASSERT_NOT_EQUAL (send_id, request_id);
    assert_part_consumed (&request_part);

    router_part_t request;
    do {
        request = receive_router_part_eventually (router);
    } while (request.reply_token == 0);
    TEST_ASSERT_EQUAL_STRING ("mixed-request", request.payload.c_str ());

    zlink_msg_t reply;
    init_part (&reply, "mixed-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid, request.reply_token,
                        &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);

    bool saw_send = false;
    bool saw_request = false;
    for (int i = 0; i != 2; ++i) {
        zlink_completion_t completion =
          receive_completion_eventually (dealer);
        TEST_ASSERT_EQUAL_UINT (0, completion.peer_rid.size);
        if (completion.completion_id == send_id) {
            TEST_ASSERT_FALSE (saw_send);
            saw_send = true;
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
            TEST_ASSERT_EQUAL_PTR (&send_context, completion.user_context);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                                   completion.send_result);
        } else if (completion.completion_id == request_id) {
            TEST_ASSERT_FALSE (saw_request);
            saw_request = true;
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                                   completion.kind);
            TEST_ASSERT_EQUAL_PTR (&request_context,
                                   completion.user_context);
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                                   completion.request_result);
            TEST_ASSERT_NOT_NULL (completion.reply_parts);
            TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
            TEST_ASSERT_EQUAL_STRING (
              "mixed-reply",
              part_string (&completion.reply_parts[0]).c_str ());
        } else {
            TEST_FAIL_MESSAGE (
              "mixed completion queue returned an unknown completion id");
        }
        zlink_completion_close (&completion);
    }
    TEST_ASSERT_TRUE (saw_send);
    TEST_ASSERT_TRUE (saw_request);

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

void test_send_and_request_share_65536_completion_reservations ()
{
    const char *const request_endpoint =
      "inproc://phase3-mixed-completion-cap-request";
    const char *const data_endpoint =
      "inproc://phase3-mixed-completion-cap-data";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *data_peer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (data_peer);
    set_routing_id_text (router, "mixed-cap-router");
    set_routing_id_text (dealer, "mixed-cap-dealer");
    set_routing_id_text (data_peer, "mixed-cap-data-peer");

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (dealer, request_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (router, request_endpoint));
    msleep (SETTLE_TIME);
    prime_router_dealer_route (dealer, router);
    test_context_socket_close_zero_linger (router);
    router = NULL;
    for (int i = 0; i != 40; ++i) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
    }

    // The empty-pipe oversize exception admits exactly one application frame;
    // without a receiver, all later SEND records must retain completion slots.
    // Apply the small HWM only after the request route has been primed.
    const uint64_t one_byte_hwm = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &one_byte_hwm,
                        sizeof (one_byte_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (data_peer, ZLINK_OPT_RCVHWM, &one_byte_hwm,
                        sizeof (one_byte_hwm)));

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (data_peer, data_endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, data_endpoint));
    msleep (SETTLE_TIME);
    zlink_msg_t data_prime;
    init_part (&data_prime, "mixed-cap-prime");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &data_prime, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&data_prime);
    receive_dealer_data_eventually (data_peer, "mixed-cap-prime");

    int first_request_context = 301;
    zlink_msg_t first_request;
    init_part (&first_request, "mixed-cap-first-request");
    zlink_completion_id_t first_request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &first_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          &first_request_context, &first_request_id));
    TEST_ASSERT_NOT_EQUAL (0, first_request_id);
    assert_part_consumed (&first_request);

    const size_t required_send_reservations = kReplyTokenCapacity - 1;
    size_t send_reservations = 0;
    bool pending_started = false;
    for (size_t attempts = 0;
         send_reservations != required_send_reservations;
         ++attempts) {
        TEST_ASSERT_TRUE_MESSAGE (
          attempts <= required_send_reservations + 16,
          "SEND pipe did not become pending at the one-frame HWM");
        zlink_msg_t data;
        init_part (&data, "mixed-cap-fill");
        zlink_completion_id_t completion_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &data, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &completion_id));
        assert_part_consumed (&data);
        if (completion_id == 0) {
            TEST_ASSERT_FALSE (pending_started);
            continue;
        }
        pending_started = true;
        ++send_reservations;
    }

    // One REQUEST plus 65,535 SEND operations owns the socket-local 65,536
    // completion budget. A cross-family FINAL cannot overbook it.
    zlink_msg_t rejected_request;
    init_part (&rejected_request, "mixed-cap-rejected-request");
    zlink_completion_id_t rejected_id = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (dealer, NULL, &rejected_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          NULL, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);
    assert_part_consumed (&rejected_request);

    // One DATA receive admits one queued SEND, and dequeuing exactly that one
    // SEND completion releases one shared reservation.
    receive_dealer_data_eventually (data_peer, "mixed-cap-fill");
    zlink_completion_t admitted_send = receive_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, admitted_send.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, admitted_send.send_result);
    zlink_completion_close (&admitted_send);

    zlink_msg_t retried_request;
    init_part (&retried_request, "mixed-cap-retried-request");
    zlink_completion_id_t retried_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &retried_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
                          NULL, &retried_id));
    TEST_ASSERT_NOT_EQUAL (0, retried_id);
    assert_part_consumed (&retried_request);

    test_context_socket_close_zero_linger (data_peer);
    test_context_socket_close_zero_linger (dealer);
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
      test_dealer_dontwait_request_pins_known_detached_endpoint_until_reconnect);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_pending_request_survives_detach_before_admission_without_prior_send);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_request_with_only_zero_weight_known_router_is_not_admitted);
    RUN_PHASE3_REQUEST_TEST (
      test_admitted_request_survives_physical_detach_and_same_rid_reconnect_without_replay);
    RUN_PHASE3_REQUEST_TEST (
      test_dealer_explicit_endpoint_removal_completes_admitted_request_not_found_once);
    RUN_PHASE3_REQUEST_TEST (
      test_router_explicit_logical_rid_removal_completes_admitted_request_not_found_once);
    RUN_PHASE3_REQUEST_TEST (
      test_send_and_request_share_pending_record_limit_and_release);
    RUN_PHASE3_REQUEST_TEST (
      test_pending_msgs_request_then_send_share_pool);
    RUN_PHASE3_REQUEST_TEST (
      test_pending_request_terminal_releases_shared_msg_slot);
    RUN_PHASE3_REQUEST_TEST (
      test_pending_bytes_request_then_send_share_pool_and_admission_releases);
    RUN_PHASE3_REQUEST_TEST (
      test_pending_byte_sum_saturates_at_uint64_max_without_wrap);
    RUN_PHASE3_REQUEST_TEST (
      test_pending_limit_runtime_shrink_preserves_existing_reservation);
    RUN_PHASE3_REQUEST_TEST (
      test_request_lifecycle_discards_pending_and_unread_completion);
    RUN_PHASE3_REQUEST_TEST (
      test_mixed_send_and_request_completions_are_drained_once_by_id);
    RUN_PHASE3_REQUEST_TEST (
      test_send_and_request_share_65536_completion_reservations);
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
      test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced);

#undef RUN_PHASE3_REQUEST_TEST

    return UNITY_END ();
}
