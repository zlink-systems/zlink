/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_IPC_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_ASIO_IPC_TRANSPORT_HPP_INCLUDED__

#include "engine/asio/i_asio_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <memory>

namespace zlink
{
class ipc_transport_t ZLINK_FINAL : public i_asio_transport
{
  public:
    ipc_transport_t ();
    ~ipc_transport_t ();

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

    const char *name () const ZLINK_OVERRIDE { return "ipc_transport"; }

  private:
    std::shared_ptr<boost::asio::local::stream_protocol::socket> _socket;
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC

#endif // __ZLINK_ASIO_IPC_TRANSPORT_HPP_INCLUDED__
