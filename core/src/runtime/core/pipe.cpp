/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdio.h>
#include <new>
#include <stddef.h>

#include "utils/macros.hpp"
#include "core/pipe.hpp"
#include "core/transport_pair_policy.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/heap_owner.hpp"

namespace
{
void pipe_debug_log (
  const zlink::pipe_t *pipe_, const char *phase_, int state_, bool delay_, const char *identifier_)
{
    if (!zlink::debug_env_enabled ("ZLINK_DEBUG_PIPE_TERM"))
        return;

    fprintf (stderr, "[pipe-term] pipe=%p phase=%s state=%d delay=%d endpoint=%s\n",
             static_cast<const void *> (pipe_), phase_ ? phase_ : "?", state_, delay_ ? 1 : 0,
             identifier_ && *identifier_ ? identifier_ : "<none>");
    fflush (stderr);
}
}
#include <ctime>

#include "core/ypipe.hpp"
#include "core/ypipe_conflate.hpp"

int zlink::pipepair (object_t *parents_[2],
                     pipe_t *pipes_[2],
                     const uint64_t hwms_[2],
                     const bool conflate_[2],
                     bool session_pipe_)
{
    //   Creates two pipe objects. These objects are connected by two ypipes,
    //   each to pass messages in one direction.

    //  Per-connection (session<->socket) pipes use a smaller chunk: servers
    //  hold one pipepair per transport connection and auto-HWM keeps their
    //  depth shallow at scale, so the default 256-slot chunk mostly wastes
    //  memory there.
    typedef ypipe_t<msg_t, message_pipe_granularity> upipe_normal_t;
    typedef ypipe_t<msg_t, session_pipe_granularity> upipe_session_t;
    typedef ypipe_conflate_t<msg_t> upipe_conflate_t;

    pipe_t::upipe_t *upipe1;
    if (conflate_[0])
        upipe1 = new (std::nothrow) upipe_conflate_t ();
    else if (session_pipe_)
        upipe1 = new (std::nothrow) upipe_session_t ();
    else
        upipe1 = new (std::nothrow) upipe_normal_t ();
    alloc_assert (upipe1);

    pipe_t::upipe_t *upipe2;
    if (conflate_[1])
        upipe2 = new (std::nothrow) upipe_conflate_t ();
    else if (session_pipe_)
        upipe2 = new (std::nothrow) upipe_session_t ();
    else
        upipe2 = new (std::nothrow) upipe_normal_t ();
    alloc_assert (upipe2);

    //  Inproc routes have no engine to assign a connection id, so allocate
    //  one here. A session-backed route remains unbound until its engine is
    //  ready. Messages queued before that first binding keep id 0 and belong
    //  to the first transport; later nonzero ids still isolate reconnects.
    const std::shared_ptr<transport_lifetime_t> transport_lifetime =
      std::make_shared<transport_lifetime_t> (
        session_pipe_ ? 0 : allocate_connection_id ());

    pipes_[0] = new (std::nothrow)
      pipe_t (parents_[0], upipe1, upipe2, hwms_[1], hwms_[0], conflate_[0],
              session_pipe_, transport_lifetime);
    alloc_assert (pipes_[0]);
    pipes_[1] = new (std::nothrow)
      pipe_t (parents_[1], upipe2, upipe1, hwms_[0], hwms_[1], conflate_[1],
              session_pipe_, transport_lifetime);
    alloc_assert (pipes_[1]);

    pipes_[0]->set_peer (pipes_[1]);
    pipes_[1]->set_peer (pipes_[0]);

    return 0;
}

void zlink::send_routing_id (pipe_t *pipe_, const options_t &options_)
{
    zlink::msg_t id;
    const int rc = id.init_size (options_.routing_id_size);
    errno_assert (rc == 0);
    memcpy (id.data (), options_.routing_id, options_.routing_id_size);
    id.set_flags (zlink::msg_t::routing_id);
    const bool written = pipe_->write_routing_id_and_flush (&id);
    zlink_assert (written);
}

void zlink::send_hello_msg (pipe_t *pipe_, const options_t &options_)
{
    zlink::msg_t hello;
    const int rc = hello.init_buffer (&options_.hello_msg[0], options_.hello_msg.size ());
    errno_assert (rc == 0);
    const bool written = pipe_->write (&hello);
    zlink_assert (written);
    pipe_->flush ();
}

zlink::pipe_stream_packet_state_t::pipe_stream_packet_state_t () :
    stage (prefix_stage),
    prefix_used (0),
    header_size (0),
    body_size (0),
    header_used (0),
    body_used (0)
{
    memset (prefix, 0, sizeof (prefix));
    const int header_rc = header.init ();
    errno_assert (header_rc == 0);
    const int body_rc = body.init ();
    errno_assert (body_rc == 0);
}

zlink::pipe_stream_packet_state_t::~pipe_stream_packet_state_t ()
{
    const int header_rc = header.close ();
    errno_assert (header_rc == 0);
    const int body_rc = body.close ();
    errno_assert (body_rc == 0);
}

void zlink::pipe_stream_packet_state_t::reset ()
{
    if (header.check ()) {
        const int header_rc = header.close ();
        errno_assert (header_rc == 0);
    }
    if (body.check ()) {
        const int body_rc = body.close ();
        errno_assert (body_rc == 0);
    }

    const int header_rc = header.init ();
    errno_assert (header_rc == 0);
    const int body_rc = body.init ();
    errno_assert (body_rc == 0);

    stage = prefix_stage;
    prefix_used = 0;
    header_size = 0;
    body_size = 0;
    header_used = 0;
    body_used = 0;
    memset (prefix, 0, sizeof (prefix));
}

zlink::pipe_t::pipe_t (object_t *parent_,
                       upipe_t *inpipe_,
                       upipe_t *outpipe_,
                       uint64_t inhwm_,
                       uint64_t outhwm_,
                       bool conflate_,
                       bool session_pipe_,
                       const std::shared_ptr<transport_lifetime_t> &transport_lifetime_) :
    object_t (parent_),
    _in_pipe (inpipe_),
    _out_pipe (outpipe_),
    _in_active (true),
    _out_active (true),
    _transport_pair_write_held (false),
    _waiting_for_byte_credit (false),
    _hwm (outhwm_),
    _lwm (compute_lwm (inhwm_)),
    _inhwm (inhwm_),
    _lwm_hint (0),
    _in_hwm_boost (0),
    _out_hwm_boost (0),
    _in_hwm_boost_set (false),
    _out_hwm_boost_set (false),
    _msgs_read (0),
    _msgs_written (0),
    _bytes_read (0),
    _bytes_written (0),
    _published_msgs_read (0),
    _published_bytes_read (0),
    _last_credit_bytes_read (0),
    _in_incomplete_bytes (0),
    _out_incomplete_bytes (0),
    _out_incomplete_payload_bytes (0),
    _out_multipart_started_empty (false),
    _max_message_bytes (0),
    _oversize_message_admission_count (0),
    _oversize_message_admission_max_bytes (0),
    _connected_time (0),
    _peers_msgs_read (0),
    _peers_bytes_read (0),
    _peer (NULL),
    _sink (NULL),
    _state (active),
    _delay (true),
    _server_socket_routing_id (0),
    _stream_connect_event_emitted (false),
    _connection_ready_event_emitted (false),
    _lifetime (),
    _conflate (conflate_),
    _session_pipe (session_pipe_),
    _transport_lifetime (transport_lifetime_),
    _transport_lane (transport_lane_application),
    _transport_pair_id (0),
    _transport_pair_generation (0),
    _locally_initiated (false)
{
    _disconnect_msg.init ();
}

zlink::pipe_t::~pipe_t ()
{
    _disconnect_msg.close ();
}

void zlink::pipe_t::set_peer (pipe_t *peer_)
{
    //  Peer can be set once only.
    zlink_assert (!_peer);
    _peer = peer_;
}

void zlink::pipe_t::detach_peer_backref ()
{
    if (_peer && _peer->_peer == this)
        _peer->_peer = NULL;
}

zlink::pipe_t::lifetime_state_t::lifetime_state_t () : _state (0)
{
}

bool zlink::pipe_t::lifetime_state_t::retain ()
{
    uint32_t state = _state.load (std::memory_order_acquire);
    for (;;) {
        if ((state & terminal_bit) != 0 || (state & refs_mask) == refs_mask)
            return false;
        if (_state.compare_exchange_weak (state, state + 1U,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return true;
    }
}

zlink::pipe_t::lifetime_state_t::transition_t
zlink::pipe_t::lifetime_state_t::release ()
{
    uint32_t state = _state.load (std::memory_order_acquire);
    for (;;) {
        const uint32_t refs = state & refs_mask;
        if (refs == 0)
            return transition_invalid;
        const uint32_t next = state - 1U;
        if (_state.compare_exchange_weak (state, next, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return (state & terminal_bit) != 0 && refs == 1
                     ? transition_delete_owner
                     : transition_complete;
    }
}

zlink::pipe_t::lifetime_state_t::transition_t
zlink::pipe_t::lifetime_state_t::complete_termination ()
{
    uint32_t state = _state.load (std::memory_order_acquire);
    for (;;) {
        if ((state & terminal_bit) != 0)
            return transition_invalid;
        const uint32_t next = state | terminal_bit;
        if (_state.compare_exchange_weak (state, next, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return (state & refs_mask) == 0 ? transition_delete_owner
                                            : transition_complete;
    }
}

bool zlink::pipe_t::lifetime_state_t::terminal () const
{
    return (_state.load (std::memory_order_acquire) & terminal_bit) != 0;
}

uint32_t zlink::pipe_t::lifetime_state_t::refs () const
{
    return _state.load (std::memory_order_acquire) & refs_mask;
}

bool zlink::pipe_t::retain_lifetime_ref ()
{
    return _lifetime.retain ();
}

void zlink::pipe_t::release_lifetime_ref ()
{
    const lifetime_state_t::transition_t transition = _lifetime.release ();
    zlink_assert (transition != lifetime_state_t::transition_invalid);
    if (transition == lifetime_state_t::transition_delete_owner)
        zlink::release_heap_owned (this);
}

bool zlink::pipe_t::has_completed_termination () const
{
    return _lifetime.terminal ();
}

void zlink::pipe_t::set_event_sink (i_pipe_events *sink_)
{
    // A paired transport assigns the socket-side sink before handshake so a
    // failed, not-yet-attached lane can still complete pipe termination. The
    // later validated bind may assign the same sink again.
    zlink_assert (!_sink || _sink == sink_);
    _sink = sink_;
}

void zlink::pipe_t::set_server_socket_routing_id (uint32_t server_socket_routing_id_)
{
    _server_socket_routing_id = server_socket_routing_id_;
}

uint32_t zlink::pipe_t::get_server_socket_routing_id () const
{
    return _server_socket_routing_id;
}

void zlink::pipe_t::set_router_socket_routing_id (const blob_t &router_socket_routing_id_)
{
    _router_socket_routing_id.set_deep_copy (router_socket_routing_id_);
}

const zlink::blob_t &zlink::pipe_t::get_routing_id () const
{
    return _router_socket_routing_id;
}

zlink::pipe_t *zlink::pipe_t::get_peer () const
{
    return _peer;
}

void zlink::pipe_t::set_peer_routing_id (const unsigned char *data_, size_t size_)
{
    blob_t routing_id;
    if (data_ && size_ > 0)
        routing_id.set (data_, size_);
    set_router_socket_routing_id (routing_id);
    if (_connected_time == 0)
        _connected_time = static_cast<uint64_t> (time (NULL));
}

void zlink::pipe_t::set_transport_peer_identity (const unsigned char *data_, size_t size_)
{
    if (!data_ || size_ == 0)
        return;
    if (_transport_peer_identity.size () == 0) {
        _transport_peer_identity.set (data_, size_);
        return;
    }
    zlink_assert (_transport_peer_identity.size () == size_);
    zlink_assert (memcmp (_transport_peer_identity.data (), data_, size_) == 0);
}

const zlink::blob_t &zlink::pipe_t::get_transport_peer_identity () const
{
    return _transport_peer_identity;
}

uint64_t zlink::pipe_t::get_msgs_written () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    return _msgs_written;
}

uint64_t zlink::pipe_t::get_msgs_read () const
{
    return _published_msgs_read.load (std::memory_order_relaxed);
}

uint64_t zlink::pipe_t::get_bytes_written () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    return _bytes_written;
}

uint64_t zlink::pipe_t::get_bytes_read () const
{
    return _published_bytes_read.load (std::memory_order_relaxed);
}

uint64_t zlink::pipe_t::get_snd_pending_msgs () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    if (_msgs_written <= _peers_msgs_read)
        return 0;
    return _msgs_written - _peers_msgs_read;
}

uint64_t zlink::pipe_t::get_rcv_pending_msgs_approx () const
{
    if (!_peer)
        return 0;

    const uint64_t peer_written = _peer->get_msgs_written ();
    const uint64_t msgs_read = get_msgs_read ();
    if (peer_written <= msgs_read)
        return 0;
    return peer_written - msgs_read;
}

uint64_t zlink::pipe_t::get_snd_pending_bytes () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    if (_bytes_written <= _peers_bytes_read)
        return 0;
    return _bytes_written - _peers_bytes_read;
}

uint64_t zlink::pipe_t::get_rcv_pending_bytes_approx () const
{
    if (!_peer)
        return 0;

    const uint64_t peer_written = _peer->get_bytes_written ();
    const uint64_t bytes_read = get_bytes_read ();
    if (peer_written <= bytes_read)
        return 0;
    return peer_written - bytes_read;
}

uint64_t zlink::pipe_t::get_oversize_message_admission_count () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    return _oversize_message_admission_count;
}

uint64_t zlink::pipe_t::get_oversize_message_admission_max_bytes () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    return _oversize_message_admission_max_bytes;
}

uint64_t zlink::pipe_t::get_connected_time () const
{
    return _connected_time;
}

void zlink::pipe_t::refresh_write_credit (uint64_t peer_msgs_read_, uint64_t peer_bytes_read_)
{
    scoped_fast_lock_t lock (_out_sync);

    if (peer_msgs_read_ > _peers_msgs_read)
        _peers_msgs_read = peer_msgs_read_;
    if (peer_bytes_read_ > _peers_bytes_read)
        _peers_bytes_read = peer_bytes_read_;

    if (!_transport_pair_write_held && !_out_active && _state == active
        && check_hwm_unlocked ()) {
        _out_active = true;
        _waiting_for_byte_credit.store (false, std::memory_order_release);
    }
}

bool zlink::pipe_t::mark_stream_connect_event_emitted ()
{
    if (_stream_connect_event_emitted.load (std::memory_order_acquire))
        return false;

    bool expected = false;
    return _stream_connect_event_emitted.compare_exchange_strong (
      expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void zlink::pipe_t::reset_stream_connect_event_emitted ()
{
    _stream_connect_event_emitted.store (false, std::memory_order_release);
}

bool zlink::pipe_t::mark_connection_ready_event_emitted ()
{
    if (_connection_ready_event_emitted.load (std::memory_order_acquire))
        return false;

    bool expected = false;
    return _connection_ready_event_emitted.compare_exchange_strong (
      expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void zlink::pipe_t::reset_connection_ready_event_emitted ()
{
    _connection_ready_event_emitted.store (false, std::memory_order_release);
}

zlink::pipe_t::stream_packet_state_t &zlink::pipe_t::stream_packet_state ()
{
    return _stream_packet_state;
}

const zlink::pipe_t::stream_packet_state_t &zlink::pipe_t::stream_packet_state () const
{
    return _stream_packet_state;
}

zlink::fast_mutex_t &zlink::pipe_t::stream_packet_dispatch_sync ()
{
    return _stream_packet_sync;
}

void zlink::pipe_t::reset_stream_packet_state ()
{
    scoped_fast_lock_t lock (_stream_packet_sync);
    _stream_packet_state.reset ();
}

bool zlink::pipe_t::check_read ()
{
    // Hot path: PAIR/DEALER steady-state recv reaches this path for every
    // message. Any extra locking or bookkeeping here shows up directly in
    // small-message throughput.
    if (unlikely (_state != active && _state != waiting_for_delimiter))
        return false;
    if (unlikely (!_in_active)) {
        // Recover from a missed read-activation edge if data is already
        // visible in the inbound pipe.
        if (!_in_pipe->check_read ())
            return false;
        _in_active = true;
    }

    //  Check if there's an item in the pipe.
    if (!_in_pipe->check_read ()) {
        _in_active = false;
        return false;
    }

    //  If the next item in the pipe is message delimiter,
    //  initiate termination process.
    if (_in_pipe->probe (is_delimiter)) {
        msg_t msg;
        const bool ok = _in_pipe->read (&msg);
        zlink_assert (ok);
        process_delimiter ();
        return false;
    }

    return true;
}

bool zlink::pipe_t::read (msg_t *msg_)
{
    if (unlikely (_state != active && _state != waiting_for_delimiter))
        return false;
    if (unlikely (!_in_active)) {
        if (!_in_pipe->check_read ())
            return false;
        _in_active = true;
    }

    while (true) {
        if (!_in_pipe->read (msg_)) {
            _in_active = false;
            return false;
        }

        //  If this is a credential, ignore it and receive next message.
        if (unlikely (msg_->is_credential ())) {
            account_inbound_frame (msg_);
            const int rc = msg_->close ();
            zlink_assert (rc == 0);
        } else {
            break;
        }
    }

    //  If delimiter was read, start termination process of the pipe.
    if (msg_->is_delimiter ()) {
        process_delimiter ();
        return false;
    }

    account_inbound_frame (msg_);

    return true;
}

bool zlink::pipe_t::check_write ()
{
    return check_write_status () == pipe_write_ready;
}

zlink::pipe_write_status_t zlink::pipe_t::check_write_status ()
{
    scoped_fast_lock_t lock (_out_sync);
    //  HWM admission sets _out_active=false until the peer returns write
    //  credit. The route still exists during that interval, so callers must
    //  keep reporting capacity pressure rather than an unreachable peer.
    if (unlikely (!_out_active))
        return pipe_write_hwm_full;
    if (unlikely (_state != active))
        return pipe_write_inactive;

    const bool full = !check_hwm_with_peer_snapshot_unlocked ();

    if (unlikely (full)) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return pipe_write_hwm_full;
    }

    return pipe_write_ready;
}

void zlink::pipe_t::hold_writes_until_transport_pair_ready ()
{
    scoped_fast_lock_t lock (_out_sync);
    _transport_pair_write_held = true;
    _out_active = false;
}

bool zlink::pipe_t::release_writes_for_transport_pair ()
{
    scoped_fast_lock_t lock (_out_sync);
    if (!_transport_pair_write_held)
        return false;
    _transport_pair_write_held = false;
    if (_state != active || !check_hwm_unlocked ())
        return false;
    _out_active = true;
    return true;
}

bool zlink::pipe_t::write (const msg_t *msg_)
{
    // Hot path: PAIR/DEALER steady-state send reaches this path for every
    // message. Keep changes here tightly justified against thread-safe pipe
    // state transitions.
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    const bool full = !check_hwm_with_peer_snapshot_unlocked ();
    if (unlikely (full)) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return false;
    }

    return write_message_unlocked (msg_, true, true);
}

bool zlink::pipe_t::write_no_hwm_check (const msg_t *msg_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    return write_message_unlocked (msg_, false);
}

bool zlink::pipe_t::write_routing_id_and_flush (const msg_t *msg_)
{
    if (!msg_ || !msg_->is_routing_id ()) {
        errno = EINVAL;
        return false;
    }

    scoped_fast_lock_t lock (_out_sync);
    if (_state != active)
        return false;
    if (!write_message_unlocked (msg_, false))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_transport_probe_and_flush (const msg_t *msg_)
{
    if (!msg_ || msg_->size () != 0 || (msg_->flags () & msg_t::more) != 0) {
        errno = EINVAL;
        return false;
    }

    scoped_fast_lock_t lock (_out_sync);
    // Application writes remain blocked until both transport lanes pass
    // admission. A ROUTER probe is queued early so a same-thread peer does not
    // need another API call to process the later lane-ready mailbox command.
    // The peer socket still withholds reads until its own pair is ready.
    if (!_transport_pair_write_held || _state != active)
        return false;
    if (!write_message_unlocked (msg_, false))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_and_flush (const msg_t *msg_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    const bool full = !check_hwm_with_peer_snapshot_unlocked ();
    if (unlikely (full)) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return false;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true, true))
        return false;
    if (!more)
        flush_unlocked ();

    return true;
}

bool zlink::pipe_t::write_no_recursive_hwm_check (const msg_t *msg_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    if (unlikely (!check_hwm_with_peer_snapshot_unlocked ())) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return false;
    }

    return write_message_unlocked (msg_, true);
}

bool zlink::pipe_t::write_and_flush_no_recursive_hwm_check (const msg_t *msg_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    if (unlikely (!check_hwm_with_peer_snapshot_unlocked ())) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return false;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true))
        return false;
    if (!more)
        flush_unlocked ();

    return true;
}

bool zlink::pipe_t::write_single_message_and_flush_no_recursive_hwm_check (const msg_t *msg_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!_out_active || _state != active))
        return false;

    if (unlikely (!check_hwm_with_peer_snapshot_unlocked ())) {
        _out_active = false;
        _waiting_for_byte_credit.store (true, std::memory_order_release);
        return false;
    }

    if (!write_message_unlocked (msg_, true))
        return false;
    flush_unlocked ();
    return true;
}

void zlink::pipe_t::rollback ()
{
    scoped_optional_fast_lock_t lock (&_out_sync);
    rollback_unlocked ();
}

void zlink::pipe_t::flush ()
{
    // Hot path: single-part send flushes on every completed message.
    scoped_fast_lock_t lock (_out_sync);
    flush_unlocked ();
}

void zlink::pipe_t::process_activate_read ()
{
    bool notify = false;
    {
        scoped_fast_lock_t lock (_out_sync);
        if (!_in_active && (_state == active || _state == waiting_for_delimiter)) {
            _in_active = true;
            notify = true;
        }
    }
    if (notify)
        _sink->read_activated (this);
}

void zlink::pipe_t::process_activate_write (uint64_t msgs_read_, uint64_t bytes_read_)
{
    bool notify = false;
    {
        scoped_fast_lock_t lock (_out_sync);

        //  Remember the peer's message sequence number.
        _peers_msgs_read = msgs_read_;
        _peers_bytes_read = bytes_read_;

        if (!_transport_pair_write_held && !_out_active && _state == active
            && check_hwm_unlocked ()) {
            _out_active = true;
            notify = true;
        }
    }

    if (notify)
        _sink->write_activated (this);
}

void zlink::pipe_t::process_hiccup (void *pipe_)
{
    bool notify = false;
    {
        scoped_fast_lock_t lock (_out_sync);

        //  Destroy old outpipe. Note that the read end of the pipe was already
        //  migrated to this thread.
        zlink_assert (_out_pipe);
        _out_pipe->flush ();
        msg_t msg;
        uint64_t drained_message_bytes = 0;
        while (_out_pipe->read (&msg)) {
            const uint64_t frame_bytes = frame_accounted_bytes (&msg);
            drained_message_bytes =
              UINT64_MAX - drained_message_bytes < frame_bytes
                ? UINT64_MAX
                : drained_message_bytes + frame_bytes;
            if (!(msg.flags () & msg_t::more) && !msg.is_routing_id ()
                && !msg.is_credential () && !msg.is_delimiter ()) {
                _msgs_written--;
                _bytes_written =
                  _bytes_written > drained_message_bytes
                    ? _bytes_written - drained_message_bytes
                    : 0;
                drained_message_bytes = 0;
            }
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        LIBZLINK_DELETE (_out_pipe);

        //  Plug in the new outpipe.
        zlink_assert (pipe_);
        _out_pipe = static_cast<upipe_t *> (pipe_);
        _out_active = !_transport_pair_write_held;

        //  If appropriate, notify the user about the hiccup.
        notify = (_state == active && !_transport_pair_write_held);
    }

    if (notify)
        _sink->hiccuped (this);
}

void zlink::pipe_t::process_pipe_term ()
{
    scoped_fast_lock_t lock (_out_sync);
    pipe_debug_log (this, "process_pipe_term", _state, _delay,
                    _endpoint_pair.identifier ().c_str ());

    //  Peer-induced termination is logically one-shot. During cascading
    //  socket teardown we can observe a duplicate term command after
    //  we've already transitioned into a peer-terminated state; treat that as
    //  an idempotent no-op instead of asserting.
    if (_state == waiting_for_delimiter) {
        return;
    }
    if (_state == term_ack_sent || _state == term_req_sent2) {
        send_pipe_term_ack (_peer);
        return;
    }

    zlink_assert (_state == active || _state == delimiter_received || _state == term_req_sent1);

    //  This is the simple case of peer-induced termination. If there are no
    //  more pending messages to read, or if the pipe was configured to drop
    //  pending messages, we can move directly to the term_ack_sent state.
    //  Otherwise we'll hang up in waiting_for_delimiter state till all
    //  pending messages are read.
    if (_state == active) {
        bool pending_to_read = _in_pipe && _in_pipe->check_read ();
        if (pending_to_read && _in_pipe->probe (is_delimiter)) {
            msg_t msg;
            pending_to_read = _in_pipe->read (&msg) ? false : _in_pipe && _in_pipe->check_read ();
        }

        if (_delay && pending_to_read)
            _state = waiting_for_delimiter;
        else {
            _state = term_ack_sent;
            _out_pipe = NULL;
            if (_sink)
                _sink->pipe_peer_terminated (this);
            send_pipe_term_ack (_peer);
        }
    }

    //  Delimiter happened to arrive before the term command. Now we have the
    //  term command as well, so we can move straight to term_ack_sent state.
    else if (_state == delimiter_received) {
        _state = term_ack_sent;
        _out_pipe = NULL;
        if (_sink)
            _sink->pipe_peer_terminated (this);
        send_pipe_term_ack (_peer);
    }

    //  This is the case where both ends of the pipe are closed in parallel.
    //  We simply reply to the request by ack and continue waiting for our
    //  own ack.
    else if (_state == term_req_sent1) {
        _state = term_req_sent2;
        _out_pipe = NULL;
        if (_sink)
            _sink->pipe_peer_terminated (this);
        send_pipe_term_ack (_peer);
    }
}

void zlink::pipe_t::process_pipe_term_ack ()
{
    pipe_debug_log (this, "process_pipe_term_ack", _state, _delay,
                    _endpoint_pair.identifier ().c_str ());
    bool ack_peer = false;
    {
        scoped_fast_lock_t lock (_out_sync);

        //  In term_ack_sent and term_req_sent2 states there's nothing to do.
        //  Simply deallocate the pipe. In term_req_sent1 state we have to ack
        //  the peer before deallocating this side of the pipe.
        //  All the other states are invalid.
        if (_state == term_req_sent1) {
            _out_pipe = NULL;
            ack_peer = true;
        } else
            zlink_assert (_state == term_ack_sent || _state == term_req_sent2);
    }

    detach_peer_backref ();

    //  A locally initiated close that receives the peer's acknowledgement
    //  owes one reciprocal acknowledgement. Queue it before reporting local
    //  completion so cascading socket/context teardown cannot discard the
    //  peer's final close notification.
    if (ack_peer)
        send_pipe_term_ack (_peer);

    //  Notify the user that all the references to the pipe should be dropped.
    zlink_assert (_sink);
    _sink->pipe_terminated (this);

    //  We'll deallocate the inbound pipe, the peer will deallocate the outbound
    //  pipe (which is an inbound pipe from its point of view).
    //  First, delete all the unread messages in the pipe. We have to do it by
    //  hand because msg_t doesn't have automatic destructor. Then deallocate
    //  the ypipe itself.

    if (!_conflate) {
        msg_t msg;
        while (_in_pipe->read (&msg)) {
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
    }

    LIBZLINK_DELETE (_in_pipe);

    //  Pipe objects are always heap-allocated and reference-counted by protocol
    //  state transitions, so termination ack is the canonical final release.
    const lifetime_state_t::transition_t transition =
      _lifetime.complete_termination ();
    zlink_assert (transition != lifetime_state_t::transition_invalid);
    if (transition == lifetime_state_t::transition_delete_owner)
        zlink::release_heap_owned (this);
}

void zlink::pipe_t::process_pipe_hwm (uint64_t inhwm_, uint64_t outhwm_)
{
    set_hwms (inhwm_, outhwm_);
}

void zlink::pipe_t::set_nodelay ()
{
    scoped_fast_lock_t lock (_out_sync);
    _delay = false;

    if (_state == waiting_for_delimiter) {
        rollback_unlocked ();
        _out_pipe = NULL;
        send_pipe_term_ack (_peer);
        _state = term_ack_sent;
    }
}

void zlink::pipe_t::terminate (bool delay_)
{
    scoped_fast_lock_t lock (_out_sync);
    pipe_debug_log (this, "terminate-begin", _state, delay_, _endpoint_pair.identifier ().c_str ());

    //  Overload the value specified at pipe creation.
    _delay = delay_;

    //  If terminate was already called, we can ignore the duplicate invocation.
    if (_state == term_req_sent1 || _state == term_req_sent2) {
        return;
    }
    //  If the pipe is in the final phase of async termination, it's going to
    //  closed anyway. No need to do anything special here.
    if (_state == term_ack_sent) {
        return;
    }
    //  The simple sync termination case. Ask the peer to terminate and wait
    //  for the ack.
    if (_state == active) {
        send_pipe_term (_peer);
        _state = term_req_sent1;
    }
    //  There are still pending messages available, but the user calls
    //  'terminate'. We can act as if all the pending messages were read.
    else if (_state == waiting_for_delimiter && !_delay) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked ();
        _out_pipe = NULL;
        send_pipe_term_ack (_peer);
        _state = term_ack_sent;
    }
    //  If there are pending messages still available, do nothing.
    else if (_state == waiting_for_delimiter) {
    }
    //  We've already got delimiter, but not term command yet. We can ignore
    //  the delimiter and ack synchronously terminate as if we were in
    //  active state.
    else if (_state == delimiter_received) {
        send_pipe_term (_peer);
        _state = term_req_sent1;
    }
    //  There are no other states.
    else {
        zlink_assert (false);
    }

    //  Stop outbound flow of messages.
    _out_active = false;

    if (_out_pipe) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked ();

        //  Write the delimiter into the pipe. Note that watermarks are not
        //  checked; thus the delimiter can be written even when the pipe is full.
        msg_t msg;
        msg.init_delimiter ();
        _out_pipe->write (msg, false);
        flush_unlocked ();
    }
    pipe_debug_log (this, "terminate-end", _state, _delay, _endpoint_pair.identifier ().c_str ());
}

bool zlink::pipe_t::is_delimiter (const msg_t &msg_)
{
    return msg_.is_delimiter ();
}

uint64_t zlink::pipe_t::compute_lwm (uint64_t hwm_)
{
    //  Compute the low water mark. Following point should be taken
    //  into consideration:
    //
    //  1. LWM has to be less than HWM.
    //  2. LWM cannot be set to very low value (such as zero) as after filling
    //     the queue it would start to refill only after all the messages are
    //     read from it and thus unnecessarily hold the progress back.
    //  3. LWM cannot be set to very high value (such as HWM-1) as it would
    //     result in lock-step filling of the queue - if a single message is
    //     read from a full queue, writer thread is resumed to write exactly one
    //     message to the queue and go back to sleep immediately. This would
    //     result in low performance.
    //
    //  Given the 3. it would be good to keep HWM and LWM as far apart as
    //  possible to reduce the thread switching overhead to almost zero.
    //  Let's make LWM 1/2 of HWM.
    return hwm_ / 2 + hwm_ % 2;
}

uint64_t zlink::pipe_t::apply_lwm_hint (uint64_t hwm_,
                                       uint64_t lwm_,
                                       uint64_t lwm_hint_)
{
    if (hwm_ <= 0 || lwm_hint_ <= 0)
        return lwm_;

    uint64_t hinted = lwm_hint_;
    if (hinted >= hwm_)
        hinted = hwm_ - 1;
    if (hinted <= 0)
        hinted = 1;

    return std::min (lwm_, hinted);
}

void zlink::pipe_t::process_delimiter ()
{
    scoped_fast_lock_t lock (_out_sync);
    pipe_debug_log (this, "process_delimiter", _state, _delay,
                    _endpoint_pair.identifier ().c_str ());
    if (_state == term_req_sent1 || _state == term_req_sent2 || _state == term_ack_sent) {
        return;
    }
    zlink_assert (_state == active || _state == waiting_for_delimiter);

    if (_state == active)
        _state = delimiter_received;
    else {
        rollback_unlocked ();
        _out_pipe = NULL;
        send_pipe_term_ack (_peer);
        _state = term_ack_sent;
    }
}

void zlink::pipe_t::hiccup ()
{
    //  If termination is already under way do nothing.
    if (_state != active)
        return;

    //  We'll drop the pointer to the inpipe. From now on, the peer is
    //  responsible for deallocating it.

    //  Create new inpipe, keeping the chunk granularity this pipe was
    //  created with (session pipes use the smaller per-connection chunk).
    if (_conflate)
        _in_pipe = new (std::nothrow) ypipe_conflate_t<msg_t> ();
    else if (_session_pipe)
        _in_pipe = new (std::nothrow) ypipe_t<msg_t, session_pipe_granularity> ();
    else
        _in_pipe = new (std::nothrow) ypipe_t<msg_t, message_pipe_granularity> ();

    alloc_assert (_in_pipe);
    _in_active = true;

    //  Notify the peer about the hiccup.
    send_hiccup (_peer, _in_pipe);
}

void zlink::pipe_t::set_hwms (uint64_t inhwm_, uint64_t outhwm_)
{
    scoped_fast_lock_t lock (_out_sync);
    uint64_t in = inhwm_;
    uint64_t out = outhwm_;

    if (_in_hwm_boost_set) {
        if (inhwm_ == 0 || _in_hwm_boost == 0)
            in = 0;
        else
            in = UINT64_MAX - inhwm_ < _in_hwm_boost
                   ? UINT64_MAX
                   : inhwm_ + _in_hwm_boost;
    }

    if (_out_hwm_boost_set) {
        if (outhwm_ == 0 || _out_hwm_boost == 0)
            out = 0;
        else
            out = UINT64_MAX - outhwm_ < _out_hwm_boost
                    ? UINT64_MAX
                    : outhwm_ + _out_hwm_boost;
    }

    if (_transport_lane == transport_lane_completion) {
        in = transport_pair_policy::completion_hwm (in);
        out = transport_pair_policy::completion_hwm (out);
    }

    _inhwm = in;
    const uint64_t lwm = apply_lwm_hint (
      _inhwm, compute_lwm (in), _lwm_hint);
    _lwm.store (lwm, std::memory_order_relaxed);
    _hwm = out;
}

void zlink::pipe_t::set_lwm_hint (uint64_t lwm_hint_)
{
    scoped_fast_lock_t lock (_out_sync);
    _lwm_hint = lwm_hint_ > 0 ? lwm_hint_ : 0;
    _lwm.store (
      apply_lwm_hint (_inhwm, compute_lwm (_inhwm), _lwm_hint),
      std::memory_order_relaxed);
}

void zlink::pipe_t::set_hwms_boost (uint64_t inhwmboost_, uint64_t outhwmboost_)
{
    scoped_fast_lock_t lock (_out_sync);
    _in_hwm_boost = inhwmboost_;
    _out_hwm_boost = outhwmboost_;
    _in_hwm_boost_set = true;
    _out_hwm_boost_set = true;
}

bool zlink::pipe_t::check_hwm () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    return _out_active && _state == active && check_hwm_unlocked ();
}

zlink::pipe_message_admission_t
zlink::pipe_t::check_hwm_for_message (const msg_t *msg_)
{
    if (!msg_)
        return pipe_message_admission_invalid;

    scoped_optional_fast_lock_t lock (&_out_sync);
    if (_state != active)
        return pipe_message_admission_inactive;
    if (!_out_active || !check_hwm_with_peer_snapshot_unlocked ())
        return pipe_message_admission_hwm_full;
    if (msg_->is_delimiter ())
        return pipe_message_admission_ready;

    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    if (frame_bytes == UINT64_MAX
        || UINT64_MAX - _out_incomplete_bytes < frame_bytes)
        return pipe_message_admission_too_large;
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < payload_bytes)
        return pipe_message_admission_too_large;

    const uint64_t prospective_payload =
      _out_incomplete_payload_bytes + payload_bytes;
    if (_max_message_bytes != 0 && prospective_payload > _max_message_bytes)
        return pipe_message_admission_too_large;
    if ((msg_->flags () & msg_t::more) != 0)
        return pipe_message_admission_ready;

    if (!can_commit_bytes_with_peer_snapshot_unlocked (
          _out_incomplete_bytes + frame_bytes, prospective_payload,
          _out_incomplete_bytes == 0 || _out_multipart_started_empty)) {
        if (_bytes_written > _peers_bytes_read) {
            _out_active = false;
            _waiting_for_byte_credit.store (true, std::memory_order_release);
        }
        return pipe_message_admission_hwm_full;
    }
    return pipe_message_admission_ready;
}

bool zlink::pipe_t::check_hwm_unlocked () const
{
    //  Auxiliary routing and credential frames may already occupy bytes
    //  before the first application message is committed. They must not
    //  consume the empty-pipe liveness exception.
    if (_msgs_written <= _peers_msgs_read)
        return true;

    const uint64_t in_flight =
      _bytes_written > _peers_bytes_read ? _bytes_written - _peers_bytes_read : 0;
    const bool full = _hwm > 0 && in_flight >= _hwm;
    return !full;
}

void zlink::pipe_t::refresh_peer_credit_snapshot_unlocked ()
{
    if (!_peer)
        return;

    const uint64_t peer_msgs_read =
      _peer->_published_msgs_read.load (std::memory_order_relaxed);
    const uint64_t peer_bytes_read =
      _peer->_published_bytes_read.load (std::memory_order_relaxed);
    if (peer_msgs_read > _peers_msgs_read)
        _peers_msgs_read = peer_msgs_read;
    if (peer_bytes_read > _peers_bytes_read)
        _peers_bytes_read = peer_bytes_read;
}

bool zlink::pipe_t::check_hwm_with_peer_snapshot_unlocked ()
{
    if (check_hwm_unlocked ())
        return true;
    refresh_peer_credit_snapshot_unlocked ();
    return check_hwm_unlocked ();
}

void zlink::pipe_t::send_hwms_to_peer (uint64_t inhwm_, uint64_t outhwm_)
{
    pipe_t *peer = NULL;
    {
        scoped_fast_lock_t lock (_out_sync);

        //  HWM propagation is meaningful only while both ends are still in the
        //  steady-state data path. During async termination the peer can be in
        //  the final ack/delete phase, so skip late updates instead of sending
        //  pipe_hwm to a dying peer object.
        if (_state != active || !_peer)
            return;

        peer = _peer;
    }

    send_pipe_hwm (peer, inhwm_, outhwm_);
}

void zlink::pipe_t::set_endpoint_pair (zlink::endpoint_uri_pair_t endpoint_pair_)
{
    set_transport_connection_id (endpoint_pair_.connection_id);
    _endpoint_pair = ZLINK_MOVE (endpoint_pair_);
}

const zlink::endpoint_uri_pair_t &zlink::pipe_t::get_endpoint_pair () const
{
    return _endpoint_pair;
}

void zlink::pipe_t::set_transport_connection_id (uint64_t connection_id_)
{
    if (_transport_lifetime)
        _transport_lifetime->connection_id.store (
          connection_id_, std::memory_order_release);
}

uint64_t zlink::pipe_t::get_transport_connection_id () const
{
    return _transport_lifetime
             ? _transport_lifetime->connection_id.load (
                 std::memory_order_acquire)
             : _endpoint_pair.connection_id;
}

void zlink::pipe_t::set_transport_pair (transport_lane_t lane_,
                                        uint64_t pair_id_,
                                        uint64_t generation_)
{
    _transport_lane = lane_;
    _transport_pair_id = pair_id_;
    _transport_pair_generation = generation_;
}

zlink::transport_lane_t zlink::pipe_t::get_transport_lane () const
{
    return _transport_lane;
}

uint64_t zlink::pipe_t::get_transport_pair_id () const
{
    return _transport_pair_id;
}

uint64_t zlink::pipe_t::get_transport_pair_generation () const
{
    return _transport_pair_generation;
}

void zlink::pipe_t::set_locally_initiated (bool value_)
{
    _locally_initiated = value_;
}

bool zlink::pipe_t::is_locally_initiated () const
{
    return _locally_initiated;
}

void zlink::pipe_t::send_disconnect_msg ()
{
    scoped_fast_lock_t lock (_out_sync);
    if (_disconnect_msg.size () > 0 && _out_pipe) {
        // Rollback any incomplete message in the pipe, and push the disconnect message.
        rollback_unlocked ();

        const bool written = write_message_unlocked (&_disconnect_msg, false);
        zlink_assert (written);
        flush_unlocked ();
        _disconnect_msg.init ();
    }
}

void zlink::pipe_t::set_disconnect_msg (const std::vector<unsigned char> &disconnect_)
{
    _disconnect_msg.close ();
    const int rc = _disconnect_msg.init_buffer (&disconnect_[0], disconnect_.size ());
    errno_assert (rc == 0);
}

void zlink::pipe_t::send_hiccup_msg (const std::vector<unsigned char> &hiccup_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (!hiccup_.empty () && _out_pipe) {
        msg_t msg;
        const int rc = msg.init_buffer (&hiccup_[0], hiccup_.size ());
        errno_assert (rc == 0);

        const bool written = write_message_unlocked (&msg, false);
        zlink_assert (written);
        flush_unlocked ();
    }
}

uint64_t zlink::pipe_t::frame_accounted_bytes (const msg_t *msg_) const
{
    const uint64_t metadata_bytes = static_cast<uint64_t> (sizeof (msg_t));
    if (msg_->is_delimiter () || msg_->is_join () || msg_->is_leave ())
        return metadata_bytes;
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    return UINT64_MAX - payload_bytes < metadata_bytes
             ? UINT64_MAX
             : payload_bytes + metadata_bytes;
}

bool zlink::pipe_t::append_outbound_frame_bytes_unlocked (const msg_t *msg_)
{
    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    if (frame_bytes == UINT64_MAX
        || UINT64_MAX - _out_incomplete_bytes < frame_bytes) {
        errno = EMSGSIZE;
        return false;
    }
    _out_incomplete_bytes += frame_bytes;
    return true;
}

bool zlink::pipe_t::can_commit_bytes_unlocked (
  uint64_t message_bytes_,
  uint64_t payload_bytes_,
  bool allow_empty_pipe_exception_) const
{
    if (_max_message_bytes != 0 && payload_bytes_ > _max_message_bytes)
        return false;
    if (_hwm == 0)
        return true;

    const uint64_t in_flight =
      _bytes_written > _peers_bytes_read ? _bytes_written - _peers_bytes_read : 0;
    if (allow_empty_pipe_exception_ && _msgs_written <= _peers_msgs_read) {
        //  An empty pipe admits one message larger than the HWM so that a
        //  complete message is not rejected for a small HWM alone. An
        //  incomplete multipart may use this exception only when a finite
        //  reader maximum bounds its retained bytes.
        return true;
    }
    if (UINT64_MAX - in_flight < message_bytes_)
        return false;
    return in_flight + message_bytes_ <= _hwm;
}

bool zlink::pipe_t::can_commit_bytes_with_peer_snapshot_unlocked (
  uint64_t message_bytes_,
  uint64_t payload_bytes_,
  bool allow_empty_pipe_exception_)
{
    if (can_commit_bytes_unlocked (
          message_bytes_, payload_bytes_, allow_empty_pipe_exception_))
        return true;
    refresh_peer_credit_snapshot_unlocked ();
    return can_commit_bytes_unlocked (
      message_bytes_, payload_bytes_, allow_empty_pipe_exception_);
}

void zlink::pipe_t::set_max_message_bytes (uint64_t max_message_bytes_)
{
    _max_message_bytes = max_message_bytes_;
}

bool zlink::pipe_t::write_message_unlocked (const msg_t *msg_,
                                            bool enforce_hwm_,
                                            bool enforce_incremental_hwm_)
{
    const uint64_t incomplete_before = _out_incomplete_bytes;
    const uint64_t payload_before = _out_incomplete_payload_bytes;
    const bool multipart_started_empty_before =
      _out_multipart_started_empty;
    if (!append_outbound_frame_bytes_unlocked (msg_))
        return false;
    const uint64_t frame_payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < frame_payload_bytes) {
        _out_incomplete_bytes = incomplete_before;
        errno = EMSGSIZE;
        return false;
    }
    _out_incomplete_payload_bytes += frame_payload_bytes;

    if (_max_message_bytes != 0
        && _out_incomplete_payload_bytes > _max_message_bytes) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        errno = EMSGSIZE;
        return false;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool commits_bytes = !more && !msg_->is_delimiter ();
    if (more && incomplete_before == 0)
        _out_multipart_started_empty =
          _msgs_written <= _peers_msgs_read;
    if ((commits_bytes || enforce_incremental_hwm_) && enforce_hwm_
        && !can_commit_bytes_with_peer_snapshot_unlocked (
          _out_incomplete_bytes, _out_incomplete_payload_bytes,
          !more
            && (incomplete_before == 0 || _out_multipart_started_empty))) {
        const bool exceeds_max_message_size =
          _max_message_bytes != 0
          && _out_incomplete_payload_bytes > _max_message_bytes;
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        if (!exceeds_max_message_size
            && _bytes_written > _peers_bytes_read) {
            _out_active = false;
            _waiting_for_byte_credit.store (true, std::memory_order_release);
        }
        errno = exceeds_max_message_size ? EMSGSIZE : EAGAIN;
        return false;
    }

    _out_pipe->write (*msg_, more);
    if (commits_bytes) {
        const uint64_t message_bytes = _out_incomplete_bytes;
        const uint64_t in_flight =
          _bytes_written > _peers_bytes_read ? _bytes_written - _peers_bytes_read : 0;
        if (enforce_hwm_ && _hwm > 0 && _msgs_written <= _peers_msgs_read
            && (message_bytes > _hwm
                || (UINT64_MAX - in_flight < message_bytes
                    || in_flight + message_bytes > _hwm))) {
            ++_oversize_message_admission_count;
            _oversize_message_admission_max_bytes =
              std::max (_oversize_message_admission_max_bytes, message_bytes);
        }
        if (_conflate) {
            _bytes_written =
              UINT64_MAX - _peers_bytes_read < message_bytes
                ? UINT64_MAX
                : _peers_bytes_read + message_bytes;
            if (!msg_->is_routing_id () && !msg_->is_credential ())
                _msgs_written = _peers_msgs_read + 1;
        } else {
            _bytes_written =
              UINT64_MAX - _bytes_written < message_bytes
                ? UINT64_MAX
                : _bytes_written + message_bytes;
            if (!msg_->is_routing_id () && !msg_->is_credential ())
                ++_msgs_written;
        }
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
    }
    return true;
}

void zlink::pipe_t::account_inbound_frame (const msg_t *msg_)
{
    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    _in_incomplete_bytes =
      frame_bytes == UINT64_MAX || UINT64_MAX - _in_incomplete_bytes < frame_bytes
        ? UINT64_MAX
        : _in_incomplete_bytes + frame_bytes;

    if ((msg_->flags () & msg_t::more) != 0)
        return;

    _bytes_read =
      UINT64_MAX - _bytes_read < _in_incomplete_bytes
        ? UINT64_MAX
        : _bytes_read + _in_incomplete_bytes;
    if (!msg_->is_routing_id () && !msg_->is_credential ())
        ++_msgs_read;
    _published_msgs_read.store (_msgs_read, std::memory_order_relaxed);
    _published_bytes_read.store (_bytes_read, std::memory_order_relaxed);
    _in_incomplete_bytes = 0;

    const uint64_t credit_delta = _bytes_read - _last_credit_bytes_read;
    const uint64_t lwm = _lwm.load (std::memory_order_relaxed);
    const bool lwm_reached = lwm > 0 && credit_delta >= lwm;
    bool blocked_writer_drained = false;
    if (!lwm_reached && credit_delta > 0 && _peer
        && _peer->_waiting_for_byte_credit.load (std::memory_order_acquire))
        blocked_writer_drained = _in_pipe && !_in_pipe->check_read ();
    if (credit_delta > 0 && (lwm_reached || blocked_writer_drained)) {
        _last_credit_bytes_read = _bytes_read;
        send_activate_write (_peer, _msgs_read, _bytes_read);
    }
}

void zlink::pipe_t::rollback_unlocked ()
{
    //  Remove incomplete message from the outbound pipe.
    msg_t msg;
    if (_out_pipe) {
        while (_out_pipe->unwrite (&msg)) {
            zlink_assert (msg.flags () & msg_t::more);
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
    }
    _out_incomplete_bytes = 0;
    _out_incomplete_payload_bytes = 0;
    _out_multipart_started_empty = false;
}

void zlink::pipe_t::flush_unlocked ()
{
    //  The peer does not exist anymore at this point.
    if (_state == term_ack_sent)
        return;

    const bool sleeping = _out_pipe && !_out_pipe->flush ();
    if (sleeping)
        send_activate_read (_peer);
}
