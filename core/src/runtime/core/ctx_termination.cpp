/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

#include <vector>

#include "core/ctx.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

void zlink::ctx_t::teardown_runtime ()
{
    _runtime_resources.teardown (*this, _socket_registry);
}

void zlink::ctx_t::flush_pending_inproc_locked ()
{
    std::vector<std::string> pending_addresses;
    const bool saved_terminating = _terminating;

    // Never nest the context slot mutex with the inproc registry mutex.  The
    // pending bind path has to take socket-local locks, while socket teardown
    // reaches this collector from the opposite direction.
    _slot_sync.unlock ();
    try {
        _inproc_registry.collect_pending_addresses (&pending_addresses);
    } catch (...) {
        _slot_sync.lock ();
        throw;
    }

    _slot_sync.lock ();
    _terminating = false;
    _slot_sync.unlock ();
    for (std::vector<std::string>::const_iterator it = pending_addresses.begin (),
                                                  end = pending_addresses.end ();
         it != end; ++it) {
        socket_base_t *socket = create_socket (ZLINK_CORE_SOCKET_PAIR);
        zlink_assert (socket);
        socket->bind (it->c_str ());
        socket->close ();
    }
    _slot_sync.lock ();
    _terminating = saved_terminating;
}

bool zlink::ctx_t::begin_shutdown_locked (bool allow_fork_cleanup_)
{
    if (_starting) {
        _terminating = true;
        return false;
    }

#ifdef HAVE_FORK
    if (allow_fork_cleanup_ && _pid != getpid ()) {
        std::vector<socket_base_t *> sockets;
        _socket_registry.collect_sockets (&sockets);
        for (std::vector<socket_base_t *>::size_type i = 0, size = sockets.size (); i != size; ++i)
            sockets[i]->get_mailbox ()->forked ();
        _term_mailbox.forked ();
    }
#else
    LIBZLINK_UNUSED (allow_fork_cleanup_);
#endif

    const bool restarted = _terminating;
    _terminating = true;
    if (restarted) {
        //  flush_pending_inproc_locked() temporarily clears _terminating while
        //  the slot lock is released. The last socket can disappear in that
        //  window without stopping the reaper, so re-arm it after the state is
        //  restored.
        if (_socket_registry.empty ())
            _runtime_resources.stop_reaper ();
        return true;
    }

    debug_dump_sockets_locked ("terminate-before-stop");
    std::vector<socket_base_t *> sockets;
    _socket_registry.collect_sockets (&sockets);
    // As in auto-HWM collection, retain socket lifetimes while the registry
    // proves membership, then drop the registry lock before entering any
    // socket. Monitor teardown may wait for its worker or process a peer's
    // mailbox, both of which may need this context registry.
    for (size_t i = 0; i < sockets.size (); ++i) {
        if (sockets[i] && !sockets[i]->try_inc_mailbox_ref ())
            sockets[i] = NULL;
    }
    _slot_sync.unlock ();
    try {
        //  Publish termination on every socket before any teardown work.
        //  stop_monitor() may wait for a monitor worker or process a peer's
        //  mailbox, and until this store lands a blocking send or receive
        //  keeps running as if the context were still alive - a receive
        //  draining buffered input can consume all of it in that window.
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (sockets[i])
                sockets[i]->publish_ctx_terminated ();
        }
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (sockets[i])
                sockets[i]->stop_monitor (false);
        }
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (sockets[i])
                sockets[i]->stop ();
        }
    } catch (...) {
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (sockets[i])
                sockets[i]->dec_mailbox_ref ();
        }
        _slot_sync.lock ();
        throw;
    }
    for (size_t i = 0; i < sockets.size (); ++i) {
        if (sockets[i])
            sockets[i]->dec_mailbox_ref ();
    }
    _slot_sync.lock ();
    if (_socket_registry.empty ())
        _runtime_resources.stop_reaper ();

    return true;
}

int zlink::ctx_t::wait_for_reaper_done ()
{
    command_t cmd;
    //  A blocking mailbox receive still reports EAGAIN for a wake that
    //  carried no command: the signaler and the command pipe are separate
    //  objects, so a stale or coalesced edge can outlive its command. That is
    //  a spurious wake, not a termination result - keep waiting for the one
    //  reaper acknowledgement this mailbox exists to deliver.
    while (true) {
        const int rc = _term_mailbox.recv (&cmd, -1);
        if (rc == 0)
            break;
        if (errno == EINTR)
            return -1;
        errno_assert (errno == EAGAIN);
    }
    zlink_assert (cmd.type == command_t::done);
    return 0;
}
