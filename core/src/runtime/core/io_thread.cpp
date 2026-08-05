/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <new>

#include "utils/macros.hpp"
#include "core/io_thread.hpp"
#include "utils/err.hpp"
#include "core/ctx.hpp"

zlink::io_thread_t::io_thread_t (ctx_t *ctx_, uint32_t tid_) : object_t (ctx_, tid_)
{
    _poller = new (std::nothrow) poller_t (ctx_->thread_context ());
    alloc_assert (_poller);
    _mailbox.set_io_context (&_poller->get_io_context (), &io_thread_t::mailbox_handler, this,
                             NULL);
    _mailbox.schedule_if_needed ();
}

zlink::io_thread_t::~io_thread_t ()
{
    LIBZLINK_DELETE (_poller);
}

void zlink::io_thread_t::start ()
{
    char name[16] = "";
    snprintf (name, sizeof (name), "IO/%u", get_tid () - zlink::ctx_t::reaper_tid - 1);
    //  Start the underlying I/O thread.
    _poller->start (name);
}

void zlink::io_thread_t::stop ()
{
    send_stop ();
}

zlink::mailbox_t *zlink::io_thread_t::get_mailbox ()
{
    return &_mailbox;
}

int zlink::io_thread_t::get_load () const
{
    return _poller->get_load ();
}

void zlink::io_thread_t::in_event ()
{
    process_mailbox ();
}

void zlink::io_thread_t::process_mailbox ()
{
    //  Drain all currently queued commands so teardown and ownership handoff
    //  progress cannot be stranded behind an arbitrary per-tick limit.

    do {
        command_t cmd;
        int rc = _mailbox.recv (&cmd, 0);

        while (rc == 0 || errno == EINTR) {
            if (rc == 0)
                cmd.destination->process_command (cmd);
            rc = _mailbox.recv (&cmd, 0);
        }

        errno_assert (rc != 0 && errno == EAGAIN);
    } while (_mailbox.reschedule_if_needed ());
}

void zlink::io_thread_t::out_event ()
{
    //  We are never polling for POLLOUT here. This function is never called.
    zlink_assert (false);
}

void zlink::io_thread_t::timer_event (int)
{
    //  No timers here. This function is never called.
    zlink_assert (false);
}

zlink::poller_t *zlink::io_thread_t::get_poller () const
{
    zlink_assert (_poller);
    return _poller;
}

boost::asio::io_context &zlink::io_thread_t::get_io_context () const
{
    zlink_assert (_poller);
    return _poller->get_io_context ();
}

void zlink::io_thread_t::process_stop ()
{
    _poller->stop ();
}

void zlink::io_thread_t::mailbox_handler (void *arg_)
{
    io_thread_t *self = static_cast<io_thread_t *> (arg_);
    self->process_mailbox ();
}
