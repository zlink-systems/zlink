/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_TLS_CONNECTER_HPP_INCLUDED__
#define __ZLINK_ASIO_TLS_CONNECTER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <memory>

#include "utils/fd.hpp"
#include "core/own.hpp"
#include "utils/stdint.hpp"
#include "core/io_object.hpp"

namespace zlink
{
class io_thread_t;
class session_base_t;
struct address_t;

//  ASIO-based TLS connecter using async_connect + SSL handshake.
//
//  This connecter:
//  1. Establishes TCP connection (like asio_tcp_connecter)
//  2. Performs SSL/TLS handshake with peer
//  3. Creates ASIO engine with encrypted transport
//
//  SSL configuration comes from options:
//    - tls_ca: CA certificate file for server verification
//    - tls_cert: Client certificate file (for mutual TLS)
//    - tls_key: Client private key file (for mutual TLS)
//    - tls_hostname: Server hostname for SNI and verification

class asio_tls_connecter_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    //  If 'delayed_start' is true connecter first waits for a while,
    //  then starts connection process.
    asio_tls_connecter_t (zlink::io_thread_t *io_thread_,
                          zlink::session_base_t *session_,
                          const options_t &options_,
                          address_t *addr_,
                          bool delayed_start_);
    ~asio_tls_connecter_t ();

  private:
    //  ID of the timer used to check the connect timeout.
    enum
    {
        reconnect_timer_id = 1,
        connect_timer_id = 2
    };

    //  Handlers for incoming commands.
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_OVERRIDE;

    //  Handlers for I/O events (from io_object_t).
    void timer_event (int id_) ZLINK_OVERRIDE;

    //  Internal function to start the actual connection establishment.
    void start_connecting ();

    //  Handle TCP connect completion
    void on_tcp_connect (const boost::system::error_code &ec);

    //  Internal function to add timers
    void add_connect_timer ();
    void add_reconnect_timer ();

    //  Internal function to return a reconnect backoff delay.
    int get_new_reconnect_ivl ();

    //  Create SSL context from options
    bool create_ssl_context (const std::string &hostname_);

    //  Create engine for connected+handshaked socket
    void create_engine (fd_t fd_, const std::string &local_address_);

    //  Tune the connected socket
    bool tune_socket (fd_t fd_);

    //  Close the socket
    void close ();

    //  Reference to the io_context from asio_poller
    boost::asio::io_context &_io_context;

    //  The ASIO socket for connecting
    boost::asio::ip::tcp::socket _socket;

    //  SSL context (created from options)
    std::unique_ptr<boost::asio::ssl::context> _ssl_context;

    //  Effective TLS hostname for SNI/verification
    std::string _tls_hostname;

    //  Target endpoint for connection
    boost::asio::ip::tcp::endpoint _endpoint;

    //  Address to connect to. Owned by session_base_t.
    address_t *const _addr;

    //  String representation of endpoint to connect to
    std::string _endpoint_str;

    //  Reference to the session we belong to.
    zlink::session_base_t *const _session;

    //  Socket
    zlink::socket_base_t *const _socket_ptr;

    //  If true, connecter is waiting a while before trying to connect.
    const bool _delayed_start;

    //  Timer states
    bool _reconnect_timer_started;
    bool _connect_timer_started;

    //  Connection states
    bool _tcp_connecting; // TCP connect in progress
    bool _terminating;    // process_term called

    //  Linger value saved from process_term for deferred termination
    int _linger;

    //  Current reconnect ivl, updated for backoff strategy
    int _current_reconnect_ivl;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_tls_connecter_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL

#endif // __ZLINK_ASIO_TLS_CONNECTER_HPP_INCLUDED__
