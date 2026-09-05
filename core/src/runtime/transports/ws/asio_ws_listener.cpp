/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_WS

#include "transports/ws/asio_ws_listener.hpp"
#include "engine/asio/asio_zmp_engine.hpp"
#include "engine/asio/asio_raw_engine.hpp"
#include "engine/asio/asio_poller.hpp"
#include "transports/ws/ws_batch_policy.hpp"
#include "transports/tls/ssl_context_helper.hpp"
#include "transports/ws/ws_transport.hpp"
#if defined ZLINK_HAVE_WSS
#include "transports/tls/wss_transport.hpp"
#endif
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"
#include "transports/asio/asio_tcp_acceptor_config.hpp"
#include "transports/asio/asio_listener_accept_policy.hpp"
#include "transports/asio/asio_socket_lifecycle.hpp"
#include "transports/asio/asio_tcp_endpoint.hpp"
#include "transports/asio/asio_tcp_tuning.hpp"
#include "utils/config.hpp"
#include "utils/err.hpp"
#include "utils/ip.hpp"
#include "transports/tcp/tcp.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif
#include <cerrno>

//  Debug logging for ASIO WS listener - set to 1 to enable
#define ASIO_WS_LISTENER_DEBUG 0

#if ASIO_WS_LISTENER_DEBUG
#include <cstdio>
#define WS_LISTENER_DBG(fmt, ...) fprintf (stderr, "[ASIO_WS_LISTENER] " fmt "\n", ##__VA_ARGS__)
#else
#define WS_LISTENER_DBG(fmt, ...)
#endif

zlink::asio_ws_listener_t::asio_ws_listener_t (io_thread_t *io_thread_,
                                               socket_base_t *socket_,
                                               const options_t &options_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    _io_context (io_thread_->get_io_context ()),
    _acceptor (_io_context),
    _socket (socket_),
    _path ("/"),
    _port (0),
    _secure (false),
    _accepting_count (0),
    _terminating (false),
    _linger (0)
{
    WS_LISTENER_DBG ("Constructor called, this=%p", static_cast<void *> (this));
}

zlink::asio_ws_listener_t::~asio_ws_listener_t ()
{
    WS_LISTENER_DBG ("Destructor called, this=%p", static_cast<void *> (this));
}

int zlink::asio_ws_listener_t::set_local_address (const ws_address_t *addr_, bool secure_)
{
    WS_LISTENER_DBG ("set_local_address: host=%s, port=%u, path=%s", addr_->host ().c_str (),
                     addr_->port (), addr_->path ().c_str ());

    _secure = secure_;
#if !defined ZLINK_HAVE_WSS
    if (_secure) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
#endif

    //  Store WebSocket-specific address components
    _host = addr_->host ();
    _path = addr_->path ();
    _port = addr_->port ();

    //  Determine protocol family from the resolved address
    boost::asio::ip::tcp protocol =
      addr_->family () == AF_INET6 ? boost::asio::ip::tcp::v6 () : boost::asio::ip::tcp::v4 ();

    //  Construct boost endpoint from ws_address
    const boost::asio::ip::tcp::endpoint bind_endpoint =
      asio_tcp_endpoint_from_sockaddr (addr_->addr ());

    if (configure_asio_tcp_acceptor (
          _acceptor, protocol, addr_->family (), bind_endpoint, options,
          [] (const char *stage, const boost::system::error_code &ec, bool) {
              WS_LISTENER_DBG ("Failed to %s: %s", stage, ec.message ().c_str ());
          })
        != 0)
        return -1;

    //  Get the actual bound port (in case port 0 was specified)
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint local_ep = _acceptor.local_endpoint (ec);
    if (!ec) {
        _port = local_ep.port ();
    }

    //  Build endpoint string with actual bound port (not the input port)
    //  Format: ws://host:port/path or ws://[ipv6]:port/path
    const std::string prefix = _secure ? "wss://" : "ws://";
    if (addr_->family () == AF_INET6) {
        _endpoint = prefix + "[" + _host + "]:" + std::to_string (_port) + _path;
    } else {
        _endpoint = prefix + _host + ":" + std::to_string (_port) + _path;
    }

    _socket->event_listening (make_unconnected_bind_endpoint_pair (_endpoint),
                              _acceptor.native_handle ());

    WS_LISTENER_DBG ("Listening on %s (fd=%d)", _endpoint.c_str (),
                     static_cast<int> (_acceptor.native_handle ()));

    return 0;
}

int zlink::asio_ws_listener_t::get_local_address (std::string &addr_) const
{
    addr_ = _endpoint;
    return addr_.empty () ? -1 : 0;
}

std::string zlink::asio_ws_listener_t::get_socket_name (fd_t fd_, socket_end_t socket_end_) const
{
    return zlink::get_socket_name<tcp_address_t> (fd_, socket_end_);
}

void zlink::asio_ws_listener_t::process_plug ()
{
    WS_LISTENER_DBG ("process_plug called");

    //  Start accepting connections
    start_accept ();
}

void zlink::asio_ws_listener_t::process_term (int linger_)
{
    WS_LISTENER_DBG ("process_term called, linger=%d, accepting=%zu", linger_, _accepting_count);

    _linger = linger_;

    process_release_endpoint ();

    //  Process pending handlers
    drain_asio_listener_pending_accepts (_io_context, &_accepting_count);

    own_t::process_term (linger_);
}

void zlink::asio_ws_listener_t::start_accept ()
{
    start_asio_listener_accepts<boost::asio::ip::tcp::socket> (
      _io_context, _acceptor, &_accepting_count, options,
      [] (size_t accepting_count_, size_t target_accepts_) {
          WS_LISTENER_DBG ("start_accept: starting async_accept (%zu/%zu)", accepting_count_,
                           target_accepts_);
      },
      [this] (const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
              const boost::system::error_code &ec_) { on_accept (accept_socket_, ec_); });
}

void zlink::asio_ws_listener_t::on_accept (
  const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
  const boost::system::error_code &ec)
{
    if (_accepting_count > 0)
        --_accepting_count;
    WS_LISTENER_DBG ("on_accept: ec=%s, terminating=%d, pending=%zu", ec.message ().c_str (),
                     _terminating, _accepting_count);

    if (_terminating) {
        if (!ec && accept_socket_ && accept_socket_->is_open ()) {
            boost::system::error_code close_ec;
            accept_socket_->close (close_ec);
        }
        return;
    }

    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            WS_LISTENER_DBG ("on_accept: operation aborted");
            start_accept ();
            return;
        }

        _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint), ec.value ());

        start_accept ();
        return;
    }

    //  Get the native handle before releasing ownership
    fd_t fd = accept_socket_->native_handle ();
    WS_LISTENER_DBG ("on_accept: accepted connection, fd=%d", fd);

    //  Get peer address for accept filter
    boost::system::error_code peer_ec;
    boost::asio::ip::tcp::endpoint remote_endpoint = accept_socket_->remote_endpoint (peer_ec);

    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof (ss);
    memset (&ss, 0, sizeof (ss));

    if (!peer_ec)
        asio_tcp_endpoint_to_sockaddr (remote_endpoint, &ss, &ss_len);

    //  Apply accept filters
    if (!apply_accept_filters (fd, ss, ss_len)) {
        WS_LISTENER_DBG ("on_accept: connection rejected by filter");
        boost::system::error_code close_ec;
        accept_socket_->close (close_ec);
        start_accept ();
        return;
    }

    //  Release ownership of the socket
    accept_socket_->release ();

    //  Tune the accepted socket
    if (tune_socket (fd) != 0) {
        WS_LISTENER_DBG ("on_accept: tune_socket failed");
        _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint),
                                      zlink_errno ());
#ifdef ZLINK_HAVE_WINDOWS
        closesocket (fd);
#else
        ::close (fd);
#endif
        start_accept ();
        return;
    }

    //  Create engine for this connection
    create_engine (fd);

    start_accept ();
}

void zlink::asio_ws_listener_t::create_engine (fd_t fd_)
{
    WS_LISTENER_DBG ("create_engine: fd=%d", fd_);

    std::string local_addr = get_socket_name (fd_, socket_end_local);
    std::string remote_addr = get_socket_name (fd_, socket_end_remote);

    if (local_addr.compare (0, 6, "tcp://") == 0)
        local_addr = local_addr.substr (6);
    if (remote_addr.compare (0, 6, "tcp://") == 0)
        remote_addr = remote_addr.substr (6);

    const std::string prefix = _secure ? "wss://" : "ws://";
    const endpoint_uri_pair_t endpoint_pair (prefix + local_addr + _path,
                                             prefix + remote_addr + _path, endpoint_type_bind);

    std::unique_ptr<i_asio_transport> transport;
#if defined ZLINK_HAVE_WSS
    std::unique_ptr<boost::asio::ssl::context> ssl_context;
    if (_secure) {
        ssl_context = ssl_context_helper_t::create_server_context_from_options (options);
        if (!ssl_context) {
            _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint), EINVAL);
#ifdef ZLINK_HAVE_WINDOWS
            closesocket (fd_);
#else
            ::close (fd_);
#endif
            return;
        }
        std::unique_ptr<wss_transport_t> wss_transport (
          new (std::nothrow) wss_transport_t (*ssl_context, _path, _host));
        alloc_assert (wss_transport);
        transport.reset (wss_transport.release ());
    } else
#endif
    {
        std::unique_ptr<ws_transport_t> ws_transport (new (std::nothrow)
                                                        ws_transport_t (_path, _host));
        alloc_assert (ws_transport);
        transport.reset (ws_transport.release ());
    }

    const bool is_stream = options.type == ZLINK_CORE_SOCKET_STREAM;
    options_t engine_options = options;
    if (!is_stream)
        engine_options.out_batch_size =
          zlink::ws_batch_policy::zmp_send_batch_size ();
    i_engine *engine = NULL;
#if defined ZLINK_HAVE_WSS
    if (_secure) {
        if (is_stream)
            engine = new (std::nothrow) asio_raw_engine_t (
              fd_, engine_options, endpoint_pair, std::move (transport), std::move (ssl_context));
        else
            engine = new (std::nothrow) asio_zmp_engine_t (
              fd_, engine_options, endpoint_pair, std::move (transport), std::move (ssl_context));
    } else
#endif
    {
        if (is_stream)
            engine = new (std::nothrow)
              asio_raw_engine_t (fd_, engine_options, endpoint_pair, std::move (transport));
        else
            engine = new (std::nothrow)
              asio_zmp_engine_t (fd_, engine_options, endpoint_pair, std::move (transport));
    }
    alloc_assert (engine);

    //  Choose I/O thread for engine
    io_thread_t *io_thread = options.type == ZLINK_CORE_SOCKET_STREAM
                               ? choose_io_thread_stream (options.affinity)
                               : choose_io_thread_transport (options.affinity);
    zlink_assert (io_thread);

    //  Create and launch session
    session_base_t *session = session_base_t::create (io_thread, false, _socket, options, NULL);
    errno_assert (session);
    session->inc_seqnum ();
    launch_child (session);
    send_attach (session, engine, false);

    _socket->event_accepted (endpoint_pair, fd_);
}

void zlink::asio_ws_listener_t::process_release_endpoint ()
{
    _terminating = true;
    WS_LISTENER_DBG ("close called");

    close_asio_socket_if_open (_acceptor, [this] (fd_t fd) {
        _socket->event_closed (make_unconnected_bind_endpoint_pair (_endpoint), fd);
    });
}

int zlink::asio_ws_listener_t::tune_socket (fd_t fd_) const
{
    return tune_asio_tcp_socket (fd_, options, true);
}

bool zlink::asio_ws_listener_t::apply_accept_filters (fd_t fd_,
                                                      const struct sockaddr_storage &ss,
                                                      socklen_t ss_len) const
{
    //  Make socket non-inheritable
    make_socket_noninheritable (fd_);

    //  Check accept filters
    if (!options.tcp_accept_filters.empty ()) {
        bool matched = false;
        for (options_t::tcp_accept_filters_t::size_type i = 0,
                                                        size = options.tcp_accept_filters.size ();
             i != size; ++i) {
            if (options.tcp_accept_filters[i].match_address (
                  reinterpret_cast<const struct sockaddr *> (&ss), ss_len)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    //  Set NOSIGPIPE
    if (zlink::set_nosigpipe (fd_)) {
        return false;
    }

    //  Set IP Type-Of-Service
    if (options.tos != 0)
        set_ip_type_of_service (fd_, options.tos);

    //  Set socket priority
    if (options.priority != 0)
        set_socket_priority (fd_, options.priority);

    return true;
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_WS
