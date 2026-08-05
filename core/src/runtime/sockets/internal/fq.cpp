/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <vector>

#include "sockets/internal/fq.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"

#ifdef ZLINK_BUILD_TESTS
#include <atomic>

namespace
{
std::atomic<zlink::fq_t::recv_test_hook_fn> recv_test_hook (NULL);
std::atomic<void *> recv_test_hook_userdata (NULL);
}

void zlink::fq_t::set_recv_test_hook (recv_test_hook_fn hook_, void *userdata_)
{
    recv_test_hook_userdata.store (userdata_, std::memory_order_release);
    recv_test_hook.store (hook_, std::memory_order_release);
}
#endif

zlink::fq_t::fq_t () : _active (0), _current (0), _more (false)
{
}

zlink::fq_t::~fq_t ()
{
    zlink_assert (_pipes.empty ());
}

bool zlink::fq_t::try_get_pipe_index (pipe_t *pipe_, pipes_t::size_type *index_out_)
{
    if (!pipe_)
        return false;

    const int claimed_index = static_cast<array_item_t<1> *> (pipe_)->get_array_index ();
    if (claimed_index < 0)
        return false;

    const pipes_t::size_type index = static_cast<pipes_t::size_type> (claimed_index);
    if (index >= _pipes.size () || _pipes[index] != pipe_)
        return false;

    if (index_out_)
        *index_out_ = index;
    return true;
}

void zlink::fq_t::normalize_state ()
{
    const pipes_t::size_type size = _pipes.size ();
    if (_active > size)
        _active = size;

    if (_active == 0) {
        _current = 0;
        _more = false;
        return;
    }

    if (_current >= _active)
        _current = 0;
}

void zlink::fq_t::attach (pipe_t *pipe_)
{
    _pipes.push_back (pipe_);
    _pipes.swap (_active, _pipes.size () - 1);
    _active++;
}

void zlink::fq_t::deactivate (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;
    if (index >= _active)
        return;

    _active--;
    _pipes.swap (index, _active);
    if (_current == _active)
        _current = 0;
}

void zlink::fq_t::pipe_terminated (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;

    //  Remove the pipe from the list; adjust number of active pipes
    //  accordingly.
    if (index < _active) {
        _active--;
        _pipes.swap (index, _active);
        if (_current == _active)
            _current = 0;
    }
    _pipes.erase (pipe_);
    normalize_state ();
}

void zlink::fq_t::activated (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;
    if (index < _active)
        return;

    //  Move the pipe to the list of active pipes.
    _pipes.swap (index, _active);
    _active++;
}

void zlink::fq_t::arm_dispatch ()
{
    normalize_state ();

    std::vector<pipe_t *> pipes;
    pipes.reserve (_pipes.size ());
    for (pipes_t::size_type i = 0; i < _pipes.size (); ++i)
        pipes.push_back (_pipes[i]);

    for (size_t i = 0; i < pipes.size (); ++i) {
        pipe_t *pipe = pipes[i];
        if (!pipe)
            continue;

        if (pipe->check_read ()) {
            // A read-activation command may still be pending when dispatch is
            // installed. Activate readable inactive pipes immediately so the
            // installation drain consumes messages already in the pipe.
            activated (pipe);
        } else {
            // Empty active pipes must produce a future read-activation edge.
            deactivate (pipe);
        }
    }
}

int zlink::fq_t::recv (msg_t *msg_)
{
    return recvpipe (msg_, NULL);
}

int zlink::fq_t::recvpipe (msg_t *msg_, pipe_t **pipe_)
{
    normalize_state ();

    //  Deallocate old content of the message.
    int rc = msg_->close ();
    if (unlikely (rc != 0)) {
        rc = msg_->init ();
        errno_assert (rc == 0);
    }

    //  Round-robin over the pipes to get the next message.
    while (_active > 0) {
        //  Try to fetch new message. If we've already read part of the message
        //  subsequent part should be immediately available.
        pipe_t *const current_pipe = _pipes[_current];
#ifdef ZLINK_BUILD_TESTS
        recv_test_hook_fn hook = recv_test_hook.load (std::memory_order_acquire);
        if (hook
            && !hook (this, current_pipe,
                      recv_test_hook_userdata.load (std::memory_order_acquire))) {
            rc = msg_->init ();
            errno_assert (rc == 0);
            errno = EAGAIN;
            return -1;
        }
#endif
        const bool fetched = current_pipe->read (msg_);

        //  Note that when message is not fetched, current pipe is deactivated
        //  and replaced by another active pipe. Thus we don't have to increase
        //  the 'current' pointer.
        if (fetched) {
            if (pipe_)
                *pipe_ = _pipes[_current];
            _more = (msg_->flags () & msg_t::more) != 0;
            if (!_more) {
                if (_active > 0)
                    _current = (_current + 1) % _active;
                else
                    _current = 0;
            }
            return 0;
        }

        //  Internal routed envelopes are multipart on the wire. If the pipe
        //  disappears between frames during teardown, drop the partial message
        //  and surface a transient miss instead of aborting or surfacing a
        //  spurious protocol failure to callers.
        if (_more) {
            _more = false;
            _active--;
            _pipes.swap (_current, _active);
            if (_current == _active)
                _current = 0;
            rc = msg_->init ();
            errno_assert (rc == 0);
            errno = EAGAIN;
            return -1;
        }

        _active--;
        _pipes.swap (_current, _active);
        if (_current == _active)
            _current = 0;
    }

    //  No message is available. Initialise the output parameter
    //  to be a 0-byte message.
    rc = msg_->init ();
    errno_assert (rc == 0);
    errno = EAGAIN;
    return -1;
}

bool zlink::fq_t::has_in ()
{
    normalize_state ();

    //  There are subsequent parts of the partly-read message available.
    if (_more)
        return true;

    //  Note that messing with current doesn't break the fairness of fair
    //  queueing algorithm. If there are no messages available current will
    //  get back to its original value. Otherwise it'll point to the first
    //  pipe holding messages, skipping only pipes with no messages available.
    while (_active > 0) {
        if (_pipes[_current]->check_read ())
            return true;

        //  Deactivate the pipe.
        _active--;
        _pipes.swap (_current, _active);
        if (_current == _active)
            _current = 0;
    }

    return false;
}
