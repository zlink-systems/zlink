/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <algorithm>

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

zlink::fq_t::fq_t (receive_activity_publication_t publication_) :
    _active (0),
    _current (0),
    _more (false),
    _multipart_abort_pending (false),
    _receive_activity_publication (publication_)
{
}

zlink::fq_t::~fq_t ()
{
    zlink_assert (_pipes.empty ());
    zlink_assert (_record_admission_blocked_set.empty ());
}

bool zlink::fq_t::record_admission_blocked (pipe_t *pipe_) const
{
    return pipe_
           && _record_admission_blocked_set.find (pipe_)
                != _record_admission_blocked_set.end ();
}

void zlink::fq_t::publish_pipe_receive_activity (pipe_t *pipe_, bool active_) const
{
    if (_receive_activity_publication == publish_receive_activity)
        pipe_->set_public_receive_active_cached (active_);
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
    publish_pipe_receive_activity (pipe_, true);
}

void zlink::fq_t::deactivate (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;
    if (index >= _active) {
        publish_pipe_receive_activity (pipe_, false);
        return;
    }

    deactivate_at (index);
}

void zlink::fq_t::deactivate_at (pipes_t::size_type index_)
{
    pipe_t *const pipe = _pipes[index_];
    _active--;
    _pipes.swap (index_, _active);
    publish_pipe_receive_activity (pipe, false);
    if (_current == _active)
        _current = 0;
}

void zlink::fq_t::deactivate_current_after_read_miss ()
{
    // Publish the miss against the current source before rotating the active
    // partition; routed receive activity observes that source identity.
    publish_pipe_receive_activity (_pipes[_current], false);
    _active--;
    _pipes.swap (_current, _active);
    if (_current == _active)
        _current = 0;
}

void zlink::fq_t::pipe_terminated (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;

    if (_record_admission_blocked_set.erase (pipe_) != 0) {
        _record_admission_blocked.erase (
          std::remove (_record_admission_blocked.begin (),
                       _record_admission_blocked.end (), pipe_),
          _record_admission_blocked.end ());
    }

    pipe_t *const current_pipe =
      _active > 0 && _current < _active ? _pipes[_current] : NULL;
    if (_more && pipe_ == current_pipe) {
        _more = false;
        _multipart_abort_pending = true;
    }

    //  Remove the pipe from the list; adjust number of active pipes
    //  accordingly.
    if (index < _active)
        deactivate_at (index);
    else
        publish_pipe_receive_activity (pipe_, false);
    _pipes.erase (pipe_);
    normalize_state ();

    //  Removal uses swaps. Preserve the pipe selected for an in-progress
    //  fair-queue turn when some other pipe terminates; otherwise the next
    //  frame could silently come from a different peer.
    if (current_pipe && current_pipe != pipe_) {
        pipes_t::size_type current_index = 0;
        if (try_get_pipe_index (current_pipe, &current_index)
            && current_index < _active)
            _current = current_index;
    } else if (_active > 0) {
        _current = index < _active ? index : 0;
    }
}

void zlink::fq_t::activated (pipe_t *pipe_)
{
    normalize_state ();

    pipes_t::size_type index = 0;
    if (!try_get_pipe_index (pipe_, &index))
        return;
    if (record_admission_blocked (pipe_))
        return;
    if (index < _active) {
        publish_pipe_receive_activity (pipe_, true);
        return;
    }

    //  Move the pipe to the list of active pipes.
    _pipes.swap (index, _active);
    _active++;
    publish_pipe_receive_activity (pipe_, true);
}

bool zlink::fq_t::block_current_for_record_admission ()
{
    normalize_state ();
    if (_active == 0 || _current >= _active)
        return false;

    pipe_t *const pipe = _pipes[_current];
    if (!record_admission_blocked (pipe)) {
        try {
            const std::pair<std::set<pipe_t *>::iterator, bool> inserted =
              _record_admission_blocked_set.insert (pipe);
            if (inserted.second) {
                try {
                    _record_admission_blocked.push_back (pipe);
                } catch (...) {
                    _record_admission_blocked_set.erase (inserted.first);
                    errno = ENOMEM;
                    return false;
                }
            }
        } catch (...) {
            errno = ENOMEM;
            return false;
        }
    }

    // A ROUTER transport envelope may already have supplied its synthetic
    // routing-id frame before admission examines the first payload frame.
    // The payload remains queued when admission is capacity-blocked, so the
    // next eligible source must start a fresh FQ record instead of inheriting
    // this source's multipart pin.
    _more = false;
    deactivate_at (_current);
    return true;
}

size_t zlink::fq_t::redrive_record_admission (size_t max_pipes_)
{
    size_t redriven = 0;
    while (redriven < max_pipes_ && !_record_admission_blocked.empty ()) {
        pipe_t *const pipe = _record_admission_blocked.front ();
        _record_admission_blocked.pop_front ();
        if (_record_admission_blocked_set.erase (pipe) == 0)
            continue;

        pipes_t::size_type index = 0;
        if (!try_get_pipe_index (pipe, &index))
            continue;
        if (!pipe->check_read ())
            continue;
        activated (pipe);
        ++redriven;
    }
    return redriven;
}

int zlink::fq_t::recv (msg_t *msg_)
{
    return recvpipe (msg_, NULL);
}

int zlink::fq_t::recvpipe (msg_t *msg_, pipe_t **pipe_)
{
    return recvpipe_internal<false> (msg_, pipe_, NULL, NULL);
}

int zlink::fq_t::recvpipe_with_record_admission (
  msg_t *msg_, pipe_t **pipe_, pipe_t::read_admission_fn *admission_,
  void *userdata_)
{
    return recvpipe_internal<true> (msg_, pipe_, admission_, userdata_);
}

template <bool WithAdmission>
int zlink::fq_t::recvpipe_internal (
  msg_t *msg_, pipe_t **pipe_, pipe_t::read_admission_fn *admission_,
  void *userdata_)
{
    normalize_state ();

    if (pipe_)
        *pipe_ = NULL;

    //  Deallocate old content of the message.
    int rc = msg_->close ();
    if (unlikely (rc != 0)) {
        rc = msg_->init ();
        errno_assert (rc == 0);
    }

    if (_multipart_abort_pending) {
        _multipart_abort_pending = false;
        rc = msg_->init ();
        errno_assert (rc == 0);
        // Keep this distinct from an ordinary empty queue. socket_base_t
        // retries EAGAIN after processing commands, which would let the retry
        // consume a frame from another pipe. The public receive boundary
        // translates this marker back to EAGAIN without retrying.
        errno = ECONNABORTED;
        return -1;
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
        bool admission_failed = false;
        bool admission_consumed = false;
        const bool fetched =
          WithAdmission
            ? current_pipe->read_with_record_admission (
                msg_, admission_, userdata_, &admission_failed,
                &admission_consumed)
            : current_pipe->read (msg_);

        if (WithAdmission && admission_failed) {
            const int saved_errno = errno;
            if (admission_consumed) {
                while (true) {
                    const bool more =
                      (msg_->flags () & msg_t::more) != 0;
                    rc = msg_->close ();
                    errno_assert (rc == 0);
                    rc = msg_->init ();
                    errno_assert (rc == 0);
                    if (!more)
                        break;
                    if (!current_pipe->read (msg_))
                        break;
                }
            } else {
                rc = msg_->init ();
                errno_assert (rc == 0);
            }
            if (!admission_consumed && saved_errno == EAGAIN) {
                if (block_current_for_record_admission ()) {
                    errno = 0;
                    continue;
                }
                // Preserve allocation failure from the paused-pipe registry;
                // the queued record itself was not consumed.
                return -1;
            }
            errno = saved_errno;
            return -1;
        }

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
            deactivate_current_after_read_miss ();
            rc = msg_->init ();
            errno_assert (rc == 0);
            // A false read while pinned can be the pipe delimiter being
            // consumed before pipe_terminated() reaches the socket. Keep it
            // distinct from an empty queue so socket_base_t cannot retry on a
            // different peer and splice two records together.
            errno = ECONNABORTED;
            return -1;
        }

        deactivate_current_after_read_miss ();
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

    if (_multipart_abort_pending)
        return true;

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

        deactivate_current_after_read_miss ();
    }

    return false;
}
