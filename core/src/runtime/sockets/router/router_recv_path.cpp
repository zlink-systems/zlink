/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "utils/macros.hpp"
#include "sockets/router/router.hpp"
#include "sockets/router/router_debug.hpp"
#include "core/pipe.hpp"
#include "protocol/zmp_protocol.hpp"
#include "utils/err.hpp"

#include <cstdio>

namespace
{
int probe_router_reply_token_admission (zlink::pipe_t *,
                                        const zlink::msg_t &msg_,
                                        void *userdata_)
{
    zlink::socket_base_t *const socket =
      static_cast<zlink::socket_base_t *> (userdata_);
    uint8_t kind = zlink::zmp_kind_data;
    uint64_t sequence = 0;
    const bool typed = msg_.get_request_reply_metadata (&kind, &sequence);
    if (!typed || kind != zlink::zmp_kind_request || sequence == 0)
        return 0;
    if (socket && socket->router_reply_receive_slot_available ())
        return 0;
    errno = EAGAIN;
    return -1;
}

}

void zlink::router_t::copy_router_pipe_source_rid (
  pipe_t *pipe_, zlink_routing_id_t *out_,
  uint64_t *route_binding_token_out_) const
{
    if (route_binding_token_out_)
        *route_binding_token_out_ = 0;
    if (!out_)
        return;

    out_->size = 0;
    if (!pipe_)
        return;

    size_t routing_id_size = 0;
    if (pipe_->try_copy_router_route_binding (
          out_->data, sizeof (out_->data), &routing_id_size,
          route_binding_token_out_)) {
        out_->size = static_cast<uint8_t> (routing_id_size);
        return;
    }

    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    //  A first-ever duplicate standby has no pipe-owned source snapshot until
    //  topology admission publishes its original RID. Keep the route table as
    //  the correctness fallback for that cold handover case.
    const std::map<pipe_t *, blob_t>::const_iterator standby =
      _standby_pipes.find (pipe_);
    const blob_t *routing_id =
      standby != _standby_pipes.end () ? &standby->second : &pipe_->get_routing_id ();
    if (routing_id->size () > 0) {
        copy_routing_id_from_bytes (routing_id->data (), routing_id->size (), out_);
        if (route_binding_token_out_)
            *route_binding_token_out_ =
              pipe_->router_route_binding_token ();
    }
    // Only registered ROUTER scheduler endpoints reach the receive path. An
    // empty local route is therefore not repaired by dereferencing the peer:
    // the peer link has a separate lifetime domain and is not protected by the
    // ROUTER route fence.
}

void zlink::router_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);

    zlink_assert (pipe_);

    bool probe_router = false;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        probe_router = _probe_router;
    }
    if (probe_router) {
        msg_t probe_msg;
        int rc = probe_msg.init ();
        errno_assert (rc == 0);

        rc = pipe_->get_transport_pair_id () != 0
               ? pipe_->write_transport_probe_and_flush (&probe_msg)
               : pipe_->write_and_flush (&probe_msg);
        LIBZLINK_UNUSED (rc);

        rc = probe_msg.close ();
        errno_assert (rc == 0);
    }

    bool scheduler_registered = false;
    route_adoption_actions_t adoption_actions;
    {
        scoped_lock_t generation_lock (pipe_->transport_sync ());
        std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
        const bool routing_id_ok = identify_peer (
          pipe_, locally_initiated_, &adoption_actions);
        if (router_debug::enabled ()) {
            fprintf (stderr, "router xattach_pipe: pipe=%p local=%d routing_id_ok=%d lane=%d pair=%llu/%llu\n",
                     static_cast<void *> (pipe_), locally_initiated_ ? 1 : 0,
                     routing_id_ok ? 1 : 0,
                     static_cast<int> (pipe_->get_transport_lane ()),
                     static_cast<unsigned long long> (pipe_->get_transport_pair_id ()),
                     static_cast<unsigned long long> (pipe_->get_transport_pair_generation ()));
        }
        if (routing_id_ok) {
            _fq.attach (pipe_);
            scheduler_registered = true;
            (void) pipe_->check_read ();
        } else {
            const blob_t &routing_id = pipe_->get_routing_id ();
            const out_pipe_t *const out_pipe =
              routing_id.size () > 0 ? lookup_out_pipe (routing_id) : NULL;
            if (out_pipe && out_pipe->pipe == pipe_) {
                _anonymous_pipes.erase (pipe_);
                _fq.attach (pipe_);
                scheduler_registered = true;
                (void) pipe_->check_read ();
            } else {
                _anonymous_pipes[pipe_] = locally_initiated_;
            }
        }
    }
    if (scheduler_registered)
        apply_recorded_peer_weight (pipe_);
    finish_route_adoption (pipe_, &adoption_actions);
    if (scheduler_registered) {
        if (local_peer_weight () != 100)
            (void) send_local_peer_weight (pipe_);
        (void) emit_transport_pair_ready (pipe_);
    }
}

void zlink::router_t::xread_activated (pipe_t *pipe_)
{
    if (pipe_ && pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_application
        && pipe_->get_transport_lane_count () == 1u
        && pipe_->transport_pair_application_ready_cached ()
        && pipe_->router_route_binding_token () != 0) {
        // Pair admission already adopted and registered this exact pipe.
        // Reclassification only changes its FQ partition; route identity and
        // generation tables cannot have changed while the ready cache holds.
        _fq.activated (pipe_);
        return;
    }

    bool route_adopted = false;
    route_adoption_actions_t adoption_actions;
    {
        scoped_lock_t generation_lock (pipe_->transport_sync ());
        std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
        const std::map<pipe_t *, bool>::iterator it =
          _anonymous_pipes.find (pipe_);
        if (router_debug::enabled ()) {
            char rid_text[160];
            router_debug::format_routing_id (
              pipe_->get_routing_id (), rid_text, sizeof (rid_text));
            fprintf (stderr, "router xread_activated: pipe=%p anonymous=%d pipe_rid=%s lane=%d pair=%llu/%llu\n",
                     static_cast<void *> (pipe_),
                     it != _anonymous_pipes.end () ? 1 : 0, rid_text,
                     static_cast<int> (pipe_->get_transport_lane ()),
                     static_cast<unsigned long long> (pipe_->get_transport_pair_id ()),
                     static_cast<unsigned long long> (pipe_->get_transport_pair_generation ()));
        }
        if (it == _anonymous_pipes.end ()) {
            _fq.activated (pipe_);
        } else {
            const bool routing_id_ok = identify_peer (
              pipe_, it->second, &adoption_actions);
            if (router_debug::enabled ()) {
                fprintf (stderr, "router xread_activated identify_peer: pipe=%p ok=%d\n",
                         static_cast<void *> (pipe_), routing_id_ok ? 1 : 0);
            }
            if (routing_id_ok) {
                _anonymous_pipes.erase (it);
                _fq.attach (pipe_);
                (void) pipe_->check_read ();
                route_adopted = true;
            }
        }
    }
    if (route_adopted)
        apply_recorded_peer_weight (pipe_);
    finish_route_adoption (pipe_, &adoption_actions);
    if (route_adopted) {
        if (local_peer_weight () != 100)
            (void) send_local_peer_weight (pipe_);
        (void) emit_transport_pair_ready (pipe_);
    }
}

void zlink::router_t::xread_deactivated (pipe_t *pipe_)
{
    if (pipe_ && pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_application
        && pipe_->get_transport_lane_count () == 1u
        && pipe_->transport_pair_application_ready_cached ()
        && pipe_->router_route_binding_token () != 0) {
        _fq.deactivate (pipe_);
        return;
    }

    std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
    if (_anonymous_pipes.find (pipe_) == _anonymous_pipes.end ())
        _fq.deactivate (pipe_);
}

void zlink::router_t::reset_current_in_after_multipart_abort ()
{
    _routing_id_sent = false;
    _current_in = NULL;
    _terminate_current_in = false;
    _more_in = false;
}

void zlink::router_t::finish_current_in_record ()
{
    pipe_t *const completed_pipe = _current_in;
    if (_terminate_current_in && _current_in) {
        _current_in->terminate (true);
        _terminate_current_in = false;
    }
    _current_in = NULL;
    (void) reclassify_transport_pair_application_head (completed_pipe);
}

int zlink::router_t::xrecv (msg_t *msg_)
{
    return xrecv_pipe (msg_, NULL);
}

int zlink::router_t::xrecv_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (_prefetched) {
        if (pipe_out_)
            *pipe_out_ = _current_in;
        if (!_routing_id_sent) {
            const int rc = msg_->move (_prefetched_id);
            errno_assert (rc == 0);
            _routing_id_sent = true;
        } else {
            const int rc = msg_->move (_prefetched_msg);
            errno_assert (rc == 0);
            _prefetched = false;
        }
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in)
            finish_current_in_record ();
        return 0;
    }

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (msg_, &pipe);
    while (rc == 0 && msg_->is_routing_id ())
        rc = _fq.recvpipe (msg_, &pipe);

    if (rc != 0) {
        if (errno == ECONNABORTED)
            reset_current_in_after_multipart_abort ();
        return -1;
    }

    zlink_assert (pipe != NULL);
    if (pipe_out_)
        *pipe_out_ = _more_in ? _current_in : pipe;

    if (_more_in) {
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in)
            finish_current_in_record ();
    } else {
        rc = _prefetched_msg.move (*msg_);
        errno_assert (rc == 0);
        _prefetched = true;
        _current_in = pipe;

        const blob_t &routing_id = pipe->get_routing_id ();
        rc = msg_->init_size (routing_id.size ());
        errno_assert (rc == 0);
        memcpy (msg_->data (), routing_id.data (), routing_id.size ());
        msg_->set_flags (msg_t::more);
        _routing_id_sent = true;
    }

    return 0;
}

int zlink::router_t::xrecv_routed (msg_t *msg_,
                                  zlink_routing_id_t *source_rid_out_,
                                  uint64_t *connection_id_out_,
                                  pipe_t **source_pipe_out_,
                                  pipe_t::read_admission_fn *admission_,
                                  void *admission_userdata_,
                                  uint64_t *route_binding_token_out_)
{
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;
    if (route_binding_token_out_)
        *route_binding_token_out_ = 0;
    if (_prefetched) {
        // A generic receive may have prefetched this frame before the routed
        // API asks for it. It is no longer in the pipe queue, but admission
        // still runs before this routed call exposes it to the caller.
        if (admission_
            && pipe_t::requires_record_admission (_prefetched_msg)) {
            const int admission_result =
              _current_in
                ? admission_ (_current_in, _prefetched_msg,
                              admission_userdata_)
                : -1;
            if (admission_result != 0) {
                const int saved_errno = errno;
                if (admission_result
                    == pipe_t::read_admission_reject_consume) {
                    bool more =
                      (_prefetched_msg.flags () & msg_t::more) != 0;
                    int rc = _prefetched_msg.close ();
                    errno_assert (rc == 0);
                    rc = _prefetched_msg.init ();
                    errno_assert (rc == 0);
                    _prefetched = false;

                    while (more) {
                        msg_t discarded;
                        rc = discarded.init ();
                        errno_assert (rc == 0);
                        pipe_t *discarded_pipe = NULL;
                        rc = _fq.recvpipe (&discarded, &discarded_pipe);
                        if (rc != 0) {
                            rc = discarded.close ();
                            errno_assert (rc == 0);
                            break;
                        }
                        more = (discarded.flags () & msg_t::more) != 0;
                        rc = discarded.close ();
                        errno_assert (rc == 0);
                    }

                    rc = _prefetched_id.close ();
                    errno_assert (rc == 0);
                    rc = _prefetched_id.init ();
                    errno_assert (rc == 0);
                    _routing_id_sent = false;
                    _more_in = false;
                    finish_current_in_record ();
                }
                errno = saved_errno;
                return -1;
            }
        }
        if (!_routing_id_sent) {
            const int close_rc = _prefetched_id.close ();
            errno_assert (close_rc == 0);
            const int init_rc = _prefetched_id.init ();
            errno_assert (init_rc == 0);
        }
        if (source_rid_out_)
            copy_router_pipe_source_rid (
              _current_in, source_rid_out_, route_binding_token_out_);
        if (connection_id_out_ && _current_in)
            *connection_id_out_ =
              _prefetched_msg.transport_connection_id ();
        if (source_pipe_out_)
            *source_pipe_out_ = _current_in;

        const int rc = msg_->move (_prefetched_msg);
        errno_assert (rc == 0);
        _prefetched = false;
        _routing_id_sent = true;
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in)
            finish_current_in_record ();
        return 0;
    }

    pipe_t *pipe = NULL;
    int rc = admission_ ? _fq.recvpipe_with_record_admission (
                            msg_, &pipe, admission_, admission_userdata_)
                        : _fq.recvpipe (msg_, &pipe);
    while (rc == 0 && msg_->is_routing_id ())
        rc = admission_ ? _fq.recvpipe_with_record_admission (
                            msg_, &pipe, admission_, admission_userdata_)
                        : _fq.recvpipe (msg_, &pipe);
    if (rc != 0) {
        if (errno == ECONNABORTED)
            reset_current_in_after_multipart_abort ();
        return -1;
    }

    zlink_assert (pipe != NULL);
    if (!_more_in) {
        _current_in = pipe;
        if (source_rid_out_)
            copy_router_pipe_source_rid (
              pipe, source_rid_out_, route_binding_token_out_);
        _routing_id_sent = true;
    } else if (_current_in && source_rid_out_) {
        copy_router_pipe_source_rid (
          _current_in, source_rid_out_, route_binding_token_out_);
    }
    if (connection_id_out_)
        *connection_id_out_ = msg_->transport_connection_id ();
    if (source_pipe_out_)
        *source_pipe_out_ = _current_in;

    _more_in = (msg_->flags () & msg_t::more) != 0;
    if (!_more_in)
        finish_current_in_record ();
    return 0;
}

bool zlink::router_t::xhas_in ()
{
    if (_more_in)
        return true;

    if (_prefetched)
        return probe_router_reply_token_admission (
                 _current_in, _prefetched_msg, this)
               == 0;

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe_with_record_admission (
      &_prefetched_msg, &pipe, &probe_router_reply_token_admission, this);

    while (rc == 0 && _prefetched_msg.is_routing_id ()) {
        rc = _fq.recvpipe_with_record_admission (
          &_prefetched_msg, &pipe, &probe_router_reply_token_admission, this);
    }

    if (rc != 0)
        return false;

    zlink_assert (pipe != NULL);
    const blob_t &routing_id = pipe->get_routing_id ();
    rc = _prefetched_id.init_size (routing_id.size ());
    errno_assert (rc == 0);
    memcpy (_prefetched_id.data (), routing_id.data (), routing_id.size ());
    _prefetched_id.set_flags (msg_t::more);

    _prefetched = true;
    _routing_id_sent = false;
    _current_in = pipe;

    return true;
}

size_t zlink::router_t::xredrive_reply_token_waiters (size_t max_pipes_)
{
    return _fq.redrive_record_admission (max_pipes_);
}

int zlink::router_t::get_peer_state (const void *routing_id_, size_t routing_id_size_) const
{
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    int res = 0;

    const blob_t routing_id_blob (static_cast<unsigned char *> (const_cast<void *> (routing_id_)),
                                  routing_id_size_, reference_tag_t ());
    const out_pipe_t *out_pipe = lookup_out_pipe (routing_id_blob);
    if (!out_pipe) {
        errno = EHOSTUNREACH;
        return -1;
    }

    if (out_pipe->weight == 0)
        return 0;

    //  Readiness has to agree with send admission, which composes the byte HWM
    //  with the remote receive-flow state. Reporting POLLOUT for a paused peer
    //  would hand the caller a send that is guaranteed to report backpressure.
    if (out_pipe->pipe->check_hwm ()
        && !out_pipe->pipe->remote_flow_blocks_next_message ())
        res |= ZLINK_POLLOUT;

    return res;
}
