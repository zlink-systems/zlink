/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdio.h>
#include <new>
#include <stddef.h>

#include "utils/macros.hpp"
#include "utils/config.hpp"
#include "core/pipe_internal.hpp"
#include "core/transport_pair_policy.hpp"
#include "core/ctx.hpp"
#include "core/flow_state_frame.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_peer_weight.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/heap_owner.hpp"
#include "core/ypipe.hpp"
#include "core/ypipe_conflate.hpp"

using zlink::pipe_detail::consume_if_delimiter;
using zlink::pipe_detail::head_reclassify_armed;
using zlink::pipe_detail::head_reclassify_idle;
using zlink::pipe_detail::head_reclassify_queued;
using zlink::pipe_detail::pipe_debug_log;

namespace zlink
{
namespace pipe_detail
{
const unsigned char head_reclassify_idle = 0;
const unsigned char head_reclassify_armed = 1;
const unsigned char head_reclassify_queued = 2;

void pipe_debug_log (
  const pipe_t *pipe_, const char *phase_, int state_, bool delay_,
  const char *identifier_)
{
    if (!debug_env_enabled ("ZLINK_DEBUG_PIPE_TERM"))
        return;

    fprintf (stderr, "[pipe-term] pipe=%p phase=%s state=%d delay=%d endpoint=%s\n",
             static_cast<const void *> (pipe_), phase_ ? phase_ : "?", state_, delay_ ? 1 : 0,
             identifier_ && *identifier_ ? identifier_ : "<none>");
    fflush (stderr);
}

bool consume_if_delimiter (const msg_t &msg_, void *)
{
    return msg_.is_delimiter ();
}
}
}

namespace
{
#ifdef ZLINK_BUILD_TESTS
std::atomic<int> g_stream_packet_allocation_failpoint (
  zlink::stream_packet_allocation_none);
#endif
}

int zlink::pipepair (object_t *parents_[2],
                     pipe_t *pipes_[2],
                     const uint64_t hwms_[2],
                     const bool conflate_[2],
                     const pipepair_options_t &options_)
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
    zlink_assert (options_.session_owner_index >= -1
                  && options_.session_owner_index <= 1);
    zlink_assert (options_.session_pipe || options_.session_owner_index == -1);

    ctx_t *const ctx = parents_[0]->get_ctx ();
    zlink_assert (ctx == parents_[1]->get_ctx ());
    const physical_queue_class_t resolved_queue_class =
      options_.lane == transport_lane_completion
        ? physical_queue_class_completion
        : options_.queue_class;
    physical_queue_handle_t physical_queues[2];
    if (ctx->create_pipepair_queues (
          hwms_[1], hwms_[0], resolved_queue_class, options_.role,
          options_.planning_enabled,
          &physical_queues[0], &physical_queues[1]) != 0)
        return -1;

    pipe_t::upipe_t *upipe1;
    if (conflate_[0])
        upipe1 = new (std::nothrow) upipe_conflate_t ();
    else if (options_.session_pipe)
        upipe1 = new (std::nothrow) upipe_session_t ();
    else
        upipe1 = new (std::nothrow) upipe_normal_t ();
    alloc_assert (upipe1);

    pipe_t::upipe_t *upipe2;
    if (conflate_[1])
        upipe2 = new (std::nothrow) upipe_conflate_t ();
    else if (options_.session_pipe)
        upipe2 = new (std::nothrow) upipe_session_t ();
    else
        upipe2 = new (std::nothrow) upipe_normal_t ();
    alloc_assert (upipe2);

    //  Inproc routes have no engine to assign a connection id, so allocate
    //  one here. A session-backed route remains unbound until its engine is
    //  ready. Messages queued before that first binding keep id 0 and belong
    //  to the first transport; later nonzero ids still isolate reconnects.
    const uint64_t initial_connection_id =
      options_.session_pipe ? 0 : allocate_connection_id ();
    const uint64_t route_incarnation_id =
      initial_connection_id != 0 ? initial_connection_id
                                 : allocate_connection_id ();
    zlink_assert (route_incarnation_id != 0);
    const std::shared_ptr<transport_lifetime_t> transport_lifetime =
      std::make_shared<transport_lifetime_t> (
        initial_connection_id, route_incarnation_id);

    pipes_[0] = new (std::nothrow)
      pipe_t (parents_[0], upipe1, upipe2, hwms_[1], hwms_[0], conflate_[0],
              options_.session_pipe, transport_lifetime, physical_queues[0],
              physical_queues[1],
              resolved_queue_class != physical_queue_class_application,
              resolved_queue_class == physical_queue_class_application
                && options_.session_owner_index == 0);
    alloc_assert (pipes_[0]);
    pipes_[1] = new (std::nothrow)
      pipe_t (parents_[1], upipe2, upipe1, hwms_[0], hwms_[1], conflate_[1],
              options_.session_pipe, transport_lifetime, physical_queues[1],
              physical_queues[0],
              resolved_queue_class != physical_queue_class_application,
              resolved_queue_class == physical_queue_class_application
                && options_.session_owner_index == 1);
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
    _request_correlation_release_epoch (0),
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
    _outbound_ledger_sequence (0),
    _inbound_ledger_sequence (0),
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
    _registry_accounting (registry_accounting_),
    _transport_pair_id (0),
    _transport_pair_generation (0),
    _locally_initiated (false),
    _peer_socket_type (0),
    _peer_weight_connection_id (UINT64_MAX),
    _peer_weight (UINT32_MAX),
    _transport_disconnected_event_connection_id (0),
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
        scoped_lock_t lock (_out_sync);
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
        scoped_lock_t peer_lock (peer->_out_sync);
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
    return _state.load (std::memory_order_acquire) == active;
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
        scoped_lock_t lock (_out_sync);
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

    scoped_lock_t lock (_out_sync);
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
    scoped_lock_t lock (_out_sync);
    _router_socket_routing_id.set_deep_copy (router_socket_routing_id_);
}

void zlink::pipe_t::snapshot_routing_id (blob_t *routing_id_) const
{
    zlink_assert (routing_id_);
    scoped_lock_t lock (const_cast<mutex_t &> (_out_sync));
    routing_id_->set_deep_copy (_router_socket_routing_id);
}

void zlink::pipe_t::publish_router_route_source (
  const blob_t &router_route_source_routing_id_)
{
    scoped_lock_t lock (_out_sync);
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

zlink::pipe_t *zlink::pipe_t::retain_peer_snapshot () const
{
    scoped_lock_t lock (const_cast<mutex_t &> (_out_sync));
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
    return _msgs_written.load (std::memory_order_acquire);
}

uint64_t zlink::pipe_t::get_msgs_read () const
{
    return _published_msgs_read.load (std::memory_order_acquire);
}

uint64_t zlink::pipe_t::get_bytes_written () const
{
    return _bytes_written.load (std::memory_order_acquire);
}

uint64_t zlink::pipe_t::get_bytes_read () const
{
    return _published_bytes_read.load (std::memory_order_acquire);
}

void zlink::pipe_t::snapshot_ledger (
  const std::atomic<uint64_t> &sequence_,
  const std::atomic<uint64_t> &msgs_,
  const std::atomic<uint64_t> &bytes_,
  uint64_t *msgs_value_, uint64_t *bytes_value_)
{
    zlink_assert (msgs_value_);
    zlink_assert (bytes_value_);

    for (;;) {
        const uint64_t sequence_before =
          sequence_.load (std::memory_order_acquire);
        if (sequence_before & 1u)
            continue;
        const uint64_t msgs_value = msgs_.load (std::memory_order_acquire);
        const uint64_t bytes_value = bytes_.load (std::memory_order_acquire);
        const uint64_t sequence_after =
          sequence_.load (std::memory_order_acquire);
        if (sequence_before == sequence_after) {
            *msgs_value_ = msgs_value;
            *bytes_value_ = bytes_value;
            return;
        }
    }
}

void zlink::pipe_t::publish_ledger_unlocked (
  std::atomic<uint64_t> *sequence_, std::atomic<uint64_t> *msgs_,
  std::atomic<uint64_t> *bytes_, uint64_t msgs_value_,
  uint64_t bytes_value_)
{
    const uint64_t sequence =
      sequence_->load (std::memory_order_relaxed);
    zlink_assert ((sequence & 1u) == 0);
    const uint64_t prior =
      sequence_->exchange (sequence + 1, std::memory_order_acq_rel);
    zlink_assert (prior == sequence);
    msgs_->store (msgs_value_, std::memory_order_release);
    bytes_->store (bytes_value_, std::memory_order_release);
    sequence_->store (sequence + 2, std::memory_order_release);
}

void zlink::pipe_t::publish_outbound_ledger_unlocked (
  uint64_t msgs_written_, uint64_t bytes_written_)
{
    publish_ledger_unlocked (&_outbound_ledger_sequence, &_msgs_written,
                             &_bytes_written, msgs_written_, bytes_written_);
}

void zlink::pipe_t::get_pending_snapshot (
  uint64_t *snd_msgs_, uint64_t *rcv_msgs_, uint64_t *snd_bytes_,
  uint64_t *rcv_bytes_) const
{
    zlink_assert (snd_msgs_);
    zlink_assert (rcv_msgs_);
    zlink_assert (snd_bytes_);
    zlink_assert (rcv_bytes_);

    scoped_optional_lock_t lock (const_cast<mutex_t *> (&_out_sync));
    uint64_t msgs_written;
    uint64_t bytes_written;
    snapshot_ledger (_outbound_ledger_sequence, _msgs_written,
                     _bytes_written, &msgs_written, &bytes_written);
    const uint64_t peers_msgs_read =
      _peers_msgs_read.load (std::memory_order_acquire);
    const uint64_t peers_bytes_read =
      _peers_bytes_read.load (std::memory_order_acquire);
    *snd_msgs_ = msgs_written > peers_msgs_read
                   ? msgs_written - peers_msgs_read
                   : 0;
    *snd_bytes_ = bytes_written > peers_bytes_read
                    ? bytes_written - peers_bytes_read
                    : 0;

    *rcv_msgs_ = 0;
    *rcv_bytes_ = 0;
    pipe_t *const peer = retain_peer_snapshot_unlocked ();
    if (!peer)
        return;

    uint64_t peer_msgs_written;
    uint64_t peer_bytes_written;
    snapshot_ledger (peer->_outbound_ledger_sequence, peer->_msgs_written,
                     peer->_bytes_written, &peer_msgs_written,
                     &peer_bytes_written);
    peer->release_lifetime_ref ();
    uint64_t msgs_read;
    uint64_t bytes_read;
    snapshot_ledger (_inbound_ledger_sequence, _published_msgs_read,
                     _published_bytes_read, &msgs_read, &bytes_read);
    *rcv_msgs_ = peer_msgs_written > msgs_read
                   ? peer_msgs_written - msgs_read
                   : 0;
    *rcv_bytes_ = peer_bytes_written > bytes_read
                    ? peer_bytes_written - bytes_read
                    : 0;
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
    scoped_optional_lock_t lock (const_cast<mutex_t *> (&_out_sync));
    const uint64_t msgs_written =
      _msgs_written.load (std::memory_order_acquire);
    const uint64_t peers_msgs_read =
      _peers_msgs_read.load (std::memory_order_acquire);
    if (msgs_written <= peers_msgs_read)
        return 0;
    return msgs_written - peers_msgs_read;
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
    scoped_optional_lock_t lock (const_cast<mutex_t *> (&_out_sync));
    uint64_t peer_bytes_read =
      _peers_bytes_read.load (std::memory_order_acquire);
    pipe_t *const peer = retain_peer_snapshot_unlocked ();
    if (peer) {
        const uint64_t published =
          peer->_published_bytes_read.load (std::memory_order_relaxed);
        if (published > peer_bytes_read)
            peer_bytes_read = published;
        peer->release_lifetime_ref ();
    }
    const uint64_t bytes_written =
      _bytes_written.load (std::memory_order_acquire);
    if (bytes_written <= peer_bytes_read)
        return 0;
    return bytes_written - peer_bytes_read;
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
    scoped_lock_t lock (_out_sync);
    if (_transport_lane == transport_lane_completion) {
        _hwm.store (0, std::memory_order_release);
        _inhwm.store (0, std::memory_order_relaxed);
        _lwm.store (0, std::memory_order_relaxed);
        return;
    }
    _hwm.store (planned_out_hwm (), std::memory_order_release);
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
    return _oversize_message_admission_count.load (std::memory_order_acquire);
}

uint64_t zlink::pipe_t::get_oversize_message_admission_max_bytes () const
{
    return _oversize_message_admission_max_bytes.load (
      std::memory_order_acquire);
}

void zlink::pipe_t::reset_oversize_message_admission_metrics ()
{
    scoped_lock_t lock (_out_sync);
    _oversize_message_admission_count.store (0, std::memory_order_release);
    _oversize_message_admission_max_bytes.store (0,
                                                  std::memory_order_release);
}

void zlink::pipe_t::record_oversize_message_admission (
  uint64_t message_bytes_)
{
    _oversize_message_admission_count.fetch_add (1,
                                                  std::memory_order_acq_rel);
    uint64_t current = _oversize_message_admission_max_bytes.load (
      std::memory_order_acquire);
    while (current < message_bytes_
           && !_oversize_message_admission_max_bytes.compare_exchange_weak (
             current, message_bytes_, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
    }
}

void zlink::pipe_t::refresh_write_credit (uint64_t peer_msgs_read_, uint64_t peer_bytes_read_)
{
    scoped_lock_t lock (_out_sync);

    if (peer_msgs_read_
        > _peers_msgs_read.load (std::memory_order_acquire))
        _peers_msgs_read.store (peer_msgs_read_, std::memory_order_release);
    if (peer_bytes_read_
        > _peers_bytes_read.load (std::memory_order_acquire))
        _peers_bytes_read.store (peer_bytes_read_, std::memory_order_release);

    bool expected = false;
    if (!_transport_pair_write_held && _state == active
        && check_hwm_unlocked ()
        && _out_active.compare_exchange_strong (
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
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

zlink::mutex_t &zlink::pipe_t::transport_sync ()
{
    return _transport_lifetime->transport_sync;
}

void zlink::pipe_t::close_stream_route ()
{
    scoped_lock_t lock (_transport_lifetime->transport_sync);
    _transport_lifetime->stream_route_closed.store (
      true, std::memory_order_release);
    _transport_lifetime->stream_packet_state.reset ();
}

bool zlink::pipe_t::stream_route_closed () const
{
    return _transport_lifetime->stream_route_closed.load (
      std::memory_order_acquire);
}
