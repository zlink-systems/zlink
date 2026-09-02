/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include "transports/ipc/asio_ipc_connecter.hpp"
#include "engine/asio/asio_poller.hpp"
#include "engine/asio/asio_raw_engine.hpp"
#include "engine/asio/asio_zmp_engine.hpp"
#include "transports/ipc/ipc_transport.hpp"
#include "transports/asio/asio_reconnect_interval.hpp"
#include "transports/asio/asio_socket_lifecycle.hpp"
#include "transports/asio/asio_timer_flag.hpp"
#include "core/address.hpp"
#include "utils/err.hpp"
#include "core/io_thread.hpp"
#include "transports/ipc/ipc_address.hpp"
#include "utils/ip.hpp"
#include "utils/random.hpp"
#include "core/session_base.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <sys/un.h>
#include <stddef.h>
#endif

#include <string.h>

#include <limits>
#include <cerrno>
#include <memory>

//  Debug logging for ASIO IPC connecter - set to 1 to enable
#define ASIO_IPC_CONNECTER_DEBUG 0

#if ASIO_IPC_CONNECTER_DEBUG
#include <cstdio>
#define IPC_CONNECTER_DBG(fmt, ...)                                                                \
    fprintf (stderr, "[ASIO_IPC_CONNECTER] " fmt "\n", ##__VA_ARGS__)
#else
#define IPC_CONNECTER_DBG(fmt, ...)
#endif

namespace
{
boost::asio::local::stream_protocol::endpoint make_ipc_endpoint (const zlink::ipc_address_t &addr_)
{
    boost::asio::local::stream_protocol::endpoint endpoint;
    memcpy (endpoint.data (), addr_.addr (), addr_.addrlen ());
    endpoint.resize (addr_.addrlen ());
    return endpoint;
}

int connect_delayed_errno_value ()
{
#ifdef ZLINK_HAVE_WINDOWS
#ifdef WSAEWOULDBLOCK
    return WSAEWOULDBLOCK;
#elif defined(WSAEINPROGRESS)
    return WSAEINPROGRESS;
#else
    return EINPROGRESS;
#endif
#else
    return EINPROGRESS;
#endif
}
}

zlink::asio_ipc_connecter_t::asio_ipc_connecter_t (io_thread_t *io_thread_,
                                                   session_base_t *session_,
                                                   const options_t &options_,
                                                   address_t *addr_,
                                                   bool delayed_start_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    _io_context (io_thread_->get_io_context ()),
    _socket (_io_context),
    _addr (addr_),
    _session (session_),
    _socket_ptr (session_->get_socket ()),
    _delayed_start (delayed_start_),
    _reconnect_timer_started (false),
    _connect_timer_started (false),
    _connecting (false),
    _terminating (false),
    _linger (0),
    _current_reconnect_ivl (-1)
{
    zlink_assert (_addr);
    zlink_assert (_addr->protocol == protocol_name::ipc);
    _addr->to_string (_endpoint_str);

    IPC_CONNECTER_DBG ("Constructor called, endpoint=%s, this=%p", _endpoint_str.c_str (),
                       static_cast<void *> (this));
}

zlink::asio_ipc_connecter_t::~asio_ipc_connecter_t ()
{
    IPC_CONNECTER_DBG ("Destructor called, this=%p", static_cast<void *> (this));
    zlink_assert (!_reconnect_timer_started);
    zlink_assert (!_connect_timer_started);
}

void zlink::asio_ipc_connecter_t::process_plug ()
{
    IPC_CONNECTER_DBG ("process_plug called, delayed_start=%d", _delayed_start);

    if (_delayed_start)
        add_reconnect_timer ();
    else
        start_connecting ();
}

void zlink::asio_ipc_connecter_t::process_term (int linger_)
{
    IPC_CONNECTER_DBG ("process_term called, linger=%d, connecting=%d", linger_, _connecting);

    prepare_asio_connecter_termination (
      linger_, &_terminating, &_linger, &_reconnect_timer_started, &_connect_timer_started,
      &_connecting, [this] () { cancel_timer (reconnect_timer_id); },
      [this] () { cancel_timer (connect_timer_id); }, [this] () { close (); },
      [this] () { _io_context.poll (); });

    own_t::process_term (linger_);
}

void zlink::asio_ipc_connecter_t::timer_event (int id_)
{
    IPC_CONNECTER_DBG ("timer_event: id=%d", id_);

    if (!handle_asio_connecter_timer_event (
          id_, reconnect_timer_id, connect_timer_id, &_reconnect_timer_started,
          &_connect_timer_started, [this] () { start_connecting (); },
          [this] () {
              if (_connecting) {
                  boost::system::error_code ec;
                  _socket.cancel (ec);
                  _connecting = false;
              }
              close ();
              add_reconnect_timer ();
          })) {
        zlink_assert (false);
    }
}

void zlink::asio_ipc_connecter_t::start_connecting ()
{
    IPC_CONNECTER_DBG ("start_connecting: endpoint=%s", _endpoint_str.c_str ());

    if (_addr->resolved.ipc_addr != NULL) {
        LIBZLINK_DELETE (_addr->resolved.ipc_addr);
    }

    _addr->resolved.ipc_addr = new (std::nothrow) ipc_address_t ();
    alloc_assert (_addr->resolved.ipc_addr);

    int rc = _addr->resolved.ipc_addr->resolve (_addr->address.c_str ());
    if (rc != 0) {
        IPC_CONNECTER_DBG ("start_connecting: resolve failed");
        LIBZLINK_DELETE (_addr->resolved.ipc_addr);
        add_reconnect_timer ();
        return;
    }

    _endpoint = make_ipc_endpoint (*_addr->resolved.ipc_addr);

    boost::system::error_code ec;
    _socket.open (_endpoint.protocol (), ec);
    if (ec) {
        IPC_CONNECTER_DBG ("start_connecting: socket open failed: %s", ec.message ().c_str ());
        add_reconnect_timer ();
        return;
    }

    _connecting = true;
    _socket.async_connect (_endpoint,
                           [this] (const boost::system::error_code &ec) { on_connect (ec); });

    add_connect_timer ();

    _socket_ptr->event_connect_delayed (make_unconnected_connect_endpoint_pair (_endpoint_str),
                                        connect_delayed_errno_value ());
}

void zlink::asio_ipc_connecter_t::on_connect (const boost::system::error_code &ec)
{
    _connecting = false;
    IPC_CONNECTER_DBG ("on_connect: ec=%s, terminating=%d", ec.message ().c_str (), _terminating);

    if (_terminating)
        return;

    cancel_asio_timer_if_started (&_connect_timer_started,
                                  [this] () { cancel_timer (connect_timer_id); });

    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            IPC_CONNECTER_DBG ("on_connect: operation aborted");
            return;
        }

        IPC_CONNECTER_DBG ("on_connect: connection failed: %s", ec.message ().c_str ());
        close ();
        add_reconnect_timer ();
        return;
    }

    fd_t fd = _socket.native_handle ();
    IPC_CONNECTER_DBG ("on_connect: connected, fd=%d", fd);

    _socket.release ();

    std::string local_address = get_socket_name<ipc_address_t> (fd, socket_end_local);

    create_engine (fd, local_address);
}

void zlink::asio_ipc_connecter_t::add_connect_timer ()
{
    start_asio_timer_if_positive (
      options.connect_timeout, &_connect_timer_started, [this] (int timeout) {
        IPC_CONNECTER_DBG ("add_connect_timer: timeout=%d", options.connect_timeout);
        add_timer (timeout, connect_timer_id);
    });
}

void zlink::asio_ipc_connecter_t::add_reconnect_timer ()
{
    const int interval = options.reconnect_ivl > 0 ? get_new_reconnect_ivl () : 0;
    start_asio_timer_if_positive (interval, &_reconnect_timer_started, [this] (int interval) {
        IPC_CONNECTER_DBG ("add_reconnect_timer: interval=%d", interval);
        add_timer (interval, reconnect_timer_id);
        _socket_ptr->event_connect_retried (make_unconnected_connect_endpoint_pair (_endpoint_str),
                                            interval);
    });
}

int zlink::asio_ipc_connecter_t::get_new_reconnect_ivl ()
{
    return next_asio_reconnect_interval (options, &_current_reconnect_ivl);
}

void zlink::asio_ipc_connecter_t::create_engine (fd_t fd_, const std::string &local_address_)
{
    IPC_CONNECTER_DBG ("create_engine: fd=%d, local=%s", fd_, local_address_.c_str ());

    const endpoint_uri_pair_t endpoint_pair (local_address_, _endpoint_str, endpoint_type_connect);

    std::unique_ptr<i_asio_transport> transport (new (std::nothrow) ipc_transport_t ());
    alloc_assert (transport.get ());

    i_engine *engine = NULL;
    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        engine =
          new (std::nothrow) asio_raw_engine_t (fd_, options, endpoint_pair, std::move (transport));
    } else {
        engine =
          new (std::nothrow) asio_zmp_engine_t (fd_, options, endpoint_pair, std::move (transport));
    }
    alloc_assert (engine);

    send_attach (_session, engine);

    terminate ();

    _socket_ptr->event_connected (
      endpoint_pair, fd_, options.transport_lane, options.transport_pair_id,
      options.transport_pair_generation);
}

void zlink::asio_ipc_connecter_t::close ()
{
    IPC_CONNECTER_DBG ("close called");

    close_asio_socket_if_open (_socket, [this] (fd_t fd) {
        _socket_ptr->event_closed (make_unconnected_connect_endpoint_pair (_endpoint_str), fd);
    });
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC
