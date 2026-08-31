/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/object.hpp"
#include "core/pipe.hpp"
#include "sockets/router/router.hpp"

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

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static void attach_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->attach_pipe (pipe_);
    }

    static void install_socket_msg_handler (
      socket_base_t *socket_, zlink_socket_msg_handler_fn handler_,
      void *userdata_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        socket_->dispatch_runtime ().socket_msg_handler_userdata.store (
          userdata_, std::memory_order_release);
        socket_->dispatch_runtime ().socket_msg_handler.store (
          handler_, std::memory_order_release);
    }

    static void clear_socket_msg_handler (socket_base_t *socket_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        socket_->dispatch_runtime ().socket_msg_handler.store (
          NULL, std::memory_order_release);
        socket_->dispatch_runtime ().socket_msg_handler_userdata.store (
          NULL, std::memory_order_release);
    }
};
}

namespace
{
class passive_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
};

struct target_failure_pause_t
{
    target_failure_pause_t () : entered (false), release (false) {}

    std::mutex mutex;
    std::condition_variable changed;
    bool entered;
    bool release;
};

void pause_target_failure_after_one_record (void *userdata_)
{
    target_failure_pause_t *pause =
      static_cast<target_failure_pause_t *> (userdata_);
    if (!pause)
        return;
    std::unique_lock<std::mutex> lock (pause->mutex);
    pause->entered = true;
    pause->changed.notify_all ();
    pause->changed.wait (lock, [pause] { return pause->release; });
}

struct async_completion_observation_t
{
    void *tag;
    zlink_send_complete_result_t result;
    int terminal_errno;
};

struct async_completion_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<async_completion_observation_t> observations;
};

void capture_async_completion (void *,
                               const zlink_send_complete_event_t *event_,
                               void *userdata_)
{
    async_completion_probe_t *probe =
      static_cast<async_completion_probe_t *> (userdata_);
    if (!probe || !event_)
        return;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        const async_completion_observation_t observation = {
          event_->userdata, event_->result, event_->terminal_errno};
        probe->observations.push_back (observation);
    }
    probe->changed.notify_all ();
}

bool wait_for_async_completion (async_completion_probe_t *probe_, void *tag_,
                                zlink_send_complete_result_t result_,
                                int terminal_errno_, int timeout_ms_ = 3000)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->changed.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_), [&] {
          for (std::vector<async_completion_observation_t>::const_iterator it =
                 probe_->observations.begin ();
               it != probe_->observations.end (); ++it) {
              if (it->tag == tag_ && it->result == result_
                  && it->terminal_errno == terminal_errno_)
                  return true;
          }
          return false;
      });
}

bool has_async_completion (async_completion_probe_t *probe_, void *tag_)
{
    std::lock_guard<std::mutex> lock (probe_->mutex);
    for (std::vector<async_completion_observation_t>::const_iterator it =
           probe_->observations.begin ();
         it != probe_->observations.end (); ++it) {
        if (it->tag == tag_)
            return true;
    }
    return false;
}

void discard_socket_msg_record (const zlink_routing_id_t *,
                                zlink_msg_t *parts_, size_t part_count_, void *)
{
    for (size_t i = 0; i < part_count_; ++i)
        (void) zlink_msg_close (&parts_[i]);
}

void init_routing_id_frame (zlink::msg_t *msg_, const char *routing_id_)
{
    TEST_ASSERT_SUCCESS_ERRNO (msg_->init_size (strlen (routing_id_)));
    memcpy (msg_->data (), routing_id_, strlen (routing_id_));
    msg_->set_flags (zlink::msg_t::routing_id);
}

void fill_pipe_once (zlink::pipe_t *pipe_, const char *payload_)
{
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (strlen (payload_)));
    memcpy (msg.data (), payload_, strlen (payload_));
    zlink::pipe_message_admission_t admission =
      zlink::pipe_message_admission_invalid;
    TEST_ASSERT_TRUE (
      pipe_->write_single_message_and_flush_no_recursive_hwm_check (
        &msg, &admission));
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_ready, admission);
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

std::string read_pipe_record (zlink::pipe_t *pipe_)
{
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init ());
    TEST_ASSERT_TRUE (pipe_->read (&msg));
    const std::string payload (static_cast<const char *> (msg.data ()),
                               msg.size ());
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
    return payload;
}

struct retained_test_pipe_pair_t
{
    retained_test_pipe_pair_t ()
    {
        pipes[0] = NULL;
        pipes[1] = NULL;
    }
    zlink::pipe_t *pipes[2];
};

retained_test_pipe_pair_t make_unpaired_router_pipe (
  zlink::router_t *router_, passive_pipe_sink_t *peer_sink_, uint64_t hwm_)
{
    retained_test_pipe_pair_t pair;
    zlink::object_t *parents[2] = {router_, router_};
    const uint64_t hwms[2] = {hwm_, hwm_};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pair.pipes, hwms, conflates));
    TEST_ASSERT_TRUE (pair.pipes[0]->retain_lifetime_ref ());
    TEST_ASSERT_TRUE (pair.pipes[1]->retain_lifetime_ref ());
    pair.pipes[1]->set_event_sink (peer_sink_);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      router_, pair.pipes[0]);
    return pair;
}

void terminate_test_pipe_pair (retained_test_pipe_pair_t *pair_)
{
    for (size_t i = 0; i < 2; ++i) {
        if (!pair_->pipes[i])
            continue;
        if (!pair_->pipes[i]->has_completed_termination ())
            pair_->pipes[i]->terminate (false);
    }
}

bool test_pipe_pair_terminated (const retained_test_pipe_pair_t &pair_)
{
    return pair_.pipes[0] && pair_.pipes[1]
           && pair_.pipes[0]->has_completed_termination ()
           && pair_.pipes[1]->has_completed_termination ();
}

void release_test_pipe_pair (retained_test_pipe_pair_t *pair_)
{
    for (size_t i = 0; i < 2; ++i) {
        if (!pair_->pipes[i])
            continue;
        pair_->pipes[i]->release_lifetime_ref ();
        pair_->pipes[i] = NULL;
    }
}

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

    // Handover keeps the displaced physical pipe alive as standby, but its
    // old public exact target is already terminal. Route adoption must wake
    // and fail that pending operation without waiting for pipe teardown or
    // its deadline.
    {
        std::unique_lock<std::mutex> lock (terminal.mutex);
        TEST_ASSERT_TRUE_MESSAGE (
          terminal.changed.wait_for (
            lock, std::chrono::seconds (3),
            [&terminal] { return terminal.seen; }),
          "same-RID handover did not promptly fail the displaced exact target");
        TEST_ASSERT_EQUAL_UINT64 (target_a.transport_pair_id,
                                  terminal.pair_id);
        TEST_ASSERT_EQUAL_UINT64 (target_a.transport_pair_generation,
                                  terminal.pair_generation);
        TEST_ASSERT_EQUAL_INT (ENOTCONN, terminal.terminal_errno);
    }

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

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      router_send_text (router, &target_b, "new-pair"));
    TEST_ASSERT_TRUE (recv_part_eventually (dealer_b, "new-pair"));

    test_context_socket_close_zero_linger (dealer_b);
    test_context_socket_close_zero_linger (router);
}

void test_unpaired_router_incarnation_survives_reconnect_and_handover ()
{
    void *router_handle = test_context_socket (ZLINK_SOCKET_ROUTER);
    socket_handle_t router_pin = as_socket_handle (router_handle);
    TEST_ASSERT_NOT_NULL (router_pin.socket);
    zlink::router_t *const router =
      static_cast<zlink::router_t *> (router_pin.socket);

    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_handle, ZLINK_OPT_RID_DUPLICATE_POLICY,
                        &handover, sizeof (handover)));
    const int mandatory = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router_handle, ZLINK_ROUTER_OPT_MANDATORY,
                               &mandatory, sizeof (mandatory)));
    async_completion_probe_t completions;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router_handle, &capture_async_completion,
                                   &completions));
    zlink::session_termination_test_access_t::install_socket_msg_handler (
      router, &discard_socket_msg_record, NULL);

    const char fill_payload[] = "12345678";
    const uint64_t hwm = sizeof (zlink::msg_t) + sizeof (fill_payload) - 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_handle, ZLINK_OPT_SNDHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router_handle, ZLINK_OPT_RCVHWM, &hwm,
                        sizeof (hwm)));
    passive_pipe_sink_t peer_a_sink;
    passive_pipe_sink_t peer_b_sink;
    passive_pipe_sink_t peer_c_sink;
    retained_test_pipe_pair_t pair_a =
      make_unpaired_router_pipe (router, &peer_a_sink, hwm);
    retained_test_pipe_pair_t pair_b =
      make_unpaired_router_pipe (router, &peer_b_sink, hwm);
    const uint64_t route_incarnation_a =
      pair_a.pipes[0]->get_route_incarnation_id ();
    const uint64_t route_incarnation_b =
      pair_b.pipes[0]->get_route_incarnation_id ();
    TEST_ASSERT_NOT_EQUAL (0, route_incarnation_a);
    TEST_ASSERT_NOT_EQUAL (0, route_incarnation_b);
    TEST_ASSERT_NOT_EQUAL (route_incarnation_a, route_incarnation_b);

    zlink::msg_t identity_a;
    init_routing_id_frame (&identity_a, "D");
    TEST_ASSERT_EQUAL_INT (
      1, router->socket_msg_dispatch_from_io (&identity_a, pair_a.pipes[0]));
    TEST_ASSERT_SUCCESS_ERRNO (identity_a.close ());
    fill_pipe_once (pair_a.pipes[0], fill_payload);
    fill_pipe_once (pair_b.pipes[0], fill_payload);
    TEST_ASSERT_EQUAL_INT (
      zlink::pipe_message_admission_hwm_full,
      pair_a.pipes[0]->check_write_admission ());
    TEST_ASSERT_EQUAL_INT (
      zlink::pipe_message_admission_hwm_full,
      pair_b.pipes[0]->check_write_admission ());

    zlink_routed_submit_target_t rid_only;
    memset (&rid_only, 0, sizeof (rid_only));
    rid_only.peer_rid = make_rid ("D");
    int same_pipe_tag = 0;
    int published_old_tag = 0;
    int late_old_tag = 0;
    int queued_old_tag = 0;
    int new_tag = 0;

    // A reconnect mutates the network generation on the same physical pipe.
    // Pending FIFO ownership follows the immutable route incarnation instead,
    // so resetting C1 to zero and publishing C2 must not stale-reject it.
    zlink_msg_t same_pipe_part;
    init_part (&same_pipe_part, "same-C2!", 8);
    zlink_send_async_options_t same_pipe_options;
    memset (&same_pipe_options, 0, sizeof (same_pipe_options));
    same_pipe_options.struct_size = sizeof (same_pipe_options);
    same_pipe_options.userdata = &same_pipe_tag;
    same_pipe_options.target = &rid_only;
    zlink_send_op_id_t same_pipe_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_async (router_handle, &same_pipe_part, 1,
                        &same_pipe_options, &same_pipe_op));
    TEST_ASSERT_TRUE (same_pipe_op != 0);
    const uint64_t original_connection_id =
      pair_a.pipes[0]->get_transport_connection_id ();
    const uint64_t reconnect_connection_id = zlink::allocate_connection_id ();
    TEST_ASSERT_NOT_EQUAL (0, original_connection_id);
    TEST_ASSERT_NOT_EQUAL (0, reconnect_connection_id);
    TEST_ASSERT_NOT_EQUAL (original_connection_id, reconnect_connection_id);
    pair_a.pipes[0]->set_transport_connection_id (0);
    pair_a.pipes[0]->set_transport_connection_id (reconnect_connection_id);
    TEST_ASSERT_EQUAL_UINT64 (
      route_incarnation_a,
      pair_a.pipes[0]->get_route_incarnation_id ());
    TEST_ASSERT_EQUAL_STRING (fill_payload,
                              read_pipe_record (pair_a.pipes[1]).c_str ());
    TEST_ASSERT_TRUE (wait_for_async_completion (
      &completions, &same_pipe_tag, ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_EQUAL_STRING ("same-C2!",
                              read_pipe_record (pair_a.pipes[1]).c_str ());
    fill_pipe_once (pair_a.pipes[0], fill_payload);
    TEST_ASSERT_EQUAL_INT (
      zlink::pipe_message_admission_hwm_full,
      pair_a.pipes[0]->check_write_admission ());

    // Window 1: the old physical attempt has observed EAGAIN, but has not yet
    // published its pending record. Handover must be allowed to finish its
    // empty failure scan; the later publication then validates under the route
    // fence and resolves terminal instead of becoming permanently stranded.
    target_failure_pause_t inline_pause;
    router->test_set_send_inline_fallback_hook (
      &pause_target_failure_after_one_record, &inline_pause);
    struct submit_observation_t
    {
        submit_observation_t () :
            init_rc (ZLINK_CONFIG_INTERNAL_ERROR),
            result (ZLINK_SUBMIT_INTERNAL_ERROR),
            err (0),
            op_id (0)
        {
        }
        int init_rc;
        zlink_submit_result_t result;
        int err;
        zlink_send_op_id_t op_id;
    };
    std::promise<submit_observation_t> late_submit_promise;
    std::future<submit_observation_t> late_submit =
      late_submit_promise.get_future ();
    submit_observation_t published_old_result;
    std::thread late_submitter ([&] {
        submit_observation_t observed;
        zlink_msg_t part;
        observed.init_rc = zlink_msg_init_size (&part, 8);
        if (observed.init_rc == ZLINK_CONFIG_OK) {
            memcpy (zlink_msg_data (&part), "late-old", 8);
            zlink_send_async_options_t options;
            memset (&options, 0, sizeof (options));
            options.struct_size = sizeof (options);
            options.userdata = &late_old_tag;
            options.target = &rid_only;
            observed.result = zlink_send_async (
              router_handle, &part, 1, &options, &observed.op_id);
            observed.err = zlink_errno ();
            if (observed.result != ZLINK_SUBMIT_OK)
                (void) zlink_msg_close (&part);
        }
        late_submit_promise.set_value (observed);
    });

    bool inline_entered = false;
    {
        std::unique_lock<std::mutex> lock (inline_pause.mutex);
        inline_entered = inline_pause.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&inline_pause] { return inline_pause.entered; });
    }
    int identity_b_rc = -1;
    if (inline_entered) {
        // Publish a second A operation while the first one is paused before
        // map insertion. This record is the original reset regression: it is
        // already keyed by A's incarnation when the mutable C1 becomes zero.
        zlink_msg_t published_old_part;
        published_old_result.init_rc =
          zlink_msg_init_size (&published_old_part, 8);
        if (published_old_result.init_rc == ZLINK_CONFIG_OK) {
            memcpy (zlink_msg_data (&published_old_part), "map-old!", 8);
            zlink_send_async_options_t published_old_options;
            memset (&published_old_options, 0,
                    sizeof (published_old_options));
            published_old_options.struct_size =
              sizeof (published_old_options);
            published_old_options.userdata = &published_old_tag;
            published_old_options.target = &rid_only;
            published_old_result.result = zlink_send_async (
              router_handle, &published_old_part, 1,
              &published_old_options, &published_old_result.op_id);
            published_old_result.err = zlink_errno ();
            if (published_old_result.result != ZLINK_SUBMIT_OK)
                (void) zlink_msg_close (&published_old_part);
        }
        // engine_error resets the mutable connection generation before route
        // replacement. The old pending key must still be found by A's stable
        // route incarnation and fail promptly rather than waiting forever.
        pair_a.pipes[0]->set_transport_connection_id (0);
        zlink::msg_t identity_b;
        if (identity_b.init_size (1) == 0) {
            memcpy (identity_b.data (), "D", 1);
            identity_b.set_flags (zlink::msg_t::routing_id);
            identity_b_rc = router->socket_msg_dispatch_from_io (
              &identity_b, pair_b.pipes[0]);
            (void) identity_b.close ();
        }
    }
    {
        std::lock_guard<std::mutex> lock (inline_pause.mutex);
        inline_pause.release = true;
    }
    inline_pause.changed.notify_all ();
    late_submitter.join ();
    router->test_set_send_inline_fallback_hook (NULL, NULL);
    const submit_observation_t late_result = late_submit.get ();
    TEST_ASSERT_TRUE_MESSAGE (
      inline_entered, "old inline fallback did not reach the publication gap");
    TEST_ASSERT_EQUAL_INT (1, identity_b_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, late_result.init_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, late_result.result);
    TEST_ASSERT_TRUE (late_result.op_id != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, published_old_result.init_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, published_old_result.result);
    TEST_ASSERT_TRUE (published_old_result.op_id != 0);
    TEST_ASSERT_TRUE (wait_for_async_completion (
      &completions, &published_old_tag, ZLINK_SEND_TERMINAL, ENOTCONN));
    TEST_ASSERT_TRUE (wait_for_async_completion (
      &completions, &late_old_tag, ZLINK_SEND_TERMINAL, ENOTCONN));

    // Park one normally published record on B. C then replaces the same public
    // RID. The failure loop pauses after removing B's record so a new C record
    // can publish in the exact inter-iteration ABA window.
    zlink_msg_t queued_old_part;
    init_part (&queued_old_part, "queue-old", 8);
    zlink_send_async_options_t queued_old_options;
    memset (&queued_old_options, 0, sizeof (queued_old_options));
    queued_old_options.struct_size = sizeof (queued_old_options);
    queued_old_options.userdata = &queued_old_tag;
    queued_old_options.target = &rid_only;
    zlink_send_op_id_t queued_old_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_async (router_handle, &queued_old_part, 1,
                        &queued_old_options, &queued_old_op));
    TEST_ASSERT_TRUE (queued_old_op != 0);

    retained_test_pipe_pair_t pair_c =
      make_unpaired_router_pipe (router, &peer_c_sink, hwm);
    TEST_ASSERT_NOT_EQUAL (
      route_incarnation_a,
      pair_c.pipes[0]->get_route_incarnation_id ());
    TEST_ASSERT_NOT_EQUAL (
      route_incarnation_b,
      pair_c.pipes[0]->get_route_incarnation_id ());
    fill_pipe_once (pair_c.pipes[0], fill_payload);
    target_failure_pause_t failure_pause;
    router->test_set_send_target_failure_progress_hook (
      &pause_target_failure_after_one_record, &failure_pause);
    std::atomic<int> identity_c_init_rc (-1);
    std::atomic<int> identity_c_dispatch_rc (-1);
    std::thread handover_thread ([&] {
        zlink::msg_t identity_c;
        const int init_rc = identity_c.init_size (1);
        identity_c_init_rc.store (init_rc, std::memory_order_release);
        if (init_rc == 0) {
            memcpy (identity_c.data (), "D", 1);
            identity_c.set_flags (zlink::msg_t::routing_id);
            identity_c_dispatch_rc.store (
              router->socket_msg_dispatch_from_io (
                &identity_c, pair_c.pipes[0]),
              std::memory_order_release);
            (void) identity_c.close ();
        }
    });

    bool failure_entered = false;
    {
        std::unique_lock<std::mutex> lock (failure_pause.mutex);
        failure_entered = failure_pause.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&failure_pause] { return failure_pause.entered; });
    }

    zlink_submit_result_t new_submit_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_send_op_id_t new_op = 0;
    if (failure_entered) {
        zlink_msg_t new_part;
        const int init_rc = zlink_msg_init_size (&new_part, 8);
        if (init_rc == ZLINK_CONFIG_OK) {
            memcpy (zlink_msg_data (&new_part), "new-live", 8);
            zlink_send_async_options_t new_options;
            memset (&new_options, 0, sizeof (new_options));
            new_options.struct_size = sizeof (new_options);
            new_options.userdata = &new_tag;
            new_options.target = &rid_only;
            new_submit_result = zlink_send_async (
              router_handle, &new_part, 1, &new_options, &new_op);
            if (new_submit_result != ZLINK_SUBMIT_OK)
                (void) zlink_msg_close (&new_part);
        }
    }
    {
        std::lock_guard<std::mutex> lock (failure_pause.mutex);
        failure_pause.release = true;
    }
    failure_pause.changed.notify_all ();
    handover_thread.join ();
    router->test_set_send_target_failure_progress_hook (NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (
      failure_entered, "handover failure loop did not reach the ABA gap");
    TEST_ASSERT_EQUAL_INT (
      0, identity_c_init_rc.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      1, identity_c_dispatch_rc.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      ZLINK_SUBMIT_OK, new_submit_result,
      "new physical route submit was rejected in the handover gap");
    TEST_ASSERT_TRUE (new_op != 0);
    TEST_ASSERT_TRUE (wait_for_async_completion (
      &completions, &queued_old_tag, ZLINK_SEND_TERMINAL, ENOTCONN));
    msleep (50);
    TEST_ASSERT_FALSE_MESSAGE (
      has_async_completion (&completions, &new_tag),
      "old {RID,0,0} failure scan consumed replacement-route pending work");

    TEST_ASSERT_EQUAL_STRING (fill_payload,
                              read_pipe_record (pair_c.pipes[1]).c_str ());
    TEST_ASSERT_TRUE (wait_for_async_completion (
      &completions, &new_tag, ZLINK_SEND_ADMITTED, 0));
    TEST_ASSERT_EQUAL_STRING ("new-live",
                              read_pipe_record (pair_c.pipes[1]).c_str ());

    zlink::session_termination_test_access_t::clear_socket_msg_handler (router);
    terminate_test_pipe_pair (&pair_c);
    terminate_test_pipe_pair (&pair_b);
    terminate_test_pipe_pair (&pair_a);
    TEST_ASSERT_TRUE (zlink_test_wait_until (5000, [&] {
        return test_pipe_pair_terminated (pair_a)
               && test_pipe_pair_terminated (pair_b)
               && test_pipe_pair_terminated (pair_c);
    }));
    release_test_pipe_pair (&pair_c);
    release_test_pipe_pair (&pair_b);
    release_test_pipe_pair (&pair_a);
    router_pin = socket_handle_t ();
    test_context_socket_close_zero_linger (router_handle);
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
      test_unpaired_router_incarnation_survives_reconnect_and_handover);
    RUN_TEST (
      test_router_exact_target_survives_unrelated_peer_churn);
    RUN_TEST (test_dealer_exact_target_keeps_blocked_a_isolated_from_b);
    RUN_TEST (test_dealer_exact_multipart_failure_rolls_back_only_target_a);
    RUN_TEST (
      test_dealer_exact_request_cleans_failed_pending_and_correlates_fast_reply);
    return UNITY_END ();
}
