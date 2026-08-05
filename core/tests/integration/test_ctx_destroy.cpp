/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "core/ctx.hpp"
#include "core/io_thread.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"

#include <unity.h>

namespace zlink
{
class ctx_termination_test_access_t
{
  public:
    static void set_terminating (ctx_t *ctx_, bool terminating_)
    {
        scoped_lock_t lock (ctx_->_slot_sync);
        ctx_->_terminating = terminating_;
    }
};

class session_termination_test_access_t
{
  public:
    struct own_snapshot_t
    {
        uint64_t sent;
        uint64_t processed;
        int term_acks;
        bool terminating;
    };

    static void attach_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->attach_pipe (pipe_);
    }

    static bool waiting_for_delimiter (pipe_t *pipe_)
    {
        scoped_fast_lock_t lock (pipe_->_out_sync);
        return pipe_->_state == pipe_t::waiting_for_delimiter;
    }

    static own_snapshot_t own_snapshot (own_t *object_)
    {
        own_snapshot_t out;
        out.sent = object_->_sent_seqnum.get ();
        out.processed = object_->_processed_seqnum;
        out.term_acks = object_->_term_acks;
        out.terminating = object_->_terminating;
        return out;
    }

    static void begin_socket_term (socket_base_t *socket_)
    {
        socket_->process_term (0);
    }

    static void begin_session_term (session_base_t *session_, int linger_)
    {
        session_->process_term (linger_);
    }

    static void process_socket_commands (socket_base_t *socket_)
    {
        (void) socket_->process_commands (0, false);
    }

    static void prepare_reciprocal_pipe_ack (pipe_t *local_, pipe_t *peer_)
    {
        {
            scoped_fast_lock_t lock (local_->_out_sync);
            local_->_state = pipe_t::term_req_sent1;
        }
        {
            scoped_fast_lock_t lock (peer_->_out_sync);
            peer_->_state = pipe_t::term_ack_sent;
            peer_->_out_pipe = NULL;
        }
    }

    static void process_pipe_term_ack (pipe_t *pipe_)
    {
        pipe_->process_pipe_term_ack ();
    }

    static int lifetime_refs (pipe_t *pipe_)
    {
        return static_cast<int> (pipe_->_lifetime.refs ());
    }

    static bool lifetime_underflow_rejected ()
    {
        pipe_t::lifetime_state_t state;
        return state.release ()
               == pipe_t::lifetime_state_t::transition_invalid;
    }

    static bool lifetime_overflow_rejected ()
    {
        pipe_t::lifetime_state_t state;
        state._state.store (pipe_t::lifetime_state_t::refs_mask,
                            std::memory_order_release);
        return !state.retain ();
    }

    static bool lifetime_retain_after_terminal_rejected ()
    {
        pipe_t::lifetime_state_t state;
        if (!state.retain ())
            return false;
        if (state.complete_termination ()
            != pipe_t::lifetime_state_t::transition_complete)
            return false;
        const bool rejected = !state.retain ();
        return rejected
               && state.release ()
                    == pipe_t::lifetime_state_t::transition_delete_owner;
    }

    static bool lifetime_concurrent_completion_has_one_delete_owner ()
    {
        for (size_t attempt = 0; attempt < 200; ++attempt) {
            pipe_t::lifetime_state_t state;
            if (!state.retain ())
                return false;
            pipe_t::lifetime_state_t::transition_t release_result =
              pipe_t::lifetime_state_t::transition_invalid;
            pipe_t::lifetime_state_t::transition_t terminal_result =
              pipe_t::lifetime_state_t::transition_invalid;
            std::atomic<bool> start (false);
            std::thread release_thread ([&] {
                while (!start.load (std::memory_order_acquire))
                    std::this_thread::yield ();
                release_result = state.release ();
            });
            std::thread terminal_thread ([&] {
                while (!start.load (std::memory_order_acquire))
                    std::this_thread::yield ();
                terminal_result = state.complete_termination ();
            });
            start.store (true, std::memory_order_release);
            release_thread.join ();
            terminal_thread.join ();
            const int delete_owners =
              (release_result
                 == pipe_t::lifetime_state_t::transition_delete_owner
                 ? 1
                 : 0)
              + (terminal_result
                   == pipe_t::lifetime_state_t::transition_delete_owner
                   ? 1
                   : 0);
            if (delete_owners != 1 || !state.terminal () || state.refs () != 0)
                return false;
        }
        return true;
    }

    static bool socket_destroyed (socket_base_t *socket_)
    {
        return socket_->lifecycle_coordinator ().is_destroyed ();
    }
};
}

namespace
{
void test_pipe_lifetime_state_rejects_invalid_transitions ()
{
    TEST_ASSERT_TRUE (
      zlink::session_termination_test_access_t::lifetime_underflow_rejected ());
    TEST_ASSERT_TRUE (
      zlink::session_termination_test_access_t::lifetime_overflow_rejected ());
    TEST_ASSERT_TRUE (zlink::session_termination_test_access_t::
                        lifetime_retain_after_terminal_rejected ());
}

void test_pipe_lifetime_state_assigns_one_delete_owner ()
{
    TEST_ASSERT_TRUE (zlink::session_termination_test_access_t::
                        lifetime_concurrent_completion_has_one_delete_owner ());
}

class pipe_completion_order_sink_t : public zlink::i_pipe_events
{
  public:
    explicit pipe_completion_order_sink_t (zlink::pipe_t *peer_) :
        _peer (peer_),
        peer_lifetime_refs_at_completion (-1)
    {
    }

    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}

    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        peer_lifetime_refs_at_completion =
          zlink::session_termination_test_access_t::lifetime_refs (_peer);
    }

    int peer_lifetime_refs_at_completion;

  private:
    zlink::pipe_t *_peer;
};

class passive_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    passive_pipe_sink_t () : completion_count (0) {}

    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        ++completion_count;
    }

    int completion_count;
};
}

void setUp ()
{
}

void tearDown ()
{
}

static void receiver (void *socket_)
{
    char buffer[16];
    int rc = zlink_recv (socket_, &buffer, sizeof (buffer), 0);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
}

void test_ctx_destroy ()
{
    //  Set up our context and sockets
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);

    // Close the socket
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Destroy the context
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown ()
{
    //  Set up our context and sockets
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);

    // Spawn a thread to receive on socket
    void *receiver_thread = zlink_thread_start (&receiver, socket);

    // Wait for thread to start up and block
    msleep (SETTLE_TIME);

    // Shutdown context, if we used destroy here we would deadlock.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Wait for thread to finish
    zlink_thread_join (receiver_thread);

    // Close the socket.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Destroy the context, will now not hang as we have closed the socket.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown_socket_opened_after ()
{
    //  Set up our context.
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    // Open a socket to start context, and close it immediately again.
    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Shutdown context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Opening socket should now fail.
    TEST_ASSERT_NULL (zlink_socket (ctx, ZLINK_SOCKET_DEALER));
    TEST_ASSERT_FAILURE_ERRNO (ETERM, -1);

    // Destroy the context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown_only_socket_opened_after ()
{
    //  Set up our context.
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    // Shutdown context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Opening socket should now fail.
    TEST_ASSERT_NULL (zlink_socket (ctx, ZLINK_SOCKET_DEALER));
    TEST_ASSERT_FAILURE_ERRNO (ETERM, -1);

    // Destroy the context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_term_rearms_reaper_when_last_socket_closes_during_restart ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    zlink::ctx_t *internal_ctx = static_cast<zlink::ctx_t *> (ctx);
    zlink::ctx_termination_test_access_t::set_terminating (internal_ctx, false);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));
    TEST_ASSERT_SUCCESS_ERRNO (internal_ctx->wait_for_socket_count_at_most (0, 5000));

    // Reproduce the state restored by flush_pending_inproc_locked(): shutdown
    // has started, the registry is empty, but no stop reached the reaper.
    zlink::ctx_termination_test_access_t::set_terminating (internal_ctx, true);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pending_inproc_disconnect_releases_socket_before_context_term ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (socket, "inproc://pending-disconnect"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (socket, "inproc://pending-disconnect"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    zlink::ctx_t *internal_ctx = static_cast<zlink::ctx_t *> (ctx);
    TEST_ASSERT_SUCCESS_ERRNO (internal_ctx->wait_for_socket_count_at_most (0, 5000));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_engine_less_session_releases_socket_term_ack_with_pending_message ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);

    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);
    zlink::socket_base_t *socket =
      static_cast<zlink::socket_base_t *> (socket_handle);
    zlink::io_thread_t *io_thread = ctx->choose_io_thread (0);
    TEST_ASSERT_NOT_NULL (io_thread);

    zlink::options_t options;
    options.type = ZLINK_CORE_SOCKET_PAIR;
    zlink::session_base_t *session =
      zlink::session_base_t::create (io_thread, false, socket, options, NULL);
    TEST_ASSERT_NOT_NULL (session);

    zlink::object_t *parents[2] = {session, socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {16, 16};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));
    session->attach_pipe (pipes[0]);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      socket, pipes[1]);

    //  Leave one complete outbound message ahead of the delimiter. The
    //  engine-less session cannot deliver it, which reproduces the exact
    //  waiting-for-delimiter state that retained the ROUTER owner's final
    //  termination ack during RL-C1 teardown.
    zlink::msg_t message;
    TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
    *static_cast<unsigned char *> (message.data ()) = 0x2a;
    TEST_ASSERT_TRUE (pipes[1]->write_and_flush (&message));
    TEST_ASSERT_SUCCESS_ERRNO (message.close ());
    pipes[1]->terminate (false);

    bool waiting = false;
    for (int attempt = 0; attempt < 100 && !waiting; ++attempt) {
        waiting = zlink::session_termination_test_access_t::waiting_for_delimiter (
          pipes[0]);
        if (!waiting)
            msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (
      waiting, "session pipe did not enter waiting-for-delimiter");

    zlink::session_termination_test_access_t::begin_socket_term (socket);
    const zlink::session_termination_test_access_t::own_snapshot_t blocked =
      zlink::session_termination_test_access_t::own_snapshot (socket);
    TEST_ASSERT_TRUE (blocked.terminating);
    TEST_ASSERT_EQUAL_UINT64 (blocked.sent, blocked.processed);
    TEST_ASSERT_EQUAL_INT (1, blocked.term_acks);

    //  Infinite linger cannot retain data after the engine has disappeared.
    //  The session must force the pipe handshake to complete and return the
    //  exact ack observed above to its socket owner.
    zlink::session_termination_test_access_t::begin_session_term (session, -1);
    zlink::session_termination_test_access_t::process_socket_commands (socket);
    const zlink::session_termination_test_access_t::own_snapshot_t completed =
      zlink::session_termination_test_access_t::own_snapshot (socket);
    TEST_ASSERT_EQUAL_UINT64 (completed.sent, completed.processed);
    TEST_ASSERT_EQUAL_INT (0, completed.term_acks);
    TEST_ASSERT_TRUE (
      zlink::session_termination_test_access_t::socket_destroyed (socket));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (ctx->wait_for_socket_count_at_most (0, 1000));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_reciprocal_pipe_ack_is_queued_before_local_completion ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    zlink::socket_base_t *socket =
      static_cast<zlink::socket_base_t *> (socket_handle);

    zlink::object_t *parents[2] = {socket, socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1, 1};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));

    pipe_completion_order_sink_t completion_sink (pipes[1]);
    passive_pipe_sink_t passive_sink;
    pipes[0]->set_event_sink (&completion_sink);
    pipes[1]->set_event_sink (&passive_sink);
    zlink::session_termination_test_access_t::prepare_reciprocal_pipe_ack (
      pipes[0], pipes[1]);

    //  This transition owes a final reciprocal acknowledgement. Its command
    //  reference must already protect the peer when local completion is
    //  reported, because that callback may cascade into owner teardown.
    zlink::session_termination_test_access_t::process_pipe_term_ack (pipes[0]);
    const int refs_at_completion =
      completion_sink.peer_lifetime_refs_at_completion;
    zlink::session_termination_test_access_t::process_socket_commands (socket);
    TEST_ASSERT_EQUAL_INT (1, refs_at_completion);
    TEST_ASSERT_EQUAL_INT (1, passive_sink.completion_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_zlink_ctx_term_null_fails ()
{
    int rc = zlink_ctx_term (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_term_null_fails ()
{
    int rc = zlink_ctx_term (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_ctx_shutdown_null_fails ()
{
    int rc = zlink_ctx_shutdown (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pipe_lifetime_state_rejects_invalid_transitions);
    RUN_TEST (test_pipe_lifetime_state_assigns_one_delete_owner);
    RUN_TEST (test_ctx_destroy);
    RUN_TEST (test_ctx_shutdown);
    RUN_TEST (test_ctx_shutdown_socket_opened_after);
    RUN_TEST (test_ctx_shutdown_only_socket_opened_after);
    RUN_TEST (test_ctx_term_rearms_reaper_when_last_socket_closes_during_restart);
    RUN_TEST (test_pending_inproc_disconnect_releases_socket_before_context_term);
    RUN_TEST (test_engine_less_session_releases_socket_term_ack_with_pending_message);
    RUN_TEST (test_reciprocal_pipe_ack_is_queued_before_local_completion);
    RUN_TEST (test_zlink_ctx_term_null_fails);
    RUN_TEST (test_zlink_term_null_fails);
    RUN_TEST (test_zlink_ctx_shutdown_null_fails);

    return UNITY_END ();
}
