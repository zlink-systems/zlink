/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/command.hpp"
#include "core/ctx.hpp"
#include "core/mailbox.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "core/signaler.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/pair/pair.hpp"

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

    static int process_socket_commands (socket_base_t *socket_, int timeout_)
    {
        return socket_->process_commands (timeout_, false);
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
    receive_result_t () : rc (-1), errnum (0), sequence (0) {}

    int rc;
    int errnum;
    uint64_t sequence;
    std::vector<std::string> parts;
};

struct command_sync_probe_t
{
    explicit command_sync_probe_t (
      int expected_command_type_ =
        static_cast<int> (zlink::command_t::activate_read)) :
        observed (false),
        sync_was_busy (false),
        public_api_sync_owned (false),
        expected_command_type (expected_command_type_)
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool observed;
    bool sync_was_busy;
    bool public_api_sync_owned;
    int expected_command_type;
};

struct pair_send_gate_t
{
    pair_send_gate_t () : entered (false), release (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool entered;
    bool release;
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
                                 bool sync_was_busy_,
                                 bool public_api_sync_owned_)
{
    command_sync_probe_t *probe =
      static_cast<command_sync_probe_t *> (userdata_);
    if (!probe || command_type_ != probe->expected_command_type)
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->observed = true;
        probe->sync_was_busy = sync_was_busy_;
        probe->public_api_sync_owned = public_api_sync_owned_;
    }
    probe->cv.notify_all ();
}

void pause_pair_send_before_pipe_dereference (void *userdata_)
{
    pair_send_gate_t *gate = static_cast<pair_send_gate_t *> (userdata_);
    std::unique_lock<std::mutex> lock (gate->mutex);
    gate->entered = true;
    gate->cv.notify_all ();
    gate->cv.wait (lock, [gate] { return gate->release; });
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
                           i == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL, NULL, NULL));
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

void run_two_reader_record_test ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_NOT_NULL (sender);

    const char *endpoint = "inproc://router-record-receive-transaction";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
    msleep (SETTLE_TIME);
    send_request_record (sender, 101, "first");
    send_request_record (sender, 102, "second");

    socket_handle_t handle = as_socket_handle (receiver);
    receive_record_gate_t gate;
    handle.socket->test_set_receive_record_hooks (
      &pause_after_first_record_frame, &observe_record_contention, &gate);

    receive_result_t first;
    receive_result_t second;
    std::thread first_reader ([&] {
        receive_router_record (handle, &first);
    });

    const bool acquired = wait_for_gate_flag (&gate, false);
    std::thread second_reader;
    if (acquired) {
        second_reader = std::thread ([&] {
            receive_router_record (handle, &second);
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
    // Public reply tokens are socket-owned opaque capabilities, not the
    // private wire request sequences supplied by this fixture.
    TEST_ASSERT_TRUE (first.sequence != 0);
    TEST_ASSERT_TRUE (second.sequence != 0);
    TEST_ASSERT_TRUE (first.sequence != second.sequence);

    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_router_holds_whole_record_receive_transaction ()
{
    run_two_reader_record_test ();
}

void test_router_record_fences_mailbox_read_activation ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    socket_handle_t handle = as_socket_handle (router);

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
    bool command_public_api_sync_owned = false;
    if (command_probed) {
        std::lock_guard<std::mutex> lock (command_probe.mutex);
        command_sync_was_busy = command_probe.sync_was_busy;
        command_public_api_sync_owned =
          command_probe.public_api_sync_owned;
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
    TEST_ASSERT_TRUE_MESSAGE (
      command_public_api_sync_owned,
      "public command drain did not own socket API synchronization");
    TEST_ASSERT_FALSE_MESSAGE (
      command_completed_while_record_open,
      "activate_read command mutated receive state before record completion");
    TEST_ASSERT_EQUAL_INT (0, command_rc);
    TEST_ASSERT_EQUAL_INT (0, command_errno);
    assert_two_part_record (first, "command-first");
    TEST_ASSERT_TRUE (first.sequence != 0);

    receive_result_t activated;
    receive_router_record (handle, &activated);
    assert_two_part_record (activated, "command-second");
    TEST_ASSERT_TRUE (activated.sequence != 0);
    TEST_ASSERT_TRUE (activated.sequence != first.sequence);

    // Complete both sides of the synthetic pipe lifecycle while the stack
    // event sinks are still alive. zlink_close() is asynchronous; returning
    // first would let context teardown call a destroyed sink.
    activated_pipe[0]->terminate (false);
    activated_pipe[1]->terminate (false);
    first_pipe[0]->terminate (false);
    first_pipe[1]->terminate (false);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::session_termination_test_access_t::process_socket_commands (
        handle.socket));
    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (router);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

void test_blocking_command_wait_ignores_stale_shared_poller_signal ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    socket_handle_t handle = as_socket_handle (router);
    zlink::mailbox_t *const mailbox =
      static_cast<zlink::mailbox_t *> (handle.socket->get_mailbox ());
    TEST_ASSERT_NOT_NULL (mailbox);

    // Windows pollers (and the optional POSIX shared-signaler path) register
    // one secondary signaler with multiple mailboxes. Its readiness can belong
    // to another socket and remains set until the poller consumes it. A
    // blocking command owner must neither consume it nor spin on it.
    zlink::signaler_t shared_poller_signaler;
    mailbox->add_signaler (&shared_poller_signaler);
    shared_poller_signaler.send ();

    uint64_t drains_before = 0;
    handle.socket->test_receive_owner_snapshot (NULL, &drains_before, NULL);
    const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now ();
    const int rc =
      zlink::session_termination_test_access_t::process_socket_commands (
        handle.socket, 40);
    const std::chrono::milliseconds elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - started);
    uint64_t drains_after = 0;
    handle.socket->test_receive_owner_snapshot (NULL, &drains_after, NULL);

    mailbox->remove_signaler (&shared_poller_signaler);

    TEST_ASSERT_EQUAL_INT (0, rc);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE (
      20, elapsed.count (),
      "stale secondary poller signal bypassed the command wait timeout");
    TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE (
      2, drains_after - drains_before,
      "blocking command wait spun on an undrained secondary poller signal");

    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (router);
}

void test_command_wait_preserves_signal_only_edges ()
{
    zlink::mailbox_t mailbox;

    // A handoff edge published immediately before registration must remain
    // visible without relying on the primary or a shared poller signaler.
    mailbox.signal ();
    TEST_ASSERT_EQUAL_INT (0, mailbox.wait_for_command_signal (100));

    std::atomic<int> wait_rc (-2);
    std::thread waiter ([&] {
        wait_rc.store (mailbox.wait_for_command_signal (-1),
                       std::memory_order_release);
    });
    const std::chrono::steady_clock::time_point registration_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (mailbox.test_command_waiter_count () == 0
           && std::chrono::steady_clock::now () < registration_deadline)
        std::this_thread::yield ();
    const bool registered = mailbox.test_command_waiter_count () != 0;
    mailbox.signal ();
    waiter.join ();

    TEST_ASSERT_TRUE_MESSAGE (
      registered, "command waiter did not register before signal-only wake");
    TEST_ASSERT_EQUAL_INT (0, wait_rc.load (std::memory_order_acquire));

    const std::chrono::steady_clock::time_point timeout_started =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN,
                               mailbox.wait_for_command_signal (30));
    const std::chrono::milliseconds timeout_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - timeout_started);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE (
      15, timeout_elapsed.count (),
      "command wait returned before its finite timeout");
}

void test_blocking_process_commands_returns_on_signal_only_edge ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    socket_handle_t handle = as_socket_handle (router);
    zlink::mailbox_t *const mailbox =
      static_cast<zlink::mailbox_t *> (handle.socket->get_mailbox ());

    std::atomic<int> process_rc (-2);
    std::thread waiter ([&] {
        process_rc.store (
          zlink::session_termination_test_access_t::process_socket_commands (
            handle.socket, -1),
          std::memory_order_release);
    });
    const std::chrono::steady_clock::time_point registration_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (mailbox->test_command_waiter_count () == 0
           && std::chrono::steady_clock::now () < registration_deadline)
        std::this_thread::yield ();
    const bool registered = mailbox->test_command_waiter_count () != 0;
    mailbox->signal ();
    waiter.join ();

    TEST_ASSERT_TRUE_MESSAGE (
      registered, "process_commands did not enter its infinite mailbox wait");
    TEST_ASSERT_EQUAL_INT (0, process_rc.load (std::memory_order_acquire));

    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (router);
}

void test_pair_commands_only_fence_pipe_lifetime_transitions ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (pair);
    socket_handle_t handle = as_socket_handle (pair);

    zlink::object_t *parents[2] = {handle.socket, handle.socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));
    passive_pipe_sink_t peer_sink;
    pipes[1]->set_event_sink (&peer_sink);

    command_sync_probe_t bind_probe (
      static_cast<int> (zlink::command_t::bind));
    handle.socket->test_set_receive_command_sync_probe_hook (
      &observe_command_sync_probe, &bind_probe);
    TEST_ASSERT_TRUE (handle.socket->send_bind (handle.socket, pipes[0]));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::session_termination_test_access_t::process_socket_commands (
        handle.socket));
    TEST_ASSERT_TRUE_MESSAGE (
      bind_probe.observed, "PAIR bind command was not processed");
    TEST_ASSERT_TRUE_MESSAGE (
      bind_probe.public_api_sync_owned,
      "PAIR bind did not own the multipart pipe-lifetime fence");

    command_sync_probe_t activation_probe;
    handle.socket->test_set_receive_command_sync_probe_hook (
      &observe_command_sync_probe, &activation_probe);
    TEST_ASSERT_FALSE (pipes[0]->check_read ());
    zlink::msg_t payload;
    TEST_ASSERT_SUCCESS_ERRNO (payload.init_size (1));
    *static_cast<unsigned char *> (payload.data ()) = 1;
    TEST_ASSERT_TRUE (pipes[1]->write_and_flush (&payload));
    TEST_ASSERT_SUCCESS_ERRNO (payload.close ());
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::session_termination_test_access_t::process_socket_commands (
        handle.socket));
    TEST_ASSERT_TRUE_MESSAGE (
      activation_probe.observed, "PAIR activation command was not processed");
    TEST_ASSERT_FALSE_MESSAGE (
      activation_probe.public_api_sync_owned,
      "PAIR activation command unnecessarily acquired the multipart fence");

    zlink::pipe_t *replacement[2] = {NULL, NULL};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, replacement, hwms, conflates, true));
    passive_pipe_sink_t replacement_peer_sink;
    replacement[1]->set_event_sink (&replacement_peer_sink);

    pair_send_gate_t send_gate;
    zlink::test_set_pair_xsend_gate_hook (
      &pause_pair_send_before_pipe_dereference, &send_gate);
    std::atomic<int> send_result (ZLINK_SUBMIT_INTERNAL_ERROR);
    std::thread send_thread ([&] {
        zlink_msg_t part;
        init_payload (&part, "pair-sync-gate");
        send_result.store (
          zlink_send_part (pair, &part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, NULL),
          std::memory_order_release);
        (void) zlink_msg_close (&part);
    });
    bool send_entered = false;
    {
        std::unique_lock<std::mutex> lock (send_gate.mutex);
        send_entered = send_gate.cv.wait_for (
          lock, std::chrono::seconds (3),
          [&send_gate] { return send_gate.entered; });
    }

    command_sync_probe_t replacement_bind_probe (
      static_cast<int> (zlink::command_t::bind));
    std::atomic<bool> bind_done (false);
    bool replacement_bind_queued = false;
    std::thread bind_thread;
    if (send_entered) {
        handle.socket->test_set_receive_command_sync_probe_hook (
          &observe_command_sync_probe, &replacement_bind_probe);
        replacement_bind_queued =
          handle.socket->send_bind (handle.socket, replacement[0]);
        if (replacement_bind_queued) {
            bind_thread = std::thread ([&] {
                (void) zlink::session_termination_test_access_t::process_socket_commands (
                  handle.socket);
                bind_done.store (true, std::memory_order_release);
            });
        }
    }
    bool bind_processed_while_send_open = false;
    if (send_entered) {
        std::unique_lock<std::mutex> lock (replacement_bind_probe.mutex);
        bind_processed_while_send_open = replacement_bind_probe.cv.wait_for (
          lock, std::chrono::milliseconds (100),
          [&replacement_bind_probe] {
              return replacement_bind_probe.observed;
          });
    }
    {
        std::lock_guard<std::mutex> lock (send_gate.mutex);
        send_gate.release = true;
    }
    send_gate.cv.notify_all ();
    send_thread.join ();
    if (bind_thread.joinable ())
        bind_thread.join ();
    zlink::test_set_pair_xsend_gate_hook (NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (
      send_entered, "PAIR send did not enter the deterministic pipe gate");
    TEST_ASSERT_TRUE_MESSAGE (
      replacement_bind_queued, "PAIR replacement bind was not queued");
    TEST_ASSERT_FALSE_MESSAGE (
      bind_processed_while_send_open,
      "PAIR bind mutated pipe lifetime while xsend held the raw pointer gate");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, send_result.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE_MESSAGE (
      bind_done.load (std::memory_order_acquire),
      "PAIR bind did not resume after xsend released lifecycle sync");
    TEST_ASSERT_TRUE_MESSAGE (
      replacement_bind_probe.public_api_sync_owned,
      "PAIR replacement bind did not own lifecycle sync");

    // The preceding sends intentionally leave payload queued on the passive
    // peer. Make its termination immediate so this test reaches the ack (and
    // therefore the socket-side pointer clear/deallocation boundary) without
    // needing an unrelated peer reader.
    pipes[1]->set_nodelay ();

    pair_send_gate_t termination_send_gate;
    zlink::test_set_pair_xsend_gate_hook (
      &pause_pair_send_before_pipe_dereference, &termination_send_gate);
    std::atomic<int> terminating_send_result (ZLINK_SUBMIT_INTERNAL_ERROR);
    std::thread terminating_send_thread ([&] {
        zlink_msg_t part;
        init_payload (&part, "pair-term-sync-gate");
        terminating_send_result.store (
          zlink_send_part (pair, &part, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, NULL),
          std::memory_order_release);
        (void) zlink_msg_close (&part);
    });
    bool terminating_send_entered = false;
    {
        std::unique_lock<std::mutex> lock (termination_send_gate.mutex);
        terminating_send_entered = termination_send_gate.cv.wait_for (
          lock, std::chrono::seconds (3), [&termination_send_gate] {
              return termination_send_gate.entered;
          });
    }

    command_sync_probe_t term_ack_probe (
      static_cast<int> (zlink::command_t::pipe_term_ack));
    std::atomic<bool> term_command_done (false);
    std::thread term_command_thread;
    if (terminating_send_entered) {
        handle.socket->test_set_receive_command_sync_probe_hook (
          &observe_command_sync_probe, &term_ack_probe);
        pipes[0]->terminate (false);
        term_command_thread = std::thread ([&] {
            (void) zlink::session_termination_test_access_t::process_socket_commands (
              handle.socket);
            term_command_done.store (true, std::memory_order_release);
        });
    }
    bool term_ack_processed_while_send_open = false;
    if (terminating_send_entered) {
        std::unique_lock<std::mutex> lock (term_ack_probe.mutex);
        term_ack_processed_while_send_open = term_ack_probe.cv.wait_for (
          lock, std::chrono::milliseconds (100),
          [&term_ack_probe] { return term_ack_probe.observed; });
    }
    {
        std::lock_guard<std::mutex> lock (termination_send_gate.mutex);
        termination_send_gate.release = true;
    }
    termination_send_gate.cv.notify_all ();
    terminating_send_thread.join ();
    if (term_command_thread.joinable ())
        term_command_thread.join ();
    zlink::test_set_pair_xsend_gate_hook (NULL, NULL);

    TEST_ASSERT_TRUE_MESSAGE (
      terminating_send_entered,
      "PAIR terminating send did not enter the deterministic pipe gate");
    TEST_ASSERT_FALSE_MESSAGE (
      term_ack_processed_while_send_open,
      "PAIR term ack cleared/deallocated the pipe during xsend");
    TEST_ASSERT_TRUE_MESSAGE (
      term_command_done.load (std::memory_order_acquire),
      "PAIR term ack did not resume after xsend released lifecycle sync");
    TEST_ASSERT_TRUE_MESSAGE (
      term_ack_probe.public_api_sync_owned,
      "PAIR term ack did not own lifecycle sync");
    TEST_ASSERT_NOT_EQUAL (
      ZLINK_SUBMIT_INTERNAL_ERROR,
      terminating_send_result.load (std::memory_order_acquire));

    handle.socket->test_set_receive_command_sync_probe_hook (NULL, NULL);
    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (pair);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

void test_close_commands_wait_for_parked_multipart_cleanup_sync ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);

    std::optional<zlink::socket_public_send_scope_t> multipart_scope;
    const bool scope_admitted =
      handle.socket->begin_public_send_scope (false, &multipart_scope);
    TEST_ASSERT_TRUE_MESSAGE (
      scope_admitted,
      "multipart send scope was not admitted");
    multipart_scope->suspend_multipart_call ();
    const bool handle_close_admitted = handle.begin_close ();
    TEST_ASSERT_TRUE_MESSAGE (handle_close_admitted,
                              "public handle close was not admitted");
    const int close_handoff_rc = handle.socket->begin_close_handoff ();
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      0, close_handoff_rc,
      "socket close handoff was not admitted with a parked marker");
    const bool cleanup_locked =
      multipart_scope->lock_multipart_for_close_cleanup ();
    TEST_ASSERT_TRUE_MESSAGE (
      cleanup_locked,
      "parked multipart cleanup did not acquire lifecycle sync");

    zlink::object_t *parents[2] = {handle.socket, handle.socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));
    passive_pipe_sink_t peer_sink;
    pipes[1]->set_event_sink (&peer_sink);

    command_sync_probe_t bind_probe (
      static_cast<int> (zlink::command_t::bind));
    handle.socket->test_set_receive_command_sync_probe_hook (
      &observe_command_sync_probe, &bind_probe);
    const bool bind_queued =
      handle.socket->send_bind (handle.socket, pipes[0]);
    std::atomic<bool> command_done (false);
    std::thread command_thread;
    if (bind_queued) {
        command_thread = std::thread ([&] {
            (void) zlink::session_termination_test_access_t::process_socket_commands (
              handle.socket);
            command_done.store (true, std::memory_order_release);
        });
    }

    bool bind_processed_during_cleanup = false;
    if (bind_queued) {
        std::unique_lock<std::mutex> lock (bind_probe.mutex);
        bind_processed_during_cleanup = bind_probe.cv.wait_for (
          lock, std::chrono::milliseconds (100),
          [&bind_probe] { return bind_probe.observed; });
    }

    // Mirrors part-helper close cleanup: releasing this scope drops the raw
    // sync bit and the parked multipart marker only after rollback ownership
    // is complete.
    multipart_scope.reset ();
    if (command_thread.joinable ())
        command_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (bind_queued,
                              "close-time bind command was not queued");
    TEST_ASSERT_FALSE_MESSAGE (
      bind_processed_during_cleanup,
      "closing command bypassed parked multipart cleanup lifecycle sync");
    TEST_ASSERT_TRUE_MESSAGE (
      command_done.load (std::memory_order_acquire),
      "closing command did not resume after multipart cleanup released sync");
    TEST_ASSERT_TRUE_MESSAGE (
      bind_probe.public_api_sync_owned,
      "pre-reaper closing command did not own lifecycle sync");

    handle.socket->test_set_receive_command_sync_probe_hook (NULL, NULL);
    handle.socket->complete_close_handoff ();
    handle = socket_handle_t ();
    // This test performs the close protocol manually so it can hold the
    // cleanup fence at a deterministic point. Tell the fixture not to issue a
    // second public close against the already-closing handle in tearDown().
    test_context_socket_mark_closed (dealer);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
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

    socket_handle_t handle = as_socket_handle (router);
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
    TEST_ASSERT_TRUE (first.sequence != 0);
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
    handle.socket->reply_target_slots_released (
      zlink::socket_reqrep_internal::max_reply_target_slots - 1);

    receive_result_t remaining;
    receive_router_record (handle, &remaining);
    assert_two_part_record (remaining, "capacity-second");
    TEST_ASSERT_TRUE (remaining.sequence != 0);
    TEST_ASSERT_TRUE (remaining.sequence != first.sequence);

    handle = socket_handle_t ();
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (router);
}

void test_empty_router_receive_rolls_back_capacity_attempt ()
{
    {
        void *receiver = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_NOT_NULL (receiver);
        socket_handle_t handle = as_socket_handle (receiver);
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
        receive_router_record (handle, &result);
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
        handle = socket_handle_t ();
        test_context_socket_close_zero_linger (receiver);
    }
}
}

int main ()
{
    setup_test_environment (30);
    UNITY_BEGIN ();
    RUN_TEST (test_router_holds_whole_record_receive_transaction);
    RUN_TEST (test_router_record_fences_mailbox_read_activation);
    RUN_TEST (test_blocking_command_wait_ignores_stale_shared_poller_signal);
    RUN_TEST (test_command_wait_preserves_signal_only_edges);
    RUN_TEST (test_blocking_process_commands_returns_on_signal_only_edge);
    RUN_TEST (test_pair_commands_only_fence_pipe_lifetime_transitions);
    RUN_TEST (test_close_commands_wait_for_parked_multipart_cleanup_sync);
    RUN_TEST (test_router_capacity_reservation_is_atomic_and_non_consuming);
    RUN_TEST (test_empty_router_receive_rolls_back_capacity_attempt);
    return UNITY_END ();
}
