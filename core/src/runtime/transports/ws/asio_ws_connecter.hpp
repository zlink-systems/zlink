/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_WS_CONNECTER_HPP_INCLUDED__
#define __ZLINK_ASIO_WS_CONNECTER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_WS

#include <boost/asio.hpp>
#include <string>

#include "utils/fd.hpp"
#include "core/own.hpp"
#include "core/io_object.hpp"

namespace zlink
{
class io_thread_t;
class session_base_t;
struct address_t;

//  ASIO-based WebSocket connecter (ws/wss)
//
//  This connecter handles:
//  1. TCP async_connect using Boost.Asio
//  2. Engine creation with WebSocket transport for the connected socket
//
//  Note: The WebSocket handshake is handled by the engine via the transport,
//  not by the connecter. This follows the pattern used in asio_tcp_connecter.

class asio_ws_connecter_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    //  If 'delayed_start' is true, connecter first waits before connecting
    asio_ws_connecter_t (zlink::io_thread_t *io_thread_,
                         zlink::session_base_t *session_,
                         const options_t &options_,
                         address_t *addr_,
                         bool delayed_start_);
    ~asio_ws_connecter_t ();

  private:
    //  Timer IDs
    enum
    {
        reconnect_timer_id = 1,
        connect_timer_id = 2
    };

    //  Handlers for incoming commands
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_OVERRIDE;

    //  Timer event handler
    void timer_event (int id_) ZLINK_OVERRIDE;

    //  Start connection process
    void start_connecting ();

    //  Handle connect completion
    void on_connect (const boost::system::error_code &ec);

    //  Timer management
    void add_connect_timer ();
    void add_reconnect_timer ();
    int get_new_reconnect_ivl ();

    //  Create engine for connected socket
    void create_engine (fd_t fd_, const std::string &local_address_);

    //  Tune the connected socket
    bool tune_socket (fd_t fd_);

    //  Close and cleanup
    void close ();

    //  Reference to io_context
    boost::asio::io_context &_io_context;

    //  TCP socket for connecting
    boost::asio::ip::tcp::socket _socket;

    //  Target endpoint
    boost::asio::ip::tcp::endpoint _endpoint;

    //  Address to connect to (owned by session_base_t)
    address_t *const _addr;

    //  String representation of endpoint
    std::string _endpoint_str;

    //  Session to attach engine to
    zlink::session_base_t *const _session;

    //  Socket
    zlink::socket_base_t *const _socket_ptr;

    //  WebSocket-specific data
    std::string _host;
    std::string _path;
    std::string _tls_hostname;
    bool _secure;

    //  State flags
    const bool _delayed_start;
    bool _reconnect_timer_started;
    bool _connect_timer_started;
    bool _connecting;
    bool _terminating;
    int _linger;
    int _current_reconnect_ivl;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_ws_connecter_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_WS

#endif // __ZLINK_ASIO_WS_CONNECTER_HPP_INCLUDED__
