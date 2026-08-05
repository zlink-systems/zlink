/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "sockets/internal/dist.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"
#include "utils/likely.hpp"

zlink::dist_t::dist_t () :
    _matching (0),
    _active (0),
    _eligible (0),
    _more (false),
    _matching_hwm_cache_valid (false),
    _matching_hwm_admission (pipe_message_admission_ready)
{
}

zlink::dist_t::~dist_t ()
{
    zlink_assert (_pipes.empty ());
}

void zlink::dist_t::attach (pipe_t *pipe_)
{
    //  If we are in the middle of sending a message, we'll add new pipe
    //  into the list of eligible pipes. Otherwise we add it to the list
    //  of active pipes.
    if (_more) {
        _pipes.push_back (pipe_);
        _pipes.swap (_eligible, _pipes.size () - 1);
        _eligible++;
    } else {
        _pipes.push_back (pipe_);
        _pipes.swap (_active, _pipes.size () - 1);
        _active++;
        _eligible++;
    }
    _matching_hwm_cache_valid = false;
}

bool zlink::dist_t::has_pipe (pipe_t *pipe_)
{
    std::size_t claimed_index = _pipes.index (pipe_);

    // If pipe claims to be outside the available index space it can't be in the distributor.
    if (claimed_index >= _pipes.size ()) {
        return false;
    }

    return _pipes[claimed_index] == pipe_;
}

void zlink::dist_t::match (pipe_t *pipe_)
{
    //  If pipe is already matching do nothing.
    if (_pipes.index (pipe_) < _matching)
        return;

    //  If the pipe isn't eligible, ignore it.
    if (_pipes.index (pipe_) >= _eligible)
        return;

    //  Mark the pipe as matching.
    _pipes.swap (_pipes.index (pipe_), _matching);
    _matching++;
    _matching_hwm_cache_valid = false;
}

void zlink::dist_t::reverse_match ()
{
    const pipes_t::size_type prev_matching = _matching;

    // Reset matching to 0
    unmatch ();

    // Mark all matching pipes as not matching and vice-versa.
    // To do this, push all pipes that are eligible but not
    // matched - i.e. between "matching" and "eligible" -
    // to the beginning of the queue.
    for (pipes_t::size_type i = prev_matching; i < _eligible; ++i) {
        _pipes.swap (i, _matching++);
    }
    _matching_hwm_cache_valid = false;
}

void zlink::dist_t::unmatch ()
{
    _matching = 0;
    _matching_hwm_cache_valid = false;
}

void zlink::dist_t::pipe_terminated (pipe_t *pipe_)
{
    //  Remove the pipe from the list; adjust number of matching, active and/or
    //  eligible pipes accordingly.
    if (_pipes.index (pipe_) < _matching) {
        _pipes.swap (_pipes.index (pipe_), _matching - 1);
        _matching--;
    }
    if (_pipes.index (pipe_) < _active) {
        _pipes.swap (_pipes.index (pipe_), _active - 1);
        _active--;
    }
    if (_pipes.index (pipe_) < _eligible) {
        _pipes.swap (_pipes.index (pipe_), _eligible - 1);
        _eligible--;
    }

    _pipes.erase (pipe_);
    _matching_hwm_cache_valid = false;
}

void zlink::dist_t::activated (pipe_t *pipe_)
{
    //  Move the pipe from passive to eligible state.
    if (_eligible < _pipes.size ()) {
        _pipes.swap (_pipes.index (pipe_), _eligible);
        _eligible++;
    }

    //  If there's no message being sent at the moment, move it to
    //  the active state.
    if (!_more && _active < _pipes.size ()) {
        _pipes.swap (_eligible - 1, _active);
        _active++;
    }
    _matching_hwm_cache_valid = false;
}

int zlink::dist_t::send_to_all (msg_t *msg_)
{
    _matching = _active;
    _matching_hwm_cache_valid = false;
    return send_to_matching (msg_);
}

int zlink::dist_t::send_to_matching (msg_t *msg_)
{
    //  Is this end of a multipart message?
    const bool msg_more = (msg_->flags () & msg_t::more) != 0;

    //  Push the message to matching pipes.
    distribute (msg_);

    //  If multipart message is fully sent, activate all the eligible pipes.
    if (!msg_more)
        _active = _eligible;

    _more = msg_more;

    return 0;
}

void zlink::dist_t::distribute (msg_t *msg_)
{
    //  If there are no matching pipes available, simply drop the message.
    if (_matching == 0) {
        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return;
    }

    // Hot path: PUB/XPUB single-subscriber steady state keeps exactly one
    // matching pipe. Avoid the generic distributor loop, refcount churn, and
    // repeated pipe index lookups in that narrow case.
    if (_matching == 1 && _active == 1 && _eligible == 1) {
        (void) write_at (0, msg_);
        const int rc = msg_->init ();
        errno_assert (rc == 0);
        return;
    }

    if (msg_->is_vsm ()) {
        for (pipes_t::size_type i = 0; i < _matching;) {
            if (!write_at (i, msg_)) {
                //  Use same index again because entry will have been removed.
            } else {
                ++i;
            }
        }
        int rc = msg_->init ();
        errno_assert (rc == 0);
        return;
    }

    //  Add matching-1 references to the message. We already hold one reference,
    //  that's why -1.
    msg_->add_refs (static_cast<int> (_matching) - 1);

    //  Push copy of the message to each matching pipe.
    int failed = 0;
    for (pipes_t::size_type i = 0; i < _matching;) {
        if (!write_at (i, msg_)) {
            ++failed;
            //  Use same index again because entry will have been removed.
        } else {
            ++i;
        }
    }
    if (unlikely (failed))
        msg_->rm_refs (failed);

    //  Detach the original message from the data buffer. Note that we don't
    //  close the message. That's because we've already used all the references.
    const int rc = msg_->init ();
    errno_assert (rc == 0);
}

bool zlink::dist_t::has_out ()
{
    return true;
}

void zlink::dist_t::deactivate_matching_pipe (pipes_t::size_type index_)
{
    zlink_assert (index_ < _matching);
    _pipes.swap (index_, _matching - 1);
    _matching--;
    _pipes.swap (_matching, _active - 1);
    _active--;
    _pipes.swap (_active, _eligible - 1);
    _eligible--;
    _matching_hwm_cache_valid = false;
}

bool zlink::dist_t::write_at (pipes_t::size_type index_, msg_t *msg_)
{
    pipe_t *pipe = _pipes[index_];
    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool ok = more ? pipe->write_no_recursive_hwm_check (msg_)
                         : pipe->write_and_flush_no_recursive_hwm_check (msg_);
    if (!ok) {
        if (_more)
            pipe->rollback ();
        deactivate_matching_pipe (index_);
        return false;
    }
    return true;
}

zlink::pipe_message_admission_t zlink::dist_t::check_hwm (const msg_t *msg_)
{
    if (!msg_ && _matching_hwm_cache_valid)
        return _matching_hwm_admission;

    pipe_message_admission_t result = pipe_message_admission_ready;
    for (pipes_t::size_type i = 0; i < _matching; ++i) {
        const pipe_message_admission_t current =
          msg_ ? _pipes[i]->check_hwm_for_message (msg_)
               : (_pipes[i]->check_hwm () ? pipe_message_admission_ready
                                          : pipe_message_admission_hwm_full);
        if (current == pipe_message_admission_too_large
            || current == pipe_message_admission_invalid)
            result = current;
        else if (result == pipe_message_admission_ready
                 && current != pipe_message_admission_ready)
            result = current;
    }

    if (!msg_) {
        _matching_hwm_cache_valid = true;
        _matching_hwm_admission = result;
    }
    return result;
}

void zlink::dist_t::rollback ()
{
    for (pipes_t::size_type i = 0; i < _matching; ++i)
        _pipes[i]->rollback ();

    _matching = 0;
    _active = _eligible;
    _more = false;
    _matching_hwm_cache_valid = false;
}
