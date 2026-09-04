/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/router/router.hpp"
#include "sockets/router/router_debug.hpp"
#include "core/c_api_copy_internal.hpp"
#include "utils/likely.hpp"
#include "utils/err.hpp"
#include "utils/routing_id.hpp"

#include <cstdio>

int zlink::router_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    return send_with_observer (msg_, admission_out_, NULL, NULL);
}

int zlink::router_t::send_with_observer (
  msg_t *msg_, pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    std::unique_lock<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    // Public send enters this path through socket_public_send_scope_t; route
    // lifecycle ownership below is the only additional send-side fence.

    if (!_more_out) {
        zlink_assert (!_current_out);

        if (msg_->flags () & msg_t::more) {
            _more_out = true;

            out_pipe_t *out_pipe =
              lookup_out_pipe (blob_t (static_cast<unsigned char *> (msg_->data ()), msg_->size (),
                                       zlink::reference_tag_t ()));

            if (out_pipe) {
                if (out_pipe->weight == 0) {
                    _more_out = false;
                    errno = ECONNREFUSED;
                    return -1;
                }
                select_current_out_pipe (out_pipe->pipe);

                const pipe_message_admission_t write_admission =
                  _current_out->admit_owner_message_start ();
                if (write_admission != pipe_message_admission_ready) {
                    // HWM removes the pipe from the writable set until an
                    // activation command restores credit. During that window
                    // the transport can also report a non-active pipe state;
                    // the route still exists and remains backpressured.
                    const bool pipe_full = write_admission
                                           == pipe_message_admission_hwm_full;
                    const bool transport_wait =
                      write_admission
                      == pipe_message_admission_transport_wait;
                    if (admission_out_)
                        *admission_out_ = write_admission;
                    mark_out_pipe_inactive (out_pipe);
                    clear_current_out_pipe ();

                    if (_mandatory) {
                        _more_out = false;
                        errno = pipe_full || transport_wait ? EAGAIN
                                                            : EHOSTUNREACH;
                        if (router_debug::enabled ()) {
                            fprintf (stderr, "router xsend: pipe not writable size=%zu errno=%d\n",
                                     msg_->size (), errno);
                        }
                        return -1;
                    }
                }
            } else if (_mandatory) {
                _more_out = false;
                errno = EHOSTUNREACH;
                if (router_debug::enabled ()) {
                    fprintf (stderr, "router xsend: no out pipe rid_size=%zu errno=%d\n",
                             msg_->size (), errno);
                }
                return -1;
            }
        }

        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        if (admission_out_)
            *admission_out_ = pipe_message_admission_ready;
        return 0;
    }

    if (router_debug::enabled ()) {
        fprintf (stderr, "router xsend continuation: pipe=%p size=%zu more=%d\\n",
                 static_cast<void *> (_current_out), msg_ ? msg_->size () : 0,
                 (msg_->flags () & msg_t::more) != 0 ? 1 : 0);
    }

    const bool next_more_out = (msg_->flags () & msg_t::more) != 0;
    _more_out = next_more_out;

    if (_current_out) {
        pipe_t *const write_pipe = _current_out;
        // Ordinary sends keep the route fence through the short pipe write.
        // That makes the routing-table entry itself the lifetime guard and
        // avoids a pair of lifetime CAS operations for every message part.
        // Observer-backed request/reply sends must release the route fence
        // before invoking the observer, so they retain the explicit pin.
        const bool release_route_for_write = observer_ != NULL;
        if (release_route_for_write && !write_pipe->retain_lifetime_ref ()) {
            clear_current_out_pipe ();
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        msg_->set_transport_connection_id (
          _current_out_connection_id);
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        // A terminal frame does not need _current_out after this point. Clear
        // it while the route table is still protected. Observer-backed sends
        // then keep only the lifetime pin; ordinary sends retain this fence.
        if (!next_more_out && _current_out == write_pipe)
            clear_current_out_pipe ();
        if (release_route_for_write)
            route_lifecycle_lock.unlock ();
        const bool ok =
          observer_
            ? write_pipe->write_owner_started_message_observed (
                msg_, observer_, observer_userdata_, &write_admission)
            : write_pipe->write_owner_started_message (
                msg_, &write_admission);
        if (router_debug::enabled ())
            fprintf (stderr, "router xsend write: pipe=%p ok=%d more=%d\\n",
                     static_cast<void *> (write_pipe), ok ? 1 : 0,
                     next_more_out ? 1 : 0);
        const int write_errno = errno;
        if (unlikely (!ok)) {
            write_pipe->rollback ();
            errno = write_errno;
            // Route teardown can run while the pipe write is in flight.
            // Re-acquire only on failure, where the routing entry may need
            // to be revalidated and marked inactive.
            if (release_route_for_write)
                route_lifecycle_lock.lock ();
        }
        if (unlikely (!ok)) {
            if (observer_ && errno == ECANCELED
                && write_admission == pipe_message_admission_invalid) {
                if (_current_out == write_pipe)
                    clear_current_out_pipe ();
                _more_out = false;
                route_lifecycle_lock.unlock ();
                if (release_route_for_write)
                    write_pipe->release_lifetime_ref ();
                return -1;
            }
            // The first multipart frame can pass the readiness check and a
            // later frame can encounter HWM. Preserve that as capacity
            // pressure; only an inactive pipe is unreachable.
            if (admission_out_)
                *admission_out_ = write_admission;
            const blob_t &routing_id = write_pipe->get_routing_id ();
            out_pipe_t *current_out_pipe = lookup_out_pipe (routing_id);
            if (write_admission != pipe_message_admission_request_full
                && current_out_pipe && current_out_pipe->pipe == write_pipe)
                mark_out_pipe_inactive (current_out_pipe);
            if (router_debug::enabled ()) {
                fprintf (stderr, "router xsend: drop message size=%zu\n", msg_->size ());
            }
            if (_current_out == write_pipe)
                clear_current_out_pipe ();
            if (_mandatory) {
                _more_out = false;
                errno = write_admission == pipe_message_admission_too_large
                          ? EMSGSIZE
                          : write_admission == pipe_message_admission_hwm_full
                                || write_admission
                                     == pipe_message_admission_request_full
                                || write_admission
                                     == pipe_message_admission_transport_wait
                              ? EAGAIN
                              : EHOSTUNREACH;
                // The pipe rollback discarded the already staged prefix and
                // earlier parts. A blocking retry of only this continuation
                // would start a different record, so report the multipart
                // abort distinctly to the scoped public send path.
                route_lifecycle_lock.unlock ();
                if (release_route_for_write)
                    write_pipe->release_lifetime_ref ();
                return -2;
            }
            const int rc = msg_->close ();
            errno_assert (rc == 0);
        }
        if (unlikely (!ok))
            route_lifecycle_lock.unlock ();
        if (release_route_for_write)
            write_pipe->release_lifetime_ref ();
    } else {
        if (router_debug::enabled ()) {
            fprintf (stderr, "router xsend: no current out, drop size=%zu\n", msg_->size ());
        }
        const int rc = msg_->close ();
        errno_assert (rc == 0);
    }

    const int rc = msg_->init ();
    errno_assert (rc == 0);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    return 0;
}

int zlink::router_t::xsend_pipe (
  msg_t *msg_, pipe_t **pipe_out_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    const int rc = send_with_observer (msg_, admission_out_, observer_,
                                       observer_userdata_);
    if (rc == 0 && pipe_out_) {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        if (_current_out)
            *pipe_out_ = _current_out;
    }
    return rc;
}

int zlink::router_t::xsend_routed (const zlink_routing_id_t *target_rid_,
                                  msg_t *msg_,
  uint64_t *connection_id_out_,
  uint64_t expected_connection_id_,
  pipe_t **pipe_out_,
  uint64_t expected_transport_pair_id_,
  uint64_t expected_transport_pair_generation_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_,
  routed_send_attempt_identity_t *attempt_identity_out_,
  uint64_t expected_route_incarnation_id_, bool request_only_)
{
    std::unique_lock<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    zlink_assert (!_more_out);
    zlink_assert (!_current_out);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (attempt_identity_out_)
        attempt_identity_out_->reset ();

    _more_out = (msg_->flags () & msg_t::more) != 0;

    if (router_debug::enabled ()) {
        fprintf (stderr,
                 "router xsend_routed enter: rid_size=%u pair=%llu/%llu size=%zu more=%d\\n",
                 static_cast<unsigned> (target_rid_ ? target_rid_->size : 0),
                 static_cast<unsigned long long> (expected_transport_pair_id_),
                 static_cast<unsigned long long> (expected_transport_pair_generation_),
                 msg_ ? msg_->size () : 0, _more_out ? 1 : 0);
    }

    out_pipe_t *out_pipe = NULL;
    pipe_t *scoped_pipe = NULL;
    if (expected_transport_pair_id_ != 0 || expected_transport_pair_generation_ != 0) {
        scoped_pipe = find_transport_pair_pipe (
          target_rid_, expected_transport_pair_id_, expected_transport_pair_generation_);
        if (!scoped_pipe) {
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        const out_pipe_t *const scoped_out =
          lookup_out_pipe (scoped_pipe->get_routing_id ());
        if (!scoped_out || scoped_out->pipe != scoped_pipe) {
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        if (scoped_out->weight == 0) {
            _more_out = false;
            errno = ECONNREFUSED;
            return -1;
        }
        if (request_only_
            && scoped_pipe->get_peer_socket_type ()
                 != ZLINK_CORE_SOCKET_ROUTER) {
            _more_out = false;
            errno = EPROTOTYPE;
            return -1;
        }
        select_current_out_pipe (scoped_pipe);
        if (expected_connection_id_ != 0
            && _current_out_connection_id != expected_connection_id_) {
            clear_current_out_pipe ();
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        if (connection_id_out_)
            *connection_id_out_ = _current_out_connection_id;
        if (pipe_out_)
            *pipe_out_ = _current_out;
    } else {
        out_pipe = lookup_out_pipe (
          blob_t (const_cast<unsigned char *> (target_rid_->data),
                  target_rid_->size, zlink::reference_tag_t ()));
        if (request_only_ && (!out_pipe || !out_pipe->pipe)) {
            _more_out = false;
            errno = ENOENT;
            return -1;
        }
    }
    if (scoped_pipe && _more_out) {
        const pipe_message_admission_t write_admission =
          _current_out->check_write_admission ();
        if (write_admission != pipe_message_admission_ready) {
            if (admission_out_)
                *admission_out_ = write_admission;
            clear_current_out_pipe ();
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;
            _more_out = false;
            errno = write_admission == pipe_message_admission_hwm_full
                        || write_admission
                             == pipe_message_admission_transport_wait
                      ? EAGAIN
                      : EHOSTUNREACH;
            return -1;
        }
    } else if (out_pipe) {
        if (out_pipe->weight == 0) {
            _more_out = false;
            errno = ECONNREFUSED;
            if (router_debug::enabled ()) {
                fprintf (stderr, "router xsend_routed: draining rid_size=%u\n",
                         static_cast<unsigned> (target_rid_->size));
            }
            return -1;
        }
        if (request_only_) {
            pipe_t *const request_pipe = out_pipe->pipe;
            const uint64_t pair_id =
              request_pipe->get_transport_pair_id ();
            if (pair_id != 0) {
                if (request_pipe->get_transport_pair_generation () == 0
                    || !transport_pair_application_ready (request_pipe)) {
                    _more_out = false;
                    errno = EHOSTUNREACH;
                    return -1;
                }
            } else if (request_pipe->get_transport_connection_id () == 0
                       || !request_pipe->is_lifecycle_active ()) {
                _more_out = false;
                errno = EHOSTUNREACH;
                return -1;
            }
            if (request_pipe->get_peer_socket_type ()
                != ZLINK_CORE_SOCKET_ROUTER) {
                _more_out = false;
                errno = EPROTOTYPE;
                return -1;
            }
        }
        select_current_out_pipe (out_pipe->pipe);
        if (expected_connection_id_ != 0
            && _current_out_connection_id != expected_connection_id_) {
            clear_current_out_pipe ();
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        if (connection_id_out_)
            *connection_id_out_ = _current_out_connection_id;
        if (pipe_out_)
            *pipe_out_ = _current_out;

        const pipe_message_admission_t write_admission =
          _more_out ? _current_out->check_write_admission ()
                    : pipe_message_admission_ready;
        if (write_admission != pipe_message_admission_ready) {
            // Preserve the HWM classification while this route is awaiting
            // write activation. Pipe termination removes the routing-table
            // entry and is reported as unreachable on the next lookup.
            const bool pipe_full =
              write_admission == pipe_message_admission_hwm_full;
            const bool transport_wait = write_admission
                                        == pipe_message_admission_transport_wait;
            if (admission_out_)
                *admission_out_ = write_admission;
            mark_out_pipe_inactive (out_pipe);
            clear_current_out_pipe ();
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;

            if (_mandatory) {
                _more_out = false;
                errno = pipe_full || transport_wait ? EAGAIN
                                                     : EHOSTUNREACH;
                if (router_debug::enabled ()) {
                    fprintf (stderr,
                             "router xsend_routed: pipe not writable rid_size=%u errno=%d\n",
                             static_cast<unsigned> (target_rid_->size), errno);
                }
                return -1;
            }
        }
    } else if (!scoped_pipe && _mandatory) {
        //  Only a lookup that found no route at all is unreachable. An exact
        //  transport-pair submit clears `out_pipe` on purpose after resolving
        //  `scoped_pipe`, and a single-part record leaves `_more_out` false;
        //  without this guard that combination fell through to "no out pipe"
        //  and every single-part exact ROUTER submit failed with EHOSTUNREACH.
        _more_out = false;
        errno = EHOSTUNREACH;
        if (router_debug::enabled ()) {
            char rid_text[160];
            router_debug::format_routing_id (target_rid_, rid_text,
                                             sizeof (rid_text));
            fprintf (stderr, "router xsend_routed: no out pipe rid_size=%u rid=%s\n",
                     static_cast<unsigned> (target_rid_->size), rid_text);
        }
        return -1;
    }

    if (expected_route_incarnation_id_ != 0
        && (!_current_out || _current_out->get_transport_pair_id () != 0
            || _current_out->get_transport_pair_generation () != 0
            || _current_out->get_route_incarnation_id ()
                 != expected_route_incarnation_id_)) {
        clear_current_out_pipe ();
        _more_out = false;
        if (connection_id_out_)
            *connection_id_out_ = 0;
        if (pipe_out_)
            *pipe_out_ = NULL;
        errno = EHOSTUNREACH;
        return -1;
    }

    if (_current_out && attempt_identity_out_) {
        attempt_identity_out_->transport_pair_id =
          _current_out->get_transport_pair_id ();
        attempt_identity_out_->transport_pair_generation =
          _current_out->get_transport_pair_generation ();
        attempt_identity_out_->transport_connection_id =
          _current_out_connection_id;
        if (attempt_identity_out_->transport_pair_id == 0
            && attempt_identity_out_->transport_pair_generation == 0)
            attempt_identity_out_->route_incarnation_id =
              _current_out->get_route_incarnation_id ();
    }

    if (_current_out) {
        pipe_t *const write_pipe = _current_out;
        const bool write_more = _more_out;
        // See send_with_observer(): ordinary routed sends keep the route
        // fence as their lifetime guard, while observer-backed request/reply
        // sends retain a pin before dropping that fence.
        const bool release_route_for_write = observer_ != NULL;
        if (release_route_for_write && !write_pipe->retain_lifetime_ref ()) {
            clear_current_out_pipe ();
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        if (router_debug::enabled ()) {
            fprintf (stderr, "router xsend_routed selected: pipe=%p lane=%d pair=%llu/%llu\\n",
                     static_cast<void *> (write_pipe),
                     static_cast<int> (write_pipe->get_transport_lane ()),
                     static_cast<unsigned long long> (write_pipe->get_transport_pair_id ()),
                     static_cast<unsigned long long> (write_pipe->get_transport_pair_generation ()));
        }
        msg_->set_transport_connection_id (
          _current_out_connection_id);
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        // A terminal routed send has no later continuation, so clear the
        // selected route state before the write. Observer-backed sends keep
        // the pipe valid with their lifetime pin; ordinary sends retain the
        // route fence.
        if (!write_more && _current_out == write_pipe)
            clear_current_out_pipe ();
        if (release_route_for_write)
            route_lifecycle_lock.unlock ();
        const bool ok =
          observer_
            ? write_pipe->write_message_observed (
                msg_, observer_, observer_userdata_, &write_admission)
            : write_more ? write_pipe->write (msg_, &write_admission)
                         : write_pipe
                            ->write_single_message_and_flush_no_recursive_hwm_check (
                              msg_, &write_admission);
        const int write_errno = errno;
        if (unlikely (!ok)) {
            write_pipe->rollback ();
            errno = write_errno;
            // Only the failure path needs routing-table revalidation.
            if (release_route_for_write)
                route_lifecycle_lock.lock ();
        }
        if (unlikely (!ok)) {
            if (observer_ && errno == ECANCELED
                && write_admission == pipe_message_admission_invalid) {
                if (_current_out == write_pipe)
                    clear_current_out_pipe ();
                if (connection_id_out_)
                    *connection_id_out_ = 0;
                if (pipe_out_)
                    *pipe_out_ = NULL;
                _more_out = false;
                route_lifecycle_lock.unlock ();
                if (release_route_for_write)
                    write_pipe->release_lifetime_ref ();
                return -1;
            }
            // A routed multipart send can reach HWM after its first frame.
            // Keep the admitted route and report backpressure until write
            // activation returns credit.
            if (admission_out_)
                *admission_out_ = write_admission;
            const blob_t &routing_id = write_pipe->get_routing_id ();
            out_pipe_t *current_out_pipe = lookup_out_pipe (routing_id);
            if (write_admission != pipe_message_admission_request_full
                && current_out_pipe && current_out_pipe->pipe == write_pipe)
                mark_out_pipe_inactive (current_out_pipe);
            if (router_debug::enabled ()) {
                fprintf (stderr, "router xsend_routed: write failed rid_size=%u\n",
                         static_cast<unsigned> (target_rid_->size));
            }
            if (_current_out == write_pipe)
                clear_current_out_pipe ();
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;
            if (_mandatory) {
                _more_out = false;
                errno = write_admission == pipe_message_admission_too_large
                          ? EMSGSIZE
                          : write_admission == pipe_message_admission_hwm_full
                                || write_admission
                                     == pipe_message_admission_request_full
                                || write_admission
                                     == pipe_message_admission_transport_wait
                              ? EAGAIN
                              : EHOSTUNREACH;
                // xsend_routed always starts a new record, so no earlier part
                // was staged by this call. Report an ordinary failure and let
                // the caller apply its submit-retry policy.
                route_lifecycle_lock.unlock ();
                if (release_route_for_write)
                    write_pipe->release_lifetime_ref ();
                return -1;
            }
            const int rc = msg_->close ();
            errno_assert (rc == 0);
        }
        if (unlikely (!ok))
            route_lifecycle_lock.unlock ();
        if (release_route_for_write)
            write_pipe->release_lifetime_ref ();
    } else {
        const int rc = msg_->close ();
        errno_assert (rc == 0);
    }

    const int rc = msg_->init ();
    errno_assert (rc == 0);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    return 0;
}

bool zlink::router_t::xhas_out ()
{
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    if (!_mandatory)
        return true;

    return has_writable_weighted_out_pipes ();
}

bool zlink::router_t::xsend_writable_target_ready (
  const zlink_routing_id_t *target_rid_or_null_)
{
    if (!valid_routing_id (target_rid_or_null_))
        return false;

    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    const blob_t routing_id (
      const_cast<unsigned char *> (target_rid_or_null_->data),
      target_rid_or_null_->size, reference_tag_t ());
    const out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || !out_pipe->pipe || !out_pipe->active
        || out_pipe->weight == 0 || !out_pipe->pipe->is_lifecycle_active ())
        return false;

    const uint64_t pair_id = out_pipe->pipe->get_transport_pair_id ();
    if (pair_id != 0
        && (out_pipe->pipe->get_transport_pair_generation () == 0
            || !transport_pair_application_ready (out_pipe->pipe)))
        return false;
    if (pair_id == 0
        && out_pipe->pipe->get_transport_connection_id () == 0)
        return false;

    return out_pipe->pipe->check_write_admission ()
           == pipe_message_admission_ready;
}

bool zlink::router_t::xsend_writable_target_known (
  const zlink_routing_id_t *target_rid_or_null_)
{
    if (!valid_routing_id (target_rid_or_null_))
        return false;

    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    const blob_t routing_id (
      const_cast<unsigned char *> (target_rid_or_null_->data),
      target_rid_or_null_->size, reference_tag_t ());
    const out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
    return out_pipe && out_pipe->pipe
           && out_pipe->pipe->is_lifecycle_active ();
}

bool zlink::router_t::xsend_writable_target_for_pipe (
  pipe_t *pipe_, zlink_routing_id_t *target_rid_out_)
{
    if (!pipe_ || !target_rid_out_)
        return false;

    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    const blob_t &routing_id = pipe_->get_routing_id ();
    const out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || out_pipe->pipe != pipe_ || routing_id.size () == 0
        || routing_id.size () > sizeof (target_rid_out_->data))
        return false;
    copy_routing_id_from_bytes (routing_id.data (), routing_id.size (),
                                target_rid_out_);
    return true;
}
