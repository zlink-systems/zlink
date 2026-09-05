/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_TCP_CONNECTER_HPP_INCLUDED__
#define __ZLINK_ASIO_TCP_CONNECTER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <boost/asio.hpp>
#include <string>

#include "utils/fd.hpp"
#include "core/endpoint.hpp"
#include "core/own.hpp"
#include "utils/stdint.hpp"
#include "core/io_object.hpp"

namespace zlink
{
class io_thread_t;
class session_base_t;
struct address_t;

//  ASIO-based TCP connecter using async_connect for connection handling.
//  Connections are handled using true proactor mode with asio_zmp_engine.

class asio_tcp_connecter_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    //  If 'delayed_start' is true connecter first waits for a while,
    //  then starts connection process.
    asio_tcp_connecter_t (zlink::io_thread_t *io_thread_,
                          zlink::session_base_t *session_,
                          const options_t &options_,
                          address_t *addr_,
                          bool delayed_start_);
    ~asio_tcp_connecter_t ();

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
    //  These are provided for timer_event from poller_base.
    void timer_event (int id_) ZLINK_OVERRIDE;

    //  Internal function to start the actual connection establishment.
    void start_connecting ();

    //  Handle connect completion
    void on_connect (const boost::system::error_code &ec);

    //  Internal function to add a connect timer
    void add_connect_timer ();

    //  Internal function to add a reconnect timer
    void add_reconnect_timer ();

    //  Internal function to return a reconnect backoff delay.
    //  Will modify the current_reconnect_ivl used for next call
    //  Returns the currently used interval
    int get_new_reconnect_ivl ();

    //  Create engine for connected socket
    void create_engine (fd_t fd_, const std::string &local_address_);

    //  Tune the connected socket
    bool tune_socket (fd_t fd_);

    //  Close the socket
    void close ();

    //  Reference to the io_context from asio_poller
    boost::asio::io_context &_io_context;

    //  The ASIO socket for connecting
    boost::asio::ip::tcp::socket _socket;

    //  Target endpoint for connection
    boost::asio::ip::tcp::endpoint _endpoint;

    //  Address to connect to. Owned by session_base_t.
    address_t *const _addr;

    //  String representation of endpoint to connect to
    std::string _endpoint_str;
    //  Shared by every monitor event emitted for one physical connect attempt.
    endpoint_uri_pair_t _attempt_endpoint_pair;

    //  Reference to the session we belong to.
    zlink::session_base_t *const _session;

    //  Socket
    zlink::socket_base_t *const _socket_ptr;

    //  If true, connecter is waiting a while before trying to connect.
    const bool _delayed_start;

    //  True iff a timer has been started.
    bool _reconnect_timer_started;
    bool _connect_timer_started;

    //  True iff an async_connect is in progress
    bool _connecting;

    //  True if process_term has been called
    bool _terminating;

    //  Linger value saved from process_term for deferred termination
    int _linger;

    //  Current reconnect ivl, updated for backoff strategy
    int _current_reconnect_ivl;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_tcp_connecter_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_ASIO_TCP_CONNECTER_HPP_INCLUDED__
