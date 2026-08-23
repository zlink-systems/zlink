/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "sockets/internal/lb.hpp"
#include "core/options.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"

#include <algorithm>

bool zlink::lb_t::candidate_order_t::operator() (const candidate_t &lhs_,
                                                 const candidate_t &rhs_) const
{
    //  Peer routing ID is the identifier the spec exposes. An empty routing ID
    //  is the empty byte string and therefore sorts before every non-empty one.
    const blob_t &lhs_routing_id = lhs_.pipe->get_routing_id ();
    const blob_t &rhs_routing_id = rhs_.pipe->get_routing_id ();
    if (lhs_routing_id < rhs_routing_id)
        return true;
    if (rhs_routing_id < lhs_routing_id)
        return false;

    //  Peers that share a routing ID (including peers without one) are ordered
    //  by the endpoint they were established through.
    const std::string &lhs_endpoint = lhs_.pipe->get_endpoint_pair ().identifier ();
    const std::string &rhs_endpoint = rhs_.pipe->get_endpoint_pair ().identifier ();
    if (lhs_endpoint != rhs_endpoint)
        return lhs_endpoint < rhs_endpoint;

    //  Last resort so the order stays total. Candidates that reach this point
    //  are indistinguishable to the application.
    return lhs_.entry->attach_seq < rhs_.entry->attach_seq;
}

zlink::lb_t::lb_t () :
    _active (0),
    _current (0),
    _more (false),
    _dropping (false),
    _attach_seq (0),
    _order_dirty (true),
    _weighted_multipart_pipe (NULL)
{
}

zlink::lb_t::~lb_t ()
{
    zlink_assert (_pipes.empty ());
}

void zlink::lb_t::attach (pipe_t *pipe_)
{
    _pipes.push_back (pipe_);
    pipe_entry_t entry;
    entry.weight = 100;
    entry.running_value = 0;
    entry.attach_seq = _attach_seq++;
    _entries[pipe_] = entry;
    mark_selection_dirty ();
    activated (pipe_);
}

void zlink::lb_t::pipe_terminated (pipe_t *pipe_)
{
    const pipes_t::size_type index = _pipes.index (pipe_);

    //  If we are in the middle of multipart message and current pipe
    //  have disconnected, we have to drop the remainder of the message.
    if (index == _current && _more)
        _dropping = true;
    if (pipe_ == _weighted_multipart_pipe && _more)
        _dropping = true;

    //  Remove the pipe from the list; adjust number of active pipes
    //  accordingly.
    if (index < _active) {
        _active--;
        _pipes.swap (index, _active);
        if (_current == _active)
            _current = 0;
        mark_selection_dirty ();
    }
    _pipes.erase (pipe_);
    //  A disconnect drops the running value together with the connection. A
    //  pipe that is only deactivated keeps its value and resumes from it. The
    //  pipes that remain keep theirs so the configured ratio survives.
    _entries.erase (pipe_);
    mark_selection_dirty ();
}

void zlink::lb_t::activated (pipe_t *pipe_)
{
    const entries_t::const_iterator entry_it = _entries.find (pipe_);
    if (entry_it != _entries.end () && entry_it->second.weight == 0)
        return;

    const pipes_t::size_type index = _pipes.index (pipe_);
    if (index < _active)
        return;

    //  Move the pipe to the list of active pipes.
    _pipes.swap (index, _active);
    _active++;
    mark_selection_dirty ();
}

void zlink::lb_t::set_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return;

    //  Internal defence only. The public setters reject out-of-range input
    //  before it reaches this point.
    if (weight_ > max_peer_weight)
        weight_ = max_peer_weight;

    entries_t::iterator it = _entries.find (pipe_);
    if (it == _entries.end ())
        return;
    if (it->second.weight == weight_)
        return;

    it->second.weight = weight_;
    mark_selection_dirty ();

    const pipes_t::size_type index = _pipes.index (pipe_);
    if (weight_ == 0) {
        if (index == _current && _more)
            _dropping = true;
        if (pipe_ == _weighted_multipart_pipe && _more)
            _dropping = true;

        if (index < _active) {
            _active--;
            _pipes.swap (index, _active);
            if (_current == _active)
                _current = 0;
            else if (_current > index && _current <= _active)
                --_current;
            mark_selection_dirty ();
        }
        return;
    }

    if (index >= _active && pipe_->check_write ()) {
        _pipes.swap (index, _active);
        _active++;
        mark_selection_dirty ();
    }
}

uint32_t zlink::lb_t::weight (pipe_t *pipe_) const
{
    const entries_t::const_iterator it = _entries.find (pipe_);
    return it != _entries.end () ? it->second.weight : 0;
}

bool zlink::lb_t::has_positive_weight_pipe () const
{
    for (entries_t::const_iterator it = _entries.begin (); it != _entries.end (); ++it) {
        if (it->second.weight > 0)
            return true;
    }
    return false;
}

bool zlink::lb_t::contains (pipe_t *pipe_) const
{
    if (!pipe_)
        return false;

    for (pipes_t::size_type i = 0; i < _pipes.size (); ++i) {
        if (_pipes[i] == pipe_)
            return true;
    }
    return false;
}

int zlink::lb_t::select_connected_pipe (pipe_t **pipe_out_,
                                        connected_pipe_filter_fn filter_,
                                        void *filter_userdata_)
{
    if (!pipe_out_) {
        errno = EFAULT;
        return -1;
    }
    *pipe_out_ = NULL;

    std::vector<candidate_t> connected;
    bool has_positive_weight = false;
    connected.reserve (_pipes.size ());
    for (pipes_t::size_type i = 0; i < _pipes.size (); ++i) {
        entries_t::iterator entry = _entries.find (_pipes[i]);
        if (entry == _entries.end () || entry->second.weight == 0)
            continue;
        has_positive_weight = true;
        if (filter_ && !filter_ (_pipes[i], filter_userdata_))
            continue;
        connected.push_back (candidate_t (_pipes[i], &entry->second));
    }
    if (connected.empty ()) {
        errno = has_positive_weight || _pipes.empty () ? ENOTCONN
                                                       : ECONNREFUSED;
        return -1;
    }

    candidate_t *selected = NULL;
    int64_t selected_value = 0;
    uint32_t total_weight = 0;
    for (std::vector<candidate_t>::iterator it = connected.begin ();
         it != connected.end (); ++it) {
        total_weight += it->entry->weight;
        const int64_t value =
          it->entry->running_value + static_cast<int64_t> (it->entry->weight);
        if (!selected || value > selected_value
            || (value == selected_value
                && candidate_order_t () (*it, *selected))) {
            selected = &(*it);
            selected_value = value;
        }
    }

    for (std::vector<candidate_t>::iterator it = connected.begin ();
         it != connected.end (); ++it)
        it->entry->running_value += static_cast<int64_t> (it->entry->weight);
    selected->entry->running_value -= static_cast<int64_t> (total_weight);
    *pipe_out_ = selected->pipe;
    return 0;
}

zlink::pipe_t *zlink::lb_t::find_connected_pipe (
  const unsigned char *peer_rid_, size_t peer_rid_size_,
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    if (!peer_rid_ || peer_rid_size_ == 0 || transport_pair_id_ == 0
        || transport_pair_generation_ == 0)
        return NULL;

    for (pipes_t::size_type i = 0; i < _pipes.size (); ++i) {
        pipe_t *pipe = _pipes[i];
        if (!pipe || pipe->get_transport_pair_id () != transport_pair_id_
            || pipe->get_transport_pair_generation ()
                 != transport_pair_generation_)
            continue;
        const blob_t &routing_id = pipe->get_routing_id ();
        if (routing_id.size () == peer_rid_size_
            && memcmp (routing_id.data (), peer_rid_, peer_rid_size_) == 0)
            return pipe;
    }
    return NULL;
}

int zlink::lb_t::sendpipe_to (
  pipe_t *pipe_, msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (!pipe_ || !msg_ || !msg_->check ()) {
        errno = EFAULT;
        return -1;
    }
    entries_t::iterator entry = _entries.find (pipe_);
    if (entry == _entries.end ()) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (entry->second.weight == 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    if (_more) {
        errno = EFSM;
        return -1;
    }

    const pipe_message_admission_t initial_admission =
      pipe_->check_write_admission ();
    if (initial_admission != pipe_message_admission_ready) {
        deactivate (pipe_);
        if (admission_out_)
            *admission_out_ = initial_admission;
        errno = initial_admission == pipe_message_admission_hwm_full
                    || initial_admission == pipe_message_admission_transport_wait
                  ? EAGAIN
                  : EHOSTUNREACH;
        return -1;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    pipe_message_admission_t write_admission =
      pipe_message_admission_invalid;
    const bool ok = more ? pipe_->write (msg_, &write_admission)
                         : pipe_->write_single_message_and_flush_no_recursive_hwm_check (
                             msg_, &write_admission);
    if (!ok) {
        if (admission_out_)
            *admission_out_ = write_admission;
        if (write_admission != pipe_message_admission_too_large) {
            deactivate (pipe_);
            errno = write_admission == pipe_message_admission_hwm_full
                        || write_admission
                             == pipe_message_admission_transport_wait
                      ? EAGAIN
                      : EHOSTUNREACH;
        }
        return -1;
    }

    _more = more;
    _weighted_multipart_pipe = more ? pipe_ : NULL;
    const int rc = msg_->init ();
    errno_assert (rc == 0);
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    return 0;
}

void zlink::lb_t::deactivate (pipe_t *pipe_)
{
    const pipes_t::size_type index = _pipes.index (pipe_);
    if (index >= _active)
        return;

    _active--;
    _pipes.swap (index, _active);
    if (_current == _active)
        _current = 0;
    else if (_current > index && _current <= _active)
        --_current;
    mark_selection_dirty ();
}

void zlink::lb_t::mark_selection_dirty ()
{
    _order_dirty = true;
}

void zlink::lb_t::rebuild_selection_order ()
{
    //  The size guard keeps a missed dirty mark from selecting a pipe that is
    //  no longer active.
    if (!_order_dirty && _ordered.size () == _active)
        return;

    _ordered.clear ();
    _ordered.reserve (_active);
    for (pipes_t::size_type i = 0; i < _active; ++i) {
        const entries_t::iterator entry_it = _entries.find (_pipes[i]);
        if (entry_it == _entries.end ())
            continue;
        _ordered.push_back (candidate_t (_pipes[i], &entry_it->second));
    }
    std::sort (_ordered.begin (), _ordered.end (), candidate_order_t ());
    _order_dirty = false;
}

const zlink::lb_t::candidate_t *zlink::lb_t::select_weighted_pipe (uint32_t *total_weight_out_)
{
    uint32_t total_weight = 0;
    const candidate_t *selected = NULL;
    int64_t selected_value = 0;

    //  _ordered is sorted ascending by identifier and the comparison below is
    //  strict, so the first candidate holding the largest value wins the tie.
    for (std::vector<candidate_t>::const_iterator it = _ordered.begin (); it != _ordered.end ();
         ++it) {
        if (it->entry->weight == 0)
            continue;

        total_weight += it->entry->weight;
        const int64_t value = it->entry->running_value + static_cast<int64_t> (it->entry->weight);
        if (!selected || value > selected_value) {
            selected = &(*it);
            selected_value = value;
        }
    }

    if (total_weight_out_)
        *total_weight_out_ = total_weight;
    return selected;
}

void zlink::lb_t::commit_weighted_selection (pipe_entry_t *selected_, uint32_t total_weight_)
{
    for (std::vector<candidate_t>::const_iterator it = _ordered.begin (); it != _ordered.end ();
         ++it) {
        if (it->entry->weight == 0)
            continue;
        it->entry->running_value += static_cast<int64_t> (it->entry->weight);
    }

    if (selected_)
        selected_->running_value -= static_cast<int64_t> (total_weight_);
}

int zlink::lb_t::send (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    return sendpipe (msg_, NULL, admission_out_);
}

int zlink::lb_t::sendpipe (
  msg_t *msg_, pipe_t **pipe_,
  pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    //  Drop the message if required. If we are at the end of the message
    //  switch back to non-dropping mode.
    if (_dropping) {
        _more = (msg_->flags () & msg_t::more) != 0;
        _dropping = _more;

        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    if (_more && _weighted_multipart_pipe) {
        const bool more = (msg_->flags () & msg_t::more) != 0;
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok =
          more ? _weighted_multipart_pipe->write (msg_, &write_admission)
               : _weighted_multipart_pipe
                   ->write_single_message_and_flush_no_recursive_hwm_check (
                     msg_, &write_admission);
        if (!ok) {
            if (admission_out_)
                *admission_out_ = write_admission;
            _weighted_multipart_pipe->rollback ();
            _weighted_multipart_pipe = NULL;
            _dropping = more;
            _more = false;
            if (write_admission != pipe_message_admission_too_large)
                errno = EAGAIN;
            return -2;
        }
        if (pipe_)
            *pipe_ = _weighted_multipart_pipe;
        _more = more;
        if (!_more)
            _weighted_multipart_pipe = NULL;
        const int rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    // Hot path: DEALER_DEALER single benchmarks run with one active pipe.
    // Keep the one-pipe steady-state out of the general load-balancing loop.
    if (_active == 1 && _current == 0) {
        pipe_t *pipe = _pipes[0];
        const bool more = (msg_->flags () & msg_t::more) != 0;
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok = more ? pipe->write (msg_, &write_admission)
                             : pipe->write_single_message_and_flush_no_recursive_hwm_check (
                                 msg_, &write_admission);
        if (!ok) {
            if (admission_out_)
                *admission_out_ = write_admission;
            if (_more) {
                pipe->rollback ();
                _weighted_multipart_pipe = NULL;
                _dropping = more;
                _more = false;
                if (write_admission != pipe_message_admission_too_large)
                    errno = EAGAIN;
                return -2;
            }
            if (write_admission != pipe_message_admission_too_large) {
                _active = 0;
                mark_selection_dirty ();
                errno = EAGAIN;
            }
            return -1;
        }

        if (pipe_)
            *pipe_ = pipe;

        _more = more;

        const int rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    rebuild_selection_order ();

    //  Every first frame runs the same selection procedure. Equal weights are
    //  not a special case: the procedure alternates on its own.
    if (!_more) {
        while (_active > 0) {
            uint32_t total_weight = 0;
            const candidate_t *candidate = select_weighted_pipe (&total_weight);
            if (!candidate)
                break;
            pipe_t *pipe = candidate->pipe;
            pipe_entry_t *entry = candidate->entry;

            const bool more = (msg_->flags () & msg_t::more) != 0;
            pipe_message_admission_t write_admission =
              pipe_message_admission_invalid;
            const bool ok = more ? pipe->write (msg_, &write_admission)
                                 : pipe->write_single_message_and_flush_no_recursive_hwm_check (
                                     msg_, &write_admission);
            if (ok) {
                //  Running values move only for a write that happened, so a
                //  retried selection never applies the same step twice.
                commit_weighted_selection (entry, total_weight);
                if (pipe_)
                    *pipe_ = pipe;
                _more = more;
                _weighted_multipart_pipe = more ? pipe : NULL;
                const int rc = msg_->init ();
                errno_assert (rc == 0);
                return 0;
            }

            //  An oversized message is rejected by every candidate, so trying
            //  the next one would only drop healthy pipes.
            if (write_admission == pipe_message_admission_too_large)
            {
                if (admission_out_)
                    *admission_out_ = pipe_message_admission_too_large;
                return -1;
            }

            if (admission_out_
                && write_admission == pipe_message_admission_hwm_full)
                *admission_out_ = pipe_message_admission_hwm_full;

            // A failed write changes current writability, not the peer's
            // advertised routing policy. Preserve the configured weight so
            // write activation can restore this pipe.
            deactivate (pipe);
            rebuild_selection_order ();
        }

        if (admission_out_ && any_hwm_blocked_pipe ())
            *admission_out_ = pipe_message_admission_hwm_full;
        errno = has_positive_weight_pipe () ? EAGAIN : ECONNREFUSED;
        return -1;
    }

    while (_active > 0) {
        const bool more = (msg_->flags () & msg_t::more) != 0;
        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok = more
                          ? _pipes[_current]->write (msg_, &write_admission)
                          : _pipes[_current]->write_and_flush (
                              msg_, &write_admission);
        if (ok) {
            if (pipe_)
                *pipe_ = _pipes[_current];
            break;
        }

        // If send fails for multi-part msg rollback other
        // parts sent earlier and return EAGAIN.
        // Application should handle this as suitable
        if (_more) {
            if (admission_out_)
                *admission_out_ = write_admission;
            _pipes[_current]->rollback ();
            // At this point the pipe is already being deallocated and the
            // frames written earlier cannot be recovered. Enter dropping mode
            // for the remaining frames so the next logical message starts with
            // a clean multipart boundary. -2/EAGAIN tells socket_base not to
            // retry this frame immediately in blocking mode.
            _dropping = (msg_->flags () & msg_t::more) != 0;
            _more = false;
            if (write_admission != pipe_message_admission_too_large)
                errno = EAGAIN;
            return -2;
        }

        if (write_admission == pipe_message_admission_too_large) {
            if (admission_out_)
                *admission_out_ = pipe_message_admission_too_large;
            return -1;
        }

        if (admission_out_
            && write_admission == pipe_message_admission_hwm_full)
            *admission_out_ = pipe_message_admission_hwm_full;

        _active--;
        if (_current < _active)
            _pipes.swap (_current, _active);
        else
            _current = 0;
        mark_selection_dirty ();
    }

    //  If there are no pipes we cannot send the message.
    if (_active == 0) {
        if (admission_out_ && any_hwm_blocked_pipe ())
            *admission_out_ = pipe_message_admission_hwm_full;
        errno = has_positive_weight_pipe () ? EAGAIN : ECONNREFUSED;
        return -1;
    }

    //  If it's final part of the message we can flush it downstream and
    //  continue round-robining (load balance).
    _more = (msg_->flags () & msg_t::more) != 0;
    if (!_more) {
        if (++_current >= _active)
            _current = 0;
    }

    //  Detach the message from the data buffer.
    const int rc = msg_->init ();
    errno_assert (rc == 0);

    return 0;
}

bool zlink::lb_t::any_hwm_blocked_pipe ()
{
    for (pipes_t::size_type i = 0; i < _pipes.size (); ++i) {
        const entries_t::const_iterator entry = _entries.find (_pipes[i]);
        if (entry != _entries.end () && entry->second.weight > 0
            && _pipes[i]->check_write_admission ()
                 == pipe_message_admission_hwm_full)
            return true;
    }
    return false;
}

void zlink::lb_t::rollback ()
{
    if (_weighted_multipart_pipe)
        _weighted_multipart_pipe->rollback ();
    else if (_more && _current < _pipes.size ())
        _pipes[_current]->rollback ();

    _more = false;
    _dropping = false;
    _weighted_multipart_pipe = NULL;
}

bool zlink::lb_t::has_out ()
{
    //  If one part of the message was already written we can definitely
    //  write the rest of the message.
    if (_more)
        return true;

    if (_active == 1 && _current == 0) {
        if (_pipes[0]->check_write ())
            return true;

        _active = 0;
        mark_selection_dirty ();
        return false;
    }

    while (_active > 0) {
        //  Check whether a pipe has room for another message.
        if (_pipes[_current]->check_write ())
            return true;

        //  Deactivate the pipe.
        _active--;
        _pipes.swap (_current, _active);
        mark_selection_dirty ();
        if (_current == _active)
            _current = 0;
    }

    return false;
}
