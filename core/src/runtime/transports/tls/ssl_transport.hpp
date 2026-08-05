/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SSL_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_SSL_TRANSPORT_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <string>

#include "engine/asio/i_asio_transport.hpp"

namespace zlink
{

//  SSL transport implementation using Boost.Asio SSL
//
//  This transport wraps a TCP socket with SSL/TLS encryption.
//  It implements the i_asio_transport interface for use with asio_engine_t.
//
//  Usage:
//    1. Create SSL context with certificate/key
//    2. Create ssl_transport_t with the context
//    3. Call open() to wrap an existing TCP socket
//    4. Call async_handshake() before data transfer
//    5. Use async_read_some/async_write_some for encrypted I/O

class ssl_transport_t : public i_asio_transport
{
  public:
    //  Handshake types matching boost::asio::ssl::stream_base
    enum handshake_type
    {
        client = 0,
        server = 1
    };

    //  Create SSL transport with given SSL context
    explicit ssl_transport_t (boost::asio::ssl::context &ssl_ctx);
    ~ssl_transport_t () ZLINK_OVERRIDE;

    //  i_asio_transport interface
    bool open (boost::asio::io_context &io_context, fd_t fd) ZLINK_OVERRIDE;
    bool is_open () const ZLINK_OVERRIDE;
    void close () ZLINK_OVERRIDE;

    void async_read_some (unsigned char *buffer,
                          std::size_t buffer_size,
                          completion_handler_t handler) ZLINK_OVERRIDE;

    std::size_t read_some (std::uint8_t *buffer, std::size_t len) ZLINK_OVERRIDE;

    void async_write_some (const unsigned char *buffer,
                           std::size_t buffer_size,
                           completion_handler_t handler) ZLINK_OVERRIDE;

    std::size_t write_some (const std::uint8_t *data, std::size_t len) ZLINK_OVERRIDE;

    //  SSL-specific overrides
    bool requires_handshake () const ZLINK_OVERRIDE { return true; }
    void async_handshake (int handshake_type, completion_handler_t handler) ZLINK_OVERRIDE;
    bool supports_speculative_write () const ZLINK_OVERRIDE { return false; }
    bool is_encrypted () const ZLINK_OVERRIDE { return true; }
    const char *name () const ZLINK_OVERRIDE { return "ssl"; }

    void set_hostname (const std::string &hostname) { _hostname = hostname; }

  private:
    typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_stream_t;

    boost::asio::ssl::context &_ssl_ctx;
    std::shared_ptr<ssl_stream_t> _ssl_stream;
    bool _handshake_complete;
    std::string _hostname;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ssl_transport_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL

#endif // __ZLINK_SSL_TRANSPORT_HPP_INCLUDED__
