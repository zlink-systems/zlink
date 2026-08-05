/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SESSION_BASE_HPP_INCLUDED__
#define __ZLINK_SESSION_BASE_HPP_INCLUDED__

#include <stdarg.h>

#include "core/own.hpp"
#include "core/io_object.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "engine/i_engine.hpp"
#include "core/msg.hpp"

namespace zlink
{
class io_thread_t;
struct i_engine;
struct address_t;

class session_base_t : public own_t, public io_object_t, public i_pipe_events
{
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif
  public:
    //  Create a session of the particular type.
    static session_base_t *create (zlink::io_thread_t *io_thread_,
                                   bool active_,
                                   zlink::socket_base_t *socket_,
                                   const options_t &options_,
                                   address_t *addr_);

    //  To be used once only, when creating the session.
    void attach_pipe (zlink::pipe_t *pipe_);

    //  Following functions are the interface exposed towards the engine.
    virtual void reset ();
    void flush ();
    void rollback ();
    void engine_error (bool handshaked_, zlink::i_engine::error_reason_t reason_);
    void engine_ready ();

    //  i_pipe_events interface implementation.
    void read_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void write_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void hiccuped (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void pipe_peer_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void pipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;

    //  Delivers a message. Returns 0 if successful; -1 otherwise.
    //  The function takes ownership of the message.
    virtual int push_msg (msg_t *msg_);

    //  Fetches a message. Returns 0 if successful; -1 otherwise.
    //  The caller is responsible for freeing the message when no
    //  longer used.
    virtual int pull_msg (msg_t *msg_);

    socket_base_t *get_socket () const;
    const endpoint_uri_pair_t &get_endpoint () const;
    void set_peer_routing_id (const unsigned char *data_, size_t size_);
    const blob_t &peer_routing_id () const;
    int set_peer_transport_pair (transport_lane_t lane_,
                                 uint64_t pair_id_,
                                 uint64_t generation_);
    void set_peer_max_message_bytes (uint64_t max_message_bytes_);

  protected:
    session_base_t (zlink::io_thread_t *io_thread_,
                    bool active_,
                    zlink::socket_base_t *socket_,
                    const options_t &options_,
                    address_t *addr_);
    ~session_base_t () ZLINK_OVERRIDE;

  private:
    void start_connecting (bool wait_);

    void reconnect ();
    void retain_socket_pipe (zlink::pipe_t *pipe_);
    void release_socket_pipe ();
    bool is_active_transport_pair () const;
    void start_transport_pair_reconnect (bool force_);

    //  Handlers for incoming commands.
    void process_plug () ZLINK_FINAL;
    void process_attach (zlink::i_engine *engine_) ZLINK_FINAL;
    void process_term (int linger_) ZLINK_FINAL;
    void process_conn_failed () ZLINK_OVERRIDE;

    //  i_poll_events handlers.
    void timer_event (int id_) ZLINK_FINAL;

    //  Remove any half processed messages. Flush unflushed messages.
    //  Call this function when engine disconnect to get rid of leftovers.
    void clean_pipes ();

    //  If true, this session (re)connects to the peer. Otherwise, it's
    //  a transient session created by the listener.
    const bool _active;

    //  Pipe connecting the session to its socket.
    zlink::pipe_t *_pipe;

    //  Socket-side peer retained while the session engine can read or update
    //  handshake metadata. The reference prevents endpoint teardown from
    //  freeing the peer on another thread.
    zlink::pipe_t *_socket_pipe;

    //  This set is added to with pipes we are disconnecting, but haven't yet completed
    std::set<pipe_t *> _terminating_pipes;

    //  This flag is true if the remainder of the message being processed
    //  is still in the in pipe.
    bool _incomplete_in;
    //  A ROUTER multipart message stamped for a retired transport is dropped
    //  through its final frame before a reconnecting engine can send again.
    bool _dropping_stale_transport_message;

    //  True if termination have been suspended to push the pending
    //  messages to the network.
    bool _pending;

    //  The protocol I/O engine connected to the session.
    zlink::i_engine *_engine;

    //  The socket the session belongs to.
    zlink::socket_base_t *_socket;

    //  Peer routing id received during handshake before the pipe exists.
    blob_t _pending_peer_routing_id;
    bool _pending_peer_routing_id_valid;
    uint64_t _peer_max_message_bytes;
    transport_lane_t _transport_lane;
    uint64_t _transport_pair_id;
    uint64_t _transport_pair_generation;
    bool _socket_pipe_bound;
    bool _transport_pair_reconnect_in_progress;

    //  I/O thread the session is living in. It will be used to plug in
    //  the engines into the same thread.
    zlink::io_thread_t *_io_thread;

    //  ID of the linger timer
    enum
    {
        linger_timer_id = 0x20
    };

    //  True is linger timer is running.
    bool _has_linger_timer;

    //  Protocol and address to use when connecting.
    address_t *_addr;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (session_base_t)
};
}

#endif
