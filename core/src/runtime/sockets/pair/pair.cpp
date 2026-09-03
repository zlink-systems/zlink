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
    _recv_part_index (0)
{
    options.type = ZLINK_CORE_SOCKET_PAIR;
    refresh_auto_hwm_policy ();
}

zlink::pair_t::~pair_t ()
{
    zlink_assert (!_pipe);
}

void zlink::pair_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);
    LIBZLINK_UNUSED (locally_initiated_);

    zlink_assert (pipe_ != NULL);

    //  ZLINK_CORE_SOCKET_PAIR socket can only be connected to a single peer.
    //  The socket rejects any further connection requests.
    if (_pipe == NULL)
        _pipe = pipe_;
    else
        pipe_->terminate (false);
}

void zlink::pair_t::xpipe_terminated (pipe_t *pipe_)
{
    if (pipe_ == _pipe) {
        _pipe = NULL;
        _recv_part_index = 0;
    }
}

void zlink::pair_t::xread_activated (pipe_t *)
{
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

bool zlink::pair_t::xtry_send_complete_record (msg_t *parts_,
                                               size_t part_count_)
{
    if (!parts_ || part_count_ < 2 || !_pipe)
        return false;
#ifdef ZLINK_BUILD_TESTS
    // Preserve the per-frame test interception contract by falling back to
    // xsend whenever a test has installed its gate hook.
    if (g_pair_xsend_gate_hook.load (std::memory_order_acquire))
        return false;
#endif
    if (!_pipe->try_write_complete_record_and_flush (parts_, part_count_))
        return false;

    for (size_t i = 0; i != part_count_; ++i) {
        const int rc = parts_[i].init ();
        errno_assert (rc == 0);
    }
    return true;
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
