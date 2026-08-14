/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
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
    RUN_TEST (test_dealer_exact_target_keeps_blocked_a_isolated_from_b);
    RUN_TEST (test_dealer_exact_multipart_failure_rolls_back_only_target_a);
    RUN_TEST (
      test_dealer_exact_request_cleans_failed_pending_and_correlates_fast_reply);
    return UNITY_END ();
}
