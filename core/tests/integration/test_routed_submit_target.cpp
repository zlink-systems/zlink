/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string.h>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct routed_terminal_probe_t
{
    routed_terminal_probe_t () : seen (false), expected_pair_id (0),
                                 expected_pair_generation (0), pair_id (0),
                                 pair_generation (0), terminal_errno (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool seen;
    uint64_t expected_pair_id;
    uint64_t expected_pair_generation;
    std::string expected_rid;
    uint64_t pair_id;
    uint64_t pair_generation;
    int terminal_errno;
};

//  A terminal completion is now tied to a concrete pending operation: the
//  record reserved for that exact target is what fails when the target ends.
void capture_routed_terminal (
  void *, const zlink_send_complete_event_t *event_, void *userdata_)
{
    routed_terminal_probe_t *probe =
      static_cast<routed_terminal_probe_t *> (userdata_);
    if (!probe || !event_ || event_->result != ZLINK_SEND_TERMINAL)
        return;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        const std::string rid (
          reinterpret_cast<const char *> (event_->peer_rid.data),
          event_->peer_rid.size);
        if (probe->expected_pair_id != event_->transport_pair_id
            || probe->expected_pair_generation
                 != event_->transport_pair_generation
            || probe->expected_rid != rid)
            return;
        probe->seen = true;
        probe->pair_id = event_->transport_pair_id;
        probe->pair_generation = event_->transport_pair_generation;
        probe->terminal_errno = event_->terminal_errno;
    }
    probe->changed.notify_all ();
}

zlink_routing_id_t make_rid (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    zlink_assert (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    memcpy (rid.data, value_, size);
    return rid;
}

std::string rid_string (const zlink_routing_id_t &rid_)
{
    return std::string (reinterpret_cast<const char *> (rid_.data), rid_.size);
}

void init_part (zlink_msg_t *part_, const void *data_, size_t size_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size_));
    if (size_ != 0 && data_)
        memcpy (zlink_msg_data (part_), data_, size_);
}

void init_text_parts (zlink_msg_t *parts_, const char *first_,
                      const char *second_)
{
    init_part (&parts_[0], first_, strlen (first_));
    init_part (&parts_[1], second_, strlen (second_));
}

void close_parts (zlink_msg_t *parts_, size_t part_count_)
{
    for (size_t i = 0; i < part_count_; ++i)
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts_[i]));
}

zlink_submit_result_t dealer_send_bytes (
  void *dealer_, const zlink_routed_submit_target_t *target_, size_t size_,
  zlink_part_flag_t part_flag_ = ZLINK_PART_FINAL)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, size_));
    if (size_ != 0)
        memset (zlink_msg_data (&part), 0x5a, size_);
    return zlink_dealer_send_transport_pair_part (
      dealer_, target_, &part, ZLINK_SEND_FLAGS_DONTWAIT, part_flag_);
}

zlink_submit_result_t router_send_text (
  void *router_, const zlink_routed_submit_target_t *target_,
  const char *text_)
{
    zlink_msg_t part;
    init_part (&part, text_, strlen (text_));
    return zlink_send_part_transport_pair (
      router_, &target_->peer_rid, target_->transport_pair_id,
      target_->transport_pair_generation, &part, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL);
}

zlink_submit_result_t router_send_bytes (
  void *router_, const zlink_routed_submit_target_t *target_, size_t size_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, size_));
    if (size_ != 0)
        memset (zlink_msg_data (&part), 0x5a, size_);
    return zlink_send_part_transport_pair (
      router_, &target_->peer_rid, target_->transport_pair_id,
      target_->transport_pair_generation, &part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
}

bool recv_part_eventually (void *socket_, const char *expected_,
                           int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_recv_part (
          socket_, &source_rid, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            const std::string payload (
              static_cast<const char *> (zlink_msg_data (&part)),
              zlink_msg_size (&part));
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return payload == expected_;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    return false;
}

bool recv_router_part_eventually (void *router_, const std::string &expected_,
                                  int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        uint64_t pair_id = 0;
        uint64_t pair_generation = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_router_recv_part_v2 (
          router_, &source_rid, &request_seq, &pair_id, &pair_generation,
          &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            const std::string payload (
              static_cast<const char *> (zlink_msg_data (&part)),
              zlink_msg_size (&part));
            TEST_ASSERT_NOT_NULL (source_rid);
            TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
            TEST_ASSERT_TRUE (pair_id != 0);
            TEST_ASSERT_TRUE (pair_generation != 0);
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return payload == expected_;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    return false;
}

bool recv_router_record_eventually (void *router_, const char *first_,
                                    const char *second_,
                                    uint64_t *request_seq_out_ = NULL,
                                    int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    std::vector<std::string> payloads;
    uint64_t record_sequence = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        uint64_t pair_id = 0;
        uint64_t pair_generation = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_router_recv_part_v2 (
          router_, &source_rid, &request_seq, &pair_id, &pair_generation,
          &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
        TEST_ASSERT_NOT_NULL (source_rid);
        if (payloads.empty ())
            record_sequence = request_seq;
        TEST_ASSERT_EQUAL_UINT64 (record_sequence, request_seq);
        payloads.push_back (std::string (
          static_cast<const char *> (zlink_msg_data (&part)),
          zlink_msg_size (&part)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        if (has_more == ZLINK_PART_FINAL)
            break;
    }

    if (payloads.size () != 2 || payloads[0] != first_
        || payloads[1] != second_)
        return false;
    if (request_seq_out_)
        *request_seq_out_ = record_sequence;
    return true;
}

bool recv_dealer_record_eventually (void *dealer_,
                                    std::vector<std::string> *payloads_out_,
                                    uint8_t *message_type_out_,
                                    uint64_t *request_seq_out_,
                                    int timeout_ms_ = 3000)
{
    if (!payloads_out_ || !message_type_out_ || !request_seq_out_)
        return false;
    payloads_out_->clear ();
    *message_type_out_ = 0xff;
    *request_seq_out_ = 0;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        uint8_t message_type = 0xff;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_dealer_recv_part (
          dealer_, &message_type, &request_seq, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
        if (payloads_out_->empty ()) {
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
        }
        TEST_ASSERT_EQUAL_UINT8 (*message_type_out_, message_type);
        TEST_ASSERT_EQUAL_UINT64 (*request_seq_out_, request_seq);
        payloads_out_->push_back (std::string (
          static_cast<const char *> (zlink_msg_data (&part)),
          zlink_msg_size (&part)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        if (has_more == ZLINK_PART_FINAL)
            return true;
    }
    return false;
}

bool recv_router_no_part (void *router_, int wait_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (wait_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        uint64_t pair_id = 0;
        uint64_t pair_generation = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_router_recv_part_v2 (
          router_, &source_rid, &request_seq, &pair_id, &pair_generation,
          &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        if (result == ZLINK_RECV_OK)
            return false;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        msleep (1);
    }
    return true;
}

zlink_routed_submit_target_t select_router_target_eventually (
  void *router_, const zlink_routing_id_t *peer_rid_)
{
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    for (int i = 0; i < 3000; ++i) {
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (router_, peer_rid_, &target);
        if (result == ZLINK_SUBMIT_OK)
            return target;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("ROUTER exact target did not become selectable");
    return target;
}

void select_dealer_targets_eventually (
  void *dealer_, const char *rid_a_, const char *rid_b_,
  zlink_routed_submit_target_t *target_a_out_,
  zlink_routed_submit_target_t *target_b_out_)
{
    memset (target_a_out_, 0, sizeof (*target_a_out_));
    memset (target_b_out_, 0, sizeof (*target_b_out_));
    bool have_a = false;
    bool have_b = false;
    for (int i = 0; i < 6000 && (!have_a || !have_b); ++i) {
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (dealer_, NULL, &target);
        if (result == ZLINK_SUBMIT_NOT_CONNECTED) {
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        const std::string rid = rid_string (target.peer_rid);
        if (rid == rid_a_) {
            *target_a_out_ = target;
            have_a = true;
            if (strcmp (rid_a_, rid_b_) == 0) {
                *target_b_out_ = target;
                have_b = true;
            }
        } else if (rid == rid_b_) {
            *target_b_out_ = target;
            have_b = true;
        } else {
            TEST_FAIL_MESSAGE ("DEALER selector returned an unknown peer RID");
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (have_a, "DEALER target A was not selected");
    TEST_ASSERT_TRUE_MESSAGE (have_b, "DEALER target B was not selected");
}

struct two_peer_fixture_t
{
    void *dealer;
    void *router_a;
    void *router_b;
    zlink_routed_submit_target_t target_a;
    zlink_routed_submit_target_t target_b;
};

two_peer_fixture_t make_two_peer_fixture (const char *endpoint_a_,
                                          const char *endpoint_b_,
                                          uint64_t hwm_)
{
    two_peer_fixture_t fixture;
    fixture.dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    fixture.router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    fixture.router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (fixture.dealer, "D", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (fixture.router_a, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (fixture.router_b, "B", 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.dealer, ZLINK_OPT_SNDHWM, &hwm_,
                        sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router_a, ZLINK_OPT_RCVHWM, &hwm_,
                        sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router_b, ZLINK_OPT_RCVHWM, &hwm_,
                        sizeof (hwm_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (fixture.router_a, endpoint_a_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (fixture.router_b, endpoint_b_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, endpoint_a_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (fixture.dealer, endpoint_b_));
    select_dealer_targets_eventually (
      fixture.dealer, "A", "B", &fixture.target_a, &fixture.target_b);
    return fixture;
}

void close_two_peer_fixture (two_peer_fixture_t *fixture_)
{
    fixture_->router_b =
      test_context_socket_close_zero_linger (fixture_->router_b);
    fixture_->router_a =
      test_context_socket_close_zero_linger (fixture_->router_a);
    fixture_->dealer =
      test_context_socket_close_zero_linger (fixture_->dealer);
}

struct reply_probe_t
{
    reply_probe_t () : done (false), callback_count (0),
                       result (ZLINK_REQUEST_PROTOCOL_ERROR)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool done;
    int callback_count;
    zlink_request_result_t result;
    std::string payload;
};

void capture_reply (zlink_request_result_t result_, zlink_msg_t *parts_,
                    size_t part_count_, void *userdata_)
{
    reply_probe_t *probe = static_cast<reply_probe_t *> (userdata_);
    if (!probe)
        return;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
        ++probe->callback_count;
        probe->result = result_;
        if (part_count_ != 0) {
            probe->payload.assign (
              static_cast<const char *> (zlink_msg_data (&parts_[0])),
              zlink_msg_size (&parts_[0]));
        }
    }
    probe->changed.notify_all ();
}

size_t pending_request_count (void *dealer_)
{
    const socket_handle_t handle = as_socket_handle (dealer_);
    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      zlink::socket_reqrep_internal::find_request_reply_state (handle);
    if (!state)
        return 0;
    std::lock_guard<std::mutex> lock (state->mutex);
    return state->pending_requests.size ();
}

bool progress_reply_until_done (void *dealer_, reply_probe_t *probe_,
                                int timeout_ms_)
{
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer_, NULL, ZLINK_POLLCOMPLETION));

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        {
            std::lock_guard<std::mutex> lock (probe_->mutex);
            if (probe_->done)
                break;
        }
        zlink_poller_event_t event;
        (void) zlink_poller_wait (poller, &event, 1, 10, NULL);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, dealer_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));

    std::lock_guard<std::mutex> lock (probe_->mutex);
    return probe_->done;
}

struct fast_reply_result_t
{
    fast_reply_result_t () : recv_result (ZLINK_RECV_INTERNAL_ERROR),
                             reply_result (ZLINK_SUBMIT_INTERNAL_ERROR),
                             request_seq (0), pair_id (0), pair_generation (0)
    {
    }

    zlink_recv_result_t recv_result;
    zlink_submit_result_t reply_result;
    uint64_t request_seq;
    uint64_t pair_id;
    uint64_t pair_generation;
    std::string payload;
};
}

void test_router_selects_exact_target_and_rejects_stale_generation ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router, "R", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-submit-router-select"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://routed-submit-router-select"));

    send_string_expect_success (dealer, "prime", 0);
    recv_string_expect_success (router, "D", 0);
    recv_string_expect_success (router, "prime", 0);

    const zlink_routing_id_t dealer_rid = make_rid ("D");
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (router, &dealer_rid);
    TEST_ASSERT_EQUAL_STRING ("D", rid_string (target.peer_rid).c_str ());
    TEST_ASSERT_TRUE (target.transport_pair_id != 0);
    TEST_ASSERT_TRUE (target.transport_pair_generation != 0);

    zlink_routed_submit_target_t stale = target;
    ++stale.transport_pair_generation;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED,
                           router_send_text (router, &stale, "stale"));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, zlink_errno ());

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           router_send_text (router, &target, "valid"));
    TEST_ASSERT_TRUE (recv_part_eventually (dealer, "valid"));

    const zlink_routing_id_t missing = make_rid ("missing");
    zlink_routed_submit_target_t missing_target;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_select_routed_submit_target (router, &missing, &missing_target));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_stable_router_route_never_returns_enoent_under_concurrent_send ()
{
    const int sender_count = 4;
    const int sends_per_thread = 20000;
    const int total_sends = sender_count * sends_per_thread;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t unlimited = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &unlimited, sizeof (unlimited)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &unlimited, sizeof (unlimited)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "stable-route", 12));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-submit-stable-stress"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://routed-submit-stable-stress"));

    const zlink_routing_id_t dealer_rid = make_rid ("stable-route");
    (void) select_router_target_eventually (router, &dealer_rid);

    std::atomic<int> ready (0);
    std::atomic<bool> go (false);
    std::atomic<bool> senders_done (false);
    std::atomic<int> submitted (0);
    std::atomic<int> enoent (0);
    std::atomic<int> other_failures (0);
    std::atomic<int> received (0);
    std::atomic<int> bad_records (0);

    std::thread receiver ([&] {
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (20);
        while (std::chrono::steady_clock::now () < deadline) {
            zlink_msg_t part;
            zlink_msg_init (&part);
            zlink_part_flag_t more = ZLINK_PART_FINAL;
            const zlink_recv_result_t rc = zlink_recv_part (
              dealer, NULL, &part, &more, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_OK) {
                if (more != ZLINK_PART_FINAL || zlink_msg_size (&part) != 2 * sizeof (int))
                    bad_records.fetch_add (1, std::memory_order_relaxed);
                received.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&part);
                continue;
            }
            zlink_msg_close (&part);
            if (senders_done.load (std::memory_order_acquire)
                && received.load (std::memory_order_relaxed)
                     >= submitted.load (std::memory_order_relaxed))
                return;
            std::this_thread::yield ();
        }
        bad_records.fetch_add (1, std::memory_order_relaxed);
    });

    std::vector<std::thread> senders;
    senders.reserve (sender_count);
    for (int sender_id = 0; sender_id < sender_count; ++sender_id) {
        senders.emplace_back ([&, sender_id] {
            ready.fetch_add (1, std::memory_order_release);
            while (!go.load (std::memory_order_acquire))
                std::this_thread::yield ();

            for (int sequence = 0; sequence < sends_per_thread; ++sequence) {
                zlink_msg_t part;
                if (zlink_msg_init_size (&part, 2 * sizeof (int)) != ZLINK_CONFIG_OK) {
                    other_failures.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }
                int payload[2] = {sender_id, sequence};
                memcpy (zlink_msg_data (&part), payload, sizeof (payload));
                const int rc = zlink_socket_send_rid_internal (
                  router, &dealer_rid, &part, 1, ZLINK_SEND_FLAGS_DONTWAIT);
                const int err = zlink_errno ();
                if (rc == 0)
                    submitted.fetch_add (1, std::memory_order_relaxed);
                else if (err == ENOENT)
                    enoent.fetch_add (1, std::memory_order_relaxed);
                else
                    other_failures.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&part);
            }
        });
    }

    while (ready.load (std::memory_order_acquire) != sender_count)
        std::this_thread::yield ();
    go.store (true, std::memory_order_release);
    for (std::vector<std::thread>::iterator it = senders.begin (); it != senders.end (); ++it)
        it->join ();
    senders_done.store (true, std::memory_order_release);
    receiver.join ();

    std::printf ("stable_router_route attempts=%d submitted=%d enoent=%d other_failures=%d "
                 "received=%d bad_records=%d\n",
                 total_sends, submitted.load (std::memory_order_relaxed),
                 enoent.load (std::memory_order_relaxed),
                 other_failures.load (std::memory_order_relaxed),
                 received.load (std::memory_order_relaxed),
                 bad_records.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (total_sends, submitted.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, enoent.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, other_failures.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (total_sends, received.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, bad_records.load (std::memory_order_relaxed));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_exact_target_is_invalid_after_same_rid_handover ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    routed_terminal_probe_t terminal;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_routed_terminal,
                                   &terminal));
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
                        sizeof (handover)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router, "R", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_a, "D", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_b, "D", 1));
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-submit-same-rid-handover"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_a, "inproc://routed-submit-same-rid-handover"));

    const zlink_routing_id_t dealer_rid = make_rid ("D");
    const zlink_routed_submit_target_t target_a =
      select_router_target_eventually (router, &dealer_rid);
    {
        std::lock_guard<std::mutex> lock (terminal.mutex);
        terminal.expected_pair_id = target_a.transport_pair_id;
        terminal.expected_pair_generation = target_a.transport_pair_generation;
        terminal.expected_rid = "D";
    }

    bool old_target_backpressured = false;
    for (int i = 0; i < 32; ++i) {
        const zlink_submit_result_t result =
          router_send_bytes (router, &target_a, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            old_target_backpressured = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE_MESSAGE (
      old_target_backpressured,
      "old exact target did not reach backpressure before handover");

    //  Reserve one record against the now-backpressured exact target. It
    //  cannot be admitted, so it is still pending when the same-RID handover
    //  supersedes that pair - which is exactly the failure this test wants to
    //  observe.
    zlink_msg_t pending_part;
    init_part (&pending_part, "parked-on-old-pair", 18);
    zlink_send_async_options_t pending_options;
    memset (&pending_options, 0, sizeof (pending_options));
    pending_options.struct_size = sizeof (pending_options);
    pending_options.target = &target_a;
    zlink_send_op_id_t pending_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_async (router, &pending_part, 1, &pending_options,
                        &pending_op));
    TEST_ASSERT_TRUE (pending_op != 0);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_b, "inproc://routed-submit-same-rid-handover"));
    zlink_routed_submit_target_t target_b;
    memset (&target_b, 0, sizeof (target_b));
    for (int i = 0; i < 3000; ++i) {
        target_b = select_router_target_eventually (router, &dealer_rid);
        if (target_b.transport_pair_id != target_a.transport_pair_id
            || target_b.transport_pair_generation
                 != target_a.transport_pair_generation)
            break;
        msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (
      target_b.transport_pair_id != target_a.transport_pair_id
        || target_b.transport_pair_generation
             != target_a.transport_pair_generation,
      "same-RID replacement did not publish a new exact target");
    // Hold a different complete-record admission across the exact FINAL call.
    // A complete FINAL must serialize on the socket sync and then evaluate the
    // stale target. Treating it as the start of an incremental multipart send
    // instead would fail immediately with transient EINVAL while the complete
    // scope is active.
    struct exact_final_result_t
    {
        exact_final_result_t () :
            init_rc (ZLINK_CONFIG_INTERNAL_ERROR),
            submit (ZLINK_SUBMIT_INTERNAL_ERROR),
            err (0),
            remaining (0),
            close_rc (ZLINK_CONFIG_INTERNAL_ERROR)
        {
        }

        int init_rc;
        zlink_submit_result_t submit;
        int err;
        size_t remaining;
        int close_rc;
    };

    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    std::unique_ptr<zlink::socket_public_send_scope_t> held_complete =
      router_handle.socket->begin_complete_send_scope (false);
    TEST_ASSERT_NOT_NULL (held_complete.get ());
    TEST_ASSERT_TRUE (held_complete->acquired ());

    const char stale_payload[] = "must-not-enter-old-pair";
    std::promise<void> about_to_call;
    std::future<void> call_ready = about_to_call.get_future ();
    std::promise<exact_final_result_t> finished;
    std::future<exact_final_result_t> result_future = finished.get_future ();
    std::thread contender ([&] {
        exact_final_result_t observed;
        zlink_msg_t part;
        observed.init_rc =
          zlink_msg_init_size (&part, sizeof (stale_payload) - 1);
        if (observed.init_rc == ZLINK_CONFIG_OK) {
            memcpy (zlink_msg_data (&part), stale_payload,
                    sizeof (stale_payload) - 1);
        }
        about_to_call.set_value ();
        if (observed.init_rc == ZLINK_CONFIG_OK) {
            errno = 0;
            observed.submit = zlink_send_part_transport_pair (
              router, &target_a.peer_rid, target_a.transport_pair_id,
              target_a.transport_pair_generation, &part,
              ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
            observed.err = zlink_errno ();
            observed.remaining = zlink_msg_size (&part);
            observed.close_rc = zlink_msg_close (&part);
        }
        finished.set_value (observed);
    });

    call_ready.wait ();
    const bool returned_while_complete_held =
      result_future.wait_for (std::chrono::milliseconds (750))
      == std::future_status::ready;
    held_complete.reset ();
    contender.join ();
    const exact_final_result_t exact_final = result_future.get ();
    router_handle = socket_handle_t ();

    TEST_ASSERT_FALSE_MESSAGE (
      returned_while_complete_held,
      "exact FINAL returned while another complete-record scope held socket sync");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, exact_final.init_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, exact_final.submit);
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, exact_final.err);
    TEST_ASSERT_EQUAL_UINT64 (0, exact_final.remaining);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, exact_final.close_rc);

    dealer_a = test_context_socket_close_zero_linger (dealer_a);
    {
        std::unique_lock<std::mutex> lock (terminal.mutex);
        TEST_ASSERT_TRUE_MESSAGE (
          terminal.changed.wait_for (
            lock, std::chrono::seconds (3),
            [&terminal] { return terminal.seen; }),
          "superseded exact target teardown did not report failure");
        TEST_ASSERT_EQUAL_UINT64 (target_a.transport_pair_id,
                                  terminal.pair_id);
        TEST_ASSERT_EQUAL_UINT64 (target_a.transport_pair_generation,
                                  terminal.pair_generation);
        //  Whether the admit attempt or pipe termination notices the
        //  superseded pair first decides the cause; both mean "this exact
        //  route is gone".
        TEST_ASSERT_TRUE (terminal.terminal_errno == ENOTCONN
                          || terminal.terminal_errno == EHOSTUNREACH);
    }

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      router_send_text (router, &target_b, "new-pair"));
    TEST_ASSERT_TRUE (recv_part_eventually (dealer_b, "new-pair"));

    test_context_socket_close_zero_linger (dealer_b);
    test_context_socket_close_zero_linger (router);
}

void test_router_exact_target_survives_unrelated_peer_churn ()
{
    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *churn_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router_a, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router_b, "B", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (churn_router, "C", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router_a, "inproc://routed-submit-churn-router-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router_b, "inproc://routed-submit-churn-router-b"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (router_a, "inproc://routed-submit-churn-router-b"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (router_b, "inproc://routed-submit-churn-router-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (churn_router, "inproc://routed-submit-peer-churn"));

    const zlink_routing_id_t router_b_rid = make_rid ("B");
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (router_a, &router_b_rid);

    for (int iteration = 0; iteration < 64; ++iteration) {
        void *churn_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (churn_dealer, "D", 1));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (churn_dealer,
                         "inproc://routed-submit-peer-churn"));

        const std::string attached =
          "exact-during-peer-attach-" + std::to_string (iteration);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          router_send_text (router_a, &target, attached.c_str ()));
        TEST_ASSERT_TRUE (
          recv_router_part_eventually (router_b, attached));

        churn_dealer =
          test_context_socket_close_zero_linger (churn_dealer);

        const std::string detached =
          "exact-during-peer-detach-" + std::to_string (iteration);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          router_send_text (router_a, &target, detached.c_str ()));
        TEST_ASSERT_TRUE (
          recv_router_part_eventually (router_b, detached));
    }

    test_context_socket_close_zero_linger (churn_router);
    test_context_socket_close_zero_linger (router_b);
    test_context_socket_close_zero_linger (router_a);
}

void test_dealer_exact_target_keeps_blocked_a_isolated_from_b ()
{
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    two_peer_fixture_t fixture = make_two_peer_fixture (
      "inproc://routed-submit-exact-a", "inproc://routed-submit-exact-b",
      hwm);

    bool a_backpressured = false;
    for (int i = 0; i < 16; ++i) {
        const zlink_submit_result_t result = dealer_send_bytes (
          fixture.dealer, &fixture.target_a, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            a_backpressured = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE_MESSAGE (a_backpressured,
                              "exact target A did not reach its HWM");

    zlink_routed_submit_target_t selected_a;
    zlink_routed_submit_target_t selected_b;
    select_dealer_targets_eventually (
      fixture.dealer, "A", "B", &selected_a, &selected_b);
    TEST_ASSERT_EQUAL_UINT64 (fixture.target_a.transport_pair_id,
                              selected_a.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (fixture.target_b.transport_pair_id,
                              selected_b.transport_pair_id);

    zlink_msg_t b_part;
    init_part (&b_part, "b-progress", 10);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_send_transport_pair_part (
        fixture.dealer, &fixture.target_b, &b_part,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (
      recv_router_part_eventually (fixture.router_b, "b-progress"));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      dealer_send_bytes (fixture.dealer, &fixture.target_a, 65536));
    TEST_ASSERT_TRUE_MESSAGE (
      recv_router_no_part (fixture.router_b, 50),
      "retry of exact target A was rerouted to target B");

    close_two_peer_fixture (&fixture);
}

void test_dealer_exact_multipart_failure_rolls_back_only_target_a ()
{
    const uint64_t hwm = 2048u;
    two_peer_fixture_t fixture = make_two_peer_fixture (
      "inproc://routed-submit-multipart-a",
      "inproc://routed-submit-multipart-b", hwm);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      dealer_send_bytes (fixture.dealer, &fixture.target_a, 1024));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      dealer_send_bytes (fixture.dealer, &fixture.target_a, 32,
                         ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      dealer_send_bytes (fixture.dealer, &fixture.target_a, 2048,
                         ZLINK_PART_FINAL));

    const std::string filler (1024, static_cast<char> (0x5a));
    TEST_ASSERT_TRUE (recv_router_part_eventually (fixture.router_a, filler));
    TEST_ASSERT_TRUE_MESSAGE (
      recv_router_no_part (fixture.router_a, 50),
      "failed multipart prefix became visible after rollback");

    zlink_msg_t b_part;
    init_part (&b_part, "after-rollback", 14);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_send_transport_pair_part (
        fixture.dealer, &fixture.target_b, &b_part,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (
      recv_router_part_eventually (fixture.router_b, "after-rollback"));

    close_two_peer_fixture (&fixture);
}

void test_dealer_exact_request_cleans_failed_pending_and_correlates_fast_reply ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router, "R", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D", 1));
    const int recv_timeout = 3000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVTIMEO, &recv_timeout,
                        sizeof (recv_timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-submit-exact-request"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://routed-submit-exact-request"));

    zlink_routed_submit_target_t target;
    zlink_routed_submit_target_t ignored;
    select_dealer_targets_eventually (dealer, "R", "R", &target, &ignored);

    zlink_routed_submit_target_t stale = target;
    ++stale.transport_pair_generation;
    reply_probe_t stale_probe;
    zlink_msg_t stale_request;
    init_part (&stale_request, "stale-request", 13);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_dealer_request_transport_pair_part (
        dealer, &stale, &stale_request, ZLINK_SEND_FLAGS_DONTWAIT,
        ZLINK_PART_FINAL, 3000, &capture_reply, &stale_probe));
    TEST_ASSERT_EQUAL_UINT64 (0, pending_request_count (dealer));
    {
        std::lock_guard<std::mutex> lock (stale_probe.mutex);
        TEST_ASSERT_EQUAL_INT (0, stale_probe.callback_count);
    }

    std::future<fast_reply_result_t> fast_reply = std::async (
      std::launch::async, [router] () {
          fast_reply_result_t result;
          const zlink_routing_id_t *source_rid = NULL;
          zlink_msg_t request;
          zlink_msg_init (&request);
          zlink_part_flag_t has_more = ZLINK_PART_FINAL;
          result.recv_result = zlink_router_recv_part_v2 (
            router, &source_rid, &result.request_seq, &result.pair_id,
            &result.pair_generation, &request, &has_more,
            ZLINK_RECV_FLAGS_NONE);
          if (result.recv_result == ZLINK_RECV_OK && source_rid
              && result.request_seq != 0 && has_more == ZLINK_PART_FINAL) {
              result.payload.assign (
                static_cast<const char *> (zlink_msg_data (&request)),
                zlink_msg_size (&request));
              const zlink_routing_id_t reply_rid = *source_rid;
              zlink_msg_t reply;
              const char reply_payload[] = "fast-reply";
              if (zlink_msg_init_size (&reply, sizeof (reply_payload) - 1)
                  == ZLINK_CONFIG_OK) {
                  memcpy (zlink_msg_data (&reply), reply_payload,
                          sizeof (reply_payload) - 1);
                  result.reply_result = zlink_router_reply_part (
                    router, &reply_rid, result.request_seq, &reply,
                    ZLINK_PART_FINAL);
              }
          }
          (void) zlink_msg_close (&request);
          return result;
      });

    reply_probe_t reply_probe;
    zlink_msg_t request;
    init_part (&request, "fast-request", 12);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_transport_pair_part (
        dealer, &target, &request, ZLINK_SEND_FLAGS_DONTWAIT,
        ZLINK_PART_FINAL, 3000, &capture_reply, &reply_probe));

    const fast_reply_result_t responder = fast_reply.get ();
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, responder.recv_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, responder.reply_result);
    TEST_ASSERT_EQUAL_STRING ("fast-request", responder.payload.c_str ());
    TEST_ASSERT_TRUE (responder.request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (target.transport_pair_id, responder.pair_id);
    TEST_ASSERT_EQUAL_UINT64 (target.transport_pair_generation,
                              responder.pair_generation);
    TEST_ASSERT_TRUE_MESSAGE (
      pending_request_count (dealer) <= 1,
      "one exact request installed more than one pending correlation");

    TEST_ASSERT_TRUE (progress_reply_until_done (dealer, &reply_probe, 3000));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("fast-reply", reply_probe.payload.c_str ());
    }
    TEST_ASSERT_EQUAL_UINT64 (0, pending_request_count (dealer));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

int main ()
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (
      test_router_selects_exact_target_and_rejects_stale_generation);
    RUN_TEST (test_stable_router_route_never_returns_enoent_under_concurrent_send);
    RUN_TEST (test_router_exact_target_is_invalid_after_same_rid_handover);
    RUN_TEST (
      test_router_exact_target_survives_unrelated_peer_churn);
    RUN_TEST (test_dealer_exact_target_keeps_blocked_a_isolated_from_b);
    RUN_TEST (test_dealer_exact_multipart_failure_rolls_back_only_target_a);
    RUN_TEST (
      test_dealer_exact_request_cleans_failed_pending_and_correlates_fast_reply);
    return UNITY_END ();
}
