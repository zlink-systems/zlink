/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_IPC_LISTENER_HPP_INCLUDED__
#define __ZLINK_ASIO_IPC_LISTENER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <memory>
#include <string>

#include "utils/fd.hpp"
#include "core/own.hpp"
#include "core/io_object.hpp"

namespace zlink
{
class io_thread_t;
class socket_base_t;

//  ASIO-based IPC listener using local stream sockets.
class asio_ipc_listener_t ZLINK_FINAL : public own_t, public io_object_t
{
  public:
    asio_ipc_listener_t (zlink::io_thread_t *io_thread_,
                         zlink::socket_base_t *socket_,
                         const options_t &options_);
    ~asio_ipc_listener_t ();

    //  Set address to listen on.
    int set_local_address (const char *addr_);

    //  Get last bound endpoint.
    int get_local_address (std::string &addr_) const;

  private:
    void process_plug () ZLINK_FINAL;
    void process_term (int linger_) ZLINK_OVERRIDE;

    void start_accept ();
    void
    on_accept (const std::shared_ptr<boost::asio::local::stream_protocol::socket> &accept_socket_,
               const boost::system::error_code &ec);

    void create_engine (fd_t fd_);
    void close ();

    bool apply_accept_filters (fd_t fd_);

#if defined ZLINK_HAVE_SO_PEERCRED || defined ZLINK_HAVE_LOCAL_PEERCRED
    bool filter (fd_t sock_);
#endif

    boost::asio::io_context &_io_context;
    boost::asio::local::stream_protocol::acceptor _acceptor;

    zlink::socket_base_t *const _socket;

    std::string _endpoint;
    size_t _accepting_count;
    bool _terminating;
    int _linger;

    bool _has_file;
    std::string _tmp_socket_dirname;
    std::string _filename;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_ipc_listener_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC

#endif // __ZLINK_ASIO_IPC_LISTENER_HPP_INCLUDED__
