/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"

#include "utils/err.hpp"
#include "utils/macros.hpp"

namespace
{
bool is_routing_submit_retry_errno (int err_)
{
    return err_ == ENOTCONN || err_ == EHOSTUNREACH;
}
}

zlink::routing_socket_base_t::routing_socket_base_t (class ctx_t *parent_,
                                                     uint32_t tid_,
                                                     int sid_) :
    socket_base_t (parent_, tid_, sid_), _writable_weighted_out_pipes (0)
{
}

zlink::routing_socket_base_t::~routing_socket_base_t ()
{
    // Shutdown can race with late peer teardown and leave stale routing table
    // entries after the underlying pipes are already being destroyed. Treat
    // those entries as best-effort cleanup instead of aborting the process.
    _out_pipes.clear ();
    _submit_retry_local_rids.clear ();
}

int zlink::routing_socket_base_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_CONNECT_ROUTING_ID:
            if (!optval_ || optvallen_ == 0) {
                _connect_routing_id.clear ();
                return 0;
            } else {
                _connect_routing_id.assign (static_cast<const char *> (optval_), optvallen_);
                return 0;
            }
    }
    errno = EINVAL;
    return -1;
}

void zlink::routing_socket_base_t::xwrite_activated (pipe_t *pipe_)
{
    const out_pipe_index_t::iterator index_it = _out_pipe_index.find (pipe_);
    // ROUTER keeps a duplicate peer in the anonymous set when handover is
    // disabled. A paired transport can release that pipe's initial write hold
    // after validation even though it intentionally has no outbound route.
    if (index_it == _out_pipe_index.end ())
        return;
    mark_out_pipe_active (&index_it->second->second);
}

std::string zlink::routing_socket_base_t::extract_connect_routing_id ()
{
    std::string res = ZLINK_MOVE (_connect_routing_id);
    _connect_routing_id.clear ();
    return res;
}

bool zlink::routing_socket_base_t::connect_routing_id_is_set () const
{
    return !_connect_routing_id.empty ();
}

void zlink::routing_socket_base_t::add_out_pipe (blob_t routing_id_,
                                                 pipe_t *pipe_,
                                                 bool locally_initiated_)
{
    const out_pipe_t outpipe = {pipe_, true, locally_initiated_, 100};
    if (locally_initiated_)
        _submit_retry_local_rids.insert (blob_t (routing_id_.data (), routing_id_.size ()));
    const std::pair<out_pipes_t::iterator, bool> insert_res =
      _out_pipes.ZLINK_MAP_INSERT_OR_EMPLACE (ZLINK_MOVE (routing_id_), outpipe);
    const bool ok = insert_res.second;
    zlink_assert (ok);
    _out_pipe_index[pipe_] = insert_res.first;
    ++_writable_weighted_out_pipes;
}

bool zlink::routing_socket_base_t::has_out_pipe (const blob_t &routing_id_) const
{
    return 0 != _out_pipes.count (routing_id_);
}

zlink::routing_socket_base_t::out_pipe_t *
zlink::routing_socket_base_t::lookup_out_pipe (const blob_t &routing_id_)
{
    const out_pipes_t::iterator it = _out_pipes.find (routing_id_);
    if (it != _out_pipes.end ()) {
#if !defined _MSC_VER
        __builtin_prefetch (&it->second, 0, 3);
#endif
        return &it->second;
    }
    return NULL;
}

const zlink::routing_socket_base_t::out_pipe_t *
zlink::routing_socket_base_t::lookup_out_pipe (const blob_t &routing_id_) const
{
    const out_pipes_t::const_iterator it = _out_pipes.find (routing_id_);
    if (it != _out_pipes.end ()) {
#if !defined _MSC_VER
        __builtin_prefetch (&it->second, 0, 3);
#endif
        return &it->second;
    }
    return NULL;
}

void zlink::routing_socket_base_t::erase_out_pipe (const pipe_t *pipe_)
{
    if (!pipe_)
        return;

    const out_pipe_index_t::iterator index_it = _out_pipe_index.find (const_cast<pipe_t *> (pipe_));
    if (index_it != _out_pipe_index.end ()) {
        const out_pipes_t::iterator out_it = index_it->second;
        if (out_it->second.pipe != NULL && out_it->second.active && out_it->second.weight > 0)
            --_writable_weighted_out_pipes;
        _out_pipes.erase (out_it);
        _out_pipe_index.erase (index_it);
        return;
    }

    // Routing id may have been refreshed after attach. Fall back to
    // pointer-based lookup to keep teardown idempotent and avoid stale pipes.
    for (out_pipes_t::iterator it = _out_pipes.begin (), end = _out_pipes.end (); it != end; ++it) {
        if (it->second.pipe == pipe_) {
            if (it->second.active && it->second.weight > 0)
                --_writable_weighted_out_pipes;
            _out_pipe_index.erase (it->second.pipe);
            _out_pipes.erase (it);
            return;
        }
    }
}

int zlink::routing_socket_base_t::terminate_out_pipe_by_routing_id (
  const zlink_routing_id_t *peer_rid_)
{
    if (!peer_rid_ || peer_rid_->size == 0) {
        errno = EINVAL;
        return -1;
    }

    blob_t routing_id (peer_rid_->data, peer_rid_->size);
    _submit_retry_local_rids.erase (routing_id);
    out_pipe_t *outpipe = lookup_out_pipe (routing_id);
    if (!outpipe || !outpipe->pipe) {
        errno = ENOENT;
        return -1;
    }

    outpipe->pipe->terminate (false);
    return 0;
}

zlink::routing_socket_base_t::out_pipe_t
zlink::routing_socket_base_t::try_erase_out_pipe (const blob_t &routing_id_)
{
    const out_pipes_t::iterator it = _out_pipes.find (routing_id_);
    out_pipe_t res = {NULL, false, false, 0};
    if (it != _out_pipes.end ()) {
        res = it->second;
        if (it->second.pipe != NULL && it->second.active && it->second.weight > 0)
            --_writable_weighted_out_pipes;
        _out_pipe_index.erase (it->second.pipe);
        _out_pipes.erase (it);
    }
    return res;
}

void zlink::routing_socket_base_t::mark_out_pipe_active (out_pipe_t *out_pipe_)
{
    if (!out_pipe_ || out_pipe_->active)
        return;

    if (out_pipe_->weight > 0)
        ++_writable_weighted_out_pipes;
    out_pipe_->active = true;
}

void zlink::routing_socket_base_t::mark_out_pipe_inactive (out_pipe_t *out_pipe_)
{
    if (!out_pipe_ || !out_pipe_->active)
        return;

    if (out_pipe_->weight > 0 && _writable_weighted_out_pipes > 0)
        --_writable_weighted_out_pipes;
    out_pipe_->active = false;
}

void zlink::routing_socket_base_t::update_out_pipe_weight (out_pipe_t *out_pipe_, uint32_t weight_)
{
    if (!out_pipe_ || out_pipe_->weight == weight_)
        return;

    if (out_pipe_->active && out_pipe_->weight > 0 && weight_ == 0)
        --_writable_weighted_out_pipes;
    else if (out_pipe_->active && out_pipe_->weight == 0 && weight_ > 0)
        ++_writable_weighted_out_pipes;

    out_pipe_->weight = weight_;
}

bool zlink::routing_socket_base_t::has_writable_weighted_out_pipes () const
{
    return _writable_weighted_out_pipes != 0;
}

bool zlink::routing_socket_base_t::xsubmit_retry_allowed (const zlink_routing_id_t *target_rid_,
                                                          int err_) const
{
    if (!target_rid_ || !is_routing_submit_retry_errno (err_))
        return true;

    blob_t routing_id (target_rid_->data, target_rid_->size);
    const out_pipe_t *const outpipe = lookup_out_pipe (routing_id);
    if (outpipe)
        return outpipe->locally_initiated;

    if (_connect_routing_id.size () == routing_id.size ()
        && (_connect_routing_id.empty ()
            || memcmp (_connect_routing_id.data (), routing_id.data (),
                       routing_id.size ()) == 0))
        return true;

    return _submit_retry_local_rids.count (routing_id) != 0;
}
