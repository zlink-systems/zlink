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

#include <string.h>

#include <vector>

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

    static bool attached_pipe_connection_ids_are_live (
      socket_base_t *socket_, size_t expected_count_)
    {
        std::vector<pipe_t *> pipes;
        socket_->snapshot_attached_pipes (&pipes);
        if (pipes.size () != expected_count_)
            return false;
        for (size_t i = 0; i != pipes.size (); ++i) {
            const uint64_t live_id =
              pipes[i]->get_transport_connection_id ();
            const uint64_t endpoint_id =
              pipes[i]->get_endpoint_pair ().connection_id;
            if (live_id == 0 || endpoint_id != live_id)
                return false;
        }
        return true;
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

    static void prepare_concurrent_pipe_acks (pipe_t *first_, pipe_t *second_)
    {
        {
            scoped_fast_lock_t lock (first_->_out_sync);
            first_->_state = pipe_t::term_req_sent2;
            first_->_out_pipe = NULL;
        }
        {
            scoped_fast_lock_t lock (second_->_out_sync);
            second_->_state = pipe_t::term_req_sent2;
            second_->_out_pipe = NULL;
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

class concurrent_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    concurrent_pipe_sink_t () : completion_count (0) {}

    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        completion_count.fetch_add (1, std::memory_order_relaxed);
    }

    std::atomic<int> completion_count;
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

void test_ctx_term_with_open_socket_monitors ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *router_monitor = zlink_socket_monitor_open (router, &options);
    void *dealer_monitor = zlink_socket_monitor_open (dealer, &options);
    TEST_ASSERT_NOT_NULL (router_monitor);
    TEST_ASSERT_NOT_NULL (dealer_monitor);

    // The application may close the source sockets before it consumes the raw
    // monitor handles. Context termination must detach the source monitor
    // tasks before it reaps those still-open monitor sockets.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (router));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&dealer_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&router_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_router_router_connection_ready ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const char server_id[] = "SRV01";
    const char client_id[] = "CLT01";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server, server_id, sizeof (server_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, client_id, sizeof (client_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_id,
      sizeof (server_id) - 1));

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    monitor_options.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_options);
    TEST_ASSERT_NOT_NULL (monitor);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    bool ready = false;
    zlink_monitor_event_t event;
    for (int attempt = 0; attempt < 30 && !ready; ++attempt) {
        zlink_pollitem_t items[] = {
          {monitor, 0, ZLINK_POLLIN, 0}, {server, 0, ZLINK_POLLIN, 0}};
        if (zlink_poll (items, 2, 100, NULL) <= 0
            || (items[0].revents & ZLINK_POLLIN) == 0)
            continue;

        while (zlink_socket_monitor_recv (
                 monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT)
               == ZLINK_RECV_OK) {
            if (event.event == ZLINK_EVENT_CONNECTION_READY) {
                ready = true;
                break;
            }
        }
    }

    TEST_ASSERT_TRUE (ready);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (client_id) - 1, event.routing_id.size);
    TEST_ASSERT_EQUAL_MEMORY (client_id, event.routing_id.data, event.routing_id.size);
    TEST_ASSERT_NOT_EQUAL (0, event.connection_id);

    socket_handle_t server_public_handle = as_socket_handle (server);
    socket_handle_t client_public_handle = as_socket_handle (client);
    TEST_ASSERT_TRUE (
      zlink::session_termination_test_access_t::
        attached_pipe_connection_ids_are_live (server_public_handle.socket, 2));
    TEST_ASSERT_TRUE (
      zlink::session_termination_test_access_t::
        attached_pipe_connection_ids_are_live (client_public_handle.socket, 2));
    server_public_handle = socket_handle_t ();
    client_public_handle = socket_handle_t ();

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
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
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();
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

void test_terminating_lane_cannot_complete_delayed_pair_admission ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);

    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

    zlink::object_t *parents[2] = {socket, socket};
    zlink::pipe_t *application[2] = {NULL, NULL};
    zlink::pipe_t *completion[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1, 1};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, application, hwms, conflates, true,
                       zlink::transport_lane_application));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, completion, hwms, conflates, true,
                       zlink::transport_lane_completion));

    const uint64_t pair_id = 1;
    const uint64_t generation = 1;
    application[0]->set_transport_pair (
      zlink::transport_lane_application, pair_id, generation);
    application[1]->set_transport_pair (
      zlink::transport_lane_application, pair_id, generation);
    completion[0]->set_transport_pair (
      zlink::transport_lane_completion, pair_id, generation);
    completion[1]->set_transport_pair (
      zlink::transport_lane_completion, pair_id, generation);
    const unsigned char peer_identity = 0x2a;
    application[0]->set_transport_peer_identity (&peer_identity, 1);
    completion[0]->set_transport_peer_identity (&peer_identity, 1);
    application[0]->hold_writes_until_transport_pair_ready ();

    passive_pipe_sink_t application_peer_sink;
    passive_pipe_sink_t completion_peer_sink;
    application[1]->set_event_sink (&application_peer_sink);
    completion[1]->set_event_sink (&completion_peer_sink);

    zlink::session_termination_test_access_t::attach_socket_pipe (
      socket, application[0]);
    application[0]->terminate (false);
    TEST_ASSERT_FALSE (application[0]->is_lifecycle_active ());
    TEST_ASSERT_FALSE (application[0]->has_completed_termination ());

    // The Completion bind was already queued when Application termination
    // started. Object lifetime alone must not let the delayed bind publish a
    // Ready pair backed by the inactive Application lane.
    zlink::session_termination_test_access_t::attach_socket_pipe (
      socket, completion[0]);
    TEST_ASSERT_FALSE (
      socket->transport_pair_application_ready (application[0]));

    zlink::session_termination_test_access_t::process_socket_commands (socket);
    TEST_ASSERT_EQUAL_INT (1, application_peer_sink.completion_count);
    TEST_ASSERT_EQUAL_INT (1, completion_peer_sink.completion_count);
    TEST_ASSERT_TRUE (zlink::session_termination_test_access_t::
                        attached_pipe_connection_ids_are_live (socket, 0));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_reciprocal_pipe_ack_is_queued_before_local_completion ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

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

void test_concurrent_pipe_acks_detach_pair_once ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

    for (size_t attempt = 0; attempt < 64; ++attempt) {
        zlink::object_t *parents[2] = {socket, socket};
        zlink::pipe_t *pipes[2] = {NULL, NULL};
        const uint64_t hwms[2] = {1, 1};
        const bool conflates[2] = {false, false};
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink::pipepair (parents, pipes, hwms, conflates, true));

        concurrent_pipe_sink_t sinks[2];
        pipes[0]->set_event_sink (&sinks[0]);
        pipes[1]->set_event_sink (&sinks[1]);
        zlink::session_termination_test_access_t::prepare_concurrent_pipe_acks (
          pipes[0], pipes[1]);

        std::atomic<bool> start (false);
        std::thread first ([&] {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            zlink::session_termination_test_access_t::process_pipe_term_ack (
              pipes[0]);
        });
        std::thread second ([&] {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            zlink::session_termination_test_access_t::process_pipe_term_ack (
              pipes[1]);
        });
        start.store (true, std::memory_order_release);
        first.join ();
        second.join ();

        TEST_ASSERT_EQUAL_INT (
          1, sinks[0].completion_count.load (std::memory_order_relaxed));
        TEST_ASSERT_EQUAL_INT (
          1, sinks[1].completion_count.load (std::memory_order_relaxed));
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_retained_peer_snapshot_outlives_concurrent_pipe_acks ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

    zlink::object_t *parents[2] = {socket, socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1, 1};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));

    concurrent_pipe_sink_t sinks[2];
    pipes[0]->set_event_sink (&sinks[0]);
    pipes[1]->set_event_sink (&sinks[1]);
    TEST_ASSERT_TRUE (pipes[0]->retain_lifetime_ref ());
    zlink::pipe_t *const retained_peer = pipes[0]->retain_peer_snapshot ();
    TEST_ASSERT_EQUAL_PTR (pipes[1], retained_peer);
    zlink::session_termination_test_access_t::prepare_concurrent_pipe_acks (
      pipes[0], pipes[1]);

    std::atomic<bool> start (false);
    std::thread first ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[0]);
    });
    std::thread second ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[1]);
    });
    start.store (true, std::memory_order_release);
    first.join ();
    second.join ();

    TEST_ASSERT_NULL (pipes[0]->get_peer ());
    TEST_ASSERT_NULL (retained_peer->get_peer ());
    TEST_ASSERT_TRUE (pipes[0]->has_completed_termination ());
    TEST_ASSERT_TRUE (retained_peer->has_completed_termination ());
    TEST_ASSERT_EQUAL_INT (
      1, zlink::session_termination_test_access_t::lifetime_refs (pipes[0]));
    TEST_ASSERT_EQUAL_INT (
      1, zlink::session_termination_test_access_t::lifetime_refs (retained_peer));

    retained_peer->release_lifetime_ref ();
    pipes[0]->release_lifetime_ref ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_routing_id_snapshot_is_consistent_during_publication ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

    zlink::object_t *parents[2] = {socket, socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1, 1};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));

    concurrent_pipe_sink_t sinks[2];
    pipes[0]->set_event_sink (&sinks[0]);
    pipes[1]->set_event_sink (&sinks[1]);

    const size_t routing_id_size = 64;
    unsigned char initial[routing_id_size];
    memset (initial, 1, sizeof (initial));
    zlink::blob_t initial_id (initial, sizeof (initial));
    pipes[0]->set_router_socket_routing_id (initial_id);

    const int iterations = 4000;
    std::atomic<bool> start (false);
    std::atomic<int> failures (0);
    std::thread writer ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            unsigned char bytes[routing_id_size];
            memset (bytes, (i % 251) + 1, sizeof (bytes));
            zlink::blob_t routing_id (bytes, sizeof (bytes));
            pipes[0]->set_router_socket_routing_id (routing_id);
        }
    });
    std::thread reader ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            zlink::blob_t routing_id;
            pipes[0]->snapshot_routing_id (&routing_id);
            if (routing_id.size () != routing_id_size) {
                failures.fetch_add (1, std::memory_order_relaxed);
                continue;
            }
            const unsigned char expected = routing_id.data ()[0];
            for (size_t j = 1; j != routing_id.size (); ++j) {
                if (routing_id.data ()[j] != expected) {
                    failures.fetch_add (1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    });
    start.store (true, std::memory_order_release);
    writer.join ();
    reader.join ();

    TEST_ASSERT_EQUAL_INT (0, failures.load (std::memory_order_relaxed));

    zlink::session_termination_test_access_t::prepare_concurrent_pipe_acks (
      pipes[0], pipes[1]);
    std::atomic<bool> ack_start (false);
    std::thread first ([&] {
        while (!ack_start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[0]);
    });
    std::thread second ([&] {
        while (!ack_start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[1]);
    });
    ack_start.store (true, std::memory_order_release);
    first.join ();
    second.join ();

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_session_decoder_queue_accounting_publication ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *socket_handle = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket_handle);
    socket_handle_t public_handle = as_socket_handle (socket_handle);
    zlink::socket_base_t *socket = public_handle.socket;
    public_handle = socket_handle_t ();

    zlink::object_t *parents[2] = {socket, socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {0, 0};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (
      parents, pipes, hwms, conflates, true,
      zlink::transport_lane_application, zlink::auto_hwm_role_none, false,
      zlink::physical_queue_class_application, 0));

    concurrent_pipe_sink_t sinks[2];
    pipes[0]->set_event_sink (&sinks[0]);
    pipes[1]->set_event_sink (&sinks[1]);

    zlink::msg_t frame_template;
    TEST_ASSERT_SUCCESS_ERRNO (frame_template.init_size (1));
    const uint64_t frame_bytes =
      zlink::pipe_t::test_frame_accounted_bytes (&frame_template);
    TEST_ASSERT_SUCCESS_ERRNO (frame_template.close ());

    const int iterations = 2000;
    const uint64_t final_bytes = frame_bytes * iterations;
    std::atomic<bool> start (false);
    std::atomic<bool> writer_done (false);
    std::atomic<int> failures (0);
    std::thread writer ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            zlink::msg_t msg;
            if (msg.init_size (1) != 0) {
                failures.fetch_add (1, std::memory_order_relaxed);
                break;
            }
            zlink::decoder_frame_reservation_t storage;
            zlink::decoder_frame_reservation_t *reservation = NULL;
            if (pipes[0]->reserve_inbound_decoder_frame (
                  1, 0, true, &storage, &reservation)
                  != 0
                || pipes[0]->write_reserved_decoder_frame (
                     &msg, &reservation)
                     != 0) {
                failures.fetch_add (1, std::memory_order_relaxed);
                (void) msg.close ();
                break;
            }
            if (msg.init () != 0) {
                failures.fetch_add (1, std::memory_order_relaxed);
                break;
            }
        }
        writer_done.store (true, std::memory_order_release);
    });
    std::thread sampler ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        while (!writer_done.load (std::memory_order_acquire)) {
            const uint64_t current =
              pipes[0]->get_snd_queue_accounted_bytes ();
            if (current > final_bytes || current % frame_bytes != 0)
                failures.fetch_add (1, std::memory_order_relaxed);
        }
    });
    start.store (true, std::memory_order_release);
    writer.join ();
    sampler.join ();

    TEST_ASSERT_EQUAL_INT (0, failures.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_UINT64 (
      final_bytes, pipes[0]->get_snd_queue_accounted_bytes ());
    pipes[0]->flush ();
    for (int i = 0; i != iterations; ++i) {
        zlink::msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (msg.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&msg));
        TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
    }
    TEST_ASSERT_EQUAL_UINT64 (0,
                              pipes[0]->get_snd_queue_accounted_bytes ());

    const size_t multipart_sizes[2] = {3, 5};
    uint64_t multipart_bytes[2] = {0, 0};
    for (size_t i = 0; i != 2; ++i) {
        zlink::msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (multipart_sizes[i]));
        if (i == 0)
            msg.set_flags (zlink::msg_t::more);
        multipart_bytes[i] =
          zlink::pipe_t::test_frame_accounted_bytes (&msg);
        zlink::decoder_frame_reservation_t storage;
        zlink::decoder_frame_reservation_t *reservation = NULL;
        TEST_ASSERT_SUCCESS_ERRNO (pipes[0]->reserve_inbound_decoder_frame (
          multipart_sizes[i], msg.flags (), true, &storage, &reservation));
        TEST_ASSERT_SUCCESS_ERRNO (
          pipes[0]->write_reserved_decoder_frame (&msg, &reservation));
        TEST_ASSERT_SUCCESS_ERRNO (msg.init ());
        const uint64_t expected =
          multipart_bytes[0] + (i == 0 ? 0 : multipart_bytes[1]);
        TEST_ASSERT_EQUAL_UINT64 (
          expected, pipes[0]->get_snd_queue_accounted_bytes ());
    }

    pipes[0]->flush ();
    for (size_t i = 0; i != 2; ++i) {
        zlink::msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (msg.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&msg));
        TEST_ASSERT_EQUAL_UINT64 (
          i == 0 ? multipart_bytes[1] : 0,
          pipes[0]->get_snd_queue_accounted_bytes ());
        TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
    }

    zlink::session_termination_test_access_t::prepare_concurrent_pipe_acks (
      pipes[0], pipes[1]);
    std::atomic<bool> ack_start (false);
    std::thread first ([&] {
        while (!ack_start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[0]);
    });
    std::thread second ([&] {
        while (!ack_start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        zlink::session_termination_test_access_t::process_pipe_term_ack (
          pipes[1]);
    });
    ack_start.store (true, std::memory_order_release);
    first.join ();
    second.join ();

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_handle));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_monitor_and_hwm_update_race_peer_close ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *server = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "inproc://peer-snapshot-race"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "inproc://peer-snapshot-race"));

    const unsigned char byte = 0x2a;
    unsigned char received = 0;
    TEST_ASSERT_EQUAL_INT (1, zlink_send (client, &byte, sizeof (byte), 0));
    TEST_ASSERT_EQUAL_INT (1, zlink_recv (server, &received, sizeof (received), 0));

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    void *monitor = zlink_socket_monitor_open (server, &monitor_options);
    TEST_ASSERT_NOT_NULL (monitor);

    const int iterations = 2000;
    std::atomic<int> ready (0);
    std::atomic<bool> start (false);
    std::atomic<int> option_failures (0);
    std::atomic<int> monitor_failures (0);
    std::thread update_hwm ([&] {
        ready.fetch_add (1, std::memory_order_release);
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            const uint64_t hwm = (i & 1) == 0 ? 4096 : 8192;
            if (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm,
                                  sizeof (hwm)) != ZLINK_CONFIG_OK
                || zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm,
                                     sizeof (hwm)) != ZLINK_CONFIG_OK)
                option_failures.fetch_add (1, std::memory_order_relaxed);
        }
    });
    std::thread read_monitor ([&] {
        ready.fetch_add (1, std::memory_order_release);
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            zlink_monitor_status_t status;
            memset (&status, 0, sizeof (status));
            if (zlink_monitor_status (monitor, &status) != ZLINK_CONFIG_OK)
                monitor_failures.fetch_add (1, std::memory_order_relaxed);
        }
    });

    while (ready.load (std::memory_order_acquire) != 2)
        std::this_thread::yield ();
    start.store (true, std::memory_order_release);
    std::this_thread::yield ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    update_hwm.join ();
    read_monitor.join ();

    TEST_ASSERT_EQUAL_INT (0, option_failures.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, monitor_failures.load (std::memory_order_relaxed));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
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
    RUN_TEST (test_ctx_term_with_open_socket_monitors);
    RUN_TEST (test_router_router_connection_ready);
    RUN_TEST (test_ctx_shutdown);
    RUN_TEST (test_ctx_shutdown_socket_opened_after);
    RUN_TEST (test_ctx_shutdown_only_socket_opened_after);
    RUN_TEST (test_ctx_term_rearms_reaper_when_last_socket_closes_during_restart);
    RUN_TEST (test_pending_inproc_disconnect_releases_socket_before_context_term);
    RUN_TEST (test_engine_less_session_releases_socket_term_ack_with_pending_message);
    RUN_TEST (test_terminating_lane_cannot_complete_delayed_pair_admission);
    RUN_TEST (test_reciprocal_pipe_ack_is_queued_before_local_completion);
    RUN_TEST (test_concurrent_pipe_acks_detach_pair_once);
    RUN_TEST (test_retained_peer_snapshot_outlives_concurrent_pipe_acks);
    RUN_TEST (test_routing_id_snapshot_is_consistent_during_publication);
    RUN_TEST (test_session_decoder_queue_accounting_publication);
    RUN_TEST (test_monitor_and_hwm_update_race_peer_close);
    RUN_TEST (test_zlink_ctx_term_null_fails);
    RUN_TEST (test_zlink_term_null_fails);
    RUN_TEST (test_zlink_ctx_shutdown_null_fails);

    return UNITY_END ();
}
