/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_IPC_CONNECTER_HPP_INCLUDED__
#define __ZLINK_ASIO_IPC_CONNECTER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <string>

#include "utils/fd.hpp"
#include "core/endpoint.hpp"
#include "core/own.hpp"
#include "core/io_object.hpp"

namespace zlink
{
class io_thread_t;
class session_base_t;
struct address_t;

//  ASIO-based IPC connecter using local stream sockets.
class asio_ipc_connecter_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    asio_ipc_connecter_t (zlink::io_thread_t *io_thread_,
                          zlink::session_base_t *session_,
                          const options_t &options_,
                          address_t *addr_,
                          bool delayed_start_);
    ~asio_ipc_connecter_t ();

  private:
    enum
    {
        reconnect_timer_id = 1,
        connect_timer_id = 2
    };

    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_OVERRIDE;
    void timer_event (int id_) ZLINK_OVERRIDE;

    void start_connecting ();
    void on_connect (const boost::system::error_code &ec);

    void add_connect_timer ();
    void add_reconnect_timer ();
    int get_new_reconnect_ivl ();

    void create_engine (fd_t fd_, const std::string &local_address_);

    void close ();

    boost::asio::io_context &_io_context;
    boost::asio::local::stream_protocol::socket _socket;
    boost::asio::local::stream_protocol::endpoint _endpoint;

    address_t *const _addr;
    std::string _endpoint_str;
    //  Shared by every monitor event emitted for one physical connect attempt.
    endpoint_uri_pair_t _attempt_endpoint_pair;

    zlink::session_base_t *const _session;
    zlink::socket_base_t *const _socket_ptr;

    const bool _delayed_start;
    bool _reconnect_timer_started;
    bool _connect_timer_started;
    bool _connecting;
    bool _terminating;
    int _linger;
    int _current_reconnect_ivl;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_ipc_connecter_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC

#endif // __ZLINK_ASIO_IPC_CONNECTER_HPP_INCLUDED__
