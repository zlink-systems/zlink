/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/pair/pair.hpp"
#include "utils/err.hpp"
#include "core/pipe.hpp"
#include "core/msg.hpp"

namespace
{
void drain_pair_dispatch (zlink::pair_t *self_, zlink::pipe_t *pipe_)
{
    zlink::msg_t msg;
    const int init_rc = msg.init ();
    errno_assert (init_rc == 0);

    while (pipe_ && pipe_->read (&msg)) {
        const int dispatch_rc = self_->xsocket_msg_dispatch (&msg, pipe_);
        if (dispatch_rc <= 0)
            break;
    }

    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}
}

zlink::pair_t::pair_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    socket_base_t (parent_, tid_, sid_), _pipe (NULL)
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
        if (socket_msg_dispatch_active ())
            _pipe->check_read ();
    } else
        pipe_->terminate (false);
}

void zlink::pair_t::xpipe_terminated (pipe_t *pipe_)
{
    if (pipe_ == _pipe) {
        _pipe = NULL;
    }
}

void zlink::pair_t::xread_activated (pipe_t *)
{
    //  There's just one pipe. Drain it immediately when dispatch mode is
    //  active so inproc callback consumers don't leave local pipes backed up.
    if (socket_msg_dispatch_active () && _pipe)
        drain_pair_dispatch (this, _pipe);
}

void zlink::pair_t::xwrite_activated (pipe_t *)
{
    //  There's just one pipe. No lists of active and inactive pipes.
    //  There's nothing to do here.
}

int zlink::pair_t::xsend (msg_t *msg_)
{
    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool ok = _pipe ? (more ? _pipe->write (msg_) : _pipe->write_and_flush (msg_)) : false;
    if (!ok) {
        if (errno != EMSGSIZE)
            errno = EAGAIN;
        return -1;
    }

    //  Detach the original message from the data buffer.
    const int rc = msg_->init ();
    errno_assert (rc == 0);

    return 0;
}

int zlink::pair_t::xrecv (msg_t *msg_)
{
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

    store_socket_msg_part (&_dispatch_parts, msg_);
    if ((reinterpret_cast<msg_t *> (&_dispatch_parts.back ())->flags () & msg_t::more) != 0) {
        return 1;
    }

    zlink_socket_msg_handler_fn handler = socket_msg_handler ();
    if (!handler) {
        close_socket_msg_parts (&_dispatch_parts);
        return 1;
    }

    zlink_routing_id_t source_rid;
    resolve_socket_msg_source_rid (pipe_, &source_rid);
    invoke_socket_msg_handler (handler, &source_rid, &_dispatch_parts[0], _dispatch_parts.size ());
    _dispatch_parts.clear ();
    return 1;
}

void zlink::pair_t::xdispatch_io ()
{
    if (!socket_msg_dispatch_active () || !_pipe)
        return;

    drain_pair_dispatch (this, _pipe);
}
