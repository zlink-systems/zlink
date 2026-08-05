/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include "transports/ipc/asio_ipc_listener.hpp"
#include "engine/asio/asio_poller.hpp"
#include "engine/asio/asio_zmp_engine.hpp"
#include "engine/asio/asio_raw_engine.hpp"
#include "transports/ipc/ipc_transport.hpp"
#include "core/address.hpp"
#include "utils/err.hpp"
#include "core/io_thread.hpp"
#include "transports/asio/asio_listener_accept_policy.hpp"
#include "transports/ipc/ipc_address.hpp"
#include "utils/ip.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"

#include <cerrno>
#include <string.h>
#include <memory>

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stddef.h>
#endif

#ifdef ZLINK_HAVE_LOCAL_PEERCRED
#include <sys/types.h>
#include <sys/ucred.h>
#endif
#ifdef ZLINK_HAVE_SO_PEERCRED
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#if defined ZLINK_HAVE_OPENBSD
#define ucred sockpeercred
#endif
#endif

// Debug logging for ASIO IPC listener - set to 1 to enable
#define ASIO_IPC_LISTENER_DEBUG 0

#if ASIO_IPC_LISTENER_DEBUG
#include <cstdio>
#define IPC_LISTENER_DBG(fmt, ...) fprintf (stderr, "[ASIO_IPC_LISTENER] " fmt "\n", ##__VA_ARGS__)
#else
#define IPC_LISTENER_DBG(fmt, ...)
#endif

namespace
{
void cleanup_tmp_dir (std::string &tmp_dir_)
{
    if (tmp_dir_.empty ())
        return;

    ::rmdir (tmp_dir_.c_str ());
    tmp_dir_.clear ();
}

boost::asio::local::stream_protocol::endpoint make_ipc_endpoint (const zlink::ipc_address_t &addr_)
{
    boost::asio::local::stream_protocol::endpoint endpoint;
    memcpy (endpoint.data (), addr_.addr (), addr_.addrlen ());
    endpoint.resize (addr_.addrlen ());
    return endpoint;
}

int unlink_existing_ipc_socket (const std::string &addr_)
{
    if (addr_.empty () || addr_[0] == '@')
        return 0;

    struct stat st;
    if (::lstat (addr_.c_str (), &st) != 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    if (!S_ISSOCK (st.st_mode)) {
        errno = EADDRINUSE;
        return -1;
    }

    if (st.st_uid != ::getuid ()) {
        errno = EACCES;
        return -1;
    }

    return ::unlink (addr_.c_str ());
}

}

zlink::asio_ipc_listener_t::asio_ipc_listener_t (io_thread_t *io_thread_,
                                                 socket_base_t *socket_,
                                                 const options_t &options_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    _io_context (io_thread_->get_io_context ()),
    _acceptor (_io_context),
    _socket (socket_),
    _accepting_count (0),
    _terminating (false),
    _linger (0),
    _has_file (false)
{
    IPC_LISTENER_DBG ("Constructor called, this=%p", static_cast<void *> (this));
}

zlink::asio_ipc_listener_t::~asio_ipc_listener_t ()
{
    IPC_LISTENER_DBG ("Destructor called, this=%p", static_cast<void *> (this));
}

int zlink::asio_ipc_listener_t::set_local_address (const char *addr_)
{
    IPC_LISTENER_DBG ("set_local_address: addr=%s", addr_);

    std::string addr (addr_);

    if (!addr.empty () && addr[0] == '*') {
        if (create_ipc_wildcard_address (_tmp_socket_dirname, addr) < 0)
            return -1;
    }

    ipc_address_t address;
    int rc = address.resolve (addr.c_str ());
    if (rc != 0) {
        const int tmp_errno = errno;
        cleanup_tmp_dir (_tmp_socket_dirname);
        errno = tmp_errno;
        return -1;
    }
    if (unlink_existing_ipc_socket (addr) != 0) {
        const int tmp_errno = errno;
        cleanup_tmp_dir (_tmp_socket_dirname);
        errno = tmp_errno;
        return -1;
    }
    _filename.clear ();

    std::string resolved_endpoint;
    address.to_string (resolved_endpoint);

    boost::system::error_code ec;
    _acceptor.open (boost::asio::local::stream_protocol (), ec);
    if (ec) {
        const int tmp_errno = ec.value ();
        cleanup_tmp_dir (_tmp_socket_dirname);
        errno = tmp_errno;
        return -1;
    }

    boost::asio::local::stream_protocol::endpoint bind_endpoint = make_ipc_endpoint (address);

    _acceptor.bind (bind_endpoint, ec);
    if (ec) {
        const int tmp_errno = ec.value ();
        _acceptor.close (ec);
        cleanup_tmp_dir (_tmp_socket_dirname);
        errno = tmp_errno;
        return -1;
    }

    _filename = addr;
    _has_file = true;

    _acceptor.listen (options.backlog, ec);
    if (ec) {
        const int tmp_errno = ec.value ();
        close ();
        errno = tmp_errno;
        return -1;
    }

    if (_filename.empty ()) {
        _filename = addr;
        _has_file = true;
    }

    _endpoint = get_socket_name<ipc_address_t> (_acceptor.native_handle (), socket_end_local);
    if (_endpoint.empty ())
        _endpoint = resolved_endpoint;

    _socket->event_listening (make_unconnected_bind_endpoint_pair (_endpoint),
                              _acceptor.native_handle ());

    return 0;
}

int zlink::asio_ipc_listener_t::get_local_address (std::string &addr_) const
{
    addr_ = _endpoint;
    return addr_.empty () ? -1 : 0;
}

void zlink::asio_ipc_listener_t::process_plug ()
{
    IPC_LISTENER_DBG ("process_plug called");
    start_accept ();
}

void zlink::asio_ipc_listener_t::process_term (int linger_)
{
    IPC_LISTENER_DBG ("process_term called, linger=%d, accepting=%zu", linger_, _accepting_count);

    _terminating = true;
    _linger = linger_;

    close ();

    drain_asio_listener_pending_accepts (_io_context, &_accepting_count);

    own_t::process_term (linger_);
}

void zlink::asio_ipc_listener_t::start_accept ()
{
    start_asio_listener_accepts<boost::asio::local::stream_protocol::socket> (
      _io_context, _acceptor, &_accepting_count, options,
      [] (size_t accepting_count_, size_t target_accepts_) {
          IPC_LISTENER_DBG ("start_accept: starting async_accept (%zu/%zu)", accepting_count_,
                            target_accepts_);
      },
      [this] (const std::shared_ptr<boost::asio::local::stream_protocol::socket> &accept_socket_,
              const boost::system::error_code &ec_) { on_accept (accept_socket_, ec_); });
}

void zlink::asio_ipc_listener_t::on_accept (
  const std::shared_ptr<boost::asio::local::stream_protocol::socket> &accept_socket_,
  const boost::system::error_code &ec)
{
    if (_accepting_count > 0)
        --_accepting_count;
    IPC_LISTENER_DBG ("on_accept: ec=%s, terminating=%d, pending=%zu", ec.message ().c_str (),
                      _terminating, _accepting_count);

    if (_terminating) {
        if (!ec && accept_socket_ && accept_socket_->is_open ()) {
            boost::system::error_code close_ec;
            accept_socket_->close (close_ec);
        }
        return;
    }

    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            start_accept ();
            return;
        }

        _socket->event_accept_failed (make_unconnected_bind_endpoint_pair (_endpoint), ec.value ());

        start_accept ();
        return;
    }

    const fd_t fd = accept_socket_->native_handle ();

    if (!apply_accept_filters (fd)) {
        boost::system::error_code close_ec;
        accept_socket_->close (close_ec);
        start_accept ();
        return;
    }

    accept_socket_->release ();

    create_engine (fd);

    start_accept ();
}

void zlink::asio_ipc_listener_t::create_engine (fd_t fd_)
{
    IPC_LISTENER_DBG ("create_engine: fd=%d", fd_);

    const endpoint_uri_pair_t endpoint_pair (
      get_socket_name<ipc_address_t> (fd_, socket_end_local),
      get_socket_name<ipc_address_t> (fd_, socket_end_remote), endpoint_type_bind);

    std::unique_ptr<i_asio_transport> transport (new (std::nothrow) ipc_transport_t ());
    alloc_assert (transport.get ());

    i_engine *engine = NULL;
    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        engine =
          new (std::nothrow) asio_raw_engine_t (fd_, options, endpoint_pair, std::move (transport));
    } else {
        engine =
          new (std::nothrow) asio_zmp_engine_t (fd_, options, endpoint_pair, std::move (transport));
    }
    alloc_assert (engine);

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zlink_assert (io_thread);

    session_base_t *session = session_base_t::create (io_thread, false, _socket, options, NULL);
    errno_assert (session);
    session->inc_seqnum ();
    launch_child (session);
    send_attach (session, engine, false);

    _socket->event_accepted (endpoint_pair, fd_);
}

void zlink::asio_ipc_listener_t::close ()
{
    if (!_acceptor.is_open ())
        return;

    const fd_t fd_for_event = _acceptor.native_handle ();
    boost::system::error_code ec;
    _acceptor.close (ec);

    if (_has_file) {
        int rc = 0;
        if (!_tmp_socket_dirname.empty ()) {
            rc = ::unlink (_filename.c_str ());
            if (rc == 0) {
                rc = ::rmdir (_tmp_socket_dirname.c_str ());
                _tmp_socket_dirname.clear ();
            }
        }

        if (rc != 0) {
            _socket->event_close_failed (make_unconnected_bind_endpoint_pair (_endpoint),
                                         zlink_errno ());
            return;
        }
    }

    _socket->event_closed (make_unconnected_bind_endpoint_pair (_endpoint), fd_for_event);
}

bool zlink::asio_ipc_listener_t::apply_accept_filters (fd_t fd_)
{
    make_socket_noninheritable (fd_);

#if defined ZLINK_HAVE_SO_PEERCRED || defined ZLINK_HAVE_LOCAL_PEERCRED
    if (!filter (fd_))
        return false;
#endif

    if (zlink::set_nosigpipe (fd_))
        return false;

    return true;
}

#if defined ZLINK_HAVE_SO_PEERCRED

bool zlink::asio_ipc_listener_t::filter (fd_t sock_)
{
    if (options.ipc_uid_accept_filters.empty () && options.ipc_pid_accept_filters.empty ()
        && options.ipc_gid_accept_filters.empty ())
        return true;

    struct ucred cred;
    socklen_t size = sizeof (cred);

    if (getsockopt (sock_, SOL_SOCKET, SO_PEERCRED, &cred, &size))
        return false;
    if (options.ipc_uid_accept_filters.find (cred.uid) != options.ipc_uid_accept_filters.end ()
        || options.ipc_gid_accept_filters.find (cred.gid) != options.ipc_gid_accept_filters.end ()
        || options.ipc_pid_accept_filters.find (cred.pid) != options.ipc_pid_accept_filters.end ())
        return true;

    const struct passwd *pw;
    const struct group *gr;

    if (!(pw = getpwuid (cred.uid)))
        return false;
    for (options_t::ipc_gid_accept_filters_t::const_iterator
           it = options.ipc_gid_accept_filters.begin (),
           end = options.ipc_gid_accept_filters.end ();
         it != end; it++) {
        if (!(gr = getgrgid (*it)))
            continue;
        for (char **mem = gr->gr_mem; *mem; mem++) {
            if (!strcmp (*mem, pw->pw_name))
                return true;
        }
    }
    return false;
}

#elif defined ZLINK_HAVE_LOCAL_PEERCRED

bool zlink::asio_ipc_listener_t::filter (fd_t sock_)
{
    if (options.ipc_uid_accept_filters.empty () && options.ipc_gid_accept_filters.empty ())
        return true;

    struct xucred cred;
    socklen_t size = sizeof (cred);

    if (getsockopt (sock_, 0, LOCAL_PEERCRED, &cred, &size))
        return false;
    if (cred.cr_version != XUCRED_VERSION)
        return false;
    if (options.ipc_uid_accept_filters.find (cred.cr_uid) != options.ipc_uid_accept_filters.end ())
        return true;
    for (int i = 0; i < cred.cr_ngroups; i++) {
        if (options.ipc_gid_accept_filters.find (cred.cr_groups[i])
            != options.ipc_gid_accept_filters.end ())
            return true;
    }

    return false;
}

#endif

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC
