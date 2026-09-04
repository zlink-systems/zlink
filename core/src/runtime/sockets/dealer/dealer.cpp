/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/dealer/dealer.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"

zlink::dealer_t::dealer_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_),
    _fq (fq_t::publish_receive_activity),
    _probe_router (false)
{
    options.type = ZLINK_CORE_SOCKET_DEALER;
    options.can_send_hello_msg = true;
    options.can_recv_hiccup_msg = true;
    refresh_auto_hwm_policy ();
}

zlink::dealer_t::~dealer_t ()
{
}

int zlink::dealer_t::sendpipe_to (
  pipe_t *pipe_, msg_t *msg_, int flags_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    if (!pipe_ || !msg_ || !msg_->check ()) {
        errno = EFAULT;
        return -1;
    }
    msg_->reset_flags (msg_t::more);
    if ((flags_ & ZLINK_SNDMORE) != 0)
        msg_->set_flags (msg_t::more);
    return _lb.sendpipe_to (pipe_, msg_, admission_out_, observer_,
                            observer_userdata_);
}

bool zlink::dealer_t::active_submit_candidate (dealer_t *dealer_, pipe_t *pipe_)
{
    return pipe_->is_lifecycle_active ()
           && pipe_->get_transport_connection_id () != 0
           && dealer_->transport_pair_application_ready (pipe_);
}

bool zlink::dealer_t::routed_submit_candidate (pipe_t *pipe_, void *userdata_)
{
    dealer_t *const dealer = static_cast<dealer_t *> (userdata_);
    return dealer && pipe_ && pipe_->get_routing_id ().size () != 0
           && active_submit_candidate (dealer, pipe_);
}

bool zlink::dealer_t::request_submit_candidate (pipe_t *pipe_, void *userdata_)
{
    dealer_t *const dealer = static_cast<dealer_t *> (userdata_);
    return dealer && pipe_
           && pipe_->get_peer_socket_type () == ZLINK_CORE_SOCKET_ROUTER
           && active_submit_candidate (dealer, pipe_);
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

    _fq.attach (pipe_);
    {
        scoped_fast_lock_t generation_lock (pipe_->transport_sync ());
        uint32_t initial_weight = 100;
        (void) recorded_peer_weight_ready_locked (pipe_, &initial_weight);
        _lb.attach (pipe_, initial_weight);
        remember_request_route (pipe_, initial_weight);
    }
    // Paired pipes remain held here and publish from the one pair-ready resync;
    // unpaired network/inproc pipes publish at their normal attach boundary.
    if (local_peer_weight () != 100)
        (void) send_local_peer_weight (pipe_);
}

int zlink::dealer_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_) {
        case ZLINK_INTERNAL_OPT_CONFLATE:
            // DEALER exchanges protocol control frames on the same pipe as
            // application records.  Frame-level conflation cannot preserve
            // both, so enabling it is deliberately unsupported.
            if (is_int && value == 1) {
                errno = ENOTSUP;
                return -1;
            }
            break;

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
    return _lb.sendpipe (msg_, NULL, admission_out_);
}

int zlink::dealer_t::xsend_pipe (
  msg_t *msg_, pipe_t **pipe_out_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    return _lb.sendpipe (msg_, pipe_out_, admission_out_, observer_,
                         observer_userdata_);
}

int zlink::dealer_t::xsend_routed (
  const zlink_routing_id_t *target_rid_, msg_t *msg_,
  uint64_t *connection_id_out_, uint64_t expected_connection_id_,
  pipe_t **pipe_out_, uint64_t expected_transport_pair_id_,
  uint64_t expected_transport_pair_generation_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_,
  routed_send_attempt_identity_t *attempt_identity_out_,
  uint64_t expected_route_incarnation_id_, bool request_only_)
{
    LIBZLINK_UNUSED (expected_route_incarnation_id_);
    LIBZLINK_UNUSED (request_only_);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (attempt_identity_out_)
        attempt_identity_out_->reset ();
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
    if (attempt_identity_out_) {
        attempt_identity_out_->transport_pair_id =
          pipe->get_transport_pair_id ();
        attempt_identity_out_->transport_pair_generation =
          pipe->get_transport_pair_generation ();
        attempt_identity_out_->transport_connection_id = connection_id;
    }
    return sendpipe_to (
      pipe, msg_,
      (msg_->flags () & msg_t::more) != 0 ? ZLINK_SNDMORE : 0,
      admission_out_, observer_, observer_userdata_);
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
      &selected, &dealer_t::routed_submit_candidate, this);
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

int zlink::dealer_t::xselect_routed_submit_pipe (pipe_t **pipe_out_,
                                                 bool request_only_)
{
    if (!pipe_out_) {
        errno = EFAULT;
        return -1;
    }
    *pipe_out_ = NULL;
    const int rc = _lb.select_connected_pipe (
      pipe_out_,
      request_only_ ? &dealer_t::request_submit_candidate
                    : &dealer_t::routed_submit_candidate,
      this);
    if (rc != 0)
        return -1;
    return 0;
}

int zlink::dealer_t::xcommit_request_submit_pipe (pipe_t *pipe_)
{
    if (!pipe_) {
        errno = EFAULT;
        return -1;
    }
    const std::string &endpoint = pipe_->get_endpoint_pair ().identifier ();
    if (endpoint.empty ()) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (_request_route_history.find (endpoint)
        == _request_route_history.end ())
        remember_request_route (pipe_, _lb.weight (pipe_));
    return 0;
}

int zlink::dealer_t::xsend_selected_pipe (
  pipe_t *pipe_, msg_t *msg_, int flags_, bool request_only_,
  pipe_message_admission_t *admission_out_, pipe_write_observer_fn observer_,
  void *observer_userdata_)
{
    return send_selected_pipe (pipe_, msg_, flags_, request_only_, NULL,
                               admission_out_, observer_, observer_userdata_);
}

int zlink::dealer_t::send_selected_pipe (
  pipe_t *pipe_, msg_t *msg_, int flags_, bool request_only_,
  pipe_t **pipe_out_, pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    if (!pipe_) {
        errno = EAGAIN;
        return -1;
    }
    if (request_only_
        && pipe_->get_peer_socket_type () != ZLINK_CORE_SOCKET_ROUTER) {
        errno = EPROTOTYPE;
        return -1;
    }
    if (_lb.weight (pipe_) == 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    if (!transport_pair_application_ready (pipe_)) {
        errno = EAGAIN;
        return -1;
    }
    if (pipe_out_)
        *pipe_out_ = pipe_;
    return sendpipe_to (
      pipe_, msg_, flags_, admission_out_, observer_, observer_userdata_);
}

int zlink::dealer_t::xsend_configured_endpoint (
  const std::string &endpoint_, msg_t *msg_, int flags_, bool request_only_,
  pipe_t **pipe_out_, pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    pipe_t *const pipe = _lb.find_pipe_by_endpoint (
      endpoint_, request_only_ ? &dealer_t::request_submit_candidate
                               : &dealer_t::routed_submit_candidate,
      this);
    if (!pipe) {
        errno = EAGAIN;
        return -1;
    }
    return send_selected_pipe (pipe, msg_, flags_, request_only_, pipe_out_,
                               admission_out_, observer_, observer_userdata_);
}

int zlink::dealer_t::xselect_request_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_,
  std::string *logical_endpoint_out_)
{
    if (router_rid_or_null_ || !target_out_ || !logical_endpoint_out_) {
        errno = EINVAL;
        return -1;
    }
    memset (target_out_, 0, sizeof (*target_out_));
    logical_endpoint_out_->clear ();
    if (transport_connection_id_out_)
        *transport_connection_id_out_ = 0;
    if (route_incarnation_id_out_)
        *route_incarnation_id_out_ = 0;

    pipe_t *selected = NULL;
    if (_lb.select_connected_pipe (
          &selected, &dealer_t::request_submit_candidate, this)
        == 0) {
        const std::string &endpoint =
          selected->get_endpoint_pair ().identifier ();
        if (endpoint.empty ()) {
            errno = EHOSTUNREACH;
            return -1;
        }
        if (_request_route_history.find (endpoint)
            == _request_route_history.end ())
            remember_request_route (selected, _lb.weight (selected));
        try {
            *logical_endpoint_out_ = endpoint;
        } catch (...) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }

    request_route_history_map_t::iterator selected_route =
      _request_route_history.end ();
    int64_t selected_value = 0;
    int64_t total_weight = 0;
    for (request_route_history_map_t::iterator route =
           _request_route_history.begin ();
         route != _request_route_history.end (); ++route) {
        if (route->second.weight == 0)
            continue;
        total_weight += route->second.weight;
        const int64_t value =
          route->second.running_value + route->second.weight;
        if (selected_route == _request_route_history.end ()
            || value > selected_value
            || (value == selected_value
                && (route->second.peer_rid < selected_route->second.peer_rid
                    || (route->second.peer_rid
                          == selected_route->second.peer_rid
                        && route->first < selected_route->first)))) {
            selected_route = route;
            selected_value = value;
        }
    }
    if (selected_route == _request_route_history.end ()) {
        errno = _request_route_history.empty () ? ENOTCONN : ECONNREFUSED;
        return -1;
    }
    for (request_route_history_map_t::iterator route =
           _request_route_history.begin ();
         route != _request_route_history.end (); ++route) {
        if (route->second.weight != 0)
            route->second.running_value += route->second.weight;
    }
    selected_route->second.running_value -= total_weight;
    try {
        *logical_endpoint_out_ = selected_route->first;
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void zlink::dealer_t::xforget_request_route_endpoint (
  const std::string &endpoint_)
{
    _request_route_history.erase (endpoint_);
}

int zlink::dealer_t::xrecv (msg_t *msg_)
{
    return recvpipe (msg_, NULL);
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
    _fq.activated (pipe_);
}

void zlink::dealer_t::xread_deactivated (pipe_t *pipe_)
{
    _fq.deactivate (pipe_);
}

void zlink::dealer_t::xwrite_activated (pipe_t *pipe_)
{
    _lb.activated (pipe_);
}

void zlink::dealer_t::xpipe_terminated (pipe_t *pipe_)
{
    _fq.pipe_terminated (pipe_);
    _lb.pipe_terminated (pipe_);
}

int zlink::dealer_t::recvpipe (msg_t *msg_, pipe_t **pipe_)
{
    pipe_t *source = NULL;
    const int rc = _fq.recvpipe (msg_, &source);
    if (pipe_)
        *pipe_ = rc == 0 ? source : NULL;
    if (rc == 0 && source
        && (msg_->flags () & msg_t::more) == 0)
        (void) reclassify_transport_pair_application_head (source);
    return rc;
}

int zlink::dealer_t::apply_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    // An owner command may precede socket admission. The exact pipe already
    // retains the generation-tagged value; only a registered scheduler entry
    // may mutate selection or publish a monitor event. xattach replays the
    // cache after _lb.attach.
    if (!pipe_ || !_lb.contains (pipe_))
        return 1;

    const bool changed = _lb.weight (pipe_) != weight_;
    _lb.set_weight (pipe_, weight_);
    update_request_route_weight (pipe_, weight_);
    if (!changed)
        return 1;
    notify_send_writable (pipe_);
    emit_peer_weight_changed (pipe_, weight_);
    return 1;
}

void zlink::dealer_t::initialize_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (pipe_ && _lb.contains (pipe_)) {
        _lb.set_weight (pipe_, weight_);
        update_request_route_weight (pipe_, weight_);
    }
}

void zlink::dealer_t::update_request_route_weight (pipe_t *pipe_,
                                                   uint32_t weight_)
{
    if (pipe_->get_peer_socket_type () != ZLINK_CORE_SOCKET_ROUTER)
        return;

    const std::string &endpoint = pipe_->get_endpoint_pair ().identifier ();
    request_route_history_map_t::iterator route =
      _request_route_history.find (endpoint);
    if (route != _request_route_history.end ())
        route->second.weight = weight_;
    else
        remember_request_route (pipe_, weight_);
}

void zlink::dealer_t::remember_request_route (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_
        || pipe_->get_peer_socket_type () != ZLINK_CORE_SOCKET_ROUTER)
        return;
    const std::string &endpoint = pipe_->get_endpoint_pair ().identifier ();
    if (endpoint.empty ())
        return;

    request_route_history_t &route = _request_route_history[endpoint];
    const blob_t &routing_id = pipe_->get_routing_id ();
    route.peer_rid.assign (
      routing_id.size () != 0
        ? reinterpret_cast<const char *> (routing_id.data ())
        : "",
      routing_id.size ());
    route.weight = weight_;
    // A newly attached physical route starts a fresh weighted history.
    route.running_value = 0;
}

#ifdef ZLINK_BUILD_TESTS
uint32_t zlink::dealer_t::test_peer_weight (pipe_t *pipe_) const
{
    return _lb.weight (pipe_);
}

size_t zlink::dealer_t::test_peer_weight_count (uint32_t weight_) const
{
    return _lb.test_weight_count (weight_);
}
#endif
