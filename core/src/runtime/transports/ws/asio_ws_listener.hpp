/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_WS_LISTENER_HPP_INCLUDED__
#define __ZLINK_ASIO_WS_LISTENER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_WS

#include <boost/asio.hpp>
#include <memory>
#include <string>

#include "utils/fd.hpp"
#include "core/own.hpp"
#include "utils/stdint.hpp"
#include "core/io_object.hpp"
#include "transports/ws/ws_address.hpp"

namespace zlink
{
class io_thread_t;
class socket_base_t;

//  ASIO-based WebSocket listener
//
//  This listener handles:
//  1. TCP accept using Boost.Asio async_accept
//  2. Engine creation with WebSocket transport for accepted connections
//
//  Note: The WebSocket handshake is handled by the engine via the transport,
//  not by the listener. This allows for proper async handshake processing
//  within the engine's lifecycle.

class asio_ws_listener_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    asio_ws_listener_t (zlink::io_thread_t *io_thread_,
                        zlink::socket_base_t *socket_,
                        const options_t &options_);
    ~asio_ws_listener_t ();

    //  Set address to listen on
    int set_local_address (const ws_address_t *addr_, bool secure_);

    //  Get the bound address
    int get_local_address (std::string &addr_) const;

  protected:
    std::string get_socket_name (fd_t fd_, socket_end_t socket_end_) const;

  private:
    //  Handlers for incoming commands
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_FINAL;

    //  Start accepting connections
    void start_accept ();

    //  Handle accept completion
    void on_accept (const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
                    const boost::system::error_code &ec);

    //  Create engine for accepted connection
    void create_engine (fd_t fd_);

    //  Close the listener
    void process_release_endpoint () ZLINK_OVERRIDE;

    //  Tune accepted socket
    int tune_socket (fd_t fd_) const;

    //  Apply accept filters
    bool apply_accept_filters (fd_t fd_, const struct sockaddr_storage &ss, socklen_t ss_len) const;

    //  Reference to io_context from asio_poller
    boost::asio::io_context &_io_context;

    //  TCP acceptor for incoming connections
    boost::asio::ip::tcp::acceptor _acceptor;

    //  Socket the listener belongs to
    zlink::socket_base_t *_socket;

    //  WebSocket address (host, path, port)
    std::string _host;
    std::string _path;
    uint16_t _port;
    bool _secure;

    //  String representation of endpoint
    std::string _endpoint;

    //  State flags
    size_t _accepting_count;
    bool _terminating;
    int _linger;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_ws_listener_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_WS

#endif // __ZLINK_ASIO_WS_LISTENER_HPP_INCLUDED__
