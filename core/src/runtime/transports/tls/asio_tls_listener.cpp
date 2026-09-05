/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include "transports/tls/asio_tls_listener.hpp"
#include "engine/asio/asio_poller.hpp"
#include "engine/asio/asio_zmp_engine.hpp"
#include "engine/asio/asio_raw_engine.hpp"
#include "transports/tls/ssl_transport.hpp"
#include "transports/tls/ssl_context_helper.hpp"
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

#include <cerrno>

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif

// Debug logging for ASIO TLS listener - set to 1 to enable
#define ASIO_TLS_LISTENER_DEBUG 0

#if ASIO_TLS_LISTENER_DEBUG
#include <cstdio>
#define TLS_LISTENER_DBG(fmt, ...) fprintf (stderr, "[ASIO_TLS_LISTENER] " fmt "\n", ##__VA_ARGS__)
#else
#define TLS_LISTENER_DBG(fmt, ...)
#endif

zlink::asio_tls_listener_t::asio_tls_listener_t (io_thread_t *io_thread_,
                                                 socket_base_t *socket_,
                                                 const options_t &options_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    _io_context (io_thread_->get_io_context ()),
    _acceptor (_io_context),
    _socket (socket_),
    _accepting_count (0),
    _terminating (false),
    _linger (0)
{
    TLS_LISTENER_DBG ("Constructor called, this=%p", static_cast<void *> (this));
}

zlink::asio_tls_listener_t::~asio_tls_listener_t ()
{
    TLS_LISTENER_DBG ("Destructor called, this=%p", static_cast<void *> (this));
}

int zlink::asio_tls_listener_t::set_local_address (const char *addr_)
{
    TLS_LISTENER_DBG ("set_local_address: addr=%s", addr_);

    //  Create SSL context first (server requires cert+key)
    if (!create_ssl_context ()) {
        TLS_LISTENER_DBG ("set_local_address: failed to create SSL context");
        errno = EINVAL;
        return -1;
    }

    //  Parse the address
    int rc = _address.resolve (addr_, true, options.ipv6);
    if (rc != 0)
        return -1;

    //  Determine protocol family
    boost::asio::ip::tcp protocol =
      _address.family () == AF_INET6 ? boost::asio::ip::tcp::v6 () : boost::asio::ip::tcp::v4 ();

    //  Construct boost endpoint from zlink address
    const boost::asio::ip::tcp::endpoint bind_endpoint =
      asio_tcp_endpoint_from_sockaddr (_address.addr ());

    if (configure_asio_tcp_acceptor (
          _acceptor, protocol, _address.family (), bind_endpoint, options,
          [] (const char *stage, const boost::system::error_code &ec, bool) {
              TLS_LISTENER_DBG ("Failed to %s: %s", stage, ec.message ().c_str ());
          })
        != 0)
        return -1;

    //  Get endpoint string for events (resolves wildcard port)
    _endpoint = get_socket_name (_acceptor.native_handle (), socket_end_local);
    if (_endpoint.compare (0, 6, "tcp://") == 0)
        _endpoint.replace (0, 6, "tls://");

    _socket->event_listening (make_unconnected_bind_endpoint_pair (_endpoint),
                              _acceptor.native_handle ());

    TLS_LISTENER_DBG ("Listening on %s (fd=%d)", _endpoint.c_str (),
                      static_cast<int> (_acceptor.native_handle ()));

    return 0;
}

int zlink::asio_tls_listener_t::get_local_address (std::string &addr_) const
{
    addr_ = _endpoint;
    return addr_.empty () ? -1 : 0;
}

std::string zlink::asio_tls_listener_t::get_socket_name (fd_t fd_, socket_end_t socket_end_) const
{
    return zlink::get_socket_name<tcp_address_t> (fd_, socket_end_);
}

void zlink::asio_tls_listener_t::process_plug ()
{
    TLS_LISTENER_DBG ("process_plug called");
    start_accept ();
}

void zlink::asio_tls_listener_t::process_term (int linger_)
{
    TLS_LISTENER_DBG ("process_term called, linger=%d, accepting=%zu", linger_, _accepting_count);

    _linger = linger_;

    process_release_endpoint ();

    //  Process pending handlers while object is still alive
    drain_asio_listener_pending_accepts (_io_context, &_accepting_count);

    own_t::process_term (linger_);
}

void zlink::asio_tls_listener_t::start_accept ()
{
    start_asio_listener_accepts<boost::asio::ip::tcp::socket> (
      _io_context, _acceptor, &_accepting_count, options,
      [] (size_t accepting_count_, size_t target_accepts_) {
          TLS_LISTENER_DBG ("start_accept: starting async_accept (%zu/%zu)", accepting_count_,
                            target_accepts_);
      },
      [this] (const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
              const boost::system::error_code &ec_) { on_tcp_accept (accept_socket_, ec_); });
}

void zlink::asio_tls_listener_t::on_tcp_accept (
  const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
  const boost::system::error_code &ec)
{
    if (_accepting_count > 0)
        --_accepting_count;
    TLS_LISTENER_DBG ("on_tcp_accept: ec=%s, terminating=%d, pending=%zu", ec.message ().c_str (),
                      _terminating, _accepting_count);

    if (_terminating) {
        TLS_LISTENER_DBG ("on_tcp_accept: terminating, ignoring callback");
        if (!ec && accept_socket_ && accept_socket_->is_open ()) {
            boost::system::error_code close_ec;
            accept_socket_->close (close_ec);
        }
        return;
    }

    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            TLS_LISTENER_DBG ("on_tcp_accept: operation aborted");
            start_accept ();
            return;
        }

        //  Report accept failure
        _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint), ec.value ());

        start_accept ();
        return;
    }

    TLS_LISTENER_DBG ("on_tcp_accept: TCP accepted, fd=%d",
                      static_cast<int> (accept_socket_->native_handle ()));

    //  Get the native handle before wrapping in SSL
    fd_t fd = accept_socket_->native_handle ();

    //  Get peer address for accept filter
    boost::system::error_code peer_ec;
    boost::asio::ip::tcp::endpoint remote_endpoint = accept_socket_->remote_endpoint (peer_ec);

    //  Store peer address in sockaddr_storage for filter check
    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof (ss);
    memset (&ss, 0, sizeof (ss));

    if (!peer_ec)
        asio_tcp_endpoint_to_sockaddr (remote_endpoint, &ss, &ss_len);

    //  Apply accept filters
    if (!apply_accept_filters (fd, ss, ss_len)) {
        TLS_LISTENER_DBG ("on_tcp_accept: connection rejected by filter");
        boost::system::error_code close_ec;
        accept_socket_->close (close_ec);
        start_accept ();
        return;
    }

    //  Release socket from ASIO management (engine will take ownership)
    accept_socket_->release ();

    //  Tune the socket
    if (tune_socket (fd) != 0) {
        TLS_LISTENER_DBG ("on_tcp_accept: tune_socket failed");
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

    std::unique_ptr<boost::asio::ssl::context> ssl_context = create_ssl_context ();
    if (!ssl_context) {
        TLS_LISTENER_DBG ("on_tcp_accept: failed to create SSL context");
        _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint), EINVAL);
#ifdef ZLINK_HAVE_WINDOWS
        closesocket (fd);
#else
        ::close (fd);
#endif
        start_accept ();
        return;
    }

    //  Create engine with SSL transport
    create_engine (fd, std::move (ssl_context));

    start_accept ();
}

std::unique_ptr<boost::asio::ssl::context> zlink::asio_tls_listener_t::create_ssl_context () const
{
    TLS_LISTENER_DBG ("create_ssl_context: cert=%s, key=%s, ca=%s", options.tls_cert.c_str (),
                      options.tls_key.c_str (), options.tls_ca.c_str ());

    std::unique_ptr<boost::asio::ssl::context> ssl_context =
      ssl_context_helper_t::create_server_context_from_options (options);

    if (!ssl_context) {
        TLS_LISTENER_DBG ("create_ssl_context: failed to create SSL context: %s",
                          ssl_context_helper_t::get_ssl_error_string ().c_str ());
        return std::unique_ptr<boost::asio::ssl::context> ();
    }

    TLS_LISTENER_DBG ("create_ssl_context: SSL context created successfully");
    return ssl_context;
}

void zlink::asio_tls_listener_t::create_engine (
  fd_t fd_, std::unique_ptr<boost::asio::ssl::context> ssl_context_)
{
    TLS_LISTENER_DBG ("create_engine: fd=%d", fd_);

    std::string local_endpoint = get_socket_name (fd_, socket_end_local);
    std::string remote_endpoint = get_socket_name (fd_, socket_end_remote);
    if (local_endpoint.compare (0, 6, "tcp://") == 0)
        local_endpoint.replace (0, 6, "tls://");
    if (remote_endpoint.compare (0, 6, "tcp://") == 0)
        remote_endpoint.replace (0, 6, "tls://");

    const endpoint_uri_pair_t endpoint_pair (local_endpoint, remote_endpoint, endpoint_type_bind);

    std::unique_ptr<ssl_transport_t> transport (new (std::nothrow) ssl_transport_t (*ssl_context_));
    alloc_assert (transport);

    i_engine *engine = NULL;
    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        engine = new (std::nothrow) asio_raw_engine_t (
          fd_, options, endpoint_pair, std::unique_ptr<i_asio_transport> (transport.release ()),
          std::move (ssl_context_));
    } else {
        engine = new (std::nothrow) asio_zmp_engine_t (
          fd_, options, endpoint_pair, std::unique_ptr<i_asio_transport> (transport.release ()),
          std::move (ssl_context_));
    }
    alloc_assert (engine);

    //  Choose I/O thread to run engine in
    io_thread_t *io_thread = options.type == ZLINK_CORE_SOCKET_STREAM
                               ? choose_io_thread_stream (options.affinity)
                               : choose_io_thread_transport (options.affinity);
    zlink_assert (io_thread);

    //  Create and launch a session
    session_base_t *session = session_base_t::create (io_thread, false, _socket, options, NULL);
    errno_assert (session);
    session->inc_seqnum ();
    launch_child (session);
    send_attach (session, engine, false);

    _socket->event_accepted (endpoint_pair, fd_);
}

void zlink::asio_tls_listener_t::process_release_endpoint ()
{
    _terminating = true;
    TLS_LISTENER_DBG ("close called");

    close_asio_socket_if_open (_acceptor, [this] (fd_t fd) {
        _socket->event_closed (make_unconnected_bind_endpoint_pair (_endpoint), fd);
    });
}

int zlink::asio_tls_listener_t::tune_socket (fd_t fd_) const
{
    return tune_asio_tcp_socket (fd_, options, true);
}

bool zlink::asio_tls_listener_t::apply_accept_filters (fd_t fd_,
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

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL
