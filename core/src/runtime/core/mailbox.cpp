/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/mailbox.hpp"
#include "utils/err.hpp"
#include "core/signaler.hpp"
#include "utils/clock.hpp"

#include <boost/asio.hpp>
#include <algorithm>

zlink::mailbox_t::mailbox_t ()
{
    const bool ok = _cpipe.check_read ();
    zlink_assert (!ok);
    _active = false;
    _io_context = NULL;
    _handler = NULL;
    _handler_arg = NULL;
    _pre_post = NULL;
    _scheduled.store (false, std::memory_order_release);
    _command_pending_hint.store (false, std::memory_order_release);
    _command_wait_epoch = 0;
    _command_waiters = 0;
    _command_wait_observers = 0;
    _command_wait_signal_pending = false;
    _primary_signaler_required.store (false, std::memory_order_release);
}

zlink::mailbox_t::~mailbox_t ()
{
    //  Commands remaining in the cpipe are owned by the surrounding shutdown
    //  graph; mailbox teardown only waits for concurrent senders to leave.

    // Work around problem that other threads might still be in our
    // send() method, by waiting on the mutex before disappearing.
    _sync.lock ();
    _sync.unlock ();
}

zlink::fd_t zlink::mailbox_t::get_fd () const
{
    _primary_signaler_required.store (true, std::memory_order_release);
    return _signaler.get_fd ();
}

void zlink::mailbox_t::send (const command_t &cmd_)
{
    _sync.lock ();
    _cpipe.write (cmd_, false);
    const bool ok = _cpipe.flush ();
    //  A command can join an already-active receiver without producing a
    //  primary signal. Preserve that edge only while a slow-path owner is
    //  observing the drain/wait handoff; ordinary commands pay no epoch work.
    if (_command_wait_observers != 0 || _command_waiters != 0) {
        ++_command_wait_epoch;
        if (_command_waiters != 0)
            _command_wait_cv.broadcast ();
    }
    bool send_primary_signaler = true;
    if (!ok) {
        //  Publish the command before its wakeup. Commands appended while the
        //  receiver is active are consumed by that drain and need no hint.
        _command_pending_hint.store (true, std::memory_order_release);
        // Signal all registered signalers for ZLINK_INTERNAL_OPT_FD support
        for (std::vector<signaler_t *>::iterator it = _signalers.begin (), end = _signalers.end ();
             it != end; ++it) {
            (*it)->send ();
        }
        send_primary_signaler =
          _signalers.empty () || _primary_signaler_required.load (std::memory_order_acquire);
        if (send_primary_signaler)
            _signaler.send ();
        //  Stage 1 (plan 7.1): the scheduling decision needs the same mutex
        //  this critical section already holds, and send() runs once per
        //  command. Doing it here removes one mutex round trip per command
        //  instead of dropping and retaking _sync in schedule_if_needed().
        schedule_if_needed_unlocked ();
    }
    _sync.unlock ();
}

bool zlink::mailbox_t::take_command_pending_hint ()
{
    if (!_command_pending_hint.load (std::memory_order_acquire))
        return false;
    return _command_pending_hint.exchange (false, std::memory_order_acq_rel);
}

void zlink::mailbox_t::signal ()
{
    _sync.lock ();
    if (_command_wait_observers != 0 || _command_waiters != 0)
        ++_command_wait_epoch;
    if (_command_waiters != 0) {
        _command_wait_cv.broadcast ();
    } else if (_command_wait_observers == 0) {
        //  signal() also transfers command ownership without enqueueing a
        //  command. Preserve one such edge across waiter registration.
        _command_wait_signal_pending = true;
    }
    for (std::vector<signaler_t *>::iterator it = _signalers.begin (), end = _signalers.end ();
         it != end; ++it) {
        (*it)->send ();
    }
    if (_signalers.empty () || _primary_signaler_required.load (std::memory_order_acquire))
        _signaler.send ();
    _sync.unlock ();
}

int zlink::mailbox_t::recv (command_t *cmd_, int timeout_)
{
    //  An async command owner and a public socket poller can observe the same
    //  primary descriptor. Consult the command pipe first so a poller that
    //  consumed the wake cannot strand an already-published command.
    if (!_active && _cpipe.check_read ()) {
        (void) _signaler.recv_failable ();
        _active = true;
    }

    if (!_active) {
        signaler_t *shared_signaler = NULL;
        _sync.lock ();
        if (!_signalers.empty ()
            && !_primary_signaler_required.load (std::memory_order_acquire))
            shared_signaler = _signalers.front ();
        _sync.unlock ();

        if (shared_signaler) {
            const bool has_shared_command = _cpipe.check_read ();
            if (!has_shared_command) {
                if (timeout_ == 0) {
                    errno = EAGAIN;
                    return -1;
                }
                const int rc = shared_signaler->wait (timeout_);
                if (rc == -1) {
                    errno_assert (errno == EAGAIN || errno == EINTR);
                    return -1;
                }
                if (!_cpipe.check_read ()) {
                    errno = EAGAIN;
                    return -1;
                }
            }
        } else if (timeout_ == 0) {
            // Avoid poll syscall on non-blocking checks.
            const int rc = _signaler.recv_failable ();
            if (rc == -1) {
                errno_assert (errno == EAGAIN);
                return -1;
            }
        } else {
            const int rc = _signaler.wait (timeout_);
            if (rc == -1) {
                errno_assert (errno == EAGAIN || errno == EINTR);
                return -1;
            }
            const int recv_rc = _signaler.recv_failable ();
            if (recv_rc == -1) {
                errno_assert (errno == EAGAIN);
                return -1;
            }
        }
        _active = true;
    }

    const bool has_command = _cpipe.read (cmd_);
    if (has_command)
        return 0;

    _active = false;
    errno = EAGAIN;
    return -1;
}

zlink::mailbox_t::command_probe_result_t zlink::mailbox_t::probe_command (
  bool (*predicate_) (const command_t &))
{
    zlink_assert (predicate_);

    //  Mirror recv()'s nonblocking receiver-state transition, including the
    //  primary wake drain. A sender can only append after this probe, so a
    //  matched front remains the next command until the command owner pops it.
    if (!_active && _cpipe.check_read ()) {
        (void) _signaler.recv_failable ();
        _active = true;
    }
    if (!_active)
        return command_probe_empty;
    if (!_cpipe.check_read ()) {
        _active = false;
        return command_probe_empty;
    }

    return _cpipe.probe (predicate_) ? command_probe_match
                                     : command_probe_other;
}

uint64_t zlink::mailbox_t::begin_command_wait_observation ()
{
    _sync.lock ();
    ++_command_wait_observers;
    const uint64_t epoch = _command_wait_epoch;
    _sync.unlock ();
    return epoch;
}

void zlink::mailbox_t::end_command_wait_observation ()
{
    _sync.lock ();
    zlink_assert (_command_wait_observers != 0);
    --_command_wait_observers;
    _sync.unlock ();
}

int zlink::mailbox_t::wait_for_command_signal (
  int timeout_, const uint64_t *observed_epoch_)
{
    clock_t clock;
    const uint64_t deadline =
      timeout_ > 0 ? clock.now_ms () + static_cast<uint64_t> (timeout_) : 0;

    _sync.lock ();

    //  send() publishes this hint under the same mutex before deciding
    //  whether a registered waiter needs a CV wake. This closes the enqueue
    //  versus waiter-registration race without adding work to active sends.
    if ((observed_epoch_ && _command_wait_epoch != *observed_epoch_)
        || _command_pending_hint.load (std::memory_order_acquire)
        || _command_wait_signal_pending) {
        _command_wait_signal_pending = false;
        _sync.unlock ();
        return 0;
    }
    if (timeout_ == 0) {
        _sync.unlock ();
        errno = EAGAIN;
        return -1;
    }

    const uint64_t observed_epoch = _command_wait_epoch;
    ++_command_waiters;
    int rc = 0;
    while (_command_wait_epoch == observed_epoch
           && !_command_pending_hint.load (std::memory_order_acquire)
           && !_command_wait_signal_pending) {
        int wait_timeout = timeout_;
        if (timeout_ > 0) {
            const uint64_t now = clock.now_ms ();
            if (now >= deadline) {
                errno = EAGAIN;
                rc = -1;
                break;
            }
            wait_timeout = static_cast<int> (deadline - now);
        }
        rc = _command_wait_cv.wait (&_sync, wait_timeout);
        if (rc != 0)
            break;
    }
    --_command_waiters;
    if (rc == 0)
        _command_wait_signal_pending = false;
    _sync.unlock ();

    return rc;
}

#ifdef ZLINK_BUILD_TESTS
uint32_t zlink::mailbox_t::test_command_waiter_count ()
{
    _sync.lock ();
    const uint32_t waiters = _command_waiters;
    _sync.unlock ();
    return waiters;
}
#endif

void zlink::mailbox_t::drain_primary_signaler ()
{
    while (_signaler.recv_failable () == 0) {
    }
    errno_assert (errno == EAGAIN);
}

bool zlink::mailbox_t::valid () const
{
    return _signaler.valid ();
}

void zlink::mailbox_t::set_io_context (boost::asio::io_context *io_context_,
                                       mailbox_handler_t handler_,
                                       void *handler_arg_,
                                       mailbox_pre_post_t pre_post_)
{
    _sync.lock ();
    _io_context = io_context_;
    _handler = handler_;
    _handler_arg = handler_arg_;
    _pre_post = pre_post_;
    _sync.unlock ();
}

void zlink::mailbox_t::schedule_if_needed ()
{
    _sync.lock ();
    schedule_if_needed_unlocked ();
    _sync.unlock ();
}

void zlink::mailbox_t::schedule_if_needed_unlocked ()
{
    if (!_io_context || !_handler)
        return;

    if (!_scheduled.exchange (true, std::memory_order_acquire)) {
        boost::asio::io_context *io_context = _io_context;
        const mailbox_handler_t handler = _handler;
        void *const handler_arg = _handler_arg;
        const mailbox_pre_post_t pre_post = _pre_post;
        if (pre_post)
            pre_post (handler_arg);
        boost::asio::post (*io_context, [handler, handler_arg] () { handler (handler_arg); });
    }
}

bool zlink::mailbox_t::reschedule_if_needed ()
{
    _sync.lock ();
    _scheduled.store (false, std::memory_order_release);
    const bool has_data = _cpipe.check_read ();
    if (!has_data) {
        _sync.unlock ();
        return false;
    }
    _scheduled.store (true, std::memory_order_release);
    _sync.unlock ();
    return true;
}

bool zlink::mailbox_t::detach_io_context_if_idle ()
{
    _sync.lock ();
    _scheduled.store (false, std::memory_order_release);
    if (_cpipe.check_read ()) {
        _scheduled.store (true, std::memory_order_release);
        _sync.unlock ();
        return false;
    }

    _io_context = NULL;
    _handler = NULL;
    _handler_arg = NULL;
    _pre_post = NULL;
    _sync.unlock ();
    return true;
}

void zlink::mailbox_t::add_signaler (signaler_t *signaler_)
{
    _sync.lock ();
    _signalers.push_back (signaler_);
    _sync.unlock ();
}

void zlink::mailbox_t::remove_signaler (signaler_t *signaler_)
{
    _sync.lock ();
    const std::vector<signaler_t *>::iterator end = _signalers.end ();
    const std::vector<signaler_t *>::iterator it = std::find (_signalers.begin (), end, signaler_);
    if (it != end)
        _signalers.erase (it);
    _sync.unlock ();
}

void zlink::mailbox_t::rearm_primary_signaler ()
{
    if (!_primary_signaler_required.load (std::memory_order_acquire))
        return;
    _sync.lock ();
    //  This edge exists only for callers that obtained the primary fd. Avoid
    //  a signaler syscall for async-owned sockets that have no public poller.
    if (_primary_signaler_required.load (std::memory_order_acquire))
        _signaler.send ();
    _sync.unlock ();
}

void zlink::mailbox_t::signal_pollers ()
{
    _sync.lock ();
    for (std::vector<signaler_t *>::iterator it = _signalers.begin (), end = _signalers.end ();
         it != end; ++it) {
        (*it)->send ();
    }
    _sync.unlock ();
}

void zlink::mailbox_t::clear_signalers ()
{
    _sync.lock ();
    _signalers.clear ();
    _sync.unlock ();
}
