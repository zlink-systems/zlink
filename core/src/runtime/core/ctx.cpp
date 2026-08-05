/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "utils/debug_log.hpp"
#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

#include <limits>
#include <climits>
#include <new>
#include <stdio.h>
#include <string.h>

#include "core/ctx.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/io_thread.hpp"
#include "core/reaper.hpp"
#include "core/pipe.hpp"
#include "core/control_runtime.hpp"
#include "utils/err.hpp"
#include "utils/heap_owner.hpp"
#include "utils/random.hpp"

#define ZLINK_CTX_TAG_VALUE_GOOD 0xabadcafe
#define ZLINK_CTX_TAG_VALUE_BAD 0xdeadbeef

int zlink::ctx_t::clipped_maxsocket (int max_requested_)
{
    if (max_requested_ >= zlink::poller_t::max_fds () && zlink::poller_t::max_fds () != -1)
        // -1 because we need room for the reaper mailbox.
        max_requested_ = zlink::poller_t::max_fds () - 1;

    return max_requested_;
}

namespace
{
const char *socket_type_name (int type_)
{
    switch (type_) {
        case ZLINK_CORE_SOCKET_PAIR:
            return "PAIR";
        case ZLINK_CORE_SOCKET_PUB:
            return "PUB";
        case ZLINK_CORE_SOCKET_SUB:
            return "SUB";
        case ZLINK_CORE_SOCKET_DEALER:
            return "DEALER";
        case ZLINK_CORE_SOCKET_ROUTER:
            return "ROUTER";
        case ZLINK_CORE_SOCKET_STREAM:
            return "STREAM";
        case ZLINK_CORE_SOCKET_XPUB:
            return "XPUB";
        case ZLINK_CORE_SOCKET_XSUB:
            return "XSUB";
        default:
            return "UNKNOWN";
    }
}

}

zlink::ctx_t::ctx_t () :
    _tag (ZLINK_CTX_TAG_VALUE_GOOD),
    _starting (true),
    _terminating (false),
    _max_sockets (ctx_t::clipped_maxsocket (ZLINK_MAX_SOCKETS_DFLT)),
    _max_msgsz (INT_MAX),
    _io_thread_count (ZLINK_IO_THREADS_DFLT),
    _blocky (true),
    _ipv6 (false)
{
#ifdef HAVE_FORK
    _pid = getpid ();
#endif

    //  Initialise crypto library, if needed.
    zlink::random_open ();
}

bool zlink::ctx_t::check_tag () const
{
    return _tag == ZLINK_CTX_TAG_VALUE_GOOD;
}

zlink::ctx_t::~ctx_t ()
{
    //  Check that there are no remaining _sockets.
    zlink_assert (_socket_registry.empty ());
    stop_auto_hwm_recalc_task ();
    teardown_runtime ();

    //  De-initialise crypto library, if needed.
    zlink::random_close ();

    //  Remove the tag, so that the object is considered dead.
    _tag = ZLINK_CTX_TAG_VALUE_BAD;
}

bool zlink::ctx_t::valid () const
{
    return _term_mailbox.valid ();
}

zlink::control_runtime_t *zlink::ctx_t::control_runtime ()
{
    return ensure_control_runtime ();
}

void zlink::ctx_t::start_thread (thread_t &thread_,
                                 thread_fn *tfn_,
                                 void *arg_,
                                 const char *name_) const
{
    _thread_context.start_thread (thread_, tfn_, arg_, name_);
}

const zlink::thread_ctx_t &zlink::ctx_t::thread_context () const
{
    return _thread_context;
}

void zlink::ctx_t::debug_dump_sockets_locked (const char *phase_) const
{
    if (!debug_env_enabled ("ZLINK_CTX_DEBUG_SOCKETS"))
        return;

    std::vector<socket_base_t *> sockets;
    _socket_registry.collect_sockets (&sockets);
    fprintf (stderr, "[ctx] %s socket_count=%u\n", phase_ ? phase_ : "state",
             static_cast<unsigned> (sockets.size ()));
    for (std::vector<socket_base_t *>::size_type i = 0, size = sockets.size (); i != size; ++i) {
        const socket_base_t *socket = sockets[i];
        if (!socket)
            continue;

        fprintf (stderr, "[ctx]   socket[%u]=%p type=%s(%d) sid=%d\n", static_cast<unsigned> (i),
                 static_cast<const void *> (socket), socket_type_name (socket->socket_type ()),
                 socket->socket_type (), socket->socket_id ());
    }
    fflush (stderr);
}

int zlink::ctx_t::terminate ()
{
    _slot_sync.lock ();
    flush_pending_inproc_locked ();

    if (begin_shutdown_locked (true)) {
        _slot_sync.unlock ();
        if (wait_for_reaper_done () == -1)
            return -1;
        _slot_sync.lock ();
        zlink_assert (_socket_registry.empty ());
    }
    _slot_sync.unlock ();

    if (debug_env_enabled ("ZLINK_CTX_DEBUG_SOCKETS")) {
        std::fprintf (stderr, "[ctx] before-delete\n");
        std::fflush (stderr);
    }

    //  Context is API-created on heap; shutdown path owns final deletion once
    //  reaper confirms all sockets are gone.
    zlink::release_heap_owned (this);

    return 0;
}

int zlink::ctx_t::shutdown ()
{
    scoped_lock_t locker (_slot_sync);
    (void) begin_shutdown_locked (false);
    return 0;
}

bool zlink::ctx_t::start ()
{
    return start_runtime_locked ();
}

zlink::socket_base_t *zlink::ctx_t::create_socket (int type_)
{
    scoped_lock_t locker (_slot_sync);

    //  Once zlink_ctx_term() or zlink_ctx_shutdown() was called, we can't create
    //  new sockets.
    if (_terminating) {
        errno = ETERM;
        return NULL;
    }

    if (unlikely (_starting)) {
        if (!start ())
            return NULL;
    }

    //  If max_sockets limit was reached, return error.
    if (!_socket_registry.has_available_socket_slot ()) {
        errno = EMFILE;
        return NULL;
    }

    //  Choose a slot for the socket.
    const uint32_t slot = _socket_registry.claim_socket_slot ();

    //  Generate new unique socket ID.
    const int sid = (static_cast<int> (max_socket_id.add (1))) + 1;

    //  Create the socket and register its mailbox.
    socket_base_t *s = socket_base_t::create (type_, this, slot, sid);
    if (!s) {
        _socket_registry.release_unused_socket_slot (slot);
        return NULL;
    }
    _socket_registry.publish_socket (s);

    return s;
}

void zlink::ctx_t::destroy_socket (class socket_base_t *socket_)
{
    scoped_lock_t locker (_slot_sync);

    //  Free the associated thread slot.
    _socket_registry.remove_socket (socket_);
    debug_dump_sockets_locked ("destroy-socket");

    //  If zlink_ctx_term() was already called and there are no more socket
    //  we can ask reaper thread to terminate.
    if (_terminating && _socket_registry.empty ())
        _runtime_resources.stop_reaper ();
}

int zlink::ctx_t::wait_for_socket_removal (const socket_base_t *socket_, int timeout_ms_)
{
    if (!socket_)
        return 0;

    scoped_lock_t locker (_slot_sync);
    return _socket_registry.wait_for_socket_removal (&_slot_sync, socket_, timeout_ms_);
}

int zlink::ctx_t::close_socket_and_wait (socket_base_t *&socket_, int timeout_ms_)
{
    if (!socket_)
        return 0;

    socket_base_t *socket = socket_;
    socket->stop ();
    socket->close ();
    socket_ = NULL;
    return wait_for_socket_removal (socket, timeout_ms_);
}

size_t zlink::ctx_t::socket_count () const
{
    scoped_lock_t locker (const_cast<mutex_t &> (_slot_sync));
    return _socket_registry.socket_count ();
}

int zlink::ctx_t::wait_for_socket_count_at_most (size_t max_count_, int timeout_ms_)
{
    scoped_lock_t locker (_slot_sync);
    return _socket_registry.wait_for_socket_count_at_most (&_slot_sync, max_count_, timeout_ms_);
}

zlink::object_t *zlink::ctx_t::get_reaper () const
{
    return _runtime_resources.reaper_object ();
}

void zlink::ctx_t::send_command (uint32_t tid_, const command_t &command_)
{
    _socket_registry.mailbox (tid_)->send (command_);
    if (tid_ == term_tid)
        _term_mailbox.signal ();
}

zlink::io_thread_t *zlink::ctx_t::choose_io_thread (uint64_t affinity_)
{
    return _runtime_resources.choose_io_thread (affinity_);
}

zlink::io_thread_t *zlink::ctx_t::choose_io_thread_stream (uint64_t affinity_)
{
    return _runtime_resources.choose_io_thread_stream (affinity_);
}

int zlink::ctx_t::register_endpoint (const char *addr_, const endpoint_t &endpoint_)
{
    return _inproc_registry.register_endpoint (addr_, endpoint_);
}

int zlink::ctx_t::unregister_endpoint (const std::string &addr_, const socket_base_t *const socket_)
{
    return _inproc_registry.unregister_endpoint (addr_, socket_);
}

void zlink::ctx_t::unregister_endpoints (const socket_base_t *const socket_)
{
    _inproc_registry.unregister_endpoints (socket_);
}

zlink::endpoint_t zlink::ctx_t::find_endpoint (const char *addr_)
{
    return _inproc_registry.find_endpoint (addr_);
}

bool zlink::ctx_t::pend_connection (const std::string &addr_,
                                    const endpoint_t &endpoint_,
                                    pipe_t **pipes_)
{
    return _inproc_registry.pend_connection (addr_, endpoint_, pipes_);
}

void zlink::ctx_t::connect_pending (const char *addr_, zlink::socket_base_t *bind_socket_)
{
    _inproc_registry.connect_pending (addr_, bind_socket_);
}

int zlink::ctx_t::materialize_pending_inproc (const std::string &addr_,
                                              socket_base_t *connect_socket_)
{
    if (!_inproc_registry.has_pending_for_socket (addr_, connect_socket_))
        return 0;

    socket_base_t *bind_socket = create_socket (ZLINK_CORE_SOCKET_PAIR);
    if (!bind_socket)
        return -1;

    (void) _inproc_registry.materialize_pending_for_socket (addr_, connect_socket_, bind_socket);
    bind_socket->close ();
    return 0;
}

//  The last used socket ID, or 0 if no socket was used so far. Note that this
//  is a global variable. Thus, even sockets created in different contexts have
//  unique IDs.
zlink::atomic_counter_t zlink::ctx_t::max_socket_id;
