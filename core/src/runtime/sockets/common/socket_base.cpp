/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <algorithm>
#include <new>

#include "sockets/common/socket_base.hpp"
#include "core/ctx.hpp"
#include "core/flow_state_frame.hpp"
#include "core/mailbox.hpp"
#include "utils/err.hpp"
#include "utils/random.hpp"

#include "sockets/pair/pair.hpp"
#include "sockets/pubsub/pub.hpp"
#include "sockets/pubsub/sub.hpp"
#include "sockets/dealer/dealer.hpp"
#include "sockets/router/router.hpp"
#include "sockets/stream/stream.hpp"
#include "sockets/pubsub/xpub.hpp"
#include "sockets/pubsub/xsub.hpp"

namespace
{
static void generate_default_routing_id (unsigned char out_[16])
{
    zlink::generate_random_bytes (out_, 16);

    // RFC 4122 variant/version layout.
    out_[6] = static_cast<unsigned char> ((out_[6] & 0x0F) | 0x40);
    out_[8] = static_cast<unsigned char> ((out_[8] & 0x3F) | 0x80);

    bool all_zero = true;
    for (size_t i = 0; i < 16; ++i) {
        if (out_[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero)
        out_[15] = 1;
}
}

bool zlink::socket_base_t::check_tag () const
{
    return _tag.load (std::memory_order_acquire) == 0xbaddecaf;
}

zlink::socket_base_t *
zlink::socket_base_t::create (int type_, class ctx_t *parent_, uint32_t tid_, int sid_)
{
    socket_base_t *s = NULL;
    switch (type_) {
        case ZLINK_CORE_SOCKET_PAIR:
            s = new (std::nothrow) pair_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_PUB:
            s = new (std::nothrow) pub_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_SUB:
            s = new (std::nothrow) sub_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_DEALER:
            s = new (std::nothrow) dealer_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_ROUTER:
            s = new (std::nothrow) router_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_STREAM:
            s = new (std::nothrow) stream_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_XPUB:
            s = new (std::nothrow) xpub_t (parent_, tid_, sid_);
            break;
        case ZLINK_CORE_SOCKET_XSUB:
            s = new (std::nothrow) xsub_t (parent_, tid_, sid_);
            break;
        default:
            errno = EINVAL;
            return NULL;
    }

    alloc_assert (s);

    if (s->_mailbox == NULL) {
        s->lifecycle_coordinator ().mark_destroyed ();
        LIBZLINK_DELETE (s);
        return NULL;
    }

    return s;
}

zlink::socket_base_t::socket_base_t (ctx_t *parent_, uint32_t tid_, int sid_) :
    own_t (parent_, tid_),
    _public_handle (NULL),
    _tag (0xbaddecaf),
    _ctx_terminated (false),
    _runtime (),
    _auto_hwm_policy_enabled (true),
    _manual_sndhwm (false),
    _manual_rcvhwm (false),
    _completion_processing_owner_generation (0),
    _completion_poller_refs (0),
    _completion_poller_owner (NULL),
    _request_completion_pending (false),
    _transport_pair_owner_progress_refs (0),
    _async_command_processing_stop_requested (false),
    _async_command_processing_retained (false),
    _auto_hwm_context_plan (),
    _auto_hwm_socket_plan (),
    _auto_hwm_last_recalc_ms (0),
    _auto_hwm_last_recalc_reason (ZLINK_AUTO_HWM_RECALC_REASON_NONE),
    _auto_hwm_send_attempts (0),
    _auto_hwm_send_blocked_attempts (0),
    _flow_paused_connections (0),
    _flow_pause_applied_total (0),
    _flow_resume_applied_total (0),
    _flow_state_stale_total (0),
    _flow_last_pause_duration_ms (0),
    _local_peer_weight (100),
    _ready_count1_completion_pipes (NULL),
    _public_part_receive_delivery_hold_active (false),
    _public_part_receive_delivery_hold_pipe (NULL),
    _public_part_receive_delivery_hold_key (0, 0),
    _local_receive_flow_state (flow_state::receive_flow_running),
    _local_receive_flow_epoch (0)
{
    _term_pipe_acks_registered = 0;
    _term_pipe_acks_received = 0;
    options.socket_id = sid_;
    options.ipv6 = (parent_->get (ZLINK_INTERNAL_OPT_IPV6) != 0);
    options.linger.store (parent_->get (ZLINK_INTERNAL_OPT_BLOCKY) ? -1 : 0);

    if (options.routing_id_size == 0) {
        unsigned char buf[16];
        generate_default_routing_id (buf);
        memcpy (options.routing_id, buf, sizeof buf);
        options.routing_id_size = static_cast<unsigned char> (sizeof buf);
    }

    mailbox_t *m = new (std::nothrow) mailbox_t ();
    zlink_assert (m);
    _mailbox = m;
}

void zlink::socket_base_t::set_auto_hwm_policy_enabled (bool enabled_)
{
    {
        scoped_lock_t lock (_auto_hwm_sync);
        _auto_hwm_policy_enabled = enabled_;
        _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE;
    }
    refresh_auto_hwm_policy ();
}

int zlink::socket_base_t::configure_internal_monitor_queue (
  uint64_t hwm_bytes_)
{
    if (hwm_bytes_ == 0) {
        errno = EINVAL;
        return -1;
    }

    // The class must be fixed before bind/connect publishes a physical pipe.
    // A monitor PAIR carries normal messages, but its capacity and charge are
    // diagnostic state rather than application Auto HWM state.
    options.physical_queue_class = physical_queue_class_monitor;
    set_auto_hwm_policy_enabled (false);
    if (setsockopt (ZLINK_INTERNAL_OPT_SNDHWM, &hwm_bytes_,
                    sizeof (hwm_bytes_)) != 0)
        return -1;
    return setsockopt (ZLINK_INTERNAL_OPT_RCVHWM, &hwm_bytes_,
                       sizeof (hwm_bytes_));
}

zlink::auto_hwm_socket_plan_t
zlink::socket_base_t::prepare_auto_hwm_socket_plan_locked (
  const auto_hwm_context_plan_t &context_)
{
    const auto_hwm_role_t role =
      auto_hwm_default_role_for_socket_type (options.type);

    size_t application_pipe_count = 0;
    uint64_t pending_bytes = 0;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        const size_t attached_pipe_count = endpoint_runtime ().attached_pipe_count ();
        size_t attached_application_pipe_count = 0;
        for (size_t i = 0; i != attached_pipe_count; ++i) {
            pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
            if (!pipe || pipe->get_transport_lane () == transport_lane_completion)
                continue;
            ++attached_application_pipe_count;
            // Context-wide planning can overlap peer-pipe termination.  The
            // physical queue records are shared lifetime-safe snapshots;
            // following pipe->_peer here would race peer destruction.
            const uint64_t send_accounted_bytes =
              pipe->get_snd_queue_accounted_bytes ();
            const uint64_t receive_accounted_bytes =
              pipe->get_rcv_queue_accounted_bytes ();
            const uint64_t pipe_pending_bytes =
              UINT64_MAX - send_accounted_bytes < receive_accounted_bytes
                ? UINT64_MAX
                : send_accounted_bytes + receive_accounted_bytes;
            pending_bytes =
              UINT64_MAX - pending_bytes < pipe_pending_bytes
                ? UINT64_MAX
                : pending_bytes + pipe_pending_bytes;
        }
        application_pipe_count = attached_application_pipe_count;
    }

    auto_hwm_socket_plan_t plan;
    auto_hwm_socket_plan_prepare (
      role, application_pipe_count, application_pipe_count, _manual_sndhwm,
      options.sndhwm, _manual_rcvhwm, options.rcvhwm,
      context_.enabled && _auto_hwm_policy_enabled, &plan);
    plan.pending_bytes = pending_bytes;
    return plan;
}

void zlink::socket_base_t::collect_auto_hwm_queue_policies (
  std::vector<physical_queue_endpoint_policy_t> *out_)
{
    if (!out_)
        return;
    scoped_lock_t auto_hwm_lock (_auto_hwm_sync);

    scoped_lock_t lock (monitor_runtime ().sync);
    const size_t count = endpoint_runtime ().attached_pipe_count ();
    for (size_t i = 0; i != count; ++i) {
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        if (!pipe
            || pipe->get_transport_lane () == transport_lane_completion)
            continue;

        out_->push_back (make_auto_hwm_queue_policy_locked (
          pipe->out_physical_queue (), true));
        out_->push_back (make_auto_hwm_queue_policy_locked (
          pipe->in_physical_queue (), false));
    }
}

zlink::physical_queue_endpoint_policy_t
zlink::socket_base_t::make_auto_hwm_queue_policy (
  const std::shared_ptr<physical_queue_record_t> &queue_, bool writer_) const
{
    scoped_lock_t auto_hwm_lock (_auto_hwm_sync);
    return make_auto_hwm_queue_policy_locked (queue_, writer_);
}

zlink::physical_queue_endpoint_policy_t
zlink::socket_base_t::make_auto_hwm_queue_policy_locked (
  const std::shared_ptr<physical_queue_record_t> &queue_, bool writer_) const
{
    physical_queue_endpoint_policy_t policy;
    policy.queue = queue_;
    policy.role = auto_hwm_default_role_for_socket_type (options.type);
    policy.writer = writer_;
    policy.manual = writer_ ? _manual_sndhwm : _manual_rcvhwm;
    policy.planning_enabled = _auto_hwm_policy_enabled;
    policy.hwm = writer_ ? options.sndhwm : options.rcvhwm;
    return policy;
}

void zlink::socket_base_t::apply_physical_auto_hwm_plan (
  const auto_hwm_context_plan_t &context_, uint32_t recalc_reason_)
{
    scoped_lock_t auto_hwm_lock (_auto_hwm_sync);
    _auto_hwm_context_plan = context_;
    _auto_hwm_socket_plan = prepare_auto_hwm_socket_plan_locked (context_);
    _auto_hwm_socket_plan.minimum_hwm_bytes =
      auto_hwm_profile_minimum_bytes (context_.profile,
                                      _auto_hwm_socket_plan.role);
    _auto_hwm_socket_plan.maximum_hwm_bytes =
      auto_hwm_profile_maximum_bytes (context_.profile,
                                      _auto_hwm_socket_plan.role);

    uint64_t planned_send = 0;
    uint64_t planned_receive = 0;
    bool deferred = false;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        const size_t count = endpoint_runtime ().attached_pipe_count ();
        for (size_t i = 0; i != count; ++i) {
            pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
            if (!pipe
                || pipe->get_transport_lane () == transport_lane_completion)
                continue;
            pipe->apply_physical_queue_hwm_plan ();
            const uint64_t send = pipe->planned_out_hwm ();
            const uint64_t receive = pipe->planned_in_hwm ();
            planned_send = std::max (planned_send, send);
            planned_receive = std::max (planned_receive, receive);
            deferred = deferred || send != pipe->applied_out_hwm ()
                       || receive != pipe->applied_in_hwm ();
        }
    }
    if (planned_send != 0 || _auto_hwm_socket_plan.send_queue_count != 0)
        _auto_hwm_socket_plan.sndhwm = planned_send;
    if (planned_receive != 0 || _auto_hwm_socket_plan.receive_queue_count != 0)
        _auto_hwm_socket_plan.rcvhwm = planned_receive;
    _auto_hwm_last_recalc_ms = _clock.now_ms ();
    _auto_hwm_last_recalc_reason =
      deferred
        ? ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK
        : recalc_reason_;
}

void zlink::socket_base_t::refresh_auto_hwm_policy (bool force_apply_)
{
    LIBZLINK_UNUSED (force_apply_);
    ctx_t *ctx = get_ctx ();
    if (!ctx)
        return;
    ctx->schedule_auto_hwm_recalculate ();
}

zlink::socket_base_t::~socket_base_t ()
{
    //  A detached count-1 pair can leave a stale ready entry after its final
    //  completion owner has stopped. The intrusive queue owns the pipe and its
    //  inbound ypipe independently of transport-pair teardown; release those
    //  final pins before destroying the socket mailbox.
    discard_count1_completion_ready_pipes ();
    scoped_lock_t lock (monitor_runtime ().sync);
    stop_monitor ();

    // stop_monitor() may release the monitor's async-mailbox ownership and
    // signal this mailbox from the reaper thread. Keep it alive until that
    // hand-off is complete.
    if (_mailbox)
        LIBZLINK_DELETE (_mailbox);

    zlink_assert (lifecycle_coordinator ().is_destroyed ());
}

zlink::i_mailbox *zlink::socket_base_t::get_mailbox () const
{
    return _mailbox;
}

void zlink::socket_base_t::stop ()
{
    //  Publish termination before queueing the administrative command. A
    //  ROUTER may have a receiver and a blocked sender on different threads.
    //  Either thread can consume the command mailbox
    //  edge, so an additional wake plus the shared atomic state makes both
    //  blocking paths observe ETERM. The command still performs the ordinary
    //  monitor shutdown on the socket thread.
    _ctx_terminated.store (true, std::memory_order_release);
    send_stop ();
    static_cast<mailbox_t *> (_mailbox)->signal ();
}


int zlink::socket_base_t::xsetsockopt (int, const void *, size_t)
{
    errno = EINVAL;
    return -1;
}

int zlink::socket_base_t::xgetsockopt (int, void *, size_t *)
{
    errno = EINVAL;
    return -1;
}

bool zlink::socket_base_t::xhas_out ()
{
    return false;
}

int zlink::socket_base_t::xsend (msg_t *,
                                 pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xsend_pipe (
  msg_t *msg_, pipe_t **pipe_out_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    LIBZLINK_UNUSED (observer_);
    LIBZLINK_UNUSED (observer_userdata_);
    return xsend (msg_, admission_out_);
}

int zlink::socket_base_t::xsend_routed (const zlink_routing_id_t *target_rid_,
                                       msg_t *msg_,
                                       uint64_t *connection_id_out_,
                                       uint64_t expected_connection_id_,
                                       pipe_t **pipe_out_,
                                       uint64_t expected_transport_pair_id_,
                                       uint64_t expected_transport_pair_generation_,
                                       pipe_message_admission_t *admission_out_,
                                       pipe_write_observer_fn observer_,
                                       void *observer_userdata_,
                                       routed_send_attempt_identity_t
                                         *attempt_identity_out_,
                                       uint64_t expected_route_incarnation_id_,
                                       bool request_only_)
{
    LIBZLINK_UNUSED (target_rid_);
    LIBZLINK_UNUSED (msg_);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (attempt_identity_out_)
        attempt_identity_out_->reset ();
    LIBZLINK_UNUSED (expected_connection_id_);
    LIBZLINK_UNUSED (expected_transport_pair_id_);
    LIBZLINK_UNUSED (expected_transport_pair_generation_);
    LIBZLINK_UNUSED (expected_route_incarnation_id_);
    LIBZLINK_UNUSED (request_only_);
    LIBZLINK_UNUSED (observer_);
    LIBZLINK_UNUSED (observer_userdata_);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xselect_routed_submit_pipe (pipe_t **pipe_out_,
                                                      bool request_only_)
{
    LIBZLINK_UNUSED (request_only_);
    if (pipe_out_)
        *pipe_out_ = NULL;
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xcommit_request_submit_pipe (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xsend_selected_pipe (
  pipe_t *pipe_, msg_t *msg_, int flags_, bool request_only_,
  pipe_message_admission_t *admission_out_, pipe_write_observer_fn observer_,
  void *observer_userdata_)
{
    LIBZLINK_UNUSED (pipe_);
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (flags_);
    LIBZLINK_UNUSED (request_only_);
    LIBZLINK_UNUSED (observer_);
    LIBZLINK_UNUSED (observer_userdata_);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xsend_configured_endpoint (
  const std::string &endpoint_, msg_t *msg_, int flags_, bool request_only_,
  pipe_t **pipe_out_, pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    LIBZLINK_UNUSED (endpoint_);
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (flags_);
    LIBZLINK_UNUSED (request_only_);
    LIBZLINK_UNUSED (observer_);
    LIBZLINK_UNUSED (observer_userdata_);
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xselect_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    LIBZLINK_UNUSED (router_rid_or_null_);
    LIBZLINK_UNUSED (target_out_);
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xselect_routed_submit_target_internal (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_)
{
    if (transport_connection_id_out_)
        *transport_connection_id_out_ = 0;
    if (route_incarnation_id_out_)
        *route_incarnation_id_out_ = 0;
    return xselect_routed_submit_target (router_rid_or_null_, target_out_);
}

int zlink::socket_base_t::xselect_request_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_,
  std::string *logical_endpoint_out_)
{
    LIBZLINK_UNUSED (router_rid_or_null_);
    LIBZLINK_UNUSED (target_out_);
    if (transport_connection_id_out_)
        *transport_connection_id_out_ = 0;
    if (route_incarnation_id_out_)
        *route_incarnation_id_out_ = 0;
    if (logical_endpoint_out_)
        logical_endpoint_out_->clear ();
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xrollback ()
{
    return 0;
}

bool zlink::socket_base_t::xhas_in ()
{
    return false;
}

int zlink::socket_base_t::xjoin (const char *group_)
{
    LIBZLINK_UNUSED (group_);
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xleave (const char *group_)
{
    LIBZLINK_UNUSED (group_);
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xrecv (msg_t *)
{
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xrecv_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    return xrecv (msg_);
}

int zlink::socket_base_t::xrecv_routed (msg_t *msg_,
                                       zlink_routing_id_t *source_rid_out_,
                                       uint64_t *connection_id_out_,
                                       pipe_t **source_pipe_out_,
                                       pipe_t::read_admission_fn *admission_,
                                       void *admission_userdata_,
                                       uint64_t *route_binding_token_out_)
{
    LIBZLINK_UNUSED (admission_);
    LIBZLINK_UNUSED (admission_userdata_);
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;
    if (route_binding_token_out_)
        *route_binding_token_out_ = 0;

    const int rc = xrecv (msg_);
    if (rc == 0 && source_rid_out_)
        copy_last_recv_source_rid (source_rid_out_);
    return rc;
}


void zlink::socket_base_t::xread_deactivated (pipe_t *)
{
}

void zlink::socket_base_t::xread_activated (pipe_t *)
{
    zlink_assert (false);
}
void zlink::socket_base_t::xwrite_activated (pipe_t *)
{
    zlink_assert (false);
}

void zlink::socket_base_t::xhiccuped (pipe_t *)
{
    zlink_assert (false);
}
