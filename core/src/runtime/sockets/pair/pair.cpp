/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/pair/pair.hpp"
#include "utils/err.hpp"
#include "core/pipe.hpp"
#include "core/msg.hpp"

#ifdef ZLINK_BUILD_TESTS
namespace
{
std::atomic<zlink::pair_xsend_gate_hook_fn> g_pair_xsend_gate_hook (NULL);
std::atomic<void *> g_pair_xsend_gate_userdata (NULL);
}

void zlink::test_set_pair_xsend_gate_hook (pair_xsend_gate_hook_fn hook_,
                                           void *userdata_)
{
    if (!hook_)
        g_pair_xsend_gate_hook.store (NULL, std::memory_order_release);
    g_pair_xsend_gate_userdata.store (userdata_, std::memory_order_release);
    if (hook_)
        g_pair_xsend_gate_hook.store (hook_, std::memory_order_release);
}
#endif

zlink::pair_t::pair_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_),
    _pipe (NULL),
    _recv_part_index (0),
    _dispatch_pipe (NULL),
    _dispatch_malformed (false)
{
    options.type = ZLINK_CORE_SOCKET_PAIR;
    refresh_auto_hwm_policy ();
}

zlink::pair_t::~pair_t ()
{
    close_socket_msg_parts (&_dispatch_parts);
    zlink_assert (!_pipe);
}

void zlink::pair_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);
    LIBZLINK_UNUSED (locally_initiated_);

    zlink_assert (pipe_ != NULL);

    //  ZLINK_CORE_SOCKET_PAIR socket can only be connected to a single peer.
    //  The socket rejects any further connection requests.
    if (_pipe == NULL) {
        _pipe = pipe_;
        if (socket_msg_dispatch_active () && _pipe->check_read ())
            defer_socket_msg_dispatch ();
    } else
        pipe_->terminate (false);
}

void zlink::pair_t::xpipe_terminated (pipe_t *pipe_)
{
    if (pipe_ == _pipe) {
        _pipe = NULL;
        _recv_part_index = 0;
    }
}

void zlink::pair_t::xsocket_msg_pipe_terminated (pipe_t *pipe_)
{
    if (pipe_ != _dispatch_pipe)
        return;
    close_socket_msg_parts (&_dispatch_parts);
    _dispatch_pipe = NULL;
    _dispatch_malformed = false;
}

void zlink::pair_t::xread_activated (pipe_t *)
{
    // read_activated is delivered while the command executor owns receive
    // state. Publish callback work now and drain it only after that owner is
    // released, so callback recv/send re-entry cannot invert receive and
    // dispatch locks.
    if (socket_msg_dispatch_active () && _pipe)
        defer_socket_msg_dispatch ();
}

void zlink::pair_t::xwrite_activated (pipe_t *)
{
    //  There's just one pipe. No lists of active and inactive pipes.
    //  There's nothing to do here.
}

int zlink::pair_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
#ifdef ZLINK_BUILD_TESTS
    const pair_xsend_gate_hook_fn gate_hook =
      g_pair_xsend_gate_hook.load (std::memory_order_acquire);
    if (gate_hook)
        gate_hook (g_pair_xsend_gate_userdata.load (
          std::memory_order_acquire));
#endif
    const bool more = (msg_->flags () & msg_t::more) != 0;
    pipe_message_admission_t admission = pipe_message_admission_inactive;
    const bool ok = _pipe
                      ? (more ? _pipe->write (msg_, &admission)
                              : _pipe->write_and_flush (msg_, &admission))
                      : false;
    if (!ok) {
        const int failure_errno =
          admission == pipe_message_admission_too_large ? errno : EAGAIN;
        // A continuation failure must discard the prefix before the blocking
        // retry path can release the socket lifecycle lock. Otherwise an
        // independent complete send can be appended to that unfinished
        // record. Query-and-rollback under the pipe lock avoids adding a
        // multipart flag write to PAIR's successful single-message hot path.
        const bool multipart_aborted =
          _pipe && _pipe->rollback_incomplete ();
        if (admission_out_)
            *admission_out_ = admission;
        errno = failure_errno;
        return multipart_aborted ? -2 : -1;
    }

    //  Detach the original message from the data buffer.
    const int rc = msg_->init ();
    errno_assert (rc == 0);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;

    return 0;
}

int zlink::pair_t::xrollback ()
{
    if (_pipe)
        _pipe->rollback ();
    return 0;
}

int zlink::pair_t::xrecv (msg_t *msg_)
{
    return xrecv_pipe (msg_, NULL);
}

int zlink::pair_t::xrecv_pipe (msg_t *msg_, pipe_t **pipe_out_)
{
    if (pipe_out_)
        *pipe_out_ = _pipe;
    //  Deallocate old content of the message.
    int rc = msg_->close ();
    errno_assert (rc == 0);

    if (!_pipe || !_pipe->read (msg_)) {
        //  Initialise the output parameter to be a 0-byte message.
        rc = msg_->init ();
        errno_assert (rc == 0);

        errno = EAGAIN;
        return -1;
    }

    const bool first_part = _recv_part_index == 0;
    unsigned char request_reply_kind = 0;
    uint64_t request_reply_sequence = 0;
    if (!first_part
        && msg_->get_request_reply_metadata (
          &request_reply_kind, &request_reply_sequence)) {
        pipe_t *const malformed_pipe =
          _pipe->retain_lifetime_ref () ? _pipe : NULL;
        bool more = (msg_->flags () & msg_t::more) != 0;
        while (more && malformed_pipe) {
            rc = msg_->close ();
            errno_assert (rc == 0);
            rc = msg_->init ();
            errno_assert (rc == 0);
            if (!malformed_pipe->read (msg_))
                break;
            more = (msg_->flags () & msg_t::more) != 0;
        }
        if (malformed_pipe) {
            malformed_pipe->terminate (false);
            malformed_pipe->release_lifetime_ref ();
        }
        rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        _recv_part_index = 0;
        errno = EPROTO;
        return -1;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    _recv_part_index = more ? _recv_part_index + 1 : 0;
    if (first_part)
        msg_->reset_request_reply_metadata ();
    return 0;
}

bool zlink::pair_t::xhas_in ()
{
    if (!_pipe)
        return false;

    return _pipe->check_read ();
}

bool zlink::pair_t::xhas_out ()
{
    if (!_pipe)
        return false;

    return _pipe->check_write ();
}

int zlink::pair_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    if (!socket_msg_dispatch_active ())
        return 0;

    const bool final_part = (msg_->flags () & msg_t::more) == 0;
    if (_dispatch_parts.empty () && !_dispatch_malformed)
        _dispatch_pipe = pipe_;
    if (_dispatch_malformed) {
        msg_->reset_request_reply_metadata ();
        if (final_part) {
            _dispatch_malformed = false;
            _dispatch_pipe = NULL;
        }
        return 1;
    }

    unsigned char request_reply_kind = 0;
    uint64_t request_reply_sequence = 0;
    if (!_dispatch_parts.empty ()
        && msg_->get_request_reply_metadata (
          &request_reply_kind, &request_reply_sequence)) {
        close_socket_msg_parts (&_dispatch_parts);
        _dispatch_malformed = !final_part;
        if (final_part)
            _dispatch_pipe = NULL;
        msg_->reset_request_reply_metadata ();
        pipe_t *const malformed_pipe =
          pipe_ && pipe_->retain_lifetime_ref () ? pipe_ : NULL;
        if (malformed_pipe) {
            malformed_pipe->terminate (false);
            malformed_pipe->release_lifetime_ref ();
        }
        return 1;
    }

    store_socket_msg_part (&_dispatch_parts, msg_);
    if ((reinterpret_cast<msg_t *> (&_dispatch_parts.back ())->flags () & msg_t::more) != 0) {
        return 1;
    }

    zlink_socket_msg_handler_fn handler = socket_msg_handler ();
    if (!handler) {
        close_socket_msg_parts (&_dispatch_parts);
        _dispatch_pipe = NULL;
        return 1;
    }

    zlink_routing_id_t source_rid;
    resolve_socket_msg_source_rid (pipe_, &source_rid);
    invoke_socket_msg_handler (handler, &source_rid, &_dispatch_parts[0], _dispatch_parts.size ());
    _dispatch_parts.clear ();
    _dispatch_pipe = NULL;
    return 1;
}

void zlink::pair_t::xdispatch_io ()
{
    if (!socket_msg_dispatch_active ())
        return;

    for (;;) {
        msg_t msg;
        const int init_rc = msg.init ();
        errno_assert (init_rc == 0);

        pipe_t *pipe = NULL;
        bool received = false;
        {
            // PAIR has no FQ wrapper, but its SPSC queue still has exactly one
            // reader. Extract each frame under receive ownership, retain only
            // the source identity, then release before invoking user code.
            scoped_lock_t receive_lock (receive_sync ());
            pipe = _pipe;
            if (pipe && pipe->retain_lifetime_ref ()) {
                received = pipe->read (&msg);
                if (!received) {
                    pipe->release_lifetime_ref ();
                    pipe = NULL;
                }
            } else
                pipe = NULL;
        }

        if (!received) {
            const int close_rc = msg.close ();
            errno_assert (close_rc == 0);
            break;
        }

        const int dispatch_rc = socket_msg_dispatch_from_io (&msg, pipe);
        const int saved_errno = errno;
        pipe->release_lifetime_ref ();
        const int close_rc = msg.close ();
        errno_assert (close_rc == 0);
        errno = saved_errno;
        if (dispatch_rc <= 0)
            break;
    }
}
