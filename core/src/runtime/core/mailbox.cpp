/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/mailbox.hpp"
#include "utils/err.hpp"
#include "core/signaler.hpp"

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
    return _signaler.get_fd ();
}

void zlink::mailbox_t::send (const command_t &cmd_)
{
    _sync.lock ();
    _cpipe.write (cmd_, false);
    const bool ok = _cpipe.flush ();
    if (!ok) {
        // Signal all registered signalers for ZLINK_INTERNAL_OPT_FD support
        for (std::vector<signaler_t *>::iterator it = _signalers.begin (), end = _signalers.end ();
             it != end; ++it) {
            (*it)->send ();
        }
    }
    _sync.unlock ();

    if (!ok) {
        signal ();
        schedule_if_needed ();
    }
}

void zlink::mailbox_t::signal ()
{
    _signaler.send ();
}

int zlink::mailbox_t::recv (command_t *cmd_, int timeout_)
{
    if (!_active) {
        if (timeout_ == 0) {
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

    _sync.lock ();
    const bool has_command = _cpipe.read (cmd_);
    _sync.unlock ();
    if (has_command)
        return 0;

    _active = false;
    errno = EAGAIN;
    return -1;
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
    if (!_io_context || !_handler) {
        _sync.unlock ();
        return;
    }

    if (!_scheduled.exchange (true, std::memory_order_acquire)) {
        boost::asio::io_context *io_context = _io_context;
        const mailbox_handler_t handler = _handler;
        void *const handler_arg = _handler_arg;
        const mailbox_pre_post_t pre_post = _pre_post;
        if (pre_post)
            pre_post (handler_arg);
        boost::asio::post (*io_context, [handler, handler_arg] () { handler (handler_arg); });
    }
    _sync.unlock ();
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
    _signalers.push_back (signaler_);
}

void zlink::mailbox_t::remove_signaler (signaler_t *signaler_)
{
    const std::vector<signaler_t *>::iterator end = _signalers.end ();
    const std::vector<signaler_t *>::iterator it = std::find (_signalers.begin (), end, signaler_);
    if (it != end)
        _signalers.erase (it);
}

void zlink::mailbox_t::clear_signalers ()
{
    _signalers.clear ();
}
