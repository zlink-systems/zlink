/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_POLLER_HPP_INCLUDED__
#define __ZLINK_ASIO_POLLER_HPP_INCLUDED__

//  poller.hpp decides which polling mechanism to use.
#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <vector>
#include <map>
#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include "utils/fd.hpp"
#include "core/thread.hpp"
#include "core/poller_base.hpp"

namespace zlink
{
struct i_poll_events;

//  This class implements socket polling mechanism using Boost.Asio.
//  It uses the reactor pattern via async_wait to maintain compatibility
//  with existing zlink engine code.

class asio_poller_t ZLINK_FINAL : public worker_poller_base_t
{
  public:
    typedef void *handle_t;

    asio_poller_t (const thread_ctx_t &ctx_);
    ~asio_poller_t () ZLINK_OVERRIDE;

    //  "poller" concept.
    handle_t add_fd (fd_t fd_, zlink::i_poll_events *events_);
    handle_t add_tcp_socket (boost::asio::ip::tcp::socket *socket_, zlink::i_poll_events *events_);
#if defined ZLINK_HAVE_IPC
    handle_t add_ipc_socket (boost::asio::local::stream_protocol::socket *socket_,
                             zlink::i_poll_events *events_);
#endif
    void rm_fd (handle_t handle_);
    void rm_socket (handle_t handle_);
    void set_pollin (handle_t handle_);
    void reset_pollin (handle_t handle_);
    void set_pollout (handle_t handle_);
    void reset_pollout (handle_t handle_);
    void stop ();

    static int max_fds ();

    //  Get access to the io_context for ASIO-based operations
    //  (used by asio_tcp_listener, asio_tcp_connecter, and other ASIO components)
    boost::asio::io_context &get_io_context () { return _io_context; }

  private:
    //  Main event loop.
    void loop () ZLINK_OVERRIDE;

    //  Poll entry structure for tracking FD state
    struct poll_entry_t
    {
        enum socket_type_t
        {
            socket_type_none,
            socket_type_tcp,
            socket_type_ipc,
            socket_type_fd
        };

        socket_type_t type;
        void *socket;
        zlink::i_poll_events *events;
        bool pollin_enabled;
        bool pollout_enabled;
        bool in_event_pending;
        bool out_event_pending;

        poll_entry_t (socket_type_t type_, void *socket_);
    };

    //  Start async_wait for read readiness
    void start_wait_read (poll_entry_t *entry_);

    //  Start async_wait for write readiness
    void start_wait_write (poll_entry_t *entry_);

    //  Cancel pending async operations for an entry
    void cancel_ops (poll_entry_t *entry_);

    //  The Asio io_context that drives the event loop
    boost::asio::io_context _io_context;

    //  Work guard to keep io_context running even when no handlers are pending
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work_guard;

    //  List of retired event sources (to be deleted after loop iteration)
    typedef std::vector<poll_entry_t *> retired_t;
    retired_t _retired;

    //  Active poll entries (Windows uses WSAPoll for readiness checks)
    std::vector<poll_entry_t *> _entries;

    //  Flag to track stopping state
    bool _stopping;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_poller_t)
};

typedef asio_poller_t poller_t;
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_ASIO_POLLER_HPP_INCLUDED__
