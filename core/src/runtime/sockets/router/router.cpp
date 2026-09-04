/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/router/router.hpp"
#include "sockets/router/router_debug.hpp"
#include "core/pipe.hpp"
#include "utils/random.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include <cstdio>

const bool zlink::router_debug::enabled_flag =
  zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

namespace
{
const int router_transport_write_batch_size = 16 * 1024;
}

zlink::router_t::router_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
    _fq (fq_t::publish_receive_activity),
    _prefetched (false),
    _routing_id_sent (false),
    _current_in (NULL),
    _terminate_current_in (false),
    _more_in (false),
    _current_out (NULL),
    _current_out_connection_id (0),
    _more_out (false),
    _next_integral_routing_id (generate_random ()),
    _mandatory (true),
    _probe_router (false),
    _handover (options.rid_duplicate_policy == ZLINK_RID_DUPLICATE_HANDOVER)
{
    options.type = ZLINK_CORE_SOCKET_ROUTER;
    options.out_batch_size = router_transport_write_batch_size;
    options.recv_routing_id = true;
    options.can_send_hello_msg = true;
    options.can_recv_disconnect_msg = true;
    refresh_auto_hwm_policy ();

    _prefetched_id.init ();
    _prefetched_msg.init ();
}

zlink::router_t::~router_t ()
{
    zlink_assert (_anonymous_pipes.empty ());
    _prefetched_id.close ();
    _prefetched_msg.close ();
}

int zlink::router_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_) {
        case ZLINK_INTERNAL_OPT_ROUTER_MANDATORY:
            if (is_int && value >= 0) {
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _mandatory = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
            if (is_int && value >= 0) {
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _probe_router = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
            if (is_int && value >= 0) {
                if (option_ == ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY
                    && value != ZLINK_RID_DUPLICATE_REJECT && value != ZLINK_RID_DUPLICATE_HANDOVER)
                    break;
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _handover = option_ == ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY
                              ? value == ZLINK_RID_DUPLICATE_HANDOVER
                              : value != 0;
                return 0;
            }
            break;

        default:
            return routing_socket_base_t::xsetsockopt (option_, optval_, optvallen_);
    }
    errno = EINVAL;
    return -1;
}

int zlink::router_t::xgetsockopt (int option_, void *optval_, size_t *optvallen_)
{
    if (!optval_ || !optvallen_ || *optvallen_ != sizeof (int)) {
        errno = EINVAL;
        return -1;
    }

    int *value = static_cast<int *> (optval_);
    switch (option_) {
        case ZLINK_INTERNAL_OPT_ROUTER_MANDATORY:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _mandatory ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _probe_router ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _handover ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _handover ? ZLINK_RID_DUPLICATE_HANDOVER : ZLINK_RID_DUPLICATE_REJECT;
            return 0;
        }
        default:
            return routing_socket_base_t::xgetsockopt (option_, optval_, optvallen_);
    }
}


void zlink::router_t::xpipe_terminated (pipe_t *pipe_)
{
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    // Receive-side ownership is released before socket-message teardown takes
    // its dispatch fence. Keep only FQ/current-record state in this phase.
    if (pipe_ == _current_in) {
        // A prefetched frame still belongs to the terminating pipe. It must
        // not be presented with metadata from the next active pipe.
        if (_prefetched && !_routing_id_sent) {
            int rc = _prefetched_id.close ();
            errno_assert (rc == 0);
            rc = _prefetched_id.init ();
            errno_assert (rc == 0);
            rc = _prefetched_msg.close ();
            errno_assert (rc == 0);
            rc = _prefetched_msg.init ();
            errno_assert (rc == 0);
            _prefetched = false;
        }
        if (!_prefetched)
            _routing_id_sent = false;
        _current_in = NULL;
        _terminate_current_in = false;
        _more_in = false;
    }
    _fq.pipe_terminated (pipe_);
}

void zlink::router_t::xsocket_msg_pipe_terminated (pipe_t *pipe_)
{
    bool rollback_outbound = false;
    pipe_t *promoted_writable_pipe = NULL;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        // Direct network dispatch can register an anonymous ROUTER route on a
        // session thread. Route teardown therefore shares this post-receive
        // dispatch fence as well; otherwise termination can race that
        // registration or leave a late partial record keyed by a dead endpoint.
        const blob_t &terminated_routing_id = pipe_->get_routing_id ();
        const std::map<pipe_t *, blob_t>::iterator terminated_standby =
          _standby_pipes.find (pipe_);
        const bool was_standby = terminated_standby != _standby_pipes.end ();
        if (was_standby) {
            _standby_pipes.erase (terminated_standby);
        }

        pipe_t *standby_to_promote = NULL;
        blob_t standby_routing_id;
        if (!was_standby) {
            for (std::map<pipe_t *, blob_t>::iterator standby =
                   _standby_pipes.begin ();
                 standby != _standby_pipes.end (); ++standby) {
                const bool same_routing_id =
                  !(standby->second < terminated_routing_id)
                  && !(terminated_routing_id < standby->second);
                if (!same_routing_id)
                    continue;
                standby_to_promote = standby->first;
                standby_routing_id =
                  blob_t (standby->second.data (), standby->second.size ());
                _standby_pipes.erase (standby);
                break;
            }
        }

        if (router_debug::enabled ()) {
            char rid_text[160];
            router_debug::format_routing_id (
              pipe_->get_routing_id (), rid_text, sizeof (rid_text));
            fprintf (stderr,
                     "router xpipe_terminated: pipe=%p rid=%s anonymous=%d\n",
                     static_cast<void *> (pipe_), rid_text,
                     _anonymous_pipes.count (pipe_) != 0 ? 1 : 0);
        }
        if (0 == _anonymous_pipes.erase (pipe_)) {
            pipe_->invalidate_router_route_binding ();
            erase_out_pipe (pipe_);
            rollback_outbound = true;
            if (pipe_ == _current_out) {
                clear_current_out_pipe ();
                _more_out = false;
            }
        }

        if (standby_to_promote) {
            const out_pipe_t *const standby_out =
              lookup_out_pipe (standby_to_promote->get_routing_id ());
            zlink_assert (standby_out);
            const bool locally_initiated = standby_out->locally_initiated;
            const uint32_t peer_weight = standby_out->weight;
            standby_to_promote->invalidate_router_route_binding ();
            erase_out_pipe (standby_to_promote);
            standby_to_promote->set_router_socket_routing_id (
              standby_routing_id);
            add_out_pipe (ZLINK_MOVE (standby_routing_id),
                          standby_to_promote, locally_initiated);
            standby_to_promote->publish_router_route_binding ();
            out_pipe_t *const promoted =
              lookup_out_pipe (standby_to_promote->get_routing_id ());
            zlink_assert (promoted && promoted->pipe == standby_to_promote);
            update_out_pipe_weight (promoted, peer_weight);
            if (standby_to_promote->retain_lifetime_ref ())
                promoted_writable_pipe = standby_to_promote;
        }
    }
    if (rollback_outbound)
        pipe_->rollback ();
    if (promoted_writable_pipe) {
        notify_send_writable (promoted_writable_pipe);
        promoted_writable_pipe->release_lifetime_ref ();
    }
}

int zlink::router_t::xrollback ()
{
    pipe_t *rollback_pipe = NULL;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        if (_current_out && _current_out->retain_lifetime_ref ())
            rollback_pipe = _current_out;
        clear_current_out_pipe ();
        _more_out = false;
    }
    if (rollback_pipe) {
        rollback_pipe->rollback ();
        rollback_pipe->release_lifetime_ref ();
    }
    return 0;
}
