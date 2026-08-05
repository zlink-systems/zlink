/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_TCP_LISTENER_HPP_INCLUDED__
#define __ZLINK_ASIO_TCP_LISTENER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <boost/asio.hpp>
#include <memory>
#include <string>

#include "utils/fd.hpp"
#include "core/own.hpp"
#include "utils/stdint.hpp"
#include "core/io_object.hpp"
#include "transports/tcp/tcp_address.hpp"

namespace zlink
{
class io_thread_t;
class socket_base_t;

//  ASIO-based TCP listener using async_accept for connection handling.
//  Connections are handled using true proactor mode with asio_zmp_engine.

class asio_tcp_listener_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    asio_tcp_listener_t (zlink::io_thread_t *io_thread_,
                         zlink::socket_base_t *socket_,
                         const options_t &options_);
    ~asio_tcp_listener_t ();

    //  Set address to listen on.
    int set_local_address (const char *addr_);

    //  Get the bound address for use with wildcards
    int get_local_address (std::string &addr_) const;

  protected:
    std::string get_socket_name (fd_t fd_, socket_end_t socket_end_) const;

  private:
    //  Handlers for incoming commands.
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_FINAL;

    //  Start accepting incoming connections
    void start_accept ();

    //  Handle accept completion
    void on_accept (const std::shared_ptr<boost::asio::ip::tcp::socket> &accept_socket_,
                    const boost::system::error_code &ec);

    //  Create engine for accepted connection
    void create_engine (fd_t fd_);

    //  Close the listening socket
    void close ();

    //  Tune accepted socket
    int tune_socket (fd_t fd_) const;

    //  Apply accept filters to socket
    bool apply_accept_filters (fd_t fd_, const struct sockaddr_storage &ss, socklen_t ss_len) const;

    //  Reference to the io_context from asio_poller
    boost::asio::io_context &_io_context;

    //  The ASIO acceptor for handling incoming connections
    boost::asio::ip::tcp::acceptor _acceptor;

    //  Socket the listener belongs to.
    zlink::socket_base_t *_socket;

    //  Address to listen on.
    tcp_address_t _address;

    //  String representation of endpoint to bind to
    std::string _endpoint;

    //  Number of outstanding async_accept operations.
    size_t _accepting_count;

    //  True if process_term has been called
    bool _terminating;

    //  Linger value saved from process_term for deferred termination
    int _linger;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_tcp_listener_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_ASIO_TCP_LISTENER_HPP_INCLUDED__
