/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_TCP_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_TCP_TRANSPORT_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <boost/asio.hpp>

#include <memory>

#include "engine/asio/i_asio_transport.hpp"

namespace zlink
{

//  TCP transport implementation using Boost.Asio
//
//  Uses boost::asio::ip::tcp::socket with native handle assignment.

class tcp_transport_t : public i_asio_transport
{
  public:
    tcp_transport_t ();
    ~tcp_transport_t () ZLINK_OVERRIDE;

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

    void async_writev (const unsigned char *header,
                       std::size_t header_size,
                       const unsigned char *body,
                       std::size_t body_size,
                       completion_handler_t handler) ZLINK_OVERRIDE;

    std::size_t write_some (const std::uint8_t *data, std::size_t len) ZLINK_OVERRIDE;

    bool supports_speculative_read () const ZLINK_OVERRIDE;
    bool supports_speculative_write () const ZLINK_OVERRIDE;
    bool supports_gather_write () const ZLINK_OVERRIDE { return true; }

    const char *name () const ZLINK_OVERRIDE { return "tcp"; }

  private:
    std::shared_ptr<boost::asio::ip::tcp::socket> _socket;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (tcp_transport_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_TCP_TRANSPORT_HPP_INCLUDED__
