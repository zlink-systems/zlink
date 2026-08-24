/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/dealer/dealer.hpp"
#include "sockets/common/socket_dispatch_loop_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"

namespace
{
}

zlink::dealer_t::dealer_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_), _probe_router (false)
{
    options.type = ZLINK_CORE_SOCKET_DEALER;
    options.can_send_hello_msg = true;
    options.can_recv_hiccup_msg = true;
    refresh_auto_hwm_policy ();
}

zlink::dealer_t::~dealer_t ()
{
    close_socket_msg_parts (&_dispatch_parts);
    for (std::map<pipe_t *, std::vector<zlink_msg_t>>::iterator it =
           _dispatch_parts_by_pipe.begin ();
         it != _dispatch_parts_by_pipe.end (); ++it)
        close_socket_msg_parts (&it->second);
}

int zlink::dealer_t::sendpipe_to (
  pipe_t *pipe_, msg_t *msg_, int flags_,
  pipe_message_admission_t *admission_out_)
{
    if (!pipe_ || !msg_ || !msg_->check ()) {
        errno = EFAULT;
        return -1;
    }
    msg_->reset_flags (msg_t::more);
    if ((flags_ & ZLINK_SNDMORE) != 0)
        msg_->set_flags (msg_t::more);
    return _lb.sendpipe_to (pipe_, msg_, admission_out_);
}

void zlink::dealer_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);
    LIBZLINK_UNUSED (locally_initiated_);

    zlink_assert (pipe_);

    if (_probe_router) {
        msg_t probe_msg;
        int rc = probe_msg.init ();
        errno_assert (rc == 0);

        rc = pipe_->get_transport_pair_id () != 0
               ? pipe_->write_transport_probe_and_flush (&probe_msg)
               : pipe_->write_and_flush (&probe_msg);
        // zlink_assert (rc) is not applicable here, since it is not a bug.
        LIBZLINK_UNUSED (rc);

        rc = probe_msg.close ();
        errno_assert (rc == 0);
    }

    {
        socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
        _fq.attach (pipe_);
        if (socket_msg_dispatch_active ()) {
            pipe_->check_read ();
            _fq.deactivate (pipe_);
        }
    }
    _lb.attach (pipe_);
    if (local_peer_weight () != 100)
        send_local_peer_weight (pipe_);
}

int zlink::dealer_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_) {
        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
            if (is_int && value >= 0) {
                _probe_router = (value != 0);
                return 0;
            }
            break;

        default:
            break;
    }

    errno = EINVAL;
    return -1;
}

int zlink::dealer_t::xgetsockopt (int option_, void *optval_, size_t *optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_PROBE_ROUTER) {
        if (!optval_ || !optvallen_ || *optvallen_ != sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *static_cast<int *> (optval_) = _probe_router ? 1 : 0;
        return 0;
    }

    return socket_base_t::xgetsockopt (option_, optval_, optvallen_);
}

int zlink::dealer_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    return sendpipe (msg_, NULL, admission_out_);
}

int zlink::dealer_t::xsend_pipe (
  msg_t *msg_, pipe_t **pipe_out_,
  pipe_message_admission_t *admission_out_)
{
    return sendpipe (msg_, pipe_out_, admission_out_);
}

int zlink::dealer_t::xsend_routed (
  const zlink_routing_id_t *target_rid_, msg_t *msg_,
  uint64_t *connection_id_out_, uint64_t expected_connection_id_,
  pipe_t **pipe_out_, uint64_t expected_transport_pair_id_,
  uint64_t expected_transport_pair_generation_,
  pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (!target_rid_ || expected_transport_pair_id_ == 0
        || expected_transport_pair_generation_ == 0) {
        errno = ENOTSUP;
        return -1;
    }

    pipe_t *pipe = _lb.find_connected_pipe (
      target_rid_->data, target_rid_->size, expected_transport_pair_id_,
      expected_transport_pair_generation_);
    if (!pipe || !transport_pair_application_ready (pipe)) {
        errno = EHOSTUNREACH;
        return -1;
    }
    const uint64_t connection_id = pipe->get_transport_connection_id ();
    if (expected_connection_id_ != 0
        && connection_id != expected_connection_id_) {
        errno = EHOSTUNREACH;
        return -1;
    }

    if (connection_id_out_)
        *connection_id_out_ = connection_id;
    if (pipe_out_)
        *pipe_out_ = pipe;
    return sendpipe_to (
      pipe, msg_,
      (msg_->flags () & msg_t::more) != 0 ? ZLINK_SNDMORE : 0,
      admission_out_);
}

int zlink::dealer_t::xselect_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    if (router_rid_or_null_ || !target_out_) {
        errno = EINVAL;
        return -1;
    }

    pipe_t *selected = NULL;
    const int rc = _lb.select_connected_pipe (
      &selected,
      [] (pipe_t *pipe_, void *userdata_) -> bool {
          dealer_t *dealer = static_cast<dealer_t *> (userdata_);
          return dealer && pipe_ && pipe_->get_routing_id ().size () != 0
                 && dealer->transport_pair_application_ready (pipe_);
      },
      this);
    if (rc != 0)
        return -1;

    const blob_t &routing_id = selected->get_routing_id ();
    copy_routing_id_from_bytes (routing_id.data (), routing_id.size (),
                                &target_out_->peer_rid);
    target_out_->transport_pair_id = selected->get_transport_pair_id ();
    target_out_->transport_pair_generation =
      selected->get_transport_pair_generation ();
    return 0;
}

int zlink::dealer_t::xrecv (msg_t *msg_)
{
    pipe_t *pipe = NULL;
    return recvpipe (msg_, &pipe);
}

int zlink::dealer_t::xrecv_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    return recvpipe (msg_, pipe_out_);
}

bool zlink::dealer_t::xhas_in ()
{
    return _fq.has_in ();
}

bool zlink::dealer_t::xhas_out ()
{
    return _lb.has_out ();
}

int zlink::dealer_t::xrollback ()
{
    _lb.rollback ();
    return 0;
}

void zlink::dealer_t::xread_activated (pipe_t *pipe_)
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    _fq.activated (pipe_);
    if (!socket_msg_dispatch_active ())
        return;
    zlink::drain_socket_dispatch_loop (
      [this] (msg_t *msg_, pipe_t **pipe_out_) { return recvpipe (msg_, pipe_out_); },
      [this] (msg_t *msg_, pipe_t *pipe_) {
          return socket_msg_dispatch_from_io (msg_, pipe_);
      });
}

void zlink::dealer_t::xwrite_activated (pipe_t *pipe_)
{
    _lb.activated (pipe_);
}

void zlink::dealer_t::xpipe_terminated (pipe_t *pipe_)
{
    std::map<pipe_t *, std::vector<zlink_msg_t>>::iterator parts_it =
      _dispatch_parts_by_pipe.find (pipe_);
    if (parts_it != _dispatch_parts_by_pipe.end ()) {
        close_socket_msg_parts (&parts_it->second);
        _dispatch_parts_by_pipe.erase (parts_it);
    }
    _fq.pipe_terminated (pipe_);
    _lb.pipe_terminated (pipe_);
}

int zlink::dealer_t::sendpipe (
  msg_t *msg_, pipe_t **pipe_,
  pipe_message_admission_t *admission_out_)
{
    return _lb.sendpipe (msg_, pipe_, admission_out_);
}

int zlink::dealer_t::recvpipe (msg_t *msg_, pipe_t **pipe_)
{
    return _fq.recvpipe (msg_, pipe_);
}

int zlink::dealer_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    if (!socket_msg_dispatch_active ())
        return 0;

    std::vector<zlink_msg_t> *dispatch_parts = pipe_ ? &_dispatch_parts_by_pipe[pipe_]
                                                      : &_dispatch_parts;
    store_socket_msg_part (dispatch_parts, msg_);
    if ((reinterpret_cast<msg_t *> (&dispatch_parts->back ())->flags () & msg_t::more) != 0) {
        return 1;
    }

    zlink_socket_msg_handler_fn handler = socket_msg_handler ();
    if (!handler) {
        close_socket_msg_parts (dispatch_parts);
        if (pipe_)
            _dispatch_parts_by_pipe.erase (pipe_);
        return 1;
    }

    zlink_routing_id_t source_rid;
    resolve_socket_msg_source_rid (pipe_, &source_rid);
    invoke_socket_msg_handler (handler, &source_rid, &(*dispatch_parts)[0],
                               dispatch_parts->size ());
    dispatch_parts->clear ();
    if (pipe_)
        _dispatch_parts_by_pipe.erase (pipe_);
    return 1;
}

void zlink::dealer_t::xarm_socket_msg_dispatch ()
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    _fq.arm_dispatch ();
}

void zlink::dealer_t::xdispatch_io ()
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    if (!socket_msg_dispatch_active ())
        return;
    zlink::drain_socket_dispatch_loop (
      [this] (msg_t *msg_, pipe_t **pipe_out_) { return recvpipe (msg_, pipe_out_); },
      [this] (msg_t *msg_, pipe_t *pipe_) {
          return socket_msg_dispatch_from_io (msg_, pipe_);
      });
}

int zlink::dealer_t::apply_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return 1;

    const bool changed = _lb.weight (pipe_) != weight_;
    _lb.set_weight (pipe_, weight_);
    if (!changed)
        return 1;
    emit_peer_weight_changed (pipe_, weight_);
    return 1;
}
