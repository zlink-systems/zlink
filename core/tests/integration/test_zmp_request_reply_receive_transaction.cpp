/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/command.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

    static int process_socket_commands (socket_base_t *socket_)
    {
        return socket_->process_commands (0, false);
    }
};
}

namespace
{
struct receive_record_gate_t
{
    receive_record_gate_t () : acquired (false), contended (false), release (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool acquired;
    bool contended;
    bool release;
};

struct receive_result_t
{
    receive_result_t () : rc (-1), errnum (0), message_type (0), sequence (0) {}

    int rc;
    int errnum;
    uint8_t message_type;
    uint64_t sequence;
    std::vector<std::string> parts;
};

struct command_sync_probe_t
{
    command_sync_probe_t () : observed (false), sync_was_busy (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool observed;
    bool sync_was_busy;
};

class passive_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
};

void pause_after_first_record_frame (void *userdata_)
{
    receive_record_gate_t *gate =
      static_cast<receive_record_gate_t *> (userdata_);
    if (!gate)
        return;

    std::unique_lock<std::mutex> lock (gate->mutex);
    if (gate->acquired)
        return;
    gate->acquired = true;
    gate->cv.notify_all ();
    gate->cv.wait (lock, [gate] { return gate->release; });
}

void observe_record_contention (void *userdata_)
{
    receive_record_gate_t *gate =
      static_cast<receive_record_gate_t *> (userdata_);
    if (!gate)
        return;

    {
        std::lock_guard<std::mutex> lock (gate->mutex);
        gate->contended = true;
    }
    gate->cv.notify_all ();
}

void observe_command_sync_probe (void *userdata_, int command_type_,
                                 bool sync_was_busy_)
{
    command_sync_probe_t *probe =
      static_cast<command_sync_probe_t *> (userdata_);
    if (!probe
        || command_type_ != static_cast<int> (zlink::command_t::activate_read))
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->observed = true;
        probe->sync_was_busy = sync_was_busy_;
    }
    probe->cv.notify_all ();
}

bool wait_for_gate_flag (receive_record_gate_t *gate_, bool wait_for_contention_)
{
    std::unique_lock<std::mutex> lock (gate_->mutex);
    return gate_->cv.wait_for (
      lock, std::chrono::seconds (3), [gate_, wait_for_contention_] {
          return wait_for_contention_ ? gate_->contended : gate_->acquired;
      });
}

bool wait_for_command_sync_probe (command_sync_probe_t *probe_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::seconds (3),
      [probe_] { return probe_->observed; });
}

void release_gate (receive_record_gate_t *gate_)
{
    {
        std::lock_guard<std::mutex> lock (gate_->mutex);
        gate_->release = true;
    }
    gate_->cv.notify_all ();
}

void init_payload (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

void send_request_record (void *sender_, uint64_t sequence_,
                          const char *prefix_)
{
    for (size_t i = 0; i < 2; ++i) {
        const std::string payload =
          std::string (prefix_) + "-" + static_cast<char> ('0' + i);
        zlink_msg_t part;
        init_payload (&part, payload);
        if (i == 0) {
            TEST_ASSERT_SUCCESS_ERRNO (
              reinterpret_cast<zlink::msg_t *> (&part)
                ->set_request_reply_metadata (
                  zlink::request_reply::request_type, sequence_));
        }
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender_, &part, ZLINK_SEND_FLAGS_NONE,
                           i == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }
}

void write_internal_router_identity (zlink::pipe_t *pipe_,
                                     const char *routing_id_)
{
    zlink::msg_t identity;
    const size_t size = std::strlen (routing_id_);
    TEST_ASSERT_SUCCESS_ERRNO (identity.init_size (size));
    memcpy (identity.data (), routing_id_, size);
    identity.set_flags (zlink::msg_t::routing_id);
    TEST_ASSERT_TRUE (pipe_->write_and_flush (&identity));
    TEST_ASSERT_SUCCESS_ERRNO (identity.close ());
}

void write_internal_request_record (zlink::pipe_t *pipe_, uint64_t sequence_,
                                    const char *prefix_)
{
    for (size_t i = 0; i < 2; ++i) {
        const std::string payload =
          std::string (prefix_) + "-" + static_cast<char> ('0' + i);
        zlink::msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (part.init_size (payload.size ()));
        memcpy (part.data (), payload.data (), payload.size ());
        if (i == 0) {
            part.set_flags (zlink::msg_t::more);
            TEST_ASSERT_SUCCESS_ERRNO (
              part.set_request_reply_metadata (
                zlink::request_reply::request_type, sequence_));
            TEST_ASSERT_TRUE (pipe_->write (&part));
        } else {
            TEST_ASSERT_TRUE (pipe_->write_and_flush (&part));
        }
        TEST_ASSERT_SUCCESS_ERRNO (part.close ());
    }
}

void capture_parts (zlink_msg_t *parts_, size_t part_count_,
                    receive_result_t *result_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < part_count_; ++i) {
        result_->parts.push_back (
          std::string (static_cast<const char *> (zlink_msg_data (&parts_[i])),
                       zlink_msg_size (&parts_[i])));
    }
    zlink_multipart_close (parts_, part_count_);
}

void receive_router_record (const socket_handle_t &handle_,
                            receive_result_t *result_)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    result_->rc = zlink::socket_reqrep_internal::recv_router_message_direct (
      handle_, &source_rid, &result_->sequence, &parts, &part_count,
      ZLINK_DONTWAIT);
    result_->errnum = result_->rc == 0 ? 0 : errno;
    if (result_->rc == 0)
        capture_parts (parts, part_count, result_);
}

void receive_dealer_record (const socket_handle_t &handle_, bool typed_,
                            receive_result_t *result_)
{
    std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      zlink::socket_reqrep_internal::find_request_reply_state (handle_);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    result_->rc = zlink::socket_reqrep_internal::recv_dealer_message_direct (
      handle_, state, typed_, &result_->message_type, &result_->sequence,
      &parts, &part_count, ZLINK_DONTWAIT);
    result_->errnum = result_->rc == 0 ? 0 : errno;
    if (result_->rc == 0)
        capture_parts (parts, part_count, result_);
}

void assert_two_part_record (const receive_result_t &result_,
                             const char *prefix_)
{
    TEST_ASSERT_EQUAL_INT (0, result_.rc);
    TEST_ASSERT_EQUAL_INT (0, result_.errnum);
    TEST_ASSERT_EQUAL_UINT64 (2, result_.parts.size ());
    const std::string expected_first = std::string (prefix_) + "-0";
    const std::string expected_second = std::string (prefix_) + "-1";
    TEST_ASSERT_EQUAL_STRING (expected_first.c_str (), result_.parts[0].c_str ());
    TEST_ASSERT_EQUAL_STRING (expected_second.c_str (), result_.parts[1].c_str ());
}

void run_two_reader_record_test (int receiver_type_)
{
    void *receiver = test_context_socket (receiver_type_);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_NOT_NULL (sender);

    const char *endpoint = receiver_type_ == ZLINK_SOCKET_ROUTER
                             ? "inproc://router-record-receive-transaction"
                             : "inproc://dealer-record-receive-transaction";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
    msleep (SETTLE_TIME);
    send_request_record (sender, 101, "first");
    send_request_record (sender, 102, "second");

    const socket_handle_t handle = as_socket_handle (receiver);
    receive_record_gate_t gate;
    handle.socket->test_set_receive_record_hooks (
      &pause_after_first_record_frame, &observe_record_contention, &gate);

    receive_result_t first;
    receive_result_t second;
    std::thread first_reader ([&] {
        if (receiver_type_ == ZLINK_SOCKET_ROUTER)
            receive_router_record (handle, &first);
        else
            receive_dealer_record (handle, true, &first);
    });

    const bool acquired = wait_for_gate_flag (&gate, false);
    std::thread second_reader;
    if (acquired) {
        second_reader = std::thread ([&] {
            if (receiver_type_ == ZLINK_SOCKET_ROUTER)
                receive_router_record (handle, &second);
            else
                // Force the typed-vs-raw reader boundary on DEALER.
                receive_dealer_record (handle, false, &second);
        });
    }
    const bool contended =
      acquired && wait_for_gate_flag (&gate, true);
    release_gate (&gate);
    first_reader.join ();
    if (second_reader.joinable ())
        second_reader.join ();
    handle.socket->test_set_receive_record_hooks (NULL, NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (acquired,
                              "first reader did not acquire record scope");
    TEST_ASSERT_TRUE_MESSAGE (contended,
                              "second reader did not contend on record scope");
    assert_two_part_record (first, "first");
    assert_two_part_record (second, "second");
    if (receiver_type_ == ZLINK_SOCKET_ROUTER) {
        TEST_ASSERT_EQUAL_UINT64 (101, first.sequence);
        TEST_ASSERT_EQUAL_UINT64 (102, second.sequence);
    } else {
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST,
                                 first.message_type);
        TEST_ASSERT_TRUE (first.sequence != 0);
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW,
                                 second.message_type);
        TEST_ASSERT_EQUAL_UINT64 (0, second.sequence);
    }

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_router_and_dealer_hold_whole_record_receive_transaction ()
{
    run_two_reader_record_test (ZLINK_SOCKET_ROUTER);
    run_two_reader_record_test (ZLINK_SOCKET_DEALER);
}

void test_router_record_fences_mailbox_read_activation ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    const socket_handle_t handle = as_socket_handle (router);

    zlink::object_t *parents[2] = {handle.socket, handle.socket};
    zlink::pipe_t *first_pipe[2] = {NULL, NULL};
    zlink::pipe_t *activated_pipe[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, activated_pipe, hwms, conflates, true));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, first_pipe, hwms, conflates, true));

    passive_pipe_sink_t activated_peer_sink;
    passive_pipe_sink_t first_peer_sink;
    activated_pipe[1]->set_event_sink (&activated_peer_sink);
    first_pipe[1]->set_event_sink (&first_peer_sink);

    // Attach the future activation source first with only its routing id. It
    // is empty when the first receive scans it, so the later write must send a
    // real activate_read command through the socket mailbox.
    write_internal_router_identity (activated_pipe[1], "activation-peer");
    zlink::session_termination_test_access_t::attach_socket_pipe (
      handle.socket, activated_pipe[0]);

    write_internal_router_identity (first_pipe[1], "first-peer");
    write_internal_request_record (first_pipe[1], 301, "command-first");
    zlink::session_termination_test_access_t::attach_socket_pipe (
      handle.socket, first_pipe[0]);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::session_termination_test_access_t::process_socket_commands (
        handle.socket));

    receive_record_gate_t record_gate;
    handle.socket->test_set_receive_record_hooks (
      &pause_after_first_record_frame, NULL, &record_gate);
    receive_result_t first;
    std::thread first_reader (
      [&] { receive_router_record (handle, &first); });

    const bool acquired = wait_for_gate_flag (&record_gate, false);
    command_sync_probe_t command_probe;
    std::atomic<bool> command_done (false);
    int command_rc = -1;
    int command_errno = 0;
    std::thread command_thread;
    if (acquired) {
        handle.socket->test_set_receive_command_sync_probe_hook (
          &observe_command_sync_probe, &command_probe);
        write_internal_request_record (
          activated_pipe[1], 302, "command-second");
        command_thread = std::thread ([&] {
            command_rc =
              zlink::session_termination_test_access_t::process_socket_commands (
                handle.socket);
            command_errno = command_rc == 0 ? 0 : errno;
            command_done.store (true, std::memory_order_release);
        });
    }

    const bool command_probed =
      acquired && wait_for_command_sync_probe (&command_probe);
    const bool command_completed_while_record_open =
      command_done.load (std::memory_order_acquire);
    bool command_sync_was_busy = false;
    if (command_probed) {
        std::lock_guard<std::mutex> lock (command_probe.mutex);
        command_sync_was_busy = command_probe.sync_was_busy;
    }

    release_gate (&record_gate);
    first_reader.join ();
    if (command_thread.joinable ())
        command_thread.join ();
    handle.socket->test_set_receive_command_sync_probe_hook (NULL, NULL);
    handle.socket->test_set_receive_record_hooks (NULL, NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (acquired,
                              "record reader did not acquire receive scope");
    TEST_ASSERT_TRUE_MESSAGE (
      command_probed,
      "activate_read command did not reach the receive sync boundary");
    TEST_ASSERT_TRUE_MESSAGE (
      command_sync_was_busy,
      "activate_read command did not wait for the open receive record");
    TEST_ASSERT_FALSE_MESSAGE (
      command_completed_while_record_open,
      "activate_read command mutated receive state before record completion");
    TEST_ASSERT_EQUAL_INT (0, command_rc);
    TEST_ASSERT_EQUAL_INT (0, command_errno);
    assert_two_part_record (first, "command-first");
    TEST_ASSERT_EQUAL_UINT64 (301, first.sequence);

    receive_result_t activated;
    receive_router_record (handle, &activated);
    assert_two_part_record (activated, "command-second");
    TEST_ASSERT_EQUAL_UINT64 (302, activated.sequence);

    test_context_socket_close_zero_linger (router);
}

void test_router_capacity_reservation_is_atomic_and_non_consuming ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (sender);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://router-record-capacity-reservation"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://router-record-capacity-reservation"));
    msleep (SETTLE_TIME);

    const socket_handle_t handle = as_socket_handle (router);
    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      zlink::socket_reqrep_internal::find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->reply_target_slots =
          zlink::socket_reqrep_internal::max_reply_target_slots - 1;
    }

    send_request_record (sender, 201, "capacity-first");
    send_request_record (sender, 202, "capacity-second");

    receive_record_gate_t gate;
    handle.socket->test_set_receive_record_hooks (
      &pause_after_first_record_frame, &observe_record_contention, &gate);
    receive_result_t first;
    receive_result_t rejected;
    std::thread first_reader ([&] { receive_router_record (handle, &first); });
    const bool acquired = wait_for_gate_flag (&gate, false);
    std::thread second_reader;
    if (acquired) {
        second_reader = std::thread (
          [&] { receive_router_record (handle, &rejected); });
    }
    const bool contended =
      acquired && wait_for_gate_flag (&gate, true);
    release_gate (&gate);
    first_reader.join ();
    if (second_reader.joinable ())
        second_reader.join ();
    handle.socket->test_set_receive_record_hooks (NULL, NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (acquired,
                              "capacity owner did not acquire record scope");
    TEST_ASSERT_TRUE_MESSAGE (contended,
                              "capacity competitor did not wait for record scope");
    assert_two_part_record (first, "capacity-first");
    TEST_ASSERT_EQUAL_UINT64 (201, first.sequence);
    TEST_ASSERT_EQUAL_INT (-1, rejected.rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, rejected.errnum);
    TEST_ASSERT_TRUE (rejected.parts.empty ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (
          zlink::socket_reqrep_internal::max_reply_target_slots,
          state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_reservations);
        TEST_ASSERT_EQUAL_UINT64 (1, state->router_reply_targets.size ());

        // Remove only the artificial occupied baseline. The real first target
        // stays accounted while the next receive proves the rejected record
        // was not consumed.
        state->reply_target_slots = state->router_reply_targets.size ();
    }

    receive_result_t remaining;
    receive_router_record (handle, &remaining);
    assert_two_part_record (remaining, "capacity-second");
    TEST_ASSERT_EQUAL_UINT64 (202, remaining.sequence);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (router);
}

void test_empty_typed_receive_rolls_back_capacity_attempt ()
{
    const int receiver_types[] = {ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_DEALER};
    for (size_t i = 0; i < sizeof (receiver_types) / sizeof (receiver_types[0]);
         ++i) {
        void *receiver = test_context_socket (receiver_types[i]);
        TEST_ASSERT_NOT_NULL (receiver);
        const socket_handle_t handle = as_socket_handle (receiver);
        const std::shared_ptr<
          zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
          zlink::socket_reqrep_internal::find_or_create_request_reply_state (
            handle);
        TEST_ASSERT_NOT_NULL (state.get ());

        const size_t baseline_slots = 7;
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            state->reply_target_slots = baseline_slots;
            TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_reservations);
        }

        receive_result_t result;
        if (receiver_types[i] == ZLINK_SOCKET_ROUTER)
            receive_router_record (handle, &result);
        else
            receive_dealer_record (handle, true, &result);
        TEST_ASSERT_EQUAL_INT (-1, result.rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, result.errnum);
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            TEST_ASSERT_EQUAL_UINT64 (baseline_slots,
                                      state->reply_target_slots);
            TEST_ASSERT_EQUAL_UINT64 (0,
                                      state->reply_target_reservations);
        }

        // The baseline is synthetic; leave close with the same accounting a
        // real empty target table would have.
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            state->reply_target_slots = 0;
        }
        test_context_socket_close_zero_linger (receiver);
    }
}
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    RUN_TEST (test_router_and_dealer_hold_whole_record_receive_transaction);
    RUN_TEST (test_router_record_fences_mailbox_read_activation);
    RUN_TEST (test_router_capacity_reservation_is_atomic_and_non_consuming);
    RUN_TEST (test_empty_typed_receive_rolls_back_capacity_attempt);
    return UNITY_END ();
}
