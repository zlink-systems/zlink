/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_TLS_LISTENER_HPP_INCLUDED__
#define __ZLINK_ASIO_TLS_LISTENER_HPP_INCLUDED__

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
#include "transports/tcp/tcp_address.hpp"

namespace zlink
{
class io_thread_t;
class socket_base_t;

//  ASIO-based TLS listener using async_accept + SSL handshake.
//
//  This listener:
//  1. Accepts TCP connections (like asio_tcp_listener)
//  2. Performs SSL/TLS handshake with client
//  3. Creates ASIO engine with encrypted transport
//
//  SSL configuration comes from options:
//    - tls_cert: Server certificate file (REQUIRED)
//    - tls_key: Server private key file (REQUIRED)
//    - tls_ca: CA certificate file (for client verification, OPTIONAL)

class asio_tls_listener_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    asio_tls_listener_t (zlink::io_thread_t *io_thread_,
                         zlink::socket_base_t *socket_,
                         const options_t &options_);
    ~asio_tls_listener_t ();

    //  Set address to listen on.
    int set_local_address (const char *addr_);

    //  Get local address.
    int get_local_address (std::string &addr_) const;

  private:
    //  Handlers for incoming commands.
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_OVERRIDE;

    //  Start accepting connections
    void start_accept ();

    //  Handle TCP accept completion
    void on_tcp_accept (const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
                        const boost::system::error_code &ec);

    //  Create SSL context from options
    std::unique_ptr<boost::asio::ssl::context> create_ssl_context () const;

    //  Create engine for accepted connection
    void create_engine (fd_t fd_, std::unique_ptr<boost::asio::ssl::context> ssl_context_);

    //  Close the acceptor
    void process_release_endpoint () ZLINK_OVERRIDE;

    //  Tune accepted socket
    int tune_socket (fd_t fd_) const;

    //  Apply accept filters to accepted connection
    bool apply_accept_filters (fd_t fd_, const struct sockaddr_storage &ss, socklen_t ss_len) const;

    //  Get socket name helper
    std::string get_socket_name (fd_t fd_, socket_end_t socket_end_) const;

    //  Reference to the io_context from asio_poller
    boost::asio::io_context &_io_context;

    //  The ASIO acceptor for listening
    boost::asio::ip::tcp::acceptor _acceptor;

    //  Address being listened on
    tcp_address_t _address;

    //  String representation of listening endpoint
    std::string _endpoint;

    //  Reference to the socket we belong to
    zlink::socket_base_t *const _socket;

    //  Number of outstanding async_accept operations.
    size_t _accepting_count;

    //  True once the bound endpoint has been released
    bool _terminating;

    //  Linger value saved from process_term
    int _linger;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_tls_listener_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL

#endif // __ZLINK_ASIO_TLS_LISTENER_HPP_INCLUDED__
