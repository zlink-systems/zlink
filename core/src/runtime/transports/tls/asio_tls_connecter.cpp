/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include "transports/tls/asio_tls_connecter.hpp"
#include "engine/asio/asio_poller.hpp"
#include "engine/asio/asio_raw_engine.hpp"
#include "engine/asio/asio_zmp_engine.hpp"
#include "transports/tls/ssl_transport.hpp"
#include "transports/tls/ssl_context_helper.hpp"
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "core/address.hpp"
#include "transports/asio/asio_tcp_endpoint.hpp"
#include "transports/asio/asio_reconnect_interval.hpp"
#include "transports/asio/asio_socket_lifecycle.hpp"
#include "transports/asio/asio_timer_flag.hpp"
#include "transports/asio/asio_tcp_tuning.hpp"
#include "transports/tcp/tcp_address.hpp"
#include "utils/random.hpp"
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

#include <limits>
#include <cerrno>

// Debug logging for ASIO TLS connecter - set to 1 to enable
#define ASIO_TLS_CONNECTER_DEBUG 0

#if ASIO_TLS_CONNECTER_DEBUG
#include <cstdio>
#define TLS_CONNECTER_DBG(fmt, ...)                                                                \
    fprintf (stderr, "[ASIO_TLS_CONNECTER] " fmt "\n", ##__VA_ARGS__)
#else
#define TLS_CONNECTER_DBG(fmt, ...)
#endif

namespace
{
std::string extract_tls_hostname (const std::string &address)
{
    std::string target = address;
    const std::string::size_type delim = target.rfind (';');
    if (delim != std::string::npos)
        target = target.substr (delim + 1);

    if (target.empty ())
        return std::string ();

    if (target[0] == '[') {
        const std::string::size_type end = target.find (']');
        if (end == std::string::npos)
            return std::string ();
        return target.substr (1, end - 1);
    }

    const std::string::size_type colon = target.rfind (':');
    if (colon == std::string::npos)
        return std::string ();

    const std::string host = target.substr (0, colon);
    if (host.empty () || host == "*")
        return std::string ();

    return host;
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

zlink::asio_tls_connecter_t::asio_tls_connecter_t (io_thread_t *io_thread_,
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
    _tcp_connecting (false),
    _terminating (false),
    _linger (0),
    _current_reconnect_ivl (-1)
{
    zlink_assert (_addr);
    zlink_assert (_addr->protocol == protocol_name::tls);
    _addr->to_string (_endpoint_str);

    TLS_CONNECTER_DBG ("Constructor called, endpoint=%s, this=%p", _endpoint_str.c_str (),
                       static_cast<void *> (this));
}

zlink::asio_tls_connecter_t::~asio_tls_connecter_t ()
{
    TLS_CONNECTER_DBG ("Destructor called, this=%p", static_cast<void *> (this));
    zlink_assert (!_reconnect_timer_started);
    zlink_assert (!_connect_timer_started);
}

void zlink::asio_tls_connecter_t::process_plug ()
{
    TLS_CONNECTER_DBG ("process_plug called, delayed_start=%d", _delayed_start);

    if (_delayed_start)
        add_reconnect_timer ();
    else
        start_connecting ();
}

void zlink::asio_tls_connecter_t::process_term (int linger_)
{
    TLS_CONNECTER_DBG ("process_term called, linger=%d, tcp_connecting=%d", linger_,
                       _tcp_connecting);

    prepare_asio_connecter_termination (
      linger_, &_terminating, &_linger, &_reconnect_timer_started, &_connect_timer_started,
      &_tcp_connecting, [this] () { cancel_timer (reconnect_timer_id); },
      [this] () { cancel_timer (connect_timer_id); }, [this] () { close (); },
      [this] () { _io_context.poll (); });

    own_t::process_term (linger_);
}

void zlink::asio_tls_connecter_t::timer_event (int id_)
{
    TLS_CONNECTER_DBG ("timer_event: id=%d", id_);

    if (!handle_asio_connecter_timer_event (
          id_, reconnect_timer_id, connect_timer_id, &_reconnect_timer_started,
          &_connect_timer_started, [this] () { start_connecting (); },
          [this] () {
              if (_tcp_connecting) {
                  boost::system::error_code ec;
                  _socket.cancel (ec);
                  _tcp_connecting = false;
              }
              close ();
              add_reconnect_timer ();
          })) {
        zlink_assert (false);
    }
}

void zlink::asio_tls_connecter_t::start_connecting ()
{
    TLS_CONNECTER_DBG ("start_connecting: endpoint=%s", _endpoint_str.c_str ());

    //  Determine effective TLS hostname for SNI/verification
    _tls_hostname =
      !options.tls_hostname.empty () ? options.tls_hostname : extract_tls_hostname (_addr->address);

    //  Create SSL context if not already done
    if (!_ssl_context && !create_ssl_context (_tls_hostname)) {
        TLS_CONNECTER_DBG ("start_connecting: failed to create SSL context");
        add_reconnect_timer ();
        return;
    }

    //  Resolve the address if not already done
    if (_addr->resolved.tcp_addr != NULL) {
        LIBZLINK_DELETE (_addr->resolved.tcp_addr);
    }

    _addr->resolved.tcp_addr = new (std::nothrow) tcp_address_t ();
    alloc_assert (_addr->resolved.tcp_addr);

    int rc = _addr->resolved.tcp_addr->resolve (_addr->address.c_str (), false, options.ipv6);
    if (rc != 0) {
        TLS_CONNECTER_DBG ("start_connecting: resolve failed");
        LIBZLINK_DELETE (_addr->resolved.tcp_addr);
        add_reconnect_timer ();
        return;
    }

    const tcp_address_t *tcp_addr = _addr->resolved.tcp_addr;

    //  Create endpoint from resolved address
    _endpoint = asio_tcp_endpoint_from_sockaddr (tcp_addr->addr ());

    //  Open the socket
    boost::asio::ip::tcp protocol = asio_tcp_protocol_for_endpoint (_endpoint);

    boost::system::error_code ec;
    _socket.open (protocol, ec);
    if (ec) {
        TLS_CONNECTER_DBG ("start_connecting: socket open failed: %s", ec.message ().c_str ());
        add_reconnect_timer ();
        return;
    }

    //  Bind to source address if specified
    if (tcp_addr->has_src_addr ()) {
        _socket.set_option (boost::asio::socket_base::reuse_address (true), ec);

        //  Create source endpoint
        const boost::asio::ip::tcp::endpoint src_endpoint =
          asio_tcp_endpoint_from_sockaddr (tcp_addr->src_addr ());

        _socket.bind (src_endpoint, ec);
        if (ec) {
            TLS_CONNECTER_DBG ("start_connecting: bind failed: %s", ec.message ().c_str ());
            close ();
            add_reconnect_timer ();
            return;
        }
    }

    TLS_CONNECTER_DBG ("start_connecting: initiating async_connect to %s:%d",
                       _endpoint.address ().to_string ().c_str (), _endpoint.port ());

    //  Start TCP connection
    _tcp_connecting = true;
    _socket.async_connect (_endpoint,
                           [this] (const boost::system::error_code &ec) { on_tcp_connect (ec); });

    //  Add connect timeout
    add_connect_timer ();

    _socket_ptr->event_connect_delayed (make_unconnected_connect_endpoint_pair (_endpoint_str),
                                        connect_delayed_errno_value ());
}

void zlink::asio_tls_connecter_t::on_tcp_connect (const boost::system::error_code &ec)
{
    _tcp_connecting = false;
    TLS_CONNECTER_DBG ("on_tcp_connect: ec=%s, terminating=%d", ec.message ().c_str (),
                       _terminating);

    if (_terminating) {
        TLS_CONNECTER_DBG ("on_tcp_connect: terminating, ignoring callback");
        return;
    }

    //  Cancel connect timer
    cancel_asio_timer_if_started (&_connect_timer_started,
                                  [this] () { cancel_timer (connect_timer_id); });

    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            TLS_CONNECTER_DBG ("on_tcp_connect: operation aborted");
            return;
        }

        TLS_CONNECTER_DBG ("on_tcp_connect: TCP connection failed: %s", ec.message ().c_str ());
        close ();
        add_reconnect_timer ();
        return;
    }

    TLS_CONNECTER_DBG ("on_tcp_connect: TCP connected");

    //  Get the native handle before any further operations
    fd_t fd = _socket.native_handle ();
    TLS_CONNECTER_DBG ("on_tcp_connect: connected, fd=%d", fd);

    //  Release socket from ASIO management (transport takes ownership)
    _socket.release ();

    //  Tune the socket
    if (!tune_socket (fd)) {
        TLS_CONNECTER_DBG ("on_tcp_connect: tune_socket failed");
#ifdef ZLINK_HAVE_WINDOWS
        closesocket (fd);
#else
        ::close (fd);
#endif
        add_reconnect_timer ();
        return;
    }

    //  Get local address for engine
    std::string local_address = get_socket_name<tcp_address_t> (fd, socket_end_local);

    //  Create the engine with SSL transport
    create_engine (fd, local_address);
}

void zlink::asio_tls_connecter_t::add_connect_timer ()
{
    start_asio_timer_if_positive (
      options.connect_timeout, &_connect_timer_started, [this] (int timeout) {
        TLS_CONNECTER_DBG ("add_connect_timer: timeout=%d", options.connect_timeout);
        add_timer (timeout, connect_timer_id);
    });
}

void zlink::asio_tls_connecter_t::add_reconnect_timer ()
{
    const int interval = options.reconnect_ivl > 0 ? get_new_reconnect_ivl () : 0;
    start_asio_timer_if_positive (interval, &_reconnect_timer_started, [this] (int interval) {
        TLS_CONNECTER_DBG ("add_reconnect_timer: interval=%d", interval);
        add_timer (interval, reconnect_timer_id);
        _socket_ptr->event_connect_retried (make_unconnected_connect_endpoint_pair (_endpoint_str),
                                            interval);
    });
}

int zlink::asio_tls_connecter_t::get_new_reconnect_ivl ()
{
    return next_asio_reconnect_interval (options, &_current_reconnect_ivl);
}

bool zlink::asio_tls_connecter_t::create_ssl_context (const std::string &hostname_)
{
    TLS_CONNECTER_DBG ("create_ssl_context: ca=%s, cert=%s, key=%s", options.tls_ca.c_str (),
                       options.tls_cert.c_str (), options.tls_key.c_str ());

    _ssl_context = ssl_context_helper_t::create_client_context_from_options (options, hostname_);
    if (!_ssl_context) {
        TLS_CONNECTER_DBG ("create_ssl_context: failed to create SSL context: %s",
                           ssl_context_helper_t::get_ssl_error_string ().c_str ());
        return false;
    }

    TLS_CONNECTER_DBG ("create_ssl_context: SSL context created successfully");
    return true;
}

void zlink::asio_tls_connecter_t::create_engine (fd_t fd_, const std::string &local_address_)
{
    TLS_CONNECTER_DBG ("create_engine: fd=%d, local=%s", fd_, local_address_.c_str ());

    std::string local_endpoint = local_address_;
    if (local_endpoint.compare (0, 6, "tcp://") == 0)
        local_endpoint.replace (0, 6, "tls://");

    std::string remote_endpoint = _endpoint_str;
    if (remote_endpoint.compare (0, 6, "tcp://") == 0)
        remote_endpoint.replace (0, 6, "tls://");

    const endpoint_uri_pair_t endpoint_pair (local_endpoint, remote_endpoint,
                                             endpoint_type_connect);

    if (!_ssl_context) {
        TLS_CONNECTER_DBG ("create_engine: SSL context missing");
        close ();
        add_reconnect_timer ();
        return;
    }

    std::unique_ptr<ssl_transport_t> transport (new (std::nothrow) ssl_transport_t (*_ssl_context));
    alloc_assert (transport);
    if (!_tls_hostname.empty ())
        transport->set_hostname (_tls_hostname);

    i_engine *engine = NULL;
    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        engine = new (std::nothrow) asio_raw_engine_t (
          fd_, options, endpoint_pair, std::unique_ptr<i_asio_transport> (transport.release ()),
          std::move (_ssl_context));
    } else {
        engine = new (std::nothrow) asio_zmp_engine_t (
          fd_, options, endpoint_pair, std::unique_ptr<i_asio_transport> (transport.release ()),
          std::move (_ssl_context));
    }
    alloc_assert (engine);

    //  Attach the engine to the session
    send_attach (_session, engine);

    //  Shut down the connecter
    terminate ();

    _socket_ptr->event_connected (
      endpoint_pair, fd_, options.transport_lane, options.transport_pair_id,
      options.transport_pair_generation);
}

bool zlink::asio_tls_connecter_t::tune_socket (fd_t fd_)
{
    const int rc = tune_asio_tcp_socket (fd_, options, true);
    return rc == 0;
}

void zlink::asio_tls_connecter_t::close ()
{
    TLS_CONNECTER_DBG ("close called");

    close_asio_socket_if_open (_socket, [this] (fd_t fd) {
        _socket_ptr->event_closed (make_unconnected_connect_endpoint_pair (_endpoint_str), fd);
    });
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL
