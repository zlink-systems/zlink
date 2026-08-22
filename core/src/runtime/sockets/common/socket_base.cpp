/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <algorithm>
#include <new>

#include "sockets/common/socket_base.hpp"
#include "core/c_api_copy_internal.hpp"
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
const char peer_weight_cmd_name[] = "WEIGHT";
const size_t peer_weight_cmd_name_size = sizeof (peer_weight_cmd_name) - 1;

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
    return _tag == 0xbaddecaf;
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
    _tag (0xbaddecaf),
    _ctx_terminated (false),
    _runtime (),
    _auto_hwm_role (auto_hwm_role_none),
    _auto_hwm_role_override (false),
    _auto_hwm_policy_enabled (true),
    _manual_sndhwm (false),
    _manual_rcvhwm (false),
    _completion_poller_refs (0),
    _request_completion_pending (false),
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

void zlink::socket_base_t::set_auto_hwm_role (auto_hwm_role_t role_)
{
    _auto_hwm_role = role_;
    _auto_hwm_role_override = role_ != auto_hwm_role_none;
    _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE;
    refresh_auto_hwm_policy ();
}

void zlink::socket_base_t::set_auto_hwm_policy_enabled (bool enabled_)
{
    _auto_hwm_policy_enabled = enabled_;
    _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE;
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
zlink::socket_base_t::prepare_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_)
{
    auto_hwm_role_t role = _auto_hwm_role_override ? _auto_hwm_role : auto_hwm_role_none;
    if (role == auto_hwm_role_none)
        role = auto_hwm_default_role_for_socket_type (options.type);
    _auto_hwm_role = role;

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
    auto_hwm_role_t role =
      _auto_hwm_role_override ? _auto_hwm_role : auto_hwm_role_none;
    if (role == auto_hwm_role_none)
        role = auto_hwm_default_role_for_socket_type (options.type);
    _auto_hwm_role = role;

    scoped_lock_t lock (monitor_runtime ().sync);
    const size_t count = endpoint_runtime ().attached_pipe_count ();
    for (size_t i = 0; i != count; ++i) {
        pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
        if (!pipe
            || pipe->get_transport_lane () == transport_lane_completion)
            continue;

        out_->push_back (make_auto_hwm_queue_policy (
          pipe->out_physical_queue (), true));
        out_->push_back (make_auto_hwm_queue_policy (
          pipe->in_physical_queue (), false));
    }
}

zlink::physical_queue_endpoint_policy_t
zlink::socket_base_t::make_auto_hwm_queue_policy (
  const std::shared_ptr<physical_queue_record_t> &queue_, bool writer_) const
{
    physical_queue_endpoint_policy_t policy;
    policy.queue = queue_;
    policy.role = _auto_hwm_role_override
                    ? _auto_hwm_role
                    : auto_hwm_default_role_for_socket_type (options.type);
    policy.writer = writer_;
    policy.manual = writer_ ? _manual_sndhwm : _manual_rcvhwm;
    policy.planning_enabled = _auto_hwm_policy_enabled;
    policy.hwm = writer_ ? options.sndhwm : options.rcvhwm;
    return policy;
}

void zlink::socket_base_t::apply_physical_auto_hwm_plan (
  const auto_hwm_context_plan_t &context_, uint32_t recalc_reason_)
{
    _auto_hwm_context_plan = context_;
    _auto_hwm_socket_plan = prepare_auto_hwm_socket_plan (context_);
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

static void copy_routing_id (zlink_routing_id_t *out_, const zlink::blob_t &routing_id_)
{
    zlink::copy_routing_id_from_bytes (routing_id_.data (), routing_id_.size (), out_);
}

zlink::socket_base_t::~socket_base_t ()
{
    if (_mailbox)
        LIBZLINK_DELETE (_mailbox);

    scoped_lock_t lock (monitor_runtime ().sync);
    stop_monitor ();

    zlink_assert (lifecycle_coordinator ().is_destroyed ());
}

zlink::i_mailbox *zlink::socket_base_t::get_mailbox () const
{
    return _mailbox;
}

void zlink::socket_base_t::store_socket_msg_part (std::vector<zlink_msg_t> *parts_,
                                                  zlink::msg_t *msg_)
{
    if (!parts_ || !msg_)
        return;

    zlink_msg_t stored;
    memset (&stored, 0, sizeof (stored));
    zlink::msg_t *stored_msg = reinterpret_cast<zlink::msg_t *> (&stored);
    const int init_rc = stored_msg->init ();
    errno_assert (init_rc == 0);
    const int move_rc = stored_msg->move (*msg_);
    errno_assert (move_rc == 0);
    parts_->push_back (stored);
}

int zlink::socket_base_t::init_peer_weight_command (zlink::msg_t *msg_, uint32_t weight_)
{
    if (!msg_) {
        errno = EFAULT;
        return -1;
    }

    const int rc = msg_->init_size (peer_weight_cmd_name_size + 4);
    if (rc != 0)
        return -1;

    memcpy (msg_->data (), peer_weight_cmd_name, peer_weight_cmd_name_size);
    unsigned char *payload =
      static_cast<unsigned char *> (msg_->data ()) + peer_weight_cmd_name_size;
    payload[0] = static_cast<unsigned char> ((weight_ >> 24) & 0xffu);
    payload[1] = static_cast<unsigned char> ((weight_ >> 16) & 0xffu);
    payload[2] = static_cast<unsigned char> ((weight_ >> 8) & 0xffu);
    payload[3] = static_cast<unsigned char> (weight_ & 0xffu);
    msg_->set_flags (zlink::msg_t::command);
    return 0;
}

bool zlink::socket_base_t::decode_peer_weight_command (const zlink::msg_t &msg_,
                                                       uint32_t *weight_out_)
{
    if (!(msg_.flags () & zlink::msg_t::command))
        return false;
    if (msg_.size () != peer_weight_cmd_name_size + 4)
        return false;
    if (memcmp (const_cast<zlink::msg_t &> (msg_).data (), peer_weight_cmd_name,
                peer_weight_cmd_name_size)
        != 0) {
        return false;
    }

    const unsigned char *payload =
      static_cast<unsigned char *> (const_cast<zlink::msg_t &> (msg_).data ())
      + peer_weight_cmd_name_size;
    const uint32_t weight =
      (static_cast<uint32_t> (payload[0]) << 24) | (static_cast<uint32_t> (payload[1]) << 16)
      | (static_cast<uint32_t> (payload[2]) << 8) | static_cast<uint32_t> (payload[3]);
    if (weight > max_peer_weight)
        return false;

    if (weight_out_)
        *weight_out_ = weight;
    return true;
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
  pipe_message_admission_t *admission_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    return xsend (msg_, admission_out_);
}

int zlink::socket_base_t::xsend_routed (const zlink_routing_id_t *target_rid_,
                                       msg_t *msg_,
                                       uint64_t *connection_id_out_,
                                       uint64_t expected_connection_id_,
                                       pipe_t **pipe_out_,
                                       uint64_t expected_transport_pair_id_,
                                       uint64_t expected_transport_pair_generation_,
                                       pipe_message_admission_t *admission_out_)
{
    LIBZLINK_UNUSED (target_rid_);
    LIBZLINK_UNUSED (msg_);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    LIBZLINK_UNUSED (expected_connection_id_);
    LIBZLINK_UNUSED (expected_transport_pair_id_);
    LIBZLINK_UNUSED (expected_transport_pair_generation_);
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

int zlink::socket_base_t::xrecv_retained (
  msg_t *msg_, retained_credit_token_t *token_out_)
{
    if (token_out_)
        token_out_->reset ();
    return xrecv (msg_);
}

int zlink::socket_base_t::xrecv_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    return xrecv (msg_);
}

int zlink::socket_base_t::xrecv_pipe_retained (
  msg_t *msg_, pipe_t **pipe_out_, retained_credit_token_t *token_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    return xrecv_retained (msg_, token_out_);
}

int zlink::socket_base_t::xrecv_routed (msg_t *msg_,
                                       zlink_routing_id_t *source_rid_out_,
                                       uint64_t *connection_id_out_,
                                       pipe_t **source_pipe_out_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;

    const int rc = xrecv (msg_);
    if (rc == 0 && source_rid_out_)
        copy_last_recv_source_rid (source_rid_out_);
    return rc;
}

int zlink::socket_base_t::xrecv_routed_retained (
  msg_t *msg_, zlink_routing_id_t *source_rid_out_,
  uint64_t *connection_id_out_, pipe_t **source_pipe_out_,
  retained_credit_token_t *token_out_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;

    const int rc = xrecv_retained (msg_, token_out_);
    if (rc == 0 && source_rid_out_)
        copy_last_recv_source_rid (source_rid_out_);
    return rc;
}

void zlink::socket_base_t::xarm_socket_msg_dispatch ()
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
