/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdio.h>
#include <new>
#include <stddef.h>

#include "utils/macros.hpp"
#include "utils/config.hpp"
#include "core/pipe.hpp"
#include "core/transport_pair_policy.hpp"
#include "core/ctx.hpp"
#include "core/flow_state_frame.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_peer_weight.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/heap_owner.hpp"

namespace
{
const uint32_t pending_peer_weight_unset = UINT32_MAX;

#ifdef ZLINK_BUILD_TESTS
std::atomic<int> g_stream_packet_allocation_failpoint (
  zlink::stream_packet_allocation_none);
#endif

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

bool consume_if_delimiter (const zlink::msg_t &msg_, void *)
{
    return msg_.is_delimiter ();
}

struct normalized_head_probe_t
{
    zlink::pipe_normalized_head_kind_t kind;
};

void probe_normalized_head (const zlink::msg_t &msg_, void *userdata_)
{
    normalized_head_probe_t *const probe =
      static_cast<normalized_head_probe_t *> (userdata_);

    // Core control frames share the count-1 Application FIFO with public
    // DATA/REQUEST and socket-local REPLY traffic. Classify them before
    // request/reply metadata so they are drained internally at a record
    // boundary and never activated on the public receive path.
    if ((msg_.flags () & zlink::msg_t::command) != 0) {
        probe->kind = zlink::pipe_head_control;
        return;
    }

    unsigned char wire_kind = zlink::zmp_kind_data;
    uint64_t request_sequence = 0;
    if (!msg_.get_request_reply_metadata (&wire_kind, &request_sequence)) {
        probe->kind = zlink::pipe_head_data;
        return;
    }

    if (request_sequence == 0) {
        probe->kind = zlink::pipe_head_invalid;
        return;
    }

    switch (wire_kind) {
        case zlink::zmp_kind_request:
            probe->kind = zlink::pipe_head_request;
            break;
        case zlink::zmp_kind_reply:
            probe->kind = zlink::pipe_head_reply;
            break;
        case zlink::zmp_kind_error_reply:
            probe->kind = zlink::pipe_head_error_reply;
            break;
        default:
            probe->kind = zlink::pipe_head_invalid;
            break;
    }
}

const unsigned char head_reclassify_idle = 0;
const unsigned char head_reclassify_armed = 1;
const unsigned char head_reclassify_queued = 2;
}
#include "core/ypipe.hpp"
#include "core/ypipe_conflate.hpp"

int zlink::pipepair (object_t *parents_[2],
                     pipe_t *pipes_[2],
                     const uint64_t hwms_[2],
                     const bool conflate_[2],
                     bool session_pipe_,
                     transport_lane_t lane_,
                     auto_hwm_role_t role_,
                     bool planning_enabled_,
                     physical_queue_class_t queue_class_,
                     int session_owner_index_)
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

    pipes_[0] = NULL;
    pipes_[1] = NULL;
    zlink_assert (session_owner_index_ >= -1 && session_owner_index_ <= 1);
    zlink_assert (session_pipe_ || session_owner_index_ == -1);

    ctx_t *const ctx = parents_[0]->get_ctx ();
    zlink_assert (ctx == parents_[1]->get_ctx ());
    const physical_queue_class_t resolved_queue_class =
      lane_ == transport_lane_completion ? physical_queue_class_completion
                                         : queue_class_;
    physical_queue_handle_t physical_queues[2];
    if (ctx->create_pipepair_queues (
          hwms_[1], hwms_[0], resolved_queue_class, role_, planning_enabled_,
          &physical_queues[0], &physical_queues[1]) != 0)
        return -1;

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
    const uint64_t initial_connection_id =
      session_pipe_ ? 0 : allocate_connection_id ();
    const uint64_t route_incarnation_id =
      initial_connection_id != 0 ? initial_connection_id
                                 : allocate_connection_id ();
    zlink_assert (route_incarnation_id != 0);
    const std::shared_ptr<transport_lifetime_t> transport_lifetime =
      std::make_shared<transport_lifetime_t> (
        initial_connection_id, route_incarnation_id);

    pipes_[0] = new (std::nothrow)
      pipe_t (parents_[0], upipe1, upipe2, hwms_[1], hwms_[0], conflate_[0],
              session_pipe_, transport_lifetime, physical_queues[0],
              physical_queues[1],
              resolved_queue_class != physical_queue_class_application,
              resolved_queue_class == physical_queue_class_application
                && session_owner_index_ == 0);
    alloc_assert (pipes_[0]);
    pipes_[1] = new (std::nothrow)
      pipe_t (parents_[1], upipe2, upipe1, hwms_[0], hwms_[1], conflate_[1],
              session_pipe_, transport_lifetime, physical_queues[1],
              physical_queues[0],
              resolved_queue_class != physical_queue_class_application,
              resolved_queue_class == physical_queue_class_application
                && session_owner_index_ == 1);
    alloc_assert (pipes_[1]);

    pipes_[0]->set_peer (pipes_[1]);
    pipes_[1]->set_peer (pipes_[0]);
    if (resolved_queue_class == physical_queue_class_application) {
        // Each physical ypipe has one writer and one reader. Keep that
        // relationship in the registry for lazy snapshots only; application
        // writes and reads continue to use their pipe-local byte ledger.
        ctx->_physical_queue_registry.bind_application_pipe_queue (
          physical_queues[0], pipes_[1], pipes_[0]);
        ctx->_physical_queue_registry.bind_application_pipe_queue (
          physical_queues[1], pipes_[0], pipes_[1]);
    }

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

zlink::pipe_stream_packet_state_t::pipe_stream_packet_state_t () :
    stage (prefix_stage),
    storage (separate_storage),
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
    storage = separate_storage;
    memset (prefix, 0, sizeof (prefix));
}

#ifdef ZLINK_BUILD_TESTS
bool zlink::test_consume_stream_packet_allocation_failpoint (
  stream_packet_allocation_failpoint_t failpoint_)
{
    int expected = static_cast<int> (failpoint_);
    return g_stream_packet_allocation_failpoint.compare_exchange_strong (
      expected, static_cast<int> (stream_packet_allocation_none),
      std::memory_order_acq_rel, std::memory_order_acquire);
}
#endif

zlink::pipe_t::pipe_t (object_t *parent_,
                       upipe_t *inpipe_,
                       upipe_t *outpipe_,
                       uint64_t inhwm_,
                       uint64_t outhwm_,
                       bool conflate_,
                       bool session_pipe_,
                       const std::shared_ptr<transport_lifetime_t> &transport_lifetime_,
                       const std::shared_ptr<physical_queue_record_t> &in_physical_queue_,
                       const std::shared_ptr<physical_queue_record_t> &out_physical_queue_,
                       bool registry_accounting_,
                       bool session_io_writer_) :
    object_t (parent_),
    _in_pipe (inpipe_),
    _out_pipe (outpipe_),
    _in_active (true),
    _out_active (true),
    _public_receive_active (false),
    _head_reclassify_wake (head_reclassify_idle),
    _count1_completion_ready_state (count1_completion_idle),
    _count1_completion_ready_next (NULL),
    _out_complete_record_pending (false),
    _transport_pair_write_held (false),
    _remote_flow_paused (false),
    _request_correlation_waiting (false),
    _request_correlation_activation_pending (false),
    _out_owner_message_started (false),
    _out_owner_message_start_pending (false),
    _remote_flow_epoch (0),
    _remote_flow_pause_started_ms (0),
    _waiting_for_byte_credit (false),
    _request_correlation_recovery (false),
    _waiting_for_flow_resume (false),
    _hwm (outhwm_),
    _request_correlation_bytes (0),
    _request_correlation_work (0),
    _request_correlation_count (0),
    _lwm (compute_lwm (inhwm_)),
    _inhwm (inhwm_),
    _lwm_hint (0),
    _msgs_read (0),
    _msgs_written (0),
    _bytes_read (0),
    _bytes_written (0),
    _published_msgs_read (0),
    _published_bytes_read (0),
    _published_incomplete_bytes_read (0),
    _published_outbound_total_bytes (0),
    _published_outbound_provisional_bytes (0),
    _last_credit_bytes_read (0),
    _in_generation (1),
    _out_generation (1),
    _in_incomplete_bytes (0),
    _out_incomplete_bytes (0),
    _out_incomplete_payload_bytes (0),
    _out_multipart_started_empty (false),
    _decoder_multipart_started_empty (false),
    _max_message_bytes (0),
    _oversize_message_admission_count (0),
    _oversize_message_admission_max_bytes (0),
    _peers_msgs_read (0),
    _peers_bytes_read (0),
    _peer (NULL),
    _sink (NULL),
    _state (active),
    _delay (true),
    _router_route_source_published (false),
    _router_route_binding_token (0),
    _connection_ready_event_emitted (false),
    _lifetime (),
    _inbound_read_lifetime (),
    _deferred_socket_msg_termination_next (NULL),
    _conflate (conflate_),
    _session_pipe (session_pipe_),
    _session_io_writer (session_io_writer_),
    _transport_lifetime (transport_lifetime_),
    _in_physical_queue (in_physical_queue_),
    _out_physical_queue (out_physical_queue_),
    _transport_lane (transport_lane_application),
    _transport_lane_count (0),
    _transport_pair_application_ready (false),
    _transport_pair_completion_pipe (NULL),
    _state_active (true),
    _registry_accounting (registry_accounting_),
    _transport_pair_id (0),
    _transport_pair_generation (0),
    _locally_initiated (false),
    _peer_socket_type (0),
    _peer_weight_connection_id (UINT64_MAX),
    _peer_weight (UINT32_MAX),
    _transport_disconnected_event_claimed (false),
    _pending_peer_weight (pending_peer_weight_unset),
    _pending_peer_weight_sequence (0),
    _pending_flow_state (flow_state::receive_flow_running),
    _pending_flow_state_epoch (0),
    _pending_flow_state_sequence (0),
    _pending_flow_state_valid (false),
    _pending_peer_control_sequence (0)
{
    _disconnect_msg.init ();
}

zlink::pipe_t::~pipe_t ()
{
    // Pair teardown normally clears this before the Application pipe can
    // become terminal. Keep destruction self-contained for rejected or
    // partially attached pairs as well.
    if (_transport_pair_completion_pipe) {
        pipe_t *const completion = _transport_pair_completion_pipe;
        _transport_pair_completion_pipe = NULL;
        completion->release_lifetime_ref ();
    }
    retire_physical_queue_endpoints ();
    _disconnect_msg.close ();
}

void zlink::pipe_t::set_peer (pipe_t *peer_)
{
    zlink_assert (peer_);

    // The published pair link owns a lifetime reference. A hot-path acquire
    // load can therefore dereference its result until termination detaches the
    // link after both endpoints have left the data path.
    const bool retained = peer_->retain_lifetime_ref ();
    zlink_assert (retained);
    pipe_t *expected = NULL;
    const bool published = _peer.compare_exchange_strong (
      expected, peer_, std::memory_order_release, std::memory_order_relaxed);
    if (!published)
        peer_->release_lifetime_ref ();
    zlink_assert (published);
}

zlink::pipe_t *zlink::pipe_t::detach_peer_link ()
{
    pipe_t *peer = NULL;
    {
        scoped_fast_lock_t lock (_out_sync);
        // Transfer this endpoint's link reference to the caller. Taking the
        // owner lock makes load+retain snapshots atomic with this exchange.
        peer = _peer.exchange (NULL, std::memory_order_acq_rel);
    }
    if (!peer)
        return NULL;

    // Both endpoints may process their final ack concurrently. Exactly one
    // side can remove a still-published back-link; the other observes that its
    // peer already exchanged it to null. Removing the back-link also releases
    // the lifetime reference that peer owned on this endpoint. The current
    // pipe-term-ack command (or the direct test owner before terminal marking)
    // keeps `this` alive for the remainder of this method.
    pipe_t *expected = this;
    bool removed_back_link = false;
    {
        // Never hold both endpoint locks: concurrent final acknowledgements
        // first exchange their own link, then independently serialize the
        // reciprocal CAS with that endpoint's retained snapshots.
        scoped_fast_lock_t peer_lock (peer->_out_sync);
        removed_back_link = peer->_peer.compare_exchange_strong (
          expected, NULL, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }
    if (removed_back_link)
        release_lifetime_ref ();
    else
        zlink_assert (expected == NULL);

    return peer;
}

void zlink::pipe_t::retire_physical_queue_endpoints ()
{
    ctx_t *const ctx = get_ctx ();
    if (!_registry_accounting) {
        ctx->_physical_queue_registry.unbind_application_pipe_endpoint (
          _in_physical_queue, this, false);
        ctx->_physical_queue_registry.unbind_application_pipe_endpoint (
          _out_physical_queue, this, true);
    }
    ctx->_physical_queue_registry.release_endpoint (&_in_physical_queue);
    ctx->_physical_queue_registry.release_endpoint (&_out_physical_queue);
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

bool zlink::pipe_t::retain_inbound_read_ref ()
{
    return _inbound_read_lifetime.retain ();
}

void zlink::pipe_t::release_inbound_read_ref ()
{
    const lifetime_state_t::transition_t transition =
      _inbound_read_lifetime.release ();
    zlink_assert (transition != lifetime_state_t::transition_invalid);
    if (transition == lifetime_state_t::transition_delete_owner)
        cleanup_inbound_pipe ();
}

bool zlink::pipe_t::has_completed_termination () const
{
    return _lifetime.terminal ();
}

bool zlink::pipe_t::is_lifecycle_active () const
{
    return _state_active.load (std::memory_order_acquire);
}

void zlink::pipe_t::set_transport_pair_application_ready (bool ready_)
{
    _transport_pair_application_ready.store (ready_,
                                             std::memory_order_release);
}

bool zlink::pipe_t::transport_pair_application_ready_cached () const
{
    return _transport_pair_application_ready.load (
      std::memory_order_acquire);
}

void zlink::pipe_t::set_transport_pair_completion_pipe (pipe_t *completion_)
{
    if (completion_) {
        const bool retained = completion_->retain_lifetime_ref ();
        zlink_assert (retained);
        if (!retained)
            completion_ = NULL;
    }

    pipe_t *previous = NULL;
    {
        scoped_fast_lock_t lock (_out_sync);
        previous = _transport_pair_completion_pipe;
        _transport_pair_completion_pipe = completion_;
    }

    if (previous)
        previous->release_lifetime_ref ();
}

zlink::pipe_t *zlink::pipe_t::retain_transport_pair_completion_pipe () const
{
    if (!transport_pair_application_ready_cached ())
        return NULL;

    scoped_fast_lock_t lock (_out_sync);
    if (!transport_pair_application_ready_cached ()
        || !_transport_pair_completion_pipe
        || !_transport_pair_completion_pipe->retain_lifetime_ref ())
        return NULL;
    return _transport_pair_completion_pipe;
}

bool zlink::pipe_t::public_receive_active_cached () const
{
    return _public_receive_active.load (std::memory_order_acquire);
}

void zlink::pipe_t::set_public_receive_active_cached (bool active_)
{
    _public_receive_active.store (active_, std::memory_order_release);
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
    _transport_lifetime->stream_routing_id.store (server_socket_routing_id_,
                                                  std::memory_order_release);
}

uint32_t zlink::pipe_t::get_server_socket_routing_id () const
{
    return _transport_lifetime->stream_routing_id.load (std::memory_order_acquire);
}

void zlink::pipe_t::set_router_socket_routing_id (const blob_t &router_socket_routing_id_)
{
    scoped_fast_lock_t lock (_out_sync);
    _router_socket_routing_id.set_deep_copy (router_socket_routing_id_);
}

void zlink::pipe_t::snapshot_routing_id (blob_t *routing_id_) const
{
    zlink_assert (routing_id_);
    scoped_fast_lock_t lock (const_cast<fast_mutex_t &> (_out_sync));
    routing_id_->set_deep_copy (_router_socket_routing_id);
}

void zlink::pipe_t::publish_router_route_source (
  const blob_t &router_route_source_routing_id_)
{
    scoped_fast_lock_t lock (_out_sync);
    publish_router_route_source_unlocked (
      router_route_source_routing_id_);
}

void zlink::pipe_t::publish_router_route_source_unlocked (
  const blob_t &router_route_source_routing_id_)
{
    if (router_route_source_routing_id_.size () == 0)
        return;

    if (_router_route_source_published.load (std::memory_order_acquire)) {
        const bool same_size =
          _router_route_source_routing_id.size ()
          == router_route_source_routing_id_.size ();
        zlink_assert (same_size);
        if (!same_size)
            return;
        zlink_assert (
          memcmp (_router_route_source_routing_id.data (),
                  router_route_source_routing_id_.data (),
                  router_route_source_routing_id_.size ()) == 0);
        return;
    }

    _router_route_source_routing_id.set_deep_copy (
      router_route_source_routing_id_);
    _router_route_source_published.store (true, std::memory_order_release);
}

void zlink::pipe_t::invalidate_router_route_binding ()
{
    const uint64_t current =
      _router_route_binding_token.load (std::memory_order_relaxed);
    if ((current & 1u) != 0)
        _router_route_binding_token.store (current + 1,
                                           std::memory_order_release);
}

void zlink::pipe_t::publish_router_route_binding ()
{
    publish_router_route_source (_router_socket_routing_id);
    const uint64_t current =
      _router_route_binding_token.load (std::memory_order_relaxed);
    if ((current & 1u) == 0)
        _router_route_binding_token.store (current + 1,
                                           std::memory_order_release);
}

bool zlink::pipe_t::try_copy_router_route_binding (
  unsigned char *routing_id_out_, size_t routing_id_capacity_,
  size_t *routing_id_size_out_, uint64_t *token_out_) const
{
    if (routing_id_size_out_)
        *routing_id_size_out_ = 0;
    if (token_out_)
        *token_out_ = 0;
    if (!routing_id_out_ || !routing_id_size_out_
        || !_router_route_source_published.load (std::memory_order_acquire))
        return false;

    const size_t routing_id_size =
      _router_route_source_routing_id.size ();
    zlink_assert (routing_id_size <= routing_id_capacity_);
    if (routing_id_size > routing_id_capacity_)
        return false;
    memcpy (routing_id_out_, _router_route_source_routing_id.data (),
            routing_id_size);
    *routing_id_size_out_ = routing_id_size;
    if (token_out_)
        *token_out_ = router_route_binding_token ();
    return true;
}

uint64_t zlink::pipe_t::router_route_binding_token () const
{
    const uint64_t token =
      _router_route_binding_token.load (std::memory_order_acquire);
    return (token & 1u) != 0 ? token : 0;
}

const zlink::blob_t &zlink::pipe_t::get_routing_id () const
{
    return _router_socket_routing_id;
}

zlink::pipe_t *zlink::pipe_t::get_peer () const
{
    return _peer.load (std::memory_order_acquire);
}

zlink::pipe_t *zlink::pipe_t::retain_peer_snapshot () const
{
    scoped_fast_lock_t lock (const_cast<fast_mutex_t &> (_out_sync));
    return retain_peer_snapshot_unlocked ();
}

bool zlink::pipe_t::is_session_pipe () const
{
    return _session_pipe;
}

zlink::pipe_t *zlink::pipe_t::retain_peer_snapshot_unlocked () const
{
    pipe_t *const peer = get_peer ();
    return peer && peer->retain_lifetime_ref () ? peer : NULL;
}

void zlink::pipe_t::set_peer_routing_id (const unsigned char *data_, size_t size_)
{
    blob_t routing_id;
    if (data_ && size_ > 0)
        routing_id.set (data_, size_);
    set_router_socket_routing_id (routing_id);
}

void zlink::pipe_t::set_peer_socket_type (int socket_type_)
{
    _peer_socket_type.store (socket_type_, std::memory_order_release);
}

int zlink::pipe_t::get_peer_socket_type () const
{
    return _peer_socket_type.load (std::memory_order_acquire);
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

void zlink::pipe_t::record_peer_weight (uint64_t connection_id_,
                                        uint32_t weight_)
{
    _peer_weight.store (weight_, std::memory_order_release);
    _peer_weight_connection_id.store (connection_id_,
                                      std::memory_order_release);
}

bool zlink::pipe_t::record_peer_weight_if_current (uint64_t connection_id_,
                                                   uint32_t weight_)
{
    scoped_fast_lock_t generation_lock (_transport_lifetime->transport_sync);
    if (weight_ > max_peer_weight
        || _transport_lane != transport_lane_application
        || (_transport_pair_id != 0 && connection_id_ == 0)
        || get_transport_connection_id () != connection_id_
        || !is_lifecycle_active ())
        return false;
    record_peer_weight (connection_id_, weight_);
    return true;
}

bool zlink::pipe_t::peer_weight (uint32_t *weight_out_) const
{
    const uint64_t connection_id =
      _peer_weight_connection_id.load (std::memory_order_acquire);
    if (connection_id == UINT64_MAX
        || connection_id != get_transport_connection_id ())
        return false;
    const uint32_t weight = _peer_weight.load (std::memory_order_acquire);
    if (weight == UINT32_MAX)
        return false;
    if (weight_out_)
        *weight_out_ = weight;
    return true;
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
    pipe_t *const peer = retain_peer_snapshot ();
    if (!peer)
        return 0;

    const uint64_t peer_written = peer->get_msgs_written ();
    peer->release_lifetime_ref ();
    const uint64_t msgs_read = get_msgs_read ();
    if (peer_written <= msgs_read)
        return 0;
    return peer_written - msgs_read;
}

uint64_t zlink::pipe_t::get_snd_pending_bytes () const
{
    scoped_optional_fast_lock_t lock (const_cast<fast_mutex_t *> (&_out_sync));
    uint64_t peer_bytes_read = _peers_bytes_read;
    pipe_t *const peer = retain_peer_snapshot_unlocked ();
    if (peer) {
        const uint64_t published =
          peer->_published_bytes_read.load (std::memory_order_relaxed);
        if (published > peer_bytes_read)
            peer_bytes_read = published;
        peer->release_lifetime_ref ();
    }
    if (_bytes_written <= peer_bytes_read)
        return 0;
    return _bytes_written - peer_bytes_read;
}

uint64_t zlink::pipe_t::get_rcv_pending_bytes_approx () const
{
    pipe_t *const peer = retain_peer_snapshot ();
    if (!peer)
        return 0;

    const uint64_t peer_written = peer->get_bytes_written ();
    peer->release_lifetime_ref ();
    const uint64_t bytes_read = get_bytes_read ();
    if (peer_written <= bytes_read)
        return 0;
    return peer_written - bytes_read;
}

uint64_t zlink::pipe_t::get_snd_queue_accounted_bytes () const
{
    return get_ctx ()->_physical_queue_registry.current_accounted_bytes (
      _out_physical_queue);
}

uint64_t zlink::pipe_t::get_rcv_queue_accounted_bytes () const
{
    return get_ctx ()->_physical_queue_registry.current_accounted_bytes (
      _in_physical_queue);
}

const std::shared_ptr<zlink::physical_queue_record_t> &
zlink::pipe_t::in_physical_queue () const
{
    return _in_physical_queue;
}

const std::shared_ptr<zlink::physical_queue_record_t> &
zlink::pipe_t::out_physical_queue () const
{
    return _out_physical_queue;
}

uint64_t zlink::pipe_t::planned_out_hwm () const
{
    return get_ctx ()->_physical_queue_registry.planned_hwm (
      _out_physical_queue);
}

uint64_t zlink::pipe_t::applied_out_hwm () const
{
    return get_ctx ()->_physical_queue_registry.applied_hwm (
      _out_physical_queue);
}

uint64_t zlink::pipe_t::planned_in_hwm () const
{
    return get_ctx ()->_physical_queue_registry.planned_hwm (
      _in_physical_queue);
}

uint64_t zlink::pipe_t::applied_in_hwm () const
{
    return get_ctx ()->_physical_queue_registry.applied_hwm (
      _in_physical_queue);
}

void zlink::pipe_t::apply_physical_queue_hwm_plan ()
{
    scoped_fast_lock_t lock (_out_sync);
    if (_transport_lane == transport_lane_completion) {
        _hwm = 0;
        _inhwm.store (0, std::memory_order_relaxed);
        _lwm.store (0, std::memory_order_relaxed);
        return;
    }
    _hwm = planned_out_hwm ();
    const uint64_t applied_in = applied_in_hwm ();
    const uint64_t planned_in = planned_in_hwm ();
    _inhwm.store (applied_in, std::memory_order_relaxed);
    // The writer enforces its planned HWM immediately while a shrink can keep
    // the physical queue's applied accounting limit temporarily higher. Base
    // credit wakeups on that same planned window; otherwise LWM can exceed the
    // writer's whole window and delay progress until SNDTIMEO.
    _lwm.store (apply_lwm_hint (planned_in, compute_lwm (planned_in),
                                _lwm_hint),
                std::memory_order_relaxed);
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

void zlink::pipe_t::reset_oversize_message_admission_metrics ()
{
    scoped_fast_lock_t lock (_out_sync);
    _oversize_message_admission_count = 0;
    _oversize_message_admission_max_bytes = 0;
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
    if (_transport_lifetime->stream_connect_event_emitted.load (
          std::memory_order_acquire))
        return false;

    bool expected = false;
    return _transport_lifetime->stream_connect_event_emitted.compare_exchange_strong (
      expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool zlink::pipe_t::mark_connection_ready_event_emitted ()
{
    if (_connection_ready_event_emitted.load (std::memory_order_acquire))
        return false;

    bool expected = false;
    return _connection_ready_event_emitted.compare_exchange_strong (
      expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

zlink::pipe_t::stream_packet_state_t &zlink::pipe_t::stream_packet_state ()
{
    return _transport_lifetime->stream_packet_state;
}

zlink::fast_mutex_t &zlink::pipe_t::transport_sync ()
{
    return _transport_lifetime->transport_sync;
}

void zlink::pipe_t::close_stream_route ()
{
    scoped_fast_lock_t lock (_transport_lifetime->transport_sync);
    _transport_lifetime->stream_route_closed.store (
      true, std::memory_order_release);
    _transport_lifetime->stream_packet_state.reset ();
}

bool zlink::pipe_t::stream_route_closed () const
{
    return _transport_lifetime->stream_route_closed.load (
      std::memory_order_acquire);
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

    // Inspect and consume a delimiter in one queue operation. In particular,
    // a conflate writer cannot replace the inspected frame before it is read.
    msg_t delimiter;
    const ypipe_read_result_t delimiter_result =
      _in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL);
    if (delimiter_result == ypipe_read_empty) {
        _in_active = false;
        return false;
    }
    if (delimiter_result == ypipe_read_consumed) {
        process_delimiter ();
        return false;
    }

    return true;
}

zlink::pipe_normalized_head_kind_t
zlink::pipe_t::probe_normalized_head_kind ()
{
    if (unlikely (_state != active && _state != waiting_for_delimiter))
        return pipe_head_empty;

    // Arm before observing publication. If the writer publishes first, the
    // non-sleeping probe below sees it. If it publishes later, flush changes
    // this marker to queued and sends one explicit activation.
    _head_reclassify_wake.store (head_reclassify_armed,
                                 std::memory_order_seq_cst);

    normalized_head_probe_t probe = {pipe_head_invalid};
    if (!_in_pipe->probe_if_published (&probe_normalized_head, &probe))
        return pipe_head_empty;

    _in_active = true;
    _head_reclassify_wake.store (head_reclassify_idle,
                                 std::memory_order_release);
    return probe.kind;
}

bool zlink::pipe_t::read (msg_t *msg_)
{
    return read_internal<false> (msg_, NULL, NULL, NULL, NULL);
}

bool zlink::pipe_t::requires_record_admission (const msg_t &msg_)
{
    const bool candidate = (msg_.flags () & msg_t::more) != 0
                           || msg_.get_request_reply_metadata (NULL, NULL);
    if (!candidate)
        return false;

    return !msg_.is_credential () && !msg_.is_routing_id ()
           && !msg_.is_delimiter ();
}

bool zlink::pipe_t::read_with_record_admission (
  msg_t *msg_, read_admission_fn *admission_, void *userdata_,
  bool *admission_failed_out_, bool *admission_consumed_out_)
{
    return read_internal<true> (msg_, admission_, userdata_,
                                admission_failed_out_,
                                admission_consumed_out_);
}

namespace
{
struct pipe_read_admission_probe_t
{
    zlink::pipe_t *pipe;
    zlink::pipe_t::read_admission_fn *admission;
    void *userdata;
    int result;
    int error_number;
};

bool invoke_pipe_record_admission (const zlink::msg_t &msg_, void *userdata_)
{
    pipe_read_admission_probe_t *const probe =
      static_cast<pipe_read_admission_probe_t *> (userdata_);
    if (!zlink::pipe_t::requires_record_admission (msg_)) {
        probe->result = 0;
        return true;
    }
    probe->result = probe->admission (probe->pipe, msg_, probe->userdata);
    probe->error_number = errno;
    return probe->result == 0
           || probe->result == zlink::pipe_t::read_admission_reject_consume;
}

}

template <bool WithAdmission>
bool zlink::pipe_t::read_internal (msg_t *msg_,
                                   read_admission_fn *admission_,
                                   void *userdata_,
                                   bool *admission_failed_out_,
                                   bool *admission_consumed_out_)
{
    if (unlikely (_state != active && _state != waiting_for_delimiter))
        return false;
    if (WithAdmission) {
        if (admission_failed_out_)
            *admission_failed_out_ = false;
        if (admission_consumed_out_)
            *admission_consumed_out_ = false;
    }
    if (unlikely (!_in_active)) {
        if (!_in_pipe->check_read ())
            return false;
        _in_active = true;
    }

    bool prefetched_batch_exhausted = false;
    while (true) {
        prefetched_batch_exhausted = false;
        // Raw terminal and private bookkeeping frames do not need a
        // whole-record lease. Classify and conditionally dequeue them in the
        // same queue operation that runs admission for record starts.
        if (WithAdmission && admission_) {
            pipe_read_admission_probe_t probe = {this, admission_, userdata_, 0,
                                                 errno};
            const ypipe_read_result_t read_result =
              _in_pipe->read_if (msg_, &invoke_pipe_record_admission, &probe,
                                 &prefetched_batch_exhausted);
            if (read_result != ypipe_read_consumed) {
                if (probe.result != 0 && admission_failed_out_)
                    *admission_failed_out_ = true;
                if (read_result == ypipe_read_empty)
                    _in_active = false;
                errno = probe.error_number;
                return false;
            }
            if (probe.result == read_admission_reject_consume) {
                // A negative two is a terminal admission error (for example
                // lazy state allocation). The conditional queue read consumed
                // it atomically with the admission decision, unlike EAGAIN
                // capacity rejection, which leaves the frame queued.
                const int saved_errno = probe.error_number;
                if (_registry_accounting) {
                    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
                    const uint64_t counted_messages =
                      counted_pending_message_ref (*msg_);
                    get_ctx ()->_physical_queue_registry.release_committed_frame (
                      _in_physical_queue, frame_bytes, counted_messages);
                }
                account_inbound_frame (msg_, prefetched_batch_exhausted);
                if (admission_failed_out_)
                    *admission_failed_out_ = true;
                if (admission_consumed_out_)
                    *admission_consumed_out_ = true;
                errno = saved_errno;
                return true;
            }
        } else if (!_in_pipe->read (msg_, &prefetched_batch_exhausted)) {
            _in_active = false;
            return false;
        }

        //  If this is a credential, ignore it and receive next message.
        if (unlikely (msg_->is_credential ())) {
            if (_registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _in_physical_queue, frame_accounted_bytes (msg_),
                  counted_pending_message_ref (*msg_));
            account_inbound_frame (msg_, prefetched_batch_exhausted);
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

    if (_registry_accounting) {
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        const uint64_t counted_messages = counted_pending_message_ref (*msg_);
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _in_physical_queue, frame_bytes, counted_messages);
    }
    account_inbound_frame (msg_, prefetched_batch_exhausted);

    return true;
}

int zlink::pipe_t::reserve_inbound_decoder_frame (
  uint64_t payload_bytes_, unsigned char msg_flags_, bool track_multipart_,
  decoder_frame_reservation_t *reservation_storage_,
  decoder_frame_reservation_t **reservation_out_)
{
    if (!reservation_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;

    scoped_optional_fast_lock_t lock (_session_pipe ? NULL : &_out_sync);
    if (_state != active || !_out_physical_queue) {
        errno = ETERM;
        return -1;
    }

    bool multipart_started_empty = track_multipart_
      && (_decoder_multipart_started_empty
          || (_out_incomplete_bytes == 0
              && _bytes_written <= _peers_bytes_read));

    const uint64_t frame_bytes =
      payload_bytes_ > UINT64_MAX - static_cast<uint64_t> (sizeof (msg_t))
        ? UINT64_MAX
        : payload_bytes_ + static_cast<uint64_t> (sizeof (msg_t));
    const uint64_t candidate_bytes =
      frame_bytes == UINT64_MAX
          || _out_incomplete_bytes > UINT64_MAX - frame_bytes
        ? UINT64_MAX
        : _out_incomplete_bytes + frame_bytes;
    const bool more = (msg_flags_ & msg_t::more) != 0;
    bool allow_empty_exception =
      !more && multipart_started_empty;
    const bool byte_credit_ready =
      _hwm == 0 || allow_empty_exception
      || (candidate_bytes != UINT64_MAX
          && (_bytes_written <= _peers_bytes_read
                ? candidate_bytes <= _hwm
                : _bytes_written - _peers_bytes_read <= _hwm
                    && candidate_bytes
                         <= _hwm - (_bytes_written - _peers_bytes_read)));
    if (!byte_credit_ready) {
        refresh_peer_credit_snapshot_unlocked ();
        if (track_multipart_ && !multipart_started_empty
            && _out_incomplete_bytes == 0
            && _bytes_written <= _peers_bytes_read) {
            multipart_started_empty = true;
            allow_empty_exception = !more;
        }
        const uint64_t in_flight =
          _bytes_written > _peers_bytes_read
            ? _bytes_written - _peers_bytes_read
            : 0;
        if (_hwm > 0 && !allow_empty_exception
            && (candidate_bytes == UINT64_MAX || in_flight > _hwm
                || candidate_bytes > _hwm - in_flight)) {
            arm_hwm_credit_wait_unlocked ();
            refresh_peer_credit_snapshot_unlocked ();
            const uint64_t refreshed_in_flight =
              _bytes_written > _peers_bytes_read
                ? _bytes_written - _peers_bytes_read
                : 0;
            const bool refreshed_ready =
              candidate_bytes != UINT64_MAX
              && refreshed_in_flight <= _hwm
              && candidate_bytes <= _hwm - refreshed_in_flight;
            if (refreshed_ready)
                clear_hwm_credit_wait_unlocked ();
            else {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    int rc = 0;
    if (!_registry_accounting) {
        if (reservation_storage_->active) {
            errno = EBUSY;
            return -1;
        }
        reservation_storage_->queue_id = 0;
        reservation_storage_->generation = _out_generation;
        reservation_storage_->frame_bytes = frame_bytes;
        reservation_storage_->payload_bytes = payload_bytes_;
        reservation_storage_->msg_flags = msg_flags_;
        reservation_storage_->multipart_started_empty =
          multipart_started_empty;
        reservation_storage_->active = true;
        *reservation_out_ = reservation_storage_;
    } else {
        decoder_frame_reservation_request_t request;
        request.payload_bytes = payload_bytes_;
        request.msg_flags = msg_flags_;
        request.multipart_started_empty = multipart_started_empty;
        request.qualify_multipart_from_queue_state = false;
        rc = get_ctx ()->_physical_queue_registry.reserve_decoder_frame (
          _out_physical_queue, request, reservation_storage_, reservation_out_);
    }
    if (rc == 0) {
        if (track_multipart_ && (msg_flags_ & msg_t::more) != 0)
            _decoder_multipart_started_empty =
              multipart_started_empty;
        //  Admission normally stays writable for many frames. Publish the
        //  waiter transition only when credit recovery actually changes the
        //  writer state; rewriting the shared marker for every decoded frame
        //  creates avoidable reader-side cache traffic.
        if (!_out_active) {
            _out_active = true;
            _waiting_for_byte_credit.store (false,
                                             std::memory_order_release);
        }
        return 0;
    }

    if (errno != EAGAIN)
        return -1;

    //  Mark the exact writer as waiting before rechecking its physical
    //  direction. A credit return racing this transition will either observe
    //  the waiter or be visible to this second admission attempt.
    arm_hwm_credit_wait_unlocked ();
    refresh_peer_credit_snapshot_unlocked ();
    decoder_frame_reservation_request_t request;
    request.payload_bytes = payload_bytes_;
    request.msg_flags = msg_flags_;
    request.multipart_started_empty = multipart_started_empty;
    request.qualify_multipart_from_queue_state = false;
    rc = get_ctx ()->_physical_queue_registry.reserve_decoder_frame (
      _out_physical_queue, request, reservation_storage_, reservation_out_);
    if (rc == 0) {
        clear_hwm_credit_wait_unlocked ();
        if (track_multipart_ && (msg_flags_ & msg_t::more) != 0)
            _decoder_multipart_started_empty =
              multipart_started_empty;
    }
    return rc;
}

int zlink::pipe_t::write_reserved_decoder_frame (
  msg_t *msg_, decoder_frame_reservation_t **reservation_)
{
    if (!msg_ || !reservation_ || !*reservation_) {
        errno = EFAULT;
        return -1;
    }
    scoped_optional_fast_lock_t lock (_session_pipe ? NULL : &_out_sync);
    if (_state != active || !_out_pipe) {
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        errno = ETERM;
        return -1;
    }

    decoder_frame_reservation_t *const reserved = *reservation_;
    if (!_registry_accounting) {
        const uint64_t incomplete_before = _out_incomplete_bytes;
        *reservation_ = NULL;
        if (!reserved->active
            || reserved->payload_bytes != static_cast<uint64_t> (msg_->size ())
            || reserved->msg_flags != msg_->flags ()) {
            reserved->active = false;
            errno = EPROTO;
            return -1;
        }
        reserved->active = false;

        if (reserved->frame_bytes == UINT64_MAX
            || _out_incomplete_bytes
                 > UINT64_MAX - reserved->frame_bytes
            || _out_incomplete_payload_bytes
                 > UINT64_MAX - reserved->payload_bytes) {
            errno = EMSGSIZE;
            return -1;
        }
        _out_incomplete_bytes += reserved->frame_bytes;
        _out_incomplete_payload_bytes += reserved->payload_bytes;

        const bool more = (reserved->msg_flags & msg_t::more) != 0;
        const bool complete_frame = !more && !msg_->is_delimiter ();
        const uint64_t in_flight =
          _bytes_written > _peers_bytes_read
            ? _bytes_written - _peers_bytes_read
            : 0;
        const bool oversize =
          complete_frame && _hwm > 0 && in_flight == 0
          && _out_incomplete_bytes > _hwm;

        _out_pipe->write (*msg_, more);
        if (complete_frame) {
            const uint64_t message_bytes = _out_incomplete_bytes;
            _bytes_written = UINT64_MAX - _bytes_written < message_bytes
                               ? UINT64_MAX
                               : _bytes_written + message_bytes;
            if (!msg_->is_routing_id () && !msg_->is_credential ())
                ++_msgs_written;
            _out_complete_record_pending = true;
            if (oversize) {
                ++_oversize_message_admission_count;
                _oversize_message_admission_max_bytes =
                  std::max (_oversize_message_admission_max_bytes,
                            message_bytes);
            }
            _out_incomplete_bytes = 0;
            _out_incomplete_payload_bytes = 0;
            _out_multipart_started_empty = false;
            _decoder_multipart_started_empty = false;
        }
        publish_session_outbound_accounting_unlocked (
          more || incomplete_before != 0);
        return 0;
    }

    const uint64_t incomplete_before = _out_incomplete_bytes;
    const uint64_t payload_before = _out_incomplete_payload_bytes;
    const bool multipart_started_empty_before =
      _out_multipart_started_empty;
    if (!append_outbound_frame_bytes_unlocked (msg_)) {
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        return -1;
    }
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < payload_bytes) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        errno = EMSGSIZE;
        return -1;
    }
    _out_incomplete_payload_bytes += payload_bytes;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool complete_frame = !more && !msg_->is_delimiter ();
    const uint64_t in_flight =
      _bytes_written > _peers_bytes_read
        ? _bytes_written - _peers_bytes_read
        : 0;
    bool oversize = complete_frame && _hwm > 0 && in_flight == 0
                    && _out_incomplete_bytes > _hwm;
    bool registry_oversize = false;
    const int commit_rc =
      get_ctx ()->_physical_queue_registry.commit_decoder_frame (
        _out_physical_queue, reservation_, payload_bytes, msg_->flags (),
        counted_pending_message_ref (*msg_), &registry_oversize);
    if (commit_rc != 0) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        return -1;
    }
    oversize = oversize || registry_oversize;

    publish_outbound_frame_unlocked (*msg_, more);
    if (complete_frame) {
        const uint64_t message_bytes = _out_incomplete_bytes;
        _bytes_written = UINT64_MAX - _bytes_written < message_bytes
                           ? UINT64_MAX
                           : _bytes_written + message_bytes;
        if (!msg_->is_routing_id () && !msg_->is_credential ())
            ++_msgs_written;
        _out_complete_record_pending = true;
        if (oversize) {
            ++_oversize_message_admission_count;
            _oversize_message_admission_max_bytes =
              std::max (_oversize_message_admission_max_bytes,
                        message_bytes);
        }
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        _decoder_multipart_started_empty = false;
    }
    publish_session_outbound_accounting_unlocked (
      more || incomplete_before != 0);
    return 0;
}

void zlink::pipe_t::release_decoder_frame_reservation (
  decoder_frame_reservation_t **reservation_)
{
    if (!reservation_ || !*reservation_)
        return;
    decoder_frame_reservation_t *const reservation = *reservation_;
    *reservation_ = NULL;
    reservation->active = false;
}

bool zlink::pipe_t::check_write ()
{
    return check_write_admission () == pipe_message_admission_ready;
}

zlink::pipe_message_admission_t
zlink::pipe_t::write_state_admission_unlocked () const
{
    if (_state != active)
        return pipe_message_admission_inactive;
    if (_transport_pair_write_held)
        return pipe_message_admission_transport_wait;
    if (remote_flow_blocked_unlocked ()) {
        _waiting_for_flow_resume.store (true, std::memory_order_release);
        return pipe_message_admission_transport_wait;
    }
    if (!_out_active)
        return pipe_message_admission_hwm_full;
    return pipe_message_admission_ready;
}

bool zlink::pipe_t::try_reserve_request_correlation (
  uint64_t accounted_bytes_)
{
    if (accounted_bytes_ == 0)
        accounted_bytes_ = 1;

    scoped_fast_lock_t lock (_out_sync);
    if (_state != active) {
        errno = EHOSTUNREACH;
        return false;
    }
    if (_transport_pair_write_held || remote_flow_blocked_unlocked ()) {
        errno = EAGAIN;
        return false;
    }

    const bool overflow =
      UINT64_MAX - _request_correlation_bytes < accounted_bytes_;
    // Small requests use their lifecycle bytes directly. Above 1 KiB the
    // charge grows cubically so a large request cannot create the secure
    // completion backlog that a byte-linear window permits. The empty-pair
    // exception still admits one request larger than the work budget.
    const uint64_t work_charge =
      transport_pair_policy::request_correlation_work_charge (
        accounted_bytes_);
    const uint64_t work_budget =
      transport_pair_policy::request_correlation_work_budget;
    const bool work_overflow =
      UINT64_MAX - _request_correlation_work < work_charge;
    const bool work_window_empty = _request_correlation_work == 0
                                   && _request_correlation_count == 0;
    const bool work_full =
      !work_window_empty
      && (work_charge > work_budget
          || _request_correlation_work > work_budget - work_charge);
    const bool count_full =
      _request_correlation_count
      >= transport_pair_policy::request_correlation_count_budget;
    // Application HWM bounds frames still resident in the physical queue.
    // Correlation outlives that queue credit, so this independent work/count
    // window owns its admission after the peer has dequeued the request.
    if (overflow || work_overflow || work_full || count_full) {
        _request_correlation_waiting = true;
        errno = ENOBUFS;
        return false;
    }

    _request_correlation_bytes += accounted_bytes_;
    _request_correlation_work += work_charge;
    ++_request_correlation_count;
    return true;
}

void zlink::pipe_t::release_request_correlation (uint64_t accounted_bytes_)
{
    bool schedule_activation = false;
    uint64_t generation = 0;
    uint64_t msgs_read = 0;
    uint64_t bytes_read = 0;
    {
        scoped_fast_lock_t lock (_out_sync);
        zlink_assert (accounted_bytes_ <= _request_correlation_bytes);
        const uint64_t work_charge =
          transport_pair_policy::request_correlation_work_charge (
            accounted_bytes_);
        zlink_assert (work_charge <= _request_correlation_work);
        zlink_assert (_request_correlation_count != 0);
        _request_correlation_bytes -= accounted_bytes_;
        _request_correlation_work -= work_charge;
        --_request_correlation_count;
        if (_request_correlation_waiting) {
            _request_correlation_waiting = false;
            _request_correlation_activation_pending = true;
            generation = _out_generation;
            msgs_read = _peers_msgs_read;
            bytes_read = _peers_bytes_read;
            schedule_activation = true;
        }
    }
    if (schedule_activation)
        // Completion may run on the timeout scheduler. Always cross the pipe
        // mailbox so scheduler state is mutated only by the socket owner.
        send_activate_write_deferred (this, generation, msgs_read, bytes_read);
}

bool zlink::pipe_t::remote_flow_blocked_unlocked () const
{
    //  A started message keeps its existing atomicity: the remote PAUSE only
    //  blocks from the next message boundary. A message counts as started once
    //  bytes reached the pipe, and also once the pipe's owner accepted a part
    //  it has not written yet - the classic ROUTER routing-ID part.
    return _remote_flow_paused && _out_incomplete_bytes == 0
           && !_out_multipart_started_empty
           && !_out_owner_message_started;
}

zlink::pipe_message_admission_t zlink::pipe_t::admit_owner_message_start ()
{
    scoped_fast_lock_t lock (_out_sync);
    pipe_message_admission_t admission;
    if (!admit_write_unlocked (&admission))
        return admission;

    _out_owner_message_started = true;
    _out_owner_message_start_pending = true;
    return pipe_message_admission_ready;
}

bool zlink::pipe_t::write_owner_started_message (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!admit_owner_started_write_unlocked (admission_out_)))
        return false;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true, true, admission_out_))
        return false;
    if (!more)
        flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_owner_started_message_observed (
  const msg_t *msg_, pipe_write_observer_fn observer_,
  void *observer_userdata_, pipe_message_admission_t *admission_out_)
{
    if (!observer_) {
        errno = EFAULT;
        return false;
    }
    if (!observer_ (this, observer_userdata_,
                    pipe_write_observer_prepare)) {
        const int observer_errno = errno;
        if (admission_out_) {
            *admission_out_ = observer_errno == ENOBUFS
                                ? pipe_message_admission_request_full
                              : observer_errno == EAGAIN
                                ? pipe_message_admission_transport_wait
                              : observer_errno == EHOSTUNREACH
                                ? pipe_message_admission_inactive
                                : pipe_message_admission_invalid;
        }
        errno = observer_errno == ENOBUFS ? EAGAIN : observer_errno;
        return false;
    }
    if (!retain_lifetime_ref ()) {
        (void) observer_ (this, observer_userdata_,
                          pipe_write_observer_finish);
        errno = EHOSTUNREACH;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }

    bool written = false;
    bool committed = false;
    {
        scoped_fast_lock_t lock (_out_sync);
        written = admit_owner_started_write_unlocked (admission_out_);

        const bool more = (msg_->flags () & msg_t::more) != 0;
        if (written)
            written = write_message_unlocked (msg_, true, true,
                                               admission_out_);
        if (written) {
            committed = observer_ (this, observer_userdata_,
                                   pipe_write_observer_commit);
            zlink_assert (committed);
            if (committed && !more)
                flush_unlocked ();
        }
    }

    (void) observer_ (this, observer_userdata_, pipe_write_observer_finish);
    release_lifetime_ref ();
    if (written && !committed) {
        terminate (false);
        errno = EPROTO;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    return written;
}

bool zlink::pipe_t::remote_flow_blocks_next_message () const
{
    scoped_fast_lock_t lock (_out_sync);
    return remote_flow_blocked_unlocked ();
}

bool zlink::pipe_t::write_state_ready_unlocked (
  pipe_message_admission_t *admission_out_) const
{
    const pipe_message_admission_t admission =
      write_state_admission_unlocked ();
    if (admission_out_)
        *admission_out_ = admission;
    return admission == pipe_message_admission_ready;
}

void zlink::pipe_t::arm_hwm_credit_wait_unlocked ()
{
    _out_active = false;
    _waiting_for_byte_credit.store (true, std::memory_order_release);
    // Pair the waiter publication with the following peer-credit sample. A
    // racing reader either observes the waiter and emits activate_write, or
    // its already-published credit is consumed by the writer's recheck.
    std::atomic_thread_fence (std::memory_order_seq_cst);
}

void zlink::pipe_t::clear_hwm_credit_wait_unlocked ()
{
    _out_active = true;
    _waiting_for_byte_credit.store (false, std::memory_order_release);
}

bool zlink::pipe_t::hwm_credit_ready_unlocked (
  pipe_message_admission_t *admission_out_)
{
    if (check_hwm_with_peer_snapshot_unlocked ())
        return true;
    arm_hwm_credit_wait_unlocked ();
    if (check_hwm_with_peer_snapshot_unlocked ()) {
        clear_hwm_credit_wait_unlocked ();
        return true;
    }
    if (admission_out_)
        *admission_out_ = pipe_message_admission_hwm_full;
    return false;
}

bool zlink::pipe_t::admit_write_unlocked (
  pipe_message_admission_t *admission_out_)
{
    return write_state_ready_unlocked (admission_out_)
           && hwm_credit_ready_unlocked (admission_out_);
}

bool zlink::pipe_t::admit_owner_started_write_unlocked (
  pipe_message_admission_t *admission_out_)
{
    const bool reuse_start_admission = _out_owner_message_start_pending;
    _out_owner_message_start_pending = false;
    if (!reuse_start_admission)
        return admit_write_unlocked (admission_out_);

    //  PAUSE and HWM apply at the next message boundary, but termination and
    //  the initial transport-pair hold can still revoke an accepted start.
    if (_state != active) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }
    if (_transport_pair_write_held) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_transport_wait;
        return false;
    }
    return true;
}

zlink::pipe_message_admission_t zlink::pipe_t::check_write_admission ()
{
    scoped_fast_lock_t lock (_out_sync);
    pipe_message_admission_t admission = pipe_message_admission_invalid;
    (void) admit_write_unlocked (&admission);
    return admission;
}

bool zlink::pipe_t::take_hwm_credit_recovery ()
{
    scoped_fast_lock_t lock (_out_sync);
    const bool recovery =
      _waiting_for_byte_credit.load (std::memory_order_acquire);
    // process_activate_write() releases _out_sync before notifying the socket.
    // If another sender filled the pipe in that interval, _out_active is false
    // and this marker belongs to the new wait, not the activation being
    // delivered. Preserve it so the next peer drain still emits a wake.
    if (recovery && _out_active)
        _waiting_for_byte_credit.store (false, std::memory_order_release);
    return recovery;
}

bool zlink::pipe_t::take_request_correlation_recovery ()
{
    return _request_correlation_recovery.exchange (
      false, std::memory_order_acq_rel);
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
    //  This transition removes only the transport-wait cause. A remote PAUSE
    //  that is still in effect keeps the pipe unwritable.
    return !remote_flow_blocked_unlocked ();
}

bool zlink::pipe_t::transport_pair_writes_released () const
{
    scoped_fast_lock_t lock (_out_sync);
    return !_transport_pair_write_held;
}

bool zlink::pipe_t::remote_flow_paused () const
{
    scoped_fast_lock_t lock (_out_sync);
    return _remote_flow_paused;
}

#ifdef ZLINK_BUILD_TESTS
void zlink::pipe_t::test_flow_probe (bool *out_active_,
                                     bool *hwm_full_,
                                     bool *remote_paused_,
                                     bool *byte_credit_waiter_,
                                     uint64_t *in_flight_bytes_) const
{
    scoped_fast_lock_t lock (_out_sync);
    if (out_active_)
        *out_active_ = _out_active;
    //  Deliberately the cached peer credit, without refreshing it: a test has
    //  to be able to see that the writer still believes it is full.
    if (hwm_full_)
        *hwm_full_ = !check_hwm_unlocked ();
    if (remote_paused_)
        *remote_paused_ = _remote_flow_paused;
    if (byte_credit_waiter_)
        *byte_credit_waiter_ =
          _waiting_for_byte_credit.load (std::memory_order_acquire);
    if (in_flight_bytes_)
        *in_flight_bytes_ = _bytes_written > _peers_bytes_read
                              ? _bytes_written - _peers_bytes_read
                              : 0;
}

#endif

#ifdef ZLINK_BUILD_TESTS
uint64_t zlink::pipe_t::test_frame_accounted_bytes (const msg_t *msg_)
{
    return frame_accounted_bytes (msg_);
}

uint64_t zlink::pipe_t::test_compute_lwm (uint64_t hwm_)
{
    return compute_lwm (hwm_);
}

uint64_t zlink::pipe_t::test_apply_lwm_hint (uint64_t hwm_, uint64_t lwm_,
                                             uint64_t lwm_hint_)
{
    return apply_lwm_hint (hwm_, lwm_, lwm_hint_);
}

#endif

bool zlink::pipe_t::take_flow_resume_recovery ()
{
    scoped_fast_lock_t lock (_out_sync);
    const bool recovery =
      _waiting_for_flow_resume.load (std::memory_order_acquire);
    // A new PAUSE can race the socket callback after the prior RESUME released
    // _out_sync. Keep a marker armed for that newer pause instead of consuming
    // it as part of the older activation.
    if (recovery && !_remote_flow_paused)
        _waiting_for_flow_resume.store (false, std::memory_order_release);
    return recovery;
}

void zlink::pipe_t::process_flow_state (unsigned char state_, uint64_t epoch_)
{
    const unsigned char lane_count = get_transport_lane_count ();
    if (_transport_pair_id == 0 || _transport_pair_generation == 0
        || get_transport_connection_id () == 0 || !is_lifecycle_active ())
        return;

    // Count-2 inproc FLOWSTATE reaches the exact Completion endpoint as an
    // owner command. It must be mapped by the socket's ready pair table before
    // touching the Application pipe that owns send admission and accounting.
    if (_transport_lane == transport_lane_completion) {
        if (lane_count == 2u && _sink)
            _sink->flow_state_received (this, state_, epoch_);
        return;
    }

    // Network decode already maps count-2 control to Application, while
    // count-1 inproc delivers directly to Application. Both converge here.
    if (_transport_lane != transport_lane_application
        || (lane_count != 1u && lane_count != 2u))
        return;

    flow_state_transition_t transition = flow_state_no_transition;
    bool actual_writable = false;
    const bool notify =
      apply_remote_flow_state (state_, epoch_, &transition, &actual_writable);
    //  Observation never gates the send path: this call happens after the
    //  transition already committed, off the per-message write/read path, and
    //  only on an actual PAUSED<->RUNNING flip (never on a stale, duplicate,
    //  or same-state frame).
    if (transition != flow_state_no_transition)
        _sink->flow_state_applied (
          this, transition == flow_state_transition_paused, epoch_,
          actual_writable);
    if (notify)
        _sink->write_activated (this);
}

void zlink::pipe_t::process_peer_weight (uint32_t weight_,
                                         uint64_t connection_id_)
{
    scoped_fast_lock_t generation_lock (_transport_lifetime->transport_sync);
    //  The command retains this exact endpoint until processing completes.
    //  Its immutable transport identity prevents a delayed command from an
    //  old generation (or a recycled connection) changing a replacement.
    if (weight_ > max_peer_weight
        || _transport_lane != transport_lane_application
        || (_transport_pair_id != 0 && connection_id_ == 0)
        || get_transport_connection_id () != connection_id_
        || !is_lifecycle_active ())
        return;

    // The exact pipe owns the latest value even before bind/xattach installs
    // its socket sink. A later scheduler attachment reads the same generation-
    // scoped record, so owner-command ordering cannot lose pre-admission state.
    record_peer_weight (connection_id_, weight_);
    if (_sink)
        _sink->peer_weight_received (this, weight_);
}

bool zlink::pipe_t::apply_remote_flow_state (
  unsigned char state_, uint64_t epoch_,
  flow_state_transition_t *out_transition_, bool *out_actual_writable_)
{
    if (out_transition_)
        *out_transition_ = flow_state_no_transition;
    if (out_actual_writable_)
        *out_actual_writable_ = false;
    const bool paused = state_ != 0;
    bool notify = false;
    {
        scoped_fast_lock_t lock (_out_sync);
        //  A replay whose epoch does not advance is stale. Without this an
        //  attach-time replay queued after a newer acceptance would reinstate
        //  the older state, and the socket record - which already holds the
        //  newer one - would deduplicate every correction away.
        //
        //  0 is the "never set" marker and is invalid at every receiving
        //  layer, this one included: treating it as a reset would let it
        //  override whatever ordering the pipe has already established.
        if (epoch_ == 0 || epoch_ <= _remote_flow_epoch)
            return false;
        _remote_flow_epoch = epoch_;
        if (_remote_flow_paused == paused)
            return false;
        _remote_flow_paused = paused;
        if (out_transition_)
            *out_transition_ = paused ? flow_state_transition_paused
                                       : flow_state_transition_resumed;
        //  Resuming removes only the remote-pause cause. Termination and the
        //  transport-pair hold keep their own state, so the send-ready edge is
        //  published only when every cause is clear.
        if (!paused && _state == active && !_transport_pair_write_held
            && _out_active) {
            //  A send refused by the remote cause never evaluated the HWM, so
            //  no cause currently owns the pending wake. Hand it to the
            //  byte-credit cause using the classic lost-wakeup discipline:
            //  arm first, then re-read the credit the peer has published.
            //
            //  Arming first is what closes the race. A reader that publishes
            //  credit after this store sees the armed waiter and sends the
            //  activation itself; a reader that published before it is picked
            //  up by the fresh re-read below. Deciding from the cached
            //  snapshot instead would miss a sub-LWM read that published
            //  credit while no waiter was registered - that read is the last
            //  one that would ever have qualified.
            //  Arm and re-read form a store-load pair, which is the one order
            //  a release store does not constrain. The fence keeps the re-read
            //  below from being hoisted above the arm above. The reader's own
            //  half of this pair - it publishes credit before reading the
            //  waiter - lives in the inbound accounting path and needs the
            //  matching barrier there; see the worklog.
            //
            //  If _out_active was already false, byte-HWM admission owns the
            //  wake. A remote resume must not replace that decision with the
            //  coarser current-in-flight check: no peer credit was returned,
            //  and the part that reached HWM can still be rejected.
            _out_active = false;
            _waiting_for_byte_credit.store (true, std::memory_order_release);
            std::atomic_thread_fence (std::memory_order_seq_cst);
            if (check_hwm_with_peer_snapshot_unlocked ()) {
                _out_active = true;
                notify = true;
            }
        }
        if (out_actual_writable_)
            *out_actual_writable_ = write_state_ready_unlocked (NULL);
    }
    return notify;
}

void zlink::pipe_t::set_remote_flow_pause_started_ms (uint64_t ms_)
{
    _remote_flow_pause_started_ms = ms_;
}

uint64_t zlink::pipe_t::remote_flow_pause_started_ms () const
{
    return _remote_flow_pause_started_ms;
}

bool zlink::pipe_t::write (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    // Hot path: PAIR/DEALER steady-state send reaches this path for every
    // message. Keep changes here tightly justified against thread-safe pipe
    // state transitions.
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    return write_message_unlocked (msg_, true, true, admission_out_);
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

bool zlink::pipe_t::pending_peer_controls_unlocked () const
{
    // The aggregate sequence is a monotonic allocator, not a slot-presence
    // marker: it is reset only after a drain finishes.  During that drain the
    // final slot is cleared before the reset, so using the aggregate here
    // would manufacture another control forever.  Presence is owned by the
    // two slots themselves.
    return _pending_peer_weight != pending_peer_weight_unset
           || _pending_flow_state_valid;
}

bool zlink::pipe_t::peer_control_slots_enabled_unlocked () const
{
    const unsigned char lane_count =
      _transport_lane_count.load (std::memory_order_acquire);
    if (_transport_pair_id == 0 || _transport_pair_generation == 0
        || !peer_uses_routed_protocol_unlocked ())
        return false;

    if (lane_count == 1u)
        return _transport_lane == transport_lane_application;
    if (lane_count == 2u)
        return _transport_lane == transport_lane_application
               || _transport_lane == transport_lane_completion;
    return false;
}

bool zlink::pipe_t::peer_uses_routed_protocol_unlocked () const
{
    const int peer_type =
      _peer_socket_type.load (std::memory_order_acquire);
    return peer_type == ZLINK_CORE_SOCKET_DEALER
           || peer_type == ZLINK_CORE_SOCKET_ROUTER;
}

void zlink::pipe_t::discard_pending_peer_controls_unlocked ()
{
    _pending_peer_weight = pending_peer_weight_unset;
    _pending_peer_weight_sequence = 0;
    _pending_flow_state = flow_state::receive_flow_running;
    _pending_flow_state_epoch = 0;
    _pending_flow_state_sequence = 0;
    _pending_flow_state_valid = false;
    _pending_peer_control_sequence = 0;
}

uint64_t zlink::pipe_t::next_peer_control_sequence_unlocked ()
{
    if (_pending_peer_control_sequence != UINT64_MAX)
        return ++_pending_peer_control_sequence;

    // Only two slots survive. Rebase them without changing their relative
    // order, then assign the new update a strictly later sequence. This keeps
    // the long-lived socket correct even at the uint64 wrap boundary.
    const bool has_weight =
      _pending_peer_weight != pending_peer_weight_unset;
    const bool has_flow = _pending_flow_state_valid;
    if (has_weight && has_flow) {
        if (_pending_peer_weight_sequence < _pending_flow_state_sequence) {
            _pending_peer_weight_sequence = 1;
            _pending_flow_state_sequence = 2;
        } else {
            _pending_flow_state_sequence = 1;
            _pending_peer_weight_sequence = 2;
        }
        _pending_peer_control_sequence = 2;
    } else if (has_weight) {
        _pending_peer_weight_sequence = 1;
        _pending_peer_control_sequence = 1;
    } else if (has_flow) {
        _pending_flow_state_sequence = 1;
        _pending_peer_control_sequence = 1;
    } else {
        _pending_peer_control_sequence = 0;
    }
    return ++_pending_peer_control_sequence;
}

bool zlink::pipe_t::stage_peer_weight_control_unlocked (uint32_t weight_)
{
    if (weight_ > max_peer_weight)
        return false;
    _pending_peer_weight = weight_;
    _pending_peer_weight_sequence = next_peer_control_sequence_unlocked ();
    return true;
}

bool zlink::pipe_t::stage_flow_state_control_unlocked (
  unsigned char state_, uint64_t epoch_)
{
    if (!flow_state::state_valid (state_) || epoch_ == 0)
        return false;
    _pending_flow_state = state_;
    _pending_flow_state_epoch = epoch_;
    _pending_flow_state_sequence = next_peer_control_sequence_unlocked ();
    _pending_flow_state_valid = true;
    return true;
}

bool zlink::pipe_t::append_pending_peer_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0)
        return false;
    if (_state != active || _transport_pair_write_held || !_out_pipe
        || !_session_pipe)
        return false;

    while (pending_peer_controls_unlocked ()) {
        const bool append_weight =
          _pending_peer_weight != pending_peer_weight_unset
          && (!_pending_flow_state_valid
              || _pending_peer_weight_sequence
                   < _pending_flow_state_sequence);

        msg_t command;
        const int init_rc = command.init ();
        errno_assert (init_rc == 0);
        int frame_rc = 0;
        if (append_weight) {
            frame_rc =
              zmp_peer_weight::init_command (&command, _pending_peer_weight);
        } else {
            flow_state::frame_t frame;
            frame.state = _pending_flow_state;
            frame.epoch = _pending_flow_state_epoch;
            frame_rc = flow_state::init_frame (&command, frame);
        }
        if (frame_rc != 0) {
            const int close_rc = command.close ();
            errno_assert (close_rc == 0);
            return false;
        }

        // Session pull drops a nonzero stamp from a retired transport
        // generation. A hiccup also discards both pending slots before binding
        // a replacement ypipe.
        command.set_transport_connection_id (get_transport_connection_id ());
        const uint64_t control_bytes = frame_accounted_bytes (&command);
        //  A peer-control frame is later dequeued and released through the
        //  same physical-queue registry path as data frames, so commit its
        //  charge here to keep committed accounting balanced. The completion
        //  lane that carries these controls has planned_hwm 0, so this commit
        //  does not impose a byte HWM on control frames. Application-lane pipes
        //  without registry accounting keep the plain write and no release.
        if (_registry_accounting) {
            get_ctx ()->_physical_queue_registry.commit_message (
              _out_physical_queue, control_bytes,
              counted_pending_message_ref (command), false);
            publish_outbound_frame_unlocked (command, false);
        } else {
            _out_pipe->write (command, false);
        }
        _bytes_written =
          control_bytes == UINT64_MAX
              || UINT64_MAX - _bytes_written < control_bytes
            ? UINT64_MAX
            : _bytes_written + control_bytes;
        if (_msgs_written != UINT64_MAX)
            ++_msgs_written;
        _out_complete_record_pending = true;
        const int reset_rc = command.init ();
        errno_assert (reset_rc == 0);

        if (append_weight) {
            _pending_peer_weight = pending_peer_weight_unset;
            _pending_peer_weight_sequence = 0;
        } else {
            _pending_flow_state = flow_state::receive_flow_running;
            _pending_flow_state_epoch = 0;
            _pending_flow_state_sequence = 0;
            _pending_flow_state_valid = false;
        }
        publish_session_outbound_accounting_unlocked (false);
    }
    _pending_peer_control_sequence = 0;
    return true;
}

bool zlink::pipe_t::dispatch_pending_inproc_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0)
        return false;
    if (_state != active || _transport_pair_write_held || !_out_pipe
        || _session_pipe)
        return false;

    pipe_t *const peer = retain_peer_snapshot_unlocked ();
    if (!peer)
        return false;
    const uint64_t source_connection_id = get_transport_connection_id ();
    // Do not take the peer's outbound lock while holding ours. Pair identity
    // and connection id are immutable/atomic; the destination command checks
    // lifecycle on its owner thread before applying the control.
    const bool exact_peer = peer->get_transport_lane () == _transport_lane
                            && peer->get_transport_lane_count ()
                                 == get_transport_lane_count ()
                            && peer->get_transport_pair_id ()
                                 == _transport_pair_id
                            && peer->get_transport_pair_generation ()
                                 == _transport_pair_generation
                            && peer->get_transport_connection_id ()
                                 == source_connection_id
                            && (_transport_pair_id == 0
                                || (source_connection_id != 0
                                    && _transport_pair_generation != 0));
    if (!exact_peer) {
        peer->release_lifetime_ref ();
        return false;
    }

    bool delivered = true;
    while (pending_peer_controls_unlocked ()) {
        const bool deliver_weight =
          _pending_peer_weight != pending_peer_weight_unset
          && (!_pending_flow_state_valid
              || _pending_peer_weight_sequence
                   < _pending_flow_state_sequence);
        if (deliver_weight) {
            // WEIGHT remains Application-only even though the common pending
            // control machinery also serves count-2 Completion FLOWSTATE.
            if (_transport_lane != transport_lane_application
                || peer->get_transport_lane ()
                     != transport_lane_application) {
                delivered = false;
                break;
            }
            delivered = send_peer_weight (
              peer, _pending_peer_weight, source_connection_id);
            if (!delivered)
                break;
            _pending_peer_weight = pending_peer_weight_unset;
            _pending_peer_weight_sequence = 0;
        } else {
            send_flow_state (peer, _pending_flow_state,
                             _pending_flow_state_epoch);
            _pending_flow_state = flow_state::receive_flow_running;
            _pending_flow_state_epoch = 0;
            _pending_flow_state_sequence = 0;
            _pending_flow_state_valid = false;
        }
    }
    if (!pending_peer_controls_unlocked ())
        _pending_peer_control_sequence = 0;
    peer->release_lifetime_ref ();
    return delivered && !pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::flush_pending_peer_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0
        || _state != active || _transport_pair_write_held || !_out_pipe)
        return false;
    flush_unlocked ();
    return !pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_peer_weight_control_and_flush (uint32_t weight_,
                                                         bool defer_flush_)
{
    if (weight_ > max_peer_weight
        || _transport_lane != transport_lane_application
        || _registry_accounting || !peer_control_slots_enabled_unlocked ()) {
        errno = EINVAL;
        return false;
    }

    scoped_fast_lock_t lock (_out_sync);
    if (_state != active || _transport_pair_write_held)
        return false;
    // A terminal wire record (or an inproc owner command) must not overtake an
    // Application multipart. Final commit or rollback publishes the latest
    // surviving controls at the next real boundary.
    if (defer_flush_ || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0)
        return stage_peer_weight_control_unlocked (weight_);
    if (!stage_peer_weight_control_unlocked (weight_))
        return false;
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_flow_state_control_and_flush (
  unsigned char state_, uint64_t epoch_, bool defer_flush_)
{
    const unsigned char lane_count = get_transport_lane_count ();
    const bool topology_control_lane =
      (lane_count == 1u && _transport_lane == transport_lane_application)
      || (lane_count == 2u && _transport_lane == transport_lane_completion);
    if (!flow_state::state_valid (state_) || epoch_ == 0
        || !topology_control_lane || _transport_pair_id == 0
        || _transport_pair_generation == 0
        || !peer_control_slots_enabled_unlocked ()) {
        errno = EINVAL;
        return false;
    }

    scoped_fast_lock_t lock (_out_sync);
    if (_state != active || _transport_pair_write_held)
        return false;
    if (defer_flush_ || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0)
        return stage_flow_state_control_unlocked (state_, epoch_);
    if (!stage_flow_state_control_unlocked (state_, epoch_))
        return false;
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::flush_pending_peer_controls ()
{
    scoped_fast_lock_t lock (_out_sync);
    if (!peer_control_slots_enabled_unlocked ()) {
        discard_pending_peer_controls_unlocked ();
        return false;
    }
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::has_pending_peer_controls ()
{
    scoped_fast_lock_t lock (_out_sync);
    return pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_and_flush (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true, true, admission_out_))
        return false;
    if (!more)
        flush_unlocked ();

    return true;
}

bool zlink::pipe_t::try_write_complete_record_and_flush (
  const msg_t *parts_, size_t part_count_)
{
    if (!parts_ || part_count_ < 2)
        return false;

    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!write_state_ready_unlocked (NULL))
        || _registry_accounting || _conflate || !_out_pipe
        || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0
        || _out_multipart_started_empty || _out_owner_message_started
        || _out_owner_message_start_pending)
        return false;

    uint64_t record_bytes = 0;
    uint64_t payload_bytes = 0;
    for (size_t i = 0; i != part_count_; ++i) {
        const msg_t &part = parts_[i];
        const bool expected_more = i + 1 < part_count_;
        if (!part.check ()
            || ((part.flags () & msg_t::more) != 0) != expected_more
            || part.is_delimiter () || part.is_join () || part.is_leave ()
            || part.is_routing_id () || part.is_credential ())
            return false;

        const uint64_t frame_bytes = frame_accounted_bytes (&part);
        const uint64_t part_payload = static_cast<uint64_t> (part.size ());
        if (frame_bytes == UINT64_MAX
            || UINT64_MAX - record_bytes < frame_bytes
            || UINT64_MAX - payload_bytes < part_payload)
            return false;
        record_bytes += frame_bytes;
        payload_bytes += part_payload;
    }

    // This subset deliberately excludes the empty-pipe oversize exception.
    // The generic path owns incremental HWM refusal, waiter publication, and
    // retry/rollback whenever the whole record is not already admissible.
    if ((_max_message_bytes != 0 && payload_bytes > _max_message_bytes)
        || !can_commit_bytes_with_peer_snapshot_unlocked (
          record_bytes, payload_bytes, false))
        return false;

    for (size_t i = 0; i != part_count_; ++i) {
        const bool written =
          write_message_unlocked (&parts_[i], false, false, NULL);
        zlink_assert (written);
        LIBZLINK_UNUSED (written);
    }
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_no_recursive_hwm_check (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    return write_message_unlocked (msg_, true, false, admission_out_);
}

bool zlink::pipe_t::write_single_message_and_flush_no_recursive_hwm_check (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    scoped_fast_lock_t lock (_out_sync);
    if (unlikely (!write_state_ready_unlocked (admission_out_)))
        return false;

    // The distributor, ROUTER, and STREAM direct-send paths own a complete
    // single application message. Keep that explicit contract out of the generic
    // multipart/registry state machine: it avoids provisional counter writes
    // and repeated policy branches for every subscriber while preserving the
    // same byte-HWM decision under _out_sync.
    if (likely (!_registry_accounting && !_conflate
                && (msg_->flags () & msg_t::more) == 0
                && !msg_->is_delimiter ()
                && _out_incomplete_bytes == 0
                && _out_incomplete_payload_bytes == 0
                && !_out_multipart_started_empty
                && !_out_owner_message_started
                && !_out_owner_message_start_pending)) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        if (unlikely (frame_bytes == UINT64_MAX
                      || (_max_message_bytes != 0
                          && payload_bytes > _max_message_bytes))) {
            errno = EMSGSIZE;
            if (admission_out_)
                *admission_out_ = pipe_message_admission_too_large;
            return false;
        }

        if (unlikely (!can_commit_bytes_with_peer_snapshot_unlocked (
                        frame_bytes, payload_bytes, true))) {
            bool credit_ready = false;
            if (_bytes_written > _peers_bytes_read) {
                arm_hwm_credit_wait_unlocked ();
                credit_ready = can_commit_bytes_with_peer_snapshot_unlocked (
                  frame_bytes, payload_bytes, true);
                if (credit_ready)
                    clear_hwm_credit_wait_unlocked ();
            }
            if (!credit_ready) {
                errno = EAGAIN;
                if (admission_out_)
                    *admission_out_ = pipe_message_admission_hwm_full;
                return false;
            }
        }

        _out_pipe->write (*msg_, false);
        const uint64_t in_flight =
          _bytes_written > _peers_bytes_read
            ? _bytes_written - _peers_bytes_read
            : 0;
        if (_hwm > 0 && in_flight == 0 && frame_bytes > _hwm) {
            ++_oversize_message_admission_count;
            _oversize_message_admission_max_bytes = std::max (
              _oversize_message_admission_max_bytes, frame_bytes);
        }
        _bytes_written = UINT64_MAX - _bytes_written < frame_bytes
                           ? UINT64_MAX
                           : _bytes_written + frame_bytes;
        if (!msg_->is_routing_id () && !msg_->is_credential ())
            ++_msgs_written;
        _out_complete_record_pending = true;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_ready;
        publish_session_outbound_accounting_unlocked (false);
        flush_unlocked ();
        return true;
    }

    if (unlikely (!hwm_credit_ready_unlocked (admission_out_)))
        return false;

    if (!write_message_unlocked (msg_, true, false, admission_out_))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_message_observed (
  const msg_t *msg_, pipe_write_observer_fn observer_,
  void *observer_userdata_, pipe_message_admission_t *admission_out_)
{
    if (!observer_ || !msg_) {
        errno = EINVAL;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    if (!observer_ (this, observer_userdata_,
                    pipe_write_observer_prepare)) {
        const int observer_errno = errno;
        if (admission_out_) {
            *admission_out_ = observer_errno == ENOBUFS
                                ? pipe_message_admission_request_full
                              : observer_errno == EAGAIN
                                ? pipe_message_admission_transport_wait
                              : observer_errno == EHOSTUNREACH
                                ? pipe_message_admission_inactive
                                : pipe_message_admission_invalid;
        }
        errno = observer_errno == ENOBUFS ? EAGAIN : observer_errno;
        return false;
    }
    if (!retain_lifetime_ref ()) {
        (void) observer_ (this, observer_userdata_,
                          pipe_write_observer_finish);
        errno = EHOSTUNREACH;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }

    bool written = false;
    bool committed = false;
    {
        scoped_fast_lock_t lock (_out_sync);
        written = admit_write_unlocked (admission_out_);
        const bool more = (msg_->flags () & msg_t::more) != 0;
        if (written)
            written = write_message_unlocked (msg_, true, more,
                                               admission_out_);
        if (written) {
            committed = observer_ (this, observer_userdata_,
                                   pipe_write_observer_commit);
            zlink_assert (committed);
            if (committed && !more)
                flush_unlocked ();
        }
    }

    (void) observer_ (this, observer_userdata_, pipe_write_observer_finish);
    release_lifetime_ref ();
    if (written && !committed) {
        terminate (false);
        errno = EPROTO;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    return written;
}

void zlink::pipe_t::rollback ()
{
    scoped_optional_fast_lock_t lock (&_out_sync);
    rollback_unlocked ();
}

bool zlink::pipe_t::rollback_incomplete ()
{
    scoped_optional_fast_lock_t lock (&_out_sync);
    const bool incomplete =
      _out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0
      || _out_multipart_started_empty || _out_owner_message_started
      || _out_owner_message_start_pending;
    if (incomplete)
        rollback_unlocked ();
    return incomplete;
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
        if (_state == active || _state == waiting_for_delimiter) {
            if (!_in_active) {
                _in_active = true;
                notify = true;
            } else if (_head_reclassify_wake.load (
                         std::memory_order_acquire)
                         != head_reclassify_idle
                       && peer_uses_routed_protocol_unlocked ()
                       && _transport_pair_id != 0
                       && _transport_lane == transport_lane_application
                       && _transport_lane_count.load (
                            std::memory_order_acquire)
                            == 1u
                       && (!_session_pipe || !_session_io_writer)) {
                notify =
                  _head_reclassify_wake.exchange (
                    head_reclassify_idle, std::memory_order_acq_rel)
                  != head_reclassify_idle;
            }
        }
    }
    if (notify)
        _sink->read_activated (this);
}

void zlink::pipe_t::process_activate_write (uint64_t generation_,
                                            uint64_t msgs_read_,
                                            uint64_t bytes_read_)
{
    bool notify = false;
    {
        scoped_fast_lock_t lock (_out_sync);

        // Correlation release uses this command only to cross the owner
        // mailbox. Its wake remains valid even if a reconnect advanced the
        // byte-credit generation before the owner processes the command.
        if (_request_correlation_activation_pending) {
            _request_correlation_activation_pending = false;
            // Preserve the cause until the socket consumes the corresponding
            // write activation. An ordinary send may have succeeded after the
            // request was rejected, but that must not consume this wake.
            _request_correlation_recovery.store (true,
                                                  std::memory_order_release);
            notify = _state == active && _out_active
                     && !_transport_pair_write_held
                     && !remote_flow_blocked_unlocked ()
                     && check_hwm_unlocked ();
        }

        if (generation_ == _out_generation) {
            //  Remember the peer's message sequence number.
            _peers_msgs_read = msgs_read_;
            _peers_bytes_read = bytes_read_;

            if (!_transport_pair_write_held && !_out_active && _state == active
                && check_hwm_unlocked ()) {
                _out_active = true;
                //  Byte credit removes only the HWM cause. While the peer keeps
                //  this pipe PAUSED the send-ready edge stays suppressed; the
                //  resume transition publishes it once every cause is clear.
                notify = !remote_flow_blocked_unlocked ();
            }
        }
    }

    if (notify)
        _sink->write_activated (this);
}

void zlink::pipe_t::process_hiccup (void *pipe_, uint64_t generation_)
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
            if (!msg.is_delimiter () && _registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _out_physical_queue, frame_bytes,
                  counted_pending_message_ref (msg));
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
        release_discarded_pipe_accounting (_out_pipe,
                                           _out_physical_queue);
        const uint64_t incomplete_bytes = _out_incomplete_bytes;
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        //  A hiccup discards the outbound queue, including whatever the owner
        //  had accepted for the message in progress.
        _out_owner_message_started = false;
        _out_owner_message_start_pending = false;
        _decoder_multipart_started_empty = false;
        // A deferred command belongs to the discarded transport generation.
        // The session owner resynchronizes the current absolute policy after
        // publishing the replacement connection id.
        discard_pending_peer_controls_unlocked ();
        if (incomplete_bytes > 0 && _registry_accounting)
            get_ctx ()->_physical_queue_registry.rollback_provisional (
              _out_physical_queue, incomplete_bytes);
        LIBZLINK_DELETE (_out_pipe);

        //  Plug in the new outpipe.
        zlink_assert (pipe_);
        _out_pipe = static_cast<upipe_t *> (pipe_);
        _out_active = !_transport_pair_write_held;
        _out_generation = generation_;
        _msgs_written = 0;
        _out_complete_record_pending = false;
        _bytes_written = 0;
        _peers_msgs_read = 0;
        _peers_bytes_read = 0;
        _waiting_for_byte_credit.store (false, std::memory_order_release);
        publish_session_outbound_accounting_unlocked (true);

        //  If appropriate, notify the user about the hiccup.
        notify = (_state == active && !_transport_pair_write_held);
    }

    if (notify)
        _sink->hiccuped (this);
}

void zlink::pipe_t::transition_to_inactive_state_unlocked (
  lifecycle_state_t state_)
{
    zlink_assert (state_ != active);
    _state = state_;
    _state_active.store (false, std::memory_order_release);
}

void zlink::pipe_t::acknowledge_peer_termination_unlocked (
  lifecycle_state_t state_)
{
    transition_to_inactive_state_unlocked (state_);
    _out_pipe = NULL;
    if (_sink)
        _sink->pipe_peer_terminated (this);
    (void) send_pipe_term_ack (get_peer ());
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
        (void) send_pipe_term_ack (get_peer ());
        return;
    }

    zlink_assert (_state == active || _state == delimiter_received || _state == term_req_sent1);

    //  This is the simple case of peer-induced termination. If there are no
    //  more pending messages to read, or if the pipe was configured to drop
    //  pending messages, we can move directly to the term_ack_sent state.
    //  Otherwise we'll hang up in waiting_for_delimiter state till all
    //  pending messages are read.
    if (_state == active) {
        bool pending_to_read = false;
        if (_in_pipe) {
            msg_t delimiter;
            const ypipe_read_result_t delimiter_result =
              _in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL);
            pending_to_read = delimiter_result == ypipe_read_rejected;
        }

        if (_delay && pending_to_read) {
            transition_to_inactive_state_unlocked (waiting_for_delimiter);
        } else {
            acknowledge_peer_termination_unlocked (term_ack_sent);
        }
    }

    //  Delimiter happened to arrive before the term command. Now we have the
    //  term command as well, so we can move straight to term_ack_sent state.
    else if (_state == delimiter_received) {
        acknowledge_peer_termination_unlocked (term_ack_sent);
    }

    //  This is the case where both ends of the pipe are closed in parallel.
    //  We simply reply to the request by ack and continue waiting for our
    //  own ack.
    else if (_state == term_req_sent1) {
        acknowledge_peer_termination_unlocked (term_req_sent2);
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

    pipe_t *const peer = detach_peer_link ();

    //  A locally initiated close that receives the peer's acknowledgement
    //  owes one reciprocal acknowledgement. Queue it before reporting local
    //  completion so cascading socket/context teardown cannot discard the
    //  peer's final close notification. The captured pair-link reference pins
    //  this exact peer until send_pipe_term_ack acquires the command reference.
    if (ack_peer) {
        zlink_assert (peer);
        const bool queued = send_pipe_term_ack (peer);
        zlink_assert (queued);
    }
    if (peer)
        peer->release_lifetime_ref ();

    //  Notify the user that all the references to the pipe should be dropped.
    zlink_assert (_sink);
    _sink->pipe_terminated (this);

    // A Completion reader can still be outside the socket receive mutex after
    // pipe_terminated() returns. Mark the inbound queue terminal now and
    // delete it only when no retained reader can dereference it. This state is
    // independent of the object lifetime reference: the latter pins `this`,
    // while this one pins `_in_pipe` and its physical-queue endpoints.
    const lifetime_state_t::transition_t inbound_transition =
      _inbound_read_lifetime.complete_termination ();
    zlink_assert (inbound_transition != lifetime_state_t::transition_invalid);
    if (inbound_transition == lifetime_state_t::transition_delete_owner)
        cleanup_inbound_pipe ();

    //  Pipe objects are always heap-allocated and reference-counted by protocol
    //  state transitions, so termination ack is the canonical final release.
    const lifetime_state_t::transition_t transition =
      _lifetime.complete_termination ();
    zlink_assert (transition != lifetime_state_t::transition_invalid);
    if (transition == lifetime_state_t::transition_delete_owner)
        zlink::release_heap_owned (this);
}

void zlink::pipe_t::cleanup_inbound_pipe ()
{
    upipe_t *const inbound = _in_pipe;
    if (!inbound)
        return;

    // We own the terminal transition of _inbound_read_lifetime, so no new
    // reader can enter and the last retained reader has already left.
    if (!_conflate) {
        msg_t msg;
        while (inbound->read (&msg)) {
            if (!msg.is_delimiter () && _registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _in_physical_queue, frame_accounted_bytes (&msg),
                  counted_pending_message_ref (msg));
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
    }

    release_discarded_pipe_accounting (inbound, _in_physical_queue);
    LIBZLINK_DELETE (_in_pipe);
    retire_physical_queue_endpoints ();
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
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
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
        send_pipe_term (get_peer ());
        transition_to_inactive_state_unlocked (term_req_sent1);
    }
    //  There are still pending messages available, but the user calls
    //  'terminate'. We can act as if all the pending messages were read.
    else if (_state == waiting_for_delimiter && !_delay) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
    }
    //  If there are pending messages still available, do nothing.
    else if (_state == waiting_for_delimiter) {
    }
    //  We've already got delimiter, but not term command yet. We can ignore
    //  the delimiter and ack synchronously terminate as if we were in
    //  active state.
    else if (_state == delimiter_received) {
        send_pipe_term (get_peer ());
        transition_to_inactive_state_unlocked (term_req_sent1);
    }
    //  There are no other states.
    else {
        zlink_assert (false);
    }

    //  Stop outbound flow of messages.
    _out_active = false;

    if (_out_pipe) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked (false);

        //  Write the delimiter into the pipe. Note that watermarks are not
        //  checked; thus the delimiter can be written even when the pipe is full.
        msg_t msg;
        msg.init_delimiter ();
        publish_outbound_frame_unlocked (msg, false);
        flush_unlocked ();
    }
    pipe_debug_log (this, "terminate-end", _state, _delay, _endpoint_pair.identifier ().c_str ());
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

    if (_state == active) {
        transition_to_inactive_state_unlocked (delimiter_received);
    }
    else {
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
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
    get_ctx ()->_physical_queue_registry.advance_generation (_in_physical_queue);
    _in_generation = get_ctx ()->_physical_queue_registry.generation (
      _in_physical_queue);
    _msgs_read = 0;
    _bytes_read = 0;
    _published_incomplete_bytes_read.store (0, std::memory_order_release);
    _published_msgs_read.store (0, std::memory_order_release);
    _published_bytes_read.store (0, std::memory_order_release);
    _last_credit_bytes_read = 0;
    _in_incomplete_bytes = 0;

    //  Notify the peer about the hiccup.
    send_hiccup (get_peer (), _in_pipe, _in_generation);
}

void zlink::pipe_t::set_hwms (uint64_t inhwm_, uint64_t outhwm_)
{
    uint64_t in = inhwm_;
    uint64_t out = outhwm_;
    physical_queue_handle_t in_queue;
    physical_queue_handle_t out_queue;
    {
        scoped_fast_lock_t lock (_out_sync);
        if (_transport_lane == transport_lane_completion)
            in = out = 0;
        in_queue = _in_physical_queue;
        out_queue = _out_physical_queue;
    }

    // Updating a shrinking application target samples its writer pipe. Never
    // carry this endpoint's outbound lock into that registry path: the two
    // endpoints can receive HWM updates concurrently and would otherwise take
    // their `_out_sync` locks in opposite order.
    get_ctx ()->_physical_queue_registry.update_hwm_target (
      in_queue, in);
    get_ctx ()->_physical_queue_registry.update_hwm_target (
      out_queue, out);

    // Concurrent HWM updates may overlap outside the lock. Read the registry's
    // latest values only at publication time so the last publisher cannot
    // install a stale target captured before a newer update.
    scoped_fast_lock_t lock (_out_sync);
    in = get_ctx ()->_physical_queue_registry.applied_hwm (in_queue);
    const uint64_t planned_in =
      get_ctx ()->_physical_queue_registry.planned_hwm (in_queue);
    out = get_ctx ()->_physical_queue_registry.planned_hwm (out_queue);
    _inhwm.store (in, std::memory_order_relaxed);
    const uint64_t lwm = apply_lwm_hint (
      planned_in, compute_lwm (planned_in), _lwm_hint);
    _lwm.store (lwm, std::memory_order_relaxed);
    _hwm = out;
}

void zlink::pipe_t::set_lwm_hint (uint64_t lwm_hint_)
{
    scoped_fast_lock_t lock (_out_sync);
    _lwm_hint = _transport_lane == transport_lane_completion
                  ? 0
                  : lwm_hint_;
    const uint64_t planned_in = planned_in_hwm ();
    _lwm.store (
      apply_lwm_hint (planned_in, compute_lwm (planned_in),
                      _lwm_hint),
      std::memory_order_relaxed);
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
    if (_transport_pair_write_held)
        return pipe_message_admission_transport_wait;
    if (!_out_active) {
        // dist_t keeps a pipe in its matching set when message preflight is
        // rejected, so it can safely consume credit published by the peer
        // before the owner processes the matching activate_write command.
        // The generic check_hwm() probe must remain passive because its
        // callers may already have removed the pipe from their active set.
        if (!check_hwm_with_peer_snapshot_unlocked ())
            return pipe_message_admission_hwm_full;
        clear_hwm_credit_wait_unlocked ();
    }
    if (!check_hwm_with_peer_snapshot_unlocked ()) {
        arm_hwm_credit_wait_unlocked ();
        if (!check_hwm_with_peer_snapshot_unlocked ())
            return pipe_message_admission_hwm_full;
        clear_hwm_credit_wait_unlocked ();
    }
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
    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!can_commit_bytes_with_peer_snapshot_unlocked (
          _out_incomplete_bytes + frame_bytes, prospective_payload,
          !more
            && (_out_incomplete_bytes == 0
                || _out_multipart_started_empty))) {
        bool credit_ready = false;
        if (_bytes_written > _peers_bytes_read) {
            arm_hwm_credit_wait_unlocked ();
            credit_ready = can_commit_bytes_with_peer_snapshot_unlocked (
              _out_incomplete_bytes + frame_bytes, prospective_payload,
              !more
                && (_out_incomplete_bytes == 0
                    || _out_multipart_started_empty));
            if (credit_ready)
                clear_hwm_credit_wait_unlocked ();
        }
        if (!credit_ready)
            return pipe_message_admission_hwm_full;
    }
    return pipe_message_admission_ready;
}

bool zlink::pipe_t::check_hwm_unlocked () const
{
    const uint64_t in_flight =
      _bytes_written > _peers_bytes_read ? _bytes_written - _peers_bytes_read : 0;
    const bool full = _hwm > 0 && in_flight >= _hwm;
    return !full;
}

void zlink::pipe_t::refresh_peer_credit_snapshot_unlocked ()
{
    pipe_t *const peer = get_peer ();
    if (!peer)
        return;

    const uint64_t peer_msgs_read =
      peer->_published_msgs_read.load (std::memory_order_acquire);
    const uint64_t peer_bytes_read =
      peer->_published_bytes_read.load (std::memory_order_acquire);
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
        if (_state != active)
            return;

        peer = retain_peer_snapshot_unlocked ();
        if (!peer)
            return;
    }

    send_pipe_hwm (peer, inhwm_, outhwm_);
    peer->release_lifetime_ref ();
}

void zlink::pipe_t::set_endpoint_pair (zlink::endpoint_uri_pair_t endpoint_pair_)
{
    // Static endpoint metadata is published exactly once before the pipe is
    // exposed to its socket/session owner. Reconnect identity is the only
    // mutable member and publishes independently through its atomic wrapper.
    zlink_assert (_endpoint_pair.local_type == endpoint_type_none);
    const uint64_t connection_id = endpoint_pair_.connection_id.load ();
    _endpoint_pair = ZLINK_MOVE (endpoint_pair_);
    set_transport_connection_id (connection_id);
}

const zlink::endpoint_uri_pair_t &zlink::pipe_t::get_endpoint_pair () const
{
    return _endpoint_pair;
}

void zlink::pipe_t::set_transport_connection_id (uint64_t connection_id_)
{
    if (_transport_lifetime) {
        scoped_fast_lock_t generation_lock (_transport_lifetime->transport_sync);
        _transport_lifetime->connection_id.store (
          connection_id_, std::memory_order_release);
        _endpoint_pair.connection_id = connection_id_;
        return;
    }
    _endpoint_pair.connection_id = connection_id_;
}

uint64_t zlink::pipe_t::get_transport_connection_id () const
{
    return _transport_lifetime
             ? _transport_lifetime->connection_id.load (
                 std::memory_order_acquire)
             : _endpoint_pair.connection_id.load ();
}

bool zlink::pipe_t::try_claim_transport_disconnected_event ()
{
    bool expected = false;
    return get_transport_connection_id () != 0
           && _transport_disconnected_event_claimed.compare_exchange_strong (
             expected, true, std::memory_order_acq_rel,
             std::memory_order_acquire);
}

uint64_t zlink::pipe_t::get_route_incarnation_id () const
{
    zlink_assert (_transport_lifetime);
    return _transport_lifetime->route_incarnation_id;
}

void zlink::pipe_t::set_transport_pair (transport_lane_t lane_,
                                        uint64_t pair_id_,
                                        uint64_t generation_)
{
    get_ctx ()->_physical_queue_registry.classify_pipepair_queues (
      _in_physical_queue, _out_physical_queue, lane_);
    _transport_lane = lane_;
    _transport_pair_id = pair_id_;
    _transport_pair_generation = generation_;
    if (lane_ == transport_lane_completion)
        set_hwms (0, 0);
}

void zlink::pipe_t::set_transport_lane_count (unsigned char lane_count_)
{
    zlink_assert (lane_count_ == 1u || lane_count_ == 2u);
    _transport_lane_count.store (lane_count_, std::memory_order_release);
}

zlink::transport_lane_t zlink::pipe_t::get_transport_lane () const
{
    return _transport_lane;
}

unsigned char zlink::pipe_t::get_transport_lane_count () const
{
    return _transport_lane_count.load (std::memory_order_acquire);
}

bool zlink::pipe_t::uses_registry_accounting () const
{
    return _registry_accounting;
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
        rollback_unlocked (false);

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

uint64_t zlink::pipe_t::frame_accounted_bytes (const msg_t *msg_)
{
    const uint64_t metadata_bytes = static_cast<uint64_t> (sizeof (msg_t));
    if (msg_->is_delimiter () || msg_->is_join () || msg_->is_leave ())
        return metadata_bytes;
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    return UINT64_MAX - payload_bytes < metadata_bytes
             ? UINT64_MAX
             : payload_bytes + metadata_bytes;
}

uint64_t zlink::pipe_t::committed_frame_accounted_bytes_ref (
  const msg_t &msg_)
{
    return msg_.is_delimiter () ? 0 : frame_accounted_bytes (&msg_);
}

bool zlink::pipe_t::counted_pending_message_ref (const msg_t &msg_)
{
    return (msg_.flags () & msg_t::more) == 0 && !msg_.is_routing_id ()
           && !msg_.is_credential () && !msg_.is_delimiter ()
           && !msg_.is_join () && !msg_.is_leave ();
}

void zlink::pipe_t::publish_outbound_frame_unlocked (const msg_t &msg_,
                                                      bool more_)
{
    if (!_registry_accounting) {
        _out_pipe->write (msg_, more_);
        return;
    }

    ypipe_replacement_accounting_t replaced;
    _out_pipe->write_with_replacement_accounting (
      msg_, more_, &pipe_t::committed_frame_accounted_bytes_ref,
      &pipe_t::counted_pending_message_ref, &replaced);
    if (replaced.bytes > 0)
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _out_physical_queue, replaced.bytes, replaced.complete_messages);
}

void zlink::pipe_t::release_discarded_pipe_accounting (
  upipe_t *pipe_, const std::shared_ptr<physical_queue_record_t> &queue_)
{
    if (!pipe_)
        return;
    ypipe_replacement_accounting_t discarded;
    pipe_->discard_accounting (&pipe_t::committed_frame_accounted_bytes_ref,
                               &pipe_t::counted_pending_message_ref,
                               &discarded);
    if (discarded.bytes > 0 && _registry_accounting)
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          queue_, discarded.bytes, discarded.complete_messages);
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
    if (allow_empty_pipe_exception_ && in_flight == 0) {
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
    //  A completed transport handshake may publish the peer limit after the
    //  socket endpoint is already writable. All admission readers own this
    //  endpoint's outbound lock, so publish through the same state boundary.
    scoped_fast_lock_t lock (_out_sync);
    _max_message_bytes = max_message_bytes_;
}

bool zlink::pipe_t::write_message_unlocked (const msg_t *msg_,
                                            bool enforce_hwm_,
                                            bool enforce_incremental_hwm_,
                                            pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    const uint64_t incomplete_before = _out_incomplete_bytes;
    const uint64_t payload_before = _out_incomplete_payload_bytes;
    const bool multipart_started_empty_before =
      _out_multipart_started_empty;
    if (!append_outbound_frame_bytes_unlocked (msg_)) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }
    const uint64_t frame_payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < frame_payload_bytes) {
        _out_incomplete_bytes = incomplete_before;
        errno = EMSGSIZE;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }
    _out_incomplete_payload_bytes += frame_payload_bytes;

    if (_max_message_bytes != 0
        && _out_incomplete_payload_bytes > _max_message_bytes) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        errno = EMSGSIZE;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool commits_bytes = !more && !msg_->is_delimiter ();
    if (more && incomplete_before == 0) {
        refresh_peer_credit_snapshot_unlocked ();
        _out_multipart_started_empty =
          _bytes_written <= _peers_bytes_read;
    }
    bool commit_credit_ready =
      !(commits_bytes || enforce_incremental_hwm_) || !enforce_hwm_
      || can_commit_bytes_with_peer_snapshot_unlocked (
        _out_incomplete_bytes, _out_incomplete_payload_bytes,
        !more
          && (incomplete_before == 0 || _out_multipart_started_empty));
    if (!commit_credit_ready) {
        const bool exceeds_max_message_size =
          _max_message_bytes != 0
          && _out_incomplete_payload_bytes > _max_message_bytes;
        if (!exceeds_max_message_size
            && _bytes_written > _peers_bytes_read) {
            arm_hwm_credit_wait_unlocked ();
            commit_credit_ready =
              can_commit_bytes_with_peer_snapshot_unlocked (
                _out_incomplete_bytes, _out_incomplete_payload_bytes,
                !more
                  && (incomplete_before == 0
                      || _out_multipart_started_empty));
            if (commit_credit_ready)
                clear_hwm_credit_wait_unlocked ();
        }
        if (!commit_credit_ready) {
            _out_incomplete_bytes = incomplete_before;
            _out_incomplete_payload_bytes = payload_before;
            _out_multipart_started_empty = multipart_started_empty_before;
            errno = exceeds_max_message_size ? EMSGSIZE : EAGAIN;
            if (admission_out_)
                *admission_out_ = exceeds_max_message_size
                                    ? pipe_message_admission_too_large
                                    : pipe_message_admission_hwm_full;
            return false;
        }
    }

    if (_registry_accounting) {
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        if (_conflate && !msg_->is_delimiter ()) {
            //  A conflate ypipe retains at most its latest frame and does not
            //  preserve multipart prefixes. Account each physically retained
            //  frame as committed so replacement can return that exact charge.
            get_ctx ()->_physical_queue_registry.commit_message (
              _out_physical_queue, frame_bytes,
              counted_pending_message_ref (*msg_), false);
        } else if (more) {
            get_ctx ()->_physical_queue_registry.account_provisional_frame (
              _out_physical_queue, frame_bytes);
        } else if (!msg_->is_delimiter ()) {
            const uint64_t in_flight =
              _bytes_written > _peers_bytes_read
                ? _bytes_written - _peers_bytes_read
                : 0;
            const bool oversize_admission =
              enforce_hwm_ && _hwm > 0 && in_flight == 0
              && (_out_incomplete_bytes > _hwm
                  || UINT64_MAX - in_flight < _out_incomplete_bytes
                  || in_flight + _out_incomplete_bytes > _hwm);
            get_ctx ()->_physical_queue_registry.commit_message (
              _out_physical_queue, frame_bytes,
              counted_pending_message_ref (*msg_),
              oversize_admission);
        }
        //  Completion and monitor queues retain per-frame registry charge.
        publish_outbound_frame_unlocked (*msg_, more);
    } else {
        //  Application queues account the complete message in pipe-local
        //  counters; keep their frame publication equal to the legacy path.
        _out_pipe->write (*msg_, more);
    }
    if (commits_bytes) {
        const uint64_t message_bytes = _out_incomplete_bytes;
        const uint64_t in_flight =
          _bytes_written > _peers_bytes_read ? _bytes_written - _peers_bytes_read : 0;
        if (enforce_hwm_ && _hwm > 0 && in_flight == 0
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
        _out_complete_record_pending = true;
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        //  The message the owner started is now committed, so the next one
        //  faces the remote flow state again.
        _out_owner_message_started = false;
        _out_owner_message_start_pending = false;
    }
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    publish_session_outbound_accounting_unlocked (
      more || incomplete_before != 0);
    return true;
}

void zlink::pipe_t::account_inbound_frame (
  const msg_t *msg_, bool prefetched_batch_exhausted_)
{
    const bool completes_multipart = _in_incomplete_bytes != 0;
    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    _in_incomplete_bytes =
      frame_bytes == UINT64_MAX || UINT64_MAX - _in_incomplete_bytes < frame_bytes
        ? UINT64_MAX
        : _in_incomplete_bytes + frame_bytes;

    if ((msg_->flags () & msg_t::more) != 0) {
        // A snapshot may observe a multipart while its reader has consumed
        // only its prefix. Publish that prefix locally so the registry can
        // subtract it lazily without a registry update on this frame.
        _published_incomplete_bytes_read.store (_in_incomplete_bytes,
                                                std::memory_order_release);
        return;
    }

    _bytes_read =
      UINT64_MAX - _bytes_read < _in_incomplete_bytes
        ? UINT64_MAX
        : _bytes_read + _in_incomplete_bytes;
    if (!msg_->is_routing_id () && !msg_->is_credential ())
        ++_msgs_read;
    _in_incomplete_bytes = 0;
    // Publish a reset only when a multipart prefix was visible. Single-frame
    // messages leave this snapshot component at zero, so rewriting it on every
    // receive adds coherence traffic without changing observable state. Keep
    // the reset before the completed total so a sampler that observes the
    // latter cannot combine it with an old multipart prefix.
    if (completes_multipart)
        _published_incomplete_bytes_read.store (0, std::memory_order_release);
    // Completed counters are monotonic credit snapshots, not publication of
    // data owned by this reader. Single-frame traffic therefore needs no
    // synchronizes-with edge. A multipart completion does need to stay after
    // the visible prefix reset, so retain release ordering for that rarer
    // transition.
    if (completes_multipart) {
        _published_msgs_read.store (_msgs_read, std::memory_order_release);
        _published_bytes_read.store (_bytes_read, std::memory_order_release);
    } else {
        _published_msgs_read.store (_msgs_read, std::memory_order_relaxed);
        _published_bytes_read.store (_bytes_read, std::memory_order_relaxed);
    }

    const uint64_t credit_delta = _bytes_read - _last_credit_bytes_read;
    const uint64_t lwm = _lwm.load (std::memory_order_relaxed);
    const bool lwm_reached = lwm > 0 && credit_delta >= lwm;
    bool waiter_lwm_reached = false;
    bool blocked_writer_drained = false;
    pipe_t *const peer = get_peer ();
    if (!lwm_reached && credit_delta > 0 && peer) {
        bool writer_waiting =
          peer->_waiting_for_byte_credit.load (std::memory_order_acquire);
        if (!writer_waiting && prefetched_batch_exhausted_) {
            // Close the sub-LWM drain race against the writer's matching
            // waiter-before-credit fence. Either this second load observes
            // the waiter or the writer's credit recheck observes our update.
            std::atomic_thread_fence (std::memory_order_seq_cst);
            writer_waiting = peer->_waiting_for_byte_credit.load (
              std::memory_order_acquire);
        }
        if (writer_waiting) {
            // A deferred physical-queue shrink intentionally leaves applied
            // accounting above the planned HWM, and endpoint plan application
            // can race a later replan. The peer already enforces the planned
            // value, so a blocked writer must use that same window for credit
            // wakeups instead of waiting for a stale, larger cached LWM.
            const uint64_t planned_in = planned_in_hwm ();
            const uint64_t planned_lwm =
              planned_in == 0
                ? 1
                : apply_lwm_hint (planned_in, compute_lwm (planned_in),
                                  _lwm_hint);
            waiter_lwm_reached = credit_delta >= planned_lwm;
            if (!waiter_lwm_reached && prefetched_batch_exhausted_) {
                // This is the rare blocked-writer fallback below the planned
                // LWM. Verify that the published queue is actually drained;
                // a prefetched batch boundary alone does not mean that the
                // next batch is absent and must not trigger a premature wake.
                // The non-sleeping probe also preserves the hot-path rule that
                // a preview never publishes the ypipe sleep marker.
                normalized_head_probe_t next_head = {pipe_head_invalid};
                blocked_writer_drained =
                  _in_pipe
                  && !_in_pipe->probe_if_published (&probe_normalized_head,
                                                    &next_head);
            }
        }
    }
    const bool credit_boundary =
      credit_delta > 0
      && (lwm_reached || waiter_lwm_reached || blocked_writer_drained);
    if (credit_boundary) {
        _last_credit_bytes_read = _bytes_read;
        send_activate_write (peer, _in_generation, _msgs_read,
                             _bytes_read);
    }
    if (!_registry_accounting && credit_boundary)
        get_ctx ()->_physical_queue_registry.refresh_application_hwm_if_drained (
          _in_physical_queue);
}

void zlink::pipe_t::snapshot_outbound_queue_accounting (
  const pipe_t *reader_, uint64_t *provisional_out_,
  uint64_t *committed_out_) const
{
    if (provisional_out_)
        *provisional_out_ = 0;
    if (committed_out_)
        *committed_out_ = 0;

    uint64_t completed_read = 0;
    uint64_t partial_read = 0;
    if (reader_) {
        completed_read = reader_->_published_bytes_read.load (
          std::memory_order_acquire);
        partial_read = reader_->_published_incomplete_bytes_read.load (
          std::memory_order_acquire);
    }
    const uint64_t consumed =
      UINT64_MAX - completed_read < partial_read
        ? UINT64_MAX
        : completed_read + partial_read;

    uint64_t provisional = 0;
    uint64_t available = 0;
    if (_session_io_writer) {
        // The decoder is the sole writer of this endpoint and deliberately
        // avoids _out_sync. Load its classification hint first: observing a
        // newer hint synchronizes with the preceding total publication. A
        // stale hint is harmless because it is clamped inside the authoritative
        // available total.
        const uint64_t published_provisional =
          _published_outbound_provisional_bytes.load (
            std::memory_order_acquire);
        const uint64_t published_total =
          _published_outbound_total_bytes.load (std::memory_order_acquire);
        available = published_total > consumed ? published_total - consumed : 0;
        provisional = std::min (published_provisional, available);
    } else {
        // This path is reached only by context snapshots and Auto-HWM
        // replanning. Ordinary writers use _out_sync, so their local ledger can
        // be copied exactly without adding publication work to socket sends.
        scoped_optional_fast_lock_t lock (
          const_cast<fast_mutex_t *> (&_out_sync));
        const uint64_t written = _bytes_written;
        provisional = _out_incomplete_bytes;
        available = written > consumed ? written - consumed : 0;
        available = UINT64_MAX - available < provisional
                      ? UINT64_MAX
                      : available + provisional;
    }

    if (provisional_out_)
        *provisional_out_ = provisional;
    if (committed_out_)
        *committed_out_ = available - provisional;
}

void zlink::pipe_t::publish_session_outbound_accounting_unlocked (
  bool provisional_changed_)
{
    if (likely (!_session_io_writer))
        return;

    const uint64_t total =
      UINT64_MAX - _bytes_written < _out_incomplete_bytes
        ? UINT64_MAX
        : _bytes_written + _out_incomplete_bytes;
    // Publish the authoritative total first. A concurrent snapshot can retain
    // an older provisional classification, but clamps it inside this total, so
    // Auto-HWM never double-counts or reads the decoder-owned local fields.
    _published_outbound_total_bytes.store (total, std::memory_order_release);
    if (provisional_changed_)
        _published_outbound_provisional_bytes.store (
          _out_incomplete_bytes, std::memory_order_release);
}

void zlink::pipe_t::rollback_unlocked (bool publish_peer_control_)
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
    if (_out_incomplete_bytes > 0 && _registry_accounting)
        get_ctx ()->_physical_queue_registry.rollback_provisional (
          _out_physical_queue, _out_incomplete_bytes);
    _out_incomplete_bytes = 0;
    _out_incomplete_payload_bytes = 0;
    _out_multipart_started_empty = false;
    _decoder_multipart_started_empty = false;
    //  The owner's accepted-but-unwritten part is gone with the rest of the
    //  message, so the in-progress exception ends here.
    _out_owner_message_started = false;
    _out_owner_message_start_pending = false;
    publish_session_outbound_accounting_unlocked (true);
    if (pending_peer_controls_unlocked ()
        && peer_control_slots_enabled_unlocked ()) {
        // A public Application rollback keeps the latest absolute controls
        // and publishes them at the now-clean message boundary.
        // Termination/disconnect rollbacks must not leak control into a
        // retiring generation; its normal reconnect/pair-ready resync owns
        // publication on the replacement transport.
        if (publish_peer_control_ && _state == active)
            (void) flush_pending_peer_controls_unlocked ();
        else
            discard_pending_peer_controls_unlocked ();
    }
}

void zlink::pipe_t::flush_unlocked ()
{
    //  The peer does not exist anymore at this point.
    if (_state == term_ack_sent)
        return;

    // Check the concrete slots first. Ordinary PAIR/PUB/SUB/XPUB/XSUB/STREAM
    // sends never populate them, so their hot flush path does not even load
    // pair/type policy state. Only a validated D/R control writer can make the
    // slower topology check reachable.
    const bool publish_pending_controls =
      unlikely (pending_peer_controls_unlocked ())
      && peer_control_slots_enabled_unlocked ()
      && _out_incomplete_bytes == 0 && _out_incomplete_payload_bytes == 0;
    // Network controls are records in the same physical FIFO, so append them
    // before publishing the boundary. Inproc controls are owner commands;
    // publish preceding Application bytes first, then enqueue the commands on
    // the peer owner mailbox in the same surviving-slot order.
    if (publish_pending_controls && _session_pipe)
        (void) append_pending_peer_controls_unlocked ();
    const bool completed_record = _out_complete_record_pending;
    _out_complete_record_pending = false;
    const bool sleeping = _out_pipe && !_out_pipe->flush ();
    const bool reclassify_candidate =
      !sleeping && completed_record
      && peer_uses_routed_protocol_unlocked ()
      && _transport_pair_id != 0
      && _transport_lane == transport_lane_application
      && _transport_lane_count.load (std::memory_order_acquire) == 1u
      && (!_session_pipe || _session_io_writer);
    pipe_t *const peer =
      sleeping || reclassify_candidate ? get_peer () : NULL;
    bool explicit_reclassify = false;
    if (reclassify_candidate && peer
        && peer->_head_reclassify_wake.load (std::memory_order_acquire)
             == head_reclassify_armed) {
        unsigned char expected = head_reclassify_armed;
        explicit_reclassify =
          peer->_head_reclassify_wake.compare_exchange_strong (
            expected, head_reclassify_queued, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    if (sleeping || explicit_reclassify)
        send_activate_read (peer);
    if (publish_pending_controls && !_session_pipe)
        (void) dispatch_pending_inproc_controls_unlocked ();
}
