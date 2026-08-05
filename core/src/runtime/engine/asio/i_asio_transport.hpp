/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_I_ASIO_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_I_ASIO_TRANSPORT_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace zlink
{

//  Transport abstraction interface for ASIO-based engines.
//
//  This interface abstracts the underlying stream transport (TCP, SSL, WebSocket)
//  allowing asio_engine_t to be generic over different transport types.
//
//  Supported transports:
//  - tcp_transport_t: TCP socket transport
//  - ssl_transport_t: SSL/TLS encrypted transport
//  - ws_transport_t: WebSocket transport
//  - wss_transport_t: WebSocket over SSL/TLS transport
//
//  Design rationale:
//  - Uses boost::asio::mutable_buffer/const_buffer for efficient buffer handling
//  - Completion callbacks use std::function for type erasure
//  - Transport owns the underlying socket/stream object
//  - Engine manages buffer lifecycle, transport handles I/O

class i_asio_transport
{
  public:
    //  Callback type for async operation completion
    typedef std::function<void (const boost::system::error_code &, std::size_t)>
      completion_handler_t;

    virtual ~i_asio_transport () = default;

    //  Open the transport with the given io_context and file descriptor.
    //  Returns true on success, false on failure.
    //  For TCP: assigns the fd to the socket
    //  For SSL: wraps socket in SSL stream
    //  For WS: wraps socket in Beast WebSocket stream
    virtual bool open (boost::asio::io_context &io_context, fd_t fd) = 0;

    //  Check if transport is open and ready for I/O
    virtual bool is_open () const = 0;

    //  Close the transport.
    //  Gracefully shuts down the connection if possible.
    virtual void close () = 0;

    //  Start async read operation.
    //  Reads up to buffer_size bytes into buffer.
    //  Calls handler on completion with error code and bytes transferred.
    virtual void async_read_some (unsigned char *buffer,
                                  std::size_t buffer_size,
                                  completion_handler_t handler) = 0;

    //  Synchronous read operation for speculative reads.
    //
    //  Attempts to read up to len bytes into buffer synchronously.
    //
    //  Returns:
    //    On success: Number of bytes actually read (may be less than len)
    //    On would_block: 0, with errno set to EAGAIN or EWOULDBLOCK
    //    On error: 0, with errno set to appropriate error code
    //
    //  Notes:
    //    - For TCP/TLS/IPC: Non-blocking read_some on the underlying socket
    //    - For WebSocket: May return EAGAIN if no buffered frame data exists
    //    - Caller must check errno when return value is 0
    virtual std::size_t read_some (std::uint8_t *buffer, std::size_t len) = 0;

    //  Indicates whether read_some() is guaranteed non-blocking and safe for
    //  speculative hot-path drains from completion handlers.
    virtual bool supports_speculative_read () const { return false; }

    //  Start async write operation.
    //  Writes up to buffer_size bytes from buffer.
    //  Calls handler on completion with error code and bytes written.
    virtual void async_write_some (const unsigned char *buffer,
                                   std::size_t buffer_size,
                                   completion_handler_t handler) = 0;

    //  Synchronous write operation for speculative writes.
    //
    //  Attempts to write up to len bytes from data buffer synchronously.
    //  This method is designed for non-blocking sockets and supports the
    //  speculative write optimization pattern.
    //
    //  Parameters:
    //    data: Pointer to data buffer to write
    //    len: Number of bytes to write
    //
    //  Returns:
    //    On success: Number of bytes actually written (may be less than len)
    //    On would_block: 0, with errno set to EAGAIN or EWOULDBLOCK
    //    On error: 0, with errno set to appropriate error code
    //
    //  Notes:
    //    - For TCP/TLS: Performs partial write, returns bytes written
    //    - For WebSocket: Frame-based, writes complete frame or returns 0
    //    - Caller must check errno when return value is 0 to distinguish
    //      would_block from actual errors
    //    - Must be called only after handshake is complete (if required)
    virtual std::size_t write_some (const std::uint8_t *data, std::size_t len) = 0;

    //  Indicates whether the transport supports speculative synchronous writes.
    //  Transports can opt out to force async write paths (e.g., IPC stability).
    virtual bool supports_speculative_write () const { return true; }

    //  Indicates whether the transport supports async gather writes.
    //  Default: false (unsupported).
    virtual bool supports_gather_write () const { return false; }

    //  Async gather write (header + body).
    //  Default: not supported; handler receives operation_not_supported.
    virtual void async_writev (const unsigned char *header,
                               std::size_t header_size,
                               const unsigned char *body,
                               std::size_t body_size,
                               completion_handler_t handler)
    {
        if (handler) {
            handler (boost::asio::error::operation_not_supported, 0);
        }
    }

    //  Check if this transport requires a handshake phase.
    //  TCP: false, SSL: true, WebSocket: true
    virtual bool requires_handshake () const { return false; }

    //  Start async handshake for transports that require it (SSL, WebSocket).
    //  For TCP, this is a no-op that immediately calls handler with success.
    //  handshake_type: 0 = client, 1 = server
    virtual void async_handshake (int handshake_type, completion_handler_t handler)
    {
        //  Default: no handshake needed, succeed immediately
        if (handler) {
            handler (boost::system::error_code (), 0);
        }
    }

    //  Check if transport is encrypted.
    //  TCP: false, SSL: true, WSS: true, WS: false
    virtual bool is_encrypted () const { return false; }

    //  Get transport name for debugging
    virtual const char *name () const = 0;
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_I_ASIO_TRANSPORT_HPP_INCLUDED__
