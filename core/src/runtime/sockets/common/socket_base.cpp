/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <algorithm>
#include <new>

#include "sockets/common/socket_base.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/ctx.hpp"
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
    _auto_hwm_scope (auto_hwm_scope_none),
    _auto_hwm_scope_count (1),
    _auto_hwm_role_override (false),
    _auto_hwm_policy_enabled (true),
    _auto_hwm_msg_unit_override (false),
    _manual_sndhwm (false),
    _manual_rcvhwm (false),
      _manual_sndbuf (false),
      _manual_rcvbuf (false),
      _completion_async_owned (false),
      _completion_poller_refs (0),
      _request_completion_pending (false),
    _auto_hwm_context_plan (),
    _auto_hwm_socket_plan (),
    _auto_hwm_connection_bucket_state_valid (false),
    _auto_hwm_connection_bucket_index (auto_hwm_connection_bucket_none),
    _auto_hwm_connection_bucket_profile (ZLINK_AUTO_HWM_PROFILE_BALANCED),
    _auto_hwm_connection_bucket_message_bytes (0),
    _auto_hwm_last_recalc_ms (0),
    _auto_hwm_last_recalc_reason (ZLINK_AUTO_HWM_RECALC_REASON_NONE),
    _auto_hwm_deferred_sndhwm (0),
    _auto_hwm_deferred_rcvhwm (0),
    _auto_hwm_deferred_sndhwm_valid (false),
    _auto_hwm_deferred_rcvhwm_valid (false),
    _auto_hwm_send_attempts (0),
    _auto_hwm_send_blocked_attempts (0),
    _local_peer_weight (100)
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

void zlink::socket_base_t::set_auto_hwm_scope (auto_hwm_scope_t scope_, size_t scope_count_)
{
    _auto_hwm_scope = scope_;
    _auto_hwm_scope_count = scope_count_ > 0 ? scope_count_ : 1;
    _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_REFRESH;
    refresh_auto_hwm_policy ();
}

void zlink::socket_base_t::clear_auto_hwm_manual_overrides (bool sndhwm_,
                                                            bool rcvhwm_,
                                                            bool sndbuf_,
                                                            bool rcvbuf_)
{
    if (sndhwm_)
        _manual_sndhwm = false;
    if (rcvhwm_)
        _manual_rcvhwm = false;
    if (sndbuf_)
        _manual_sndbuf = false;
    if (rcvbuf_)
        _manual_rcvbuf = false;
}

void zlink::socket_base_t::set_auto_hwm_policy_enabled (bool enabled_)
{
    const bool was_enabled = _auto_hwm_policy_enabled;
    _auto_hwm_policy_enabled = enabled_;
    _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE;

    if (was_enabled && !enabled_) {
        options_t defaults;
        defaults.type = options.type;

        bool refresh_hwms = false;
        if (!_manual_sndhwm && options.sndhwm != defaults.sndhwm) {
            options.sndhwm = defaults.sndhwm;
            refresh_hwms = true;
        }
        if (!_manual_rcvhwm && options.rcvhwm != defaults.rcvhwm) {
            options.rcvhwm = defaults.rcvhwm;
            refresh_hwms = true;
        }
        if (!_manual_sndbuf)
            options.sndbuf = defaults.sndbuf;
        if (!_manual_rcvbuf)
            options.rcvbuf = defaults.rcvbuf;

        if (refresh_hwms)
            refresh_attached_pipe_hwms ();
    }

    refresh_auto_hwm_policy ();
}

bool zlink::socket_base_t::auto_hwm_policy_enabled () const
{
    return _auto_hwm_policy_enabled;
}

zlink::auto_hwm_socket_plan_t
zlink::socket_base_t::prepare_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_)
{
    auto_hwm_role_t role = _auto_hwm_role_override ? _auto_hwm_role : auto_hwm_role_none;
    if (role == auto_hwm_role_none)
        role = auto_hwm_default_role_for_socket_type (options.type);
    _auto_hwm_role = role;

    size_t managed_connections = 0;
    uint64_t pending_messages = 0;
    uint64_t pending_bytes = 0;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        const size_t attached_pipe_count = endpoint_runtime ().attached_pipe_count ();
        managed_connections = attached_pipe_count;
        const size_t ready_connections = static_cast<size_t> (monitor_runtime ().ready_count ());
        if (ready_connections > managed_connections)
            managed_connections = ready_connections;
        for (size_t i = 0; i != attached_pipe_count; ++i) {
            pipe_t *pipe = endpoint_runtime ().attached_pipe (i);
            if (!pipe)
                continue;
            pending_messages += pipe->get_snd_pending_msgs ();
            pending_messages += pipe->get_rcv_pending_msgs_approx ();
            const uint64_t pipe_pending_bytes =
              pipe->get_snd_pending_bytes () + pipe->get_rcv_pending_bytes_approx ();
            pending_bytes =
              UINT64_MAX - pending_bytes < pipe_pending_bytes
                ? UINT64_MAX
                : pending_bytes + pipe_pending_bytes;
        }
    }

    auto_hwm_socket_plan_t plan;
    ctx_t *ctx = get_ctx ();
    const uint64_t message_unit_bytes =
      _auto_hwm_msg_unit_override
        ? options.auto_hwm_msg_unit_bytes
        : (ctx ? ctx->auto_hwm_msg_unit_bytes () : context_.message_unit_bytes);
    auto_hwm_socket_plan_prepare (role, options.type, managed_connections, managed_connections,
                                  &plan, message_unit_bytes, options.sndbuf, options.rcvbuf,
                                  _manual_sndbuf, _manual_rcvbuf, _auto_hwm_scope,
                                  _auto_hwm_scope_count, true);
    plan.pending_messages = pending_messages;
    plan.pending_bytes = pending_bytes;
    return plan;
}

void zlink::socket_base_t::apply_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_,
                                                       const auto_hwm_socket_plan_t &plan_,
                                                       bool force_apply_,
                                                       uint32_t recalc_reason_)
{
    _auto_hwm_context_plan = context_;
    _auto_hwm_socket_plan = plan_;

    if (!_auto_hwm_context_plan.enabled || (!_auto_hwm_policy_enabled && !force_apply_))
        return;

    uint32_t recalc_reason = recalc_reason_;
    bool refresh_hwms = false;
    if (!_manual_sndhwm && options.sndhwm != _auto_hwm_socket_plan.sndhwm) {
        if (_auto_hwm_socket_plan.sndhwm >= options.sndhwm
            || _auto_hwm_socket_plan.pending_bytes <= _auto_hwm_socket_plan.sndhwm) {
            options.sndhwm = _auto_hwm_socket_plan.sndhwm;
            _auto_hwm_deferred_sndhwm = 0;
            _auto_hwm_deferred_sndhwm_valid = false;
            refresh_hwms = true;
        } else {
            _auto_hwm_deferred_sndhwm = _auto_hwm_socket_plan.sndhwm;
            _auto_hwm_deferred_sndhwm_valid = true;
            recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK;
        }
    }
    if (!_manual_rcvhwm && options.rcvhwm != _auto_hwm_socket_plan.rcvhwm) {
        if (_auto_hwm_socket_plan.rcvhwm >= options.rcvhwm
            || _auto_hwm_socket_plan.pending_bytes <= _auto_hwm_socket_plan.rcvhwm) {
            options.rcvhwm = _auto_hwm_socket_plan.rcvhwm;
            _auto_hwm_deferred_rcvhwm = 0;
            _auto_hwm_deferred_rcvhwm_valid = false;
            refresh_hwms = true;
        } else {
            _auto_hwm_deferred_rcvhwm = _auto_hwm_socket_plan.rcvhwm;
            _auto_hwm_deferred_rcvhwm_valid = true;
            recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK;
        }
    }
    if (!_manual_sndbuf)
        options.sndbuf = _auto_hwm_socket_plan.requested_sndbuf;
    if (!_manual_rcvbuf)
        options.rcvbuf = _auto_hwm_socket_plan.requested_rcvbuf;

    if (refresh_hwms)
        refresh_attached_pipe_hwms ();

    _auto_hwm_last_recalc_ms = _clock.now_ms ();
    _auto_hwm_last_recalc_reason = recalc_reason;
}

void zlink::socket_base_t::record_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_,
                                                        const auto_hwm_socket_plan_t &plan_,
                                                        uint32_t recalc_reason_)
{
    uint32_t recalc_reason = recalc_reason_;
    if (recalc_reason == ZLINK_AUTO_HWM_RECALC_REASON_NONE) {
        recalc_reason = _auto_hwm_last_recalc_ms == 0 ? ZLINK_AUTO_HWM_RECALC_REASON_INITIAL
                                                      : ZLINK_AUTO_HWM_RECALC_REASON_REFRESH;
    }
    _auto_hwm_context_plan = context_;
    _auto_hwm_socket_plan = plan_;
    _auto_hwm_last_recalc_ms = _clock.now_ms ();
    _auto_hwm_last_recalc_reason = recalc_reason;
}

bool zlink::socket_base_t::auto_hwm_connection_bucket_state (
  uint32_t *bucket_index_out_,
  zlink_auto_hwm_profile_t *profile_out_,
  uint64_t *effective_message_bytes_out_) const
{
    if (!_auto_hwm_connection_bucket_state_valid)
        return false;
    if (bucket_index_out_)
        *bucket_index_out_ = _auto_hwm_connection_bucket_index;
    if (profile_out_)
        *profile_out_ = _auto_hwm_connection_bucket_profile;
    if (effective_message_bytes_out_)
        *effective_message_bytes_out_ = _auto_hwm_connection_bucket_message_bytes;
    return true;
}

void zlink::socket_base_t::set_auto_hwm_connection_bucket_state (
  uint32_t bucket_index_,
  zlink_auto_hwm_profile_t profile_,
  uint64_t effective_message_bytes_)
{
    if (bucket_index_ == auto_hwm_connection_bucket_none) {
        clear_auto_hwm_connection_bucket_state ();
        return;
    }
    _auto_hwm_connection_bucket_state_valid = true;
    _auto_hwm_connection_bucket_index = bucket_index_;
    _auto_hwm_connection_bucket_profile = profile_;
    _auto_hwm_connection_bucket_message_bytes = effective_message_bytes_;
}

void zlink::socket_base_t::clear_auto_hwm_connection_bucket_state ()
{
    _auto_hwm_connection_bucket_state_valid = false;
    _auto_hwm_connection_bucket_index = auto_hwm_connection_bucket_none;
    _auto_hwm_connection_bucket_profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    _auto_hwm_connection_bucket_message_bytes = 0;
}

void zlink::socket_base_t::refresh_auto_hwm_policy (bool force_apply_)
{
    ctx_t *ctx = get_ctx ();
    if (!ctx)
        return;

    uint32_t recalc_reason = _auto_hwm_last_recalc_reason;
    if (recalc_reason == ZLINK_AUTO_HWM_RECALC_REASON_NONE) {
        recalc_reason = _auto_hwm_last_recalc_ms == 0 ? ZLINK_AUTO_HWM_RECALC_REASON_INITIAL
                                                      : ZLINK_AUTO_HWM_RECALC_REASON_REFRESH;
    }

    const bool enabled = ctx->get (ZLINK_CTX_OPT_AUTO_HWM_ENABLE) != 0;
    auto_hwm_context_plan_make (enabled, ctx->auto_hwm_profile (), &_auto_hwm_context_plan,
                                ctx->auto_hwm_msg_unit_bytes ());
    _auto_hwm_socket_plan = prepare_auto_hwm_socket_plan (_auto_hwm_context_plan);
    auto_hwm_context_finalize (&_auto_hwm_context_plan, &_auto_hwm_socket_plan, 1);
    apply_auto_hwm_socket_plan (_auto_hwm_context_plan, _auto_hwm_socket_plan, force_apply_,
                                recalc_reason);
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

int zlink::socket_base_t::xsend (msg_t *)
{
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xsend_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    return xsend (msg_);
}

int zlink::socket_base_t::xsend_routed (const zlink_routing_id_t *target_rid_,
                                       msg_t *msg_,
                                       uint64_t *connection_id_out_,
                                       uint64_t expected_connection_id_,
                                       pipe_t **pipe_out_)
{
    LIBZLINK_UNUSED (target_rid_);
    LIBZLINK_UNUSED (msg_);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    LIBZLINK_UNUSED (expected_connection_id_);
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
