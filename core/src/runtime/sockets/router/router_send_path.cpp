/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/router/router.hpp"
#include "utils/likely.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"

#include <cstdlib>
#include <cstdio>

namespace
{
const bool router_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

bool router_debug_enabled ()
{
    return router_debug_on;
}

void format_routing_id_debug (const zlink_routing_id_t *rid_, char *buf_, size_t buf_size_)
{
    if (!buf_ || buf_size_ == 0)
        return;

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

}

int zlink::router_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    // Public send enters this path through socket_public_send_scope_t. Do not
    // add lock_socket_msg_dispatch() here: routed echo hot paths call
    // router send for every small message, and a second socket-level mutex
    // serializes those sends. Pipe teardown still updates router pipe state
    // under lock_socket_msg_dispatch() in xpipe_terminated().

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
                _current_out = out_pipe->pipe;
                _current_out_connection_id =
                  _current_out->get_transport_connection_id ();

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
                    _current_out = NULL;
                    _current_out_connection_id = 0;

                    if (_mandatory) {
                        _more_out = false;
                        errno = pipe_full || transport_wait ? EAGAIN
                                                            : EHOSTUNREACH;
                        if (router_debug_enabled ()) {
                            fprintf (stderr, "router xsend: pipe not writable size=%zu errno=%d\n",
                                     msg_->size (), errno);
                        }
                        return -1;
                    }
                }
            } else if (_mandatory) {
                _more_out = false;
                errno = EHOSTUNREACH;
                if (router_debug_enabled ()) {
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

    if (router_debug_enabled ()) {
        fprintf (stderr, "router xsend continuation: pipe=%p size=%zu more=%d\\n",
                 static_cast<void *> (_current_out), msg_ ? msg_->size () : 0,
                 (msg_->flags () & msg_t::more) != 0 ? 1 : 0);
    }

    _more_out = (msg_->flags () & msg_t::more) != 0;

    if (_current_out) {
        msg_->set_transport_connection_id (
          _current_out_connection_id);
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok = _current_out->write_owner_started_message (
          msg_, &write_admission);
        if (router_debug_enabled ())
            fprintf (stderr, "router xsend write: pipe=%p ok=%d more=%d\\n",
                     static_cast<void *> (_current_out), ok ? 1 : 0,
                     _more_out ? 1 : 0);
        if (unlikely (!ok)) {
            // The first multipart frame can pass the readiness check and a
            // later frame can encounter HWM. Preserve that as capacity
            // pressure; only an inactive pipe is unreachable.
            if (admission_out_)
                *admission_out_ = write_admission;
            const blob_t &routing_id = _current_out->get_routing_id ();
            out_pipe_t *current_out_pipe = lookup_out_pipe (routing_id);
            mark_out_pipe_inactive (current_out_pipe);
            if (router_debug_enabled ()) {
                fprintf (stderr, "router xsend: drop message size=%zu\n", msg_->size ());
            }
            _current_out->rollback ();
            _current_out = NULL;
            _current_out_connection_id = 0;
            if (_mandatory) {
                _more_out = false;
                errno = write_admission == pipe_message_admission_hwm_full
                              || write_admission
                                   == pipe_message_admission_transport_wait
                          ? EAGAIN
                          : EHOSTUNREACH;
                return -1;
            }
            const int rc = msg_->close ();
            errno_assert (rc == 0);
        } else if (!_more_out) {
            _current_out = NULL;
            _current_out_connection_id = 0;
        }
    } else {
        if (router_debug_enabled ()) {
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
  pipe_message_admission_t *admission_out_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    const int rc = xsend (msg_, admission_out_);
    if (rc == 0 && pipe_out_ && _current_out)
        *pipe_out_ = _current_out;
    return rc;
}

int zlink::router_t::xsend_routed (const zlink_routing_id_t *target_rid_,
                                  msg_t *msg_,
  uint64_t *connection_id_out_,
  uint64_t expected_connection_id_,
  pipe_t **pipe_out_,
  uint64_t expected_transport_pair_id_,
  uint64_t expected_transport_pair_generation_,
  pipe_message_admission_t *admission_out_)
{
    zlink_assert (!_more_out);
    zlink_assert (!_current_out);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;

    _more_out = (msg_->flags () & msg_t::more) != 0;

    if (router_debug_enabled ()) {
        fprintf (stderr,
                 "router xsend_routed enter: rid_size=%u pair=%llu/%llu size=%zu more=%d\\n",
                 static_cast<unsigned> (target_rid_ ? target_rid_->size : 0),
                 static_cast<unsigned long long> (expected_transport_pair_id_),
                 static_cast<unsigned long long> (expected_transport_pair_generation_),
                 msg_ ? msg_->size () : 0, _more_out ? 1 : 0);
    }

    out_pipe_t *out_pipe = lookup_out_pipe (blob_t (const_cast<unsigned char *> (target_rid_->data),
                                                    target_rid_->size, zlink::reference_tag_t ()));
    pipe_t *scoped_pipe = NULL;
    if (expected_transport_pair_id_ != 0 || expected_transport_pair_generation_ != 0) {
        scoped_pipe = find_transport_pair_pipe (
          target_rid_, expected_transport_pair_id_, expected_transport_pair_generation_);
        if (!scoped_pipe) {
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        _current_out = scoped_pipe;
        _current_out_connection_id = _current_out->get_transport_connection_id ();
        if (expected_connection_id_ != 0
            && _current_out_connection_id != expected_connection_id_) {
            _current_out = NULL;
            _current_out_connection_id = 0;
            _more_out = false;
            errno = EHOSTUNREACH;
            return -1;
        }
        if (connection_id_out_)
            *connection_id_out_ = _current_out_connection_id;
        if (pipe_out_)
            *pipe_out_ = _current_out;
        out_pipe = NULL;
    }
    if (scoped_pipe && _more_out) {
        const pipe_message_admission_t write_admission =
          _current_out->check_write_admission ();
        if (write_admission != pipe_message_admission_ready) {
            if (admission_out_)
                *admission_out_ = write_admission;
            _current_out = NULL;
            _current_out_connection_id = 0;
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
            if (router_debug_enabled ()) {
                fprintf (stderr, "router xsend_routed: draining rid_size=%u\n",
                         static_cast<unsigned> (target_rid_->size));
            }
            return -1;
        }
        _current_out = out_pipe->pipe;
        _current_out_connection_id =
          _current_out->get_transport_connection_id ();
        if (expected_connection_id_ != 0
            && _current_out_connection_id != expected_connection_id_) {
            _current_out = NULL;
            _current_out_connection_id = 0;
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
            _current_out = NULL;
            _current_out_connection_id = 0;
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;

            if (_mandatory) {
                _more_out = false;
                errno = pipe_full || transport_wait ? EAGAIN
                                                     : EHOSTUNREACH;
                if (router_debug_enabled ()) {
                    fprintf (stderr,
                             "router xsend_routed: pipe not writable rid_size=%u errno=%d\n",
                             static_cast<unsigned> (target_rid_->size), errno);
                }
                return -1;
            }
        }
    } else if (_mandatory) {
        _more_out = false;
        errno = EHOSTUNREACH;
        if (router_debug_enabled ()) {
            char rid_text[160];
            format_routing_id_debug (target_rid_, rid_text, sizeof (rid_text));
            fprintf (stderr, "router xsend_routed: no out pipe rid_size=%u rid=%s\n",
                     static_cast<unsigned> (target_rid_->size), rid_text);
        }
        return -1;
    }

    if (_current_out) {
        if (router_debug_enabled ()) {
            fprintf (stderr, "router xsend_routed selected: pipe=%p lane=%d pair=%llu/%llu\\n",
                     static_cast<void *> (_current_out),
                     static_cast<int> (_current_out->get_transport_lane ()),
                     static_cast<unsigned long long> (_current_out->get_transport_pair_id ()),
                     static_cast<unsigned long long> (_current_out->get_transport_pair_generation ()));
        }
        msg_->set_transport_connection_id (
          _current_out_connection_id);
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok = _more_out
                          ? _current_out->write (msg_, &write_admission)
                          : _current_out->write_single_message_and_flush_no_recursive_hwm_check (
                              msg_, &write_admission);
        if (unlikely (!ok)) {
            // A routed multipart send can reach HWM after its first frame.
            // Keep the admitted route and report backpressure until write
            // activation returns credit.
            if (admission_out_)
                *admission_out_ = write_admission;
            const blob_t &routing_id = _current_out->get_routing_id ();
            out_pipe_t *current_out_pipe = lookup_out_pipe (routing_id);
            mark_out_pipe_inactive (current_out_pipe);
            if (router_debug_enabled ()) {
                fprintf (stderr, "router xsend_routed: write failed rid_size=%u\n",
                         static_cast<unsigned> (target_rid_->size));
            }
            _current_out->rollback ();
            _current_out = NULL;
            _current_out_connection_id = 0;
            if (connection_id_out_)
                *connection_id_out_ = 0;
            if (pipe_out_)
                *pipe_out_ = NULL;
            if (_mandatory) {
                _more_out = false;
                errno = write_admission == pipe_message_admission_hwm_full
                              || write_admission
                                   == pipe_message_admission_transport_wait
                          ? EAGAIN
                          : EHOSTUNREACH;
                return -1;
            }
            const int rc = msg_->close ();
            errno_assert (rc == 0);
        } else if (!_more_out) {
            _current_out = NULL;
            _current_out_connection_id = 0;
        }
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
    if (!_mandatory)
        return true;

    return has_writable_weighted_out_pipes ();
}
