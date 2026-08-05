/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/c_api_copy_internal.hpp"
#include "utils/macros.hpp"
#include "sockets/router/router.hpp"
#include "sockets/common/socket_dispatch_loop_internal.hpp"
#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/random.hpp"
#include "utils/likely.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include <cstdlib>
#include <cstdio>

namespace
{
const bool router_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

void format_routing_id_debug (const zlink_routing_id_t *rid_, char *buf_, size_t buf_size_)
{
    if (!buf_ || buf_size_ == 0) {
        return;
    }

    if (!rid_ || rid_->size == 0) {
        std::snprintf (buf_, buf_size_, "<empty>");
        return;
    }

    size_t used = 0;
    for (size_t i = 0; i < rid_->size && used + 4 < buf_size_; ++i) {
        const unsigned char c = rid_->data[i];
        const int rc = std::snprintf (buf_ + used, buf_size_ - used, "%c%02X",
                                      (c >= 32 && c <= 126) ? static_cast<char> (c) : '.',
                                      static_cast<unsigned> (c));
        if (rc <= 0)
            break;
        used += static_cast<size_t> (rc);
        if (i + 1 < rid_->size && used + 2 < buf_size_) {
            buf_[used++] = ' ';
            buf_[used] = '\0';
        }
    }
}

void format_blob_routing_id_debug (const zlink::blob_t &routing_id_, char *buf_, size_t buf_size_)
{
    zlink_routing_id_t rid;
    zlink::copy_routing_id_from_bytes (routing_id_.data (), routing_id_.size (), &rid);
    format_routing_id_debug (&rid, buf_, buf_size_);
}

void store_dispatch_source_rid (std::map<zlink::pipe_t *, zlink_routing_id_t> *sources_,
                                zlink::pipe_t *pipe_,
                                zlink_routing_id_t *fallback_rid_,
                                bool *fallback_valid_,
                                zlink::msg_t *msg_)
{
    if (!sources_ || !msg_)
        return;

    zlink_routing_id_t rid;
    rid.size = 0;

    const size_t size = msg_->size ();
    zlink_assert (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    if (size > 0)
        memcpy (rid.data, msg_->data (), size);
    if (pipe_) {
        (*sources_)[pipe_] = rid;
    } else if (fallback_rid_ && fallback_valid_) {
        *fallback_rid_ = rid;
        *fallback_valid_ = true;
    }
}

bool take_dispatch_source_rid (std::map<zlink::pipe_t *, zlink_routing_id_t> *sources_,
                               zlink::pipe_t *pipe_,
                               zlink_routing_id_t *fallback_rid_,
                               bool *fallback_valid_,
                               zlink_routing_id_t *out_)
{
    if (!sources_ || !out_)
        return false;

    if (pipe_) {
        const std::map<zlink::pipe_t *, zlink_routing_id_t>::iterator it =
          sources_->find (pipe_);
        if (it == sources_->end ())
            return false;

        *out_ = it->second;
        sources_->erase (it);
        return true;
    }

    if (!fallback_rid_ || !fallback_valid_ || !*fallback_valid_)
        return false;

    *out_ = *fallback_rid_;
    fallback_rid_->size = 0;
    *fallback_valid_ = false;
    return true;
}

void copy_router_pipe_source_rid (zlink::pipe_t *pipe_, zlink_routing_id_t *out_)
{
    if (!out_) {
        return;
    }

    out_->size = 0;
    if (!pipe_)
        return;

    const zlink::blob_t &routing_id = pipe_->get_routing_id ();
    if (routing_id.size () > 0) {
        zlink::copy_routing_id_from_bytes (routing_id.data (), routing_id.size (), out_);
        return;
    }

    zlink::pipe_t *peer = pipe_->get_peer ();
    if (!peer)
        return;

    const zlink::blob_t &peer_routing_id = peer->get_routing_id ();
    zlink::copy_routing_id_from_bytes (peer_routing_id.data (), peer_routing_id.size (), out_);
}
}

static bool router_debug_enabled ()
{
    return router_debug_on;
}

zlink::router_t::router_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
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
    _handover (options.rid_duplicate_policy == ZLINK_RID_DUPLICATE_HANDOVER),
    _dispatch_source_rid_valid (false)
{
    options.type = ZLINK_CORE_SOCKET_ROUTER;
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
    close_socket_msg_parts (&_dispatch_parts);
    for (std::map<pipe_t *, std::vector<zlink_msg_t>>::iterator it =
           _dispatch_parts_by_pipe.begin ();
         it != _dispatch_parts_by_pipe.end (); ++it) {
        close_socket_msg_parts (&it->second);
    }
    _dispatch_parts_by_pipe.clear ();
    _dispatch_source_rids.clear ();
    _dispatch_source_rid.size = 0;
    _dispatch_source_rid_valid = false;
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
                _mandatory = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
            if (is_int && value >= 0) {
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
            *value = _mandatory ? 1 : 0;
            return 0;
        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
            *value = _probe_router ? 1 : 0;
            return 0;
        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
            *value = _handover ? 1 : 0;
            return 0;
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
            *value = _handover ? ZLINK_RID_DUPLICATE_HANDOVER : ZLINK_RID_DUPLICATE_REJECT;
            return 0;
        default:
            return routing_socket_base_t::xgetsockopt (option_, optval_, optvallen_);
    }
}


void zlink::router_t::xpipe_terminated (pipe_t *pipe_)
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    const blob_t &terminated_routing_id =
      pipe_->get_routing_id ();
    const bool was_standby =
      _standby_pipes.erase (pipe_) != 0;
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
    if (router_debug_enabled ()) {
        char rid_text[160];
        format_blob_routing_id_debug (pipe_->get_routing_id (), rid_text, sizeof (rid_text));
        fprintf (stderr, "router xpipe_terminated: pipe=%p rid=%s anonymous=%d\n",
                 static_cast<void *> (pipe_), rid_text,
                 _anonymous_pipes.count (pipe_) != 0 ? 1 : 0);
    }
    if (0 == _anonymous_pipes.erase (pipe_)) {
        erase_out_pipe (pipe_);
        _dispatch_source_rids.erase (pipe_);
        std::map<pipe_t *, std::vector<zlink_msg_t>>::iterator parts_it =
          _dispatch_parts_by_pipe.find (pipe_);
        if (parts_it != _dispatch_parts_by_pipe.end ()) {
            close_socket_msg_parts (&parts_it->second);
            _dispatch_parts_by_pipe.erase (parts_it);
        }
        _fq.pipe_terminated (pipe_);
        pipe_->rollback ();
        if (pipe_ == _current_out) {
            _current_out = NULL;
            _current_out_connection_id = 0;
        }
    }
    if (standby_to_promote) {
        const out_pipe_t *const standby_out =
          lookup_out_pipe (standby_to_promote->get_routing_id ());
        zlink_assert (standby_out);
        const bool locally_initiated =
          standby_out->locally_initiated;
        erase_out_pipe (standby_to_promote);
        standby_to_promote->set_router_socket_routing_id (
          standby_routing_id);
        add_out_pipe (ZLINK_MOVE (standby_routing_id),
                      standby_to_promote, locally_initiated);
    }
}

int zlink::router_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    if (!socket_msg_dispatch_active ())
        return 0;

    if (router_debug_enabled ()) {
        fprintf (stderr, "router xsocket_msg_dispatch: pipe=%p size=%zu routing_id=%d more=%d "
                         "handler=%d\n",
                 static_cast<void *> (pipe_), msg_ ? msg_->size () : 0,
                 msg_ && msg_->is_routing_id () ? 1 : 0,
                 msg_ && ((msg_->flags () & msg_t::more) != 0) ? 1 : 0,
                 socket_msg_handler () ? 1 : 0);
    }

    pipe_t *source_pipe = pipe_ ? pipe_ : current_socket_msg_dispatch_pipe ();

    if (msg_->is_routing_id ()) {
        pipe_t *socket_pipe = source_pipe;
        if (socket_pipe && _anonymous_pipes.count (socket_pipe) == 0
            && lookup_out_pipe (socket_pipe->get_routing_id ()) == NULL) {
            socket_pipe = socket_pipe->get_peer ();
            if (!socket_pipe) {
                store_dispatch_source_rid (&_dispatch_source_rids, source_pipe,
                                           &_dispatch_source_rid,
                                           &_dispatch_source_rid_valid, msg_);
                return 1;
            }
        }

        bool needs_route_registration = false;
        bool locally_initiated = false;
        if (socket_pipe) {
            const std::map<pipe_t *, bool>::const_iterator anonymous =
              _anonymous_pipes.find (socket_pipe);
            if (anonymous != _anonymous_pipes.end ())
                locally_initiated = anonymous->second;
            blob_t routing_id_ref (
              const_cast<unsigned char *> (static_cast<unsigned char *> (msg_->data ())),
              msg_->size (), zlink::reference_tag_t ());
            const out_pipe_t *const existing_outpipe = lookup_out_pipe (routing_id_ref);
            needs_route_registration =
              anonymous != _anonymous_pipes.end () || existing_outpipe == NULL
              || !existing_outpipe->active || existing_outpipe->weight == 0;
        }
        if (needs_route_registration) {
            blob_t routing_id (static_cast<unsigned char *> (msg_->data ()), msg_->size ());
            if (adopt_peer_routing_id (
                  socket_pipe, ZLINK_MOVE (routing_id), locally_initiated))
                promote_anonymous_pipe_for_dispatch (socket_pipe);
        }
        store_dispatch_source_rid (&_dispatch_source_rids, source_pipe, &_dispatch_source_rid,
                                   &_dispatch_source_rid_valid, msg_);
        return 1;
    }

    std::vector<zlink_msg_t> *dispatch_parts = source_pipe ? &_dispatch_parts_by_pipe[source_pipe]
                                                           : &_dispatch_parts;
    store_socket_msg_part (dispatch_parts, msg_);
    if ((reinterpret_cast<msg_t *> (&dispatch_parts->back ())->flags () & msg_t::more) != 0) {
        return 1;
    }

    zlink_socket_msg_handler_fn handler = socket_msg_handler ();
    if (!handler) {
        close_socket_msg_parts (dispatch_parts);
        if (source_pipe)
            _dispatch_parts_by_pipe.erase (source_pipe);
        if (source_pipe)
            _dispatch_source_rids.erase (source_pipe);
        else {
            _dispatch_source_rid.size = 0;
            _dispatch_source_rid_valid = false;
        }
        return 1;
    }

    zlink_routing_id_t source_rid;
    if (!take_dispatch_source_rid (&_dispatch_source_rids, source_pipe, &_dispatch_source_rid,
                                   &_dispatch_source_rid_valid, &source_rid))
        resolve_socket_msg_source_rid (source_pipe, &source_rid);

    invoke_socket_msg_handler (handler, &source_rid, &(*dispatch_parts)[0],
                               dispatch_parts->size ());
    dispatch_parts->clear ();
    if (source_pipe)
        _dispatch_parts_by_pipe.erase (source_pipe);
    return 1;
}

void zlink::router_t::xarm_socket_msg_dispatch ()
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    _fq.arm_dispatch ();
}

int zlink::router_t::xrollback ()
{
    if (_current_out) {
        _current_out->rollback ();
        _current_out = NULL;
        _current_out_connection_id = 0;
    }
    _more_out = false;
    return 0;
}
