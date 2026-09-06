/* SPDX-License-Identifier: MPL-2.0 */

#include "contract_socket_pair_fixture.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
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

zlink_completion_t receive_completion_now (void *socket_)
{
    contract_socket_pair_t::pump_owner (as_socket (socket_));
    zlink::completion_drain_scope_t completion_owner (as_socket (socket_));
    as_socket (socket_)->process_ready_completion_pipes ();
    zlink_completion_t completion;
    init_empty_completion (&completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    return completion;
}

void assert_no_completion (void *socket_)
{
    contract_socket_pair_t::pump_owner (as_socket (socket_));
    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
      zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    assert_empty_completion (completion);
}

struct router_part_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    zlink_part_flag_t part_flag;
    std::string payload;
};

router_part_t receive_router_part_now (void *router_)
{
    contract_socket_pair_t::pump_owner (as_socket (router_));
    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source_rid, &token, &part, &flag,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (source_rid);
    router_part_t received;
    received.source_rid = *source_rid;
    received.reply_token = token;
    received.part_flag = flag;
    received.payload = part_string (&part);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return received;
}

void assert_ready_pair (void *socket_, void *first_peer_, void *second_peer_,
                        const zlink_routing_id_t &rid_, uint64_t *pair_out_,
                        uint64_t *generation_out_)
{
    contract_socket_pair_t::pump_owner (as_socket (first_peer_));
    contract_socket_pair_t::pump_owner (as_socket (second_peer_));
    contract_socket_pair_t::pump_owner (as_socket (socket_));
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    TEST_ASSERT_EQUAL_INT (0,
      as_socket (socket_)->select_routed_submit_target (&rid_, &target));
    TEST_ASSERT_NOT_EQUAL (0, target.transport_pair_id);
    TEST_ASSERT_NOT_EQUAL (0, target.transport_pair_generation);
    TEST_ASSERT_NOT_NULL (as_socket (socket_)->test_pair_pipe (
      target.transport_pair_id, target.transport_pair_generation, false));
    TEST_ASSERT_NOT_NULL (as_socket (socket_)->test_pair_pipe (
      target.transport_pair_id, target.transport_pair_generation, true));
    *pair_out_ = target.transport_pair_id;
    *generation_out_ = target.transport_pair_generation;
}

void assert_completion_pair_queued (void *socket_, uint64_t pair_id_,
                                    uint64_t generation_)
{
    contract_socket_pair_t::pump_owner (as_socket (socket_));
    TEST_ASSERT_TRUE (as_socket (socket_)->test_completion_pair_queued (
      pair_id_, generation_));
}



bool completion_lane_detached (void *socket_, uint64_t pair_id_,
                               uint64_t generation_)
{
    contract_socket_pair_t::pump_owner (as_socket (socket_));
    return !as_socket (socket_)->test_pair_pipe (pair_id_, generation_, true);
}

zlink_completion_id_t send_router_request_to (
  void *router_, const zlink_routing_id_t &target_, const char *payload_,
  void *user_context_)
{
    zlink_msg_t request;
    init_part (&request, payload_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      ZLINK_SUBMIT_OK,
      zlink_request_part (router_, &target_, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 120000, user_context_,
                          &completion_id), payload_);
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_part_consumed (&request);
    return completion_id;
}

void receive_dealer_data_now (void *dealer_, const char *expected_)
{
    contract_socket_pair_t::pump_owner (as_socket (dealer_));
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_recv_part (dealer_, NULL, &part, &flag, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, flag);
    TEST_ASSERT_EQUAL_STRING (expected_, part_string (&part).c_str ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
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

    const router_part_t received = receive_router_part_now (router_);
    TEST_ASSERT_EQUAL_UINT64 (0, received.reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received.part_flag);
    TEST_ASSERT_EQUAL_STRING ("prime", received.payload.c_str ());
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

void test_router_reply_final_oom_releases_checkout_and_retains_token ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "reply-final-oom-peer");
    set_routing_id_text (router, "unit-router");
    contract_socket_pair_t pair (dealer, router);
    TEST_ASSERT_TRUE (pair.cores[0]->acquire_completion_poller (&pair));

    const zlink_completion_id_t request_id =
      send_public_request (dealer, "reply-final-oom-request");
    const router_part_t request = receive_router_part_now (router);
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

    zlink_completion_t completion = receive_completion_now (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "fresh-reply-after-oom",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);
    assert_no_completion (dealer);

    pair.cores[0]->release_completion_poller (&pair);
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

    set_routing_id_text (router, "unit-router");
    contract_socket_pair_t pair (dealer, router);
    TEST_ASSERT_TRUE (pair.cores[0]->acquire_completion_poller (&pair));

    const zlink_completion_id_t request_id =
      send_public_request (dealer, "reply-final-eio-request");
    const router_part_t request = receive_router_part_now (router);
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

    zlink_completion_t completion = receive_completion_now (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "fresh-reply-after-eio",
      part_string (&completion.reply_parts[0]).c_str ());
    zlink_completion_close (&completion);

    pair.cores[0]->release_completion_poller (&pair);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle ()
{
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
    contract_socket_pair_t pair (dealer, router, 1, 1, true, hwm);

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
    contract_socket_pair_t pair_a (requester, responder_a, 1);
    contract_socket_pair_t pair_b (requester, responder_b, 2);

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
        requests_a.push_back (receive_router_part_now (responder_a));
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL,
                               requests_a.back ().part_flag);
    }

    int context_b = 0x5b;
    const zlink_completion_id_t completion_id_b = send_router_request_to (
      requester, responder_b_rid, "fairness-request-b", &context_b);
    const router_part_t request_b =
      receive_router_part_now (responder_b);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, request_b.part_flag);

    //  The first routed request on each connection also proves that both
    //  inproc owners have adopted their Application and Completion halves.
    //  Resolve the exact pair keys only after that owner progress has occurred.
    assert_ready_pair (requester, responder_a, responder_b, responder_a_rid,
                         &pair_a_id, &pair_a_generation);
    assert_ready_pair (requester, responder_a, responder_b, responder_b_rid,
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
    assert_completion_pair_queued (requester, pair_a_id,
                                     pair_a_generation);

    zlink_msg_t reply_b;
    init_part (&reply_b, "fairness-b-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (responder_b, &request_b.source_rid,
                        request_b.reply_token, &reply_b, ZLINK_PART_FINAL));
    assert_part_consumed (&reply_b);
    assert_completion_pair_queued (requester, pair_b_id,
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

    pair_a.application[1]->terminate (false);
    pair_a.completion[1]->terminate (false);
    pair_a.pump ();
    const bool old_completion_detached = completion_lane_detached (
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
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED,
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
    contract_socket_pair_t replacement_pair (requester, replacement_a, 1, 2);

    uint64_t replacement_pair_id = 0;
    uint64_t replacement_generation = 0;
    int replacement_context = 0x6a;
    const zlink_completion_id_t replacement_completion_id =
      send_router_request_to (requester, responder_a_rid,
                              "replacement-request", &replacement_context);
    const router_part_t replacement_request =
      receive_router_part_now (replacement_a);
    assert_ready_pair (requester, replacement_a, responder_b,
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
    assert_completion_pair_queued (requester, replacement_pair_id,
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
    test_context_socket_close_zero_linger (responder_a);
    test_context_socket_close_zero_linger (replacement_a);
    test_context_socket_close_zero_linger (responder_b);
    test_context_socket_close_zero_linger (requester);
}

void test_count1_completion_ready_queue_preserves_fifo_and_reuses_pipe_node ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "count1-ready-router");
    set_routing_id_text (dealer, "count1-ready-dealer");
    contract_socket_pair_t pair (dealer, router);

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
        requests[i] = receive_router_part_now (router);
        TEST_ASSERT_NOT_EQUAL (0, requests[i].reply_token);
    }

    // Put one public DATA record ahead of both private replies on the same
    // physical FIFO. Classification must leave that pipe public and must not
    // publish its private-ready node until DATA has been consumed.
    const char public_payload[] = "count1-public-before-replies";
    zlink_msg_t public_part;
    init_part (&public_part, public_payload);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &requests[0].source_rid, &public_part,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL));
    assert_part_consumed (&public_part);
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
    receive_dealer_data_now (dealer, public_payload);
    assert_completion_pair_queued (
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
    requests[2] = receive_router_part_now (router);

    zlink_msg_t reply;
    init_part (&reply, reply_payloads[2]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &requests[2].source_rid,
                        requests[2].reply_token, &reply, ZLINK_PART_FINAL));
    assert_part_consumed (&reply);
    assert_completion_pair_queued (
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
    UNITY_BEGIN ();
    if (should_run_phase3_request_test ("test_router_reply_final_oom_releases_checkout_and_retains_token"))
        RUN_TEST (test_router_reply_final_oom_releases_checkout_and_retains_token);
    if (should_run_phase3_request_test ("test_router_reply_final_runtime_failure_releases_checkout_and_retains_token"))
        RUN_TEST (test_router_reply_final_runtime_failure_releases_checkout_and_retains_token);
    if (should_run_phase3_request_test ("test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle"))
        RUN_TEST (test_blocking_request_send_drains_owned_completions_to_break_hwm_cycle);
    if (should_run_phase3_request_test ("test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced"))
        RUN_TEST (test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced);
    if (should_run_phase3_request_test ("test_count1_completion_ready_queue_preserves_fifo_and_reuses_pipe_node"))
        RUN_TEST (test_count1_completion_ready_queue_preserves_fifo_and_reuses_pipe_node);
    return UNITY_END ();
}
