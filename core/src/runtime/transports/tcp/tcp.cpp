/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "utils/ip.hpp"
#include "transports/tcp/tcp.hpp"
#include "utils/err.hpp"

#if !defined ZLINK_HAVE_WINDOWS
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#ifdef ZLINK_HAVE_VXWORKS
#include <sockLib.h>
#endif
#endif

#if defined ZLINK_HAVE_OPENVMS
#include <ioctl.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

int zlink::tune_tcp_socket (fd_t s_, int tcp_nodelay_)
{
    int rc = 0;
    if (tcp_nodelay_ != -1) {
        int nodelay = tcp_nodelay_ != 0 ? 1 : 0;
        rc = setsockopt (s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *> (&nodelay),
                         sizeof (int));
        assert_success_or_recoverable (s_, rc);
        if (rc != 0)
            return rc;
    }

#ifdef ZLINK_HAVE_OPENVMS
    //  Disable delayed acknowledgements as they hurt latency significantly.
    if (tcp_nodelay_ != 0) {
        int nodelack = 1;
        rc = setsockopt (s_, IPPROTO_TCP, TCP_NODELACK, (char *) &nodelack, sizeof (int));
        assert_success_or_recoverable (s_, rc);
    }
#endif
    return rc;
}

int zlink::set_tcp_send_buffer (fd_t sockfd_, int bufsize_)
{
    const int rc = setsockopt (sockfd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *> (&bufsize_),
                               sizeof bufsize_);
    assert_success_or_recoverable (sockfd_, rc);
    return rc;
}

int zlink::set_tcp_receive_buffer (fd_t sockfd_, int bufsize_)
{
    const int rc = setsockopt (sockfd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *> (&bufsize_),
                               sizeof bufsize_);
    assert_success_or_recoverable (sockfd_, rc);
    return rc;
}

int zlink::tune_tcp_keepalives (
  fd_t s_, int keepalive_, int keepalive_cnt_, int keepalive_idle_, int keepalive_intvl_)
{
    // These options are used only under certain #ifdefs below.
    LIBZLINK_UNUSED (keepalive_);
    LIBZLINK_UNUSED (keepalive_cnt_);
    LIBZLINK_UNUSED (keepalive_idle_);
    LIBZLINK_UNUSED (keepalive_intvl_);

    // If none of the #ifdefs apply, then s_ is unused.
    LIBZLINK_UNUSED (s_);

    //  Tuning TCP keep-alives if platform allows it
    //  All values = -1 means skip and leave it for OS
#ifdef ZLINK_HAVE_WINDOWS
    if (keepalive_ != -1) {
        tcp_keepalive keepalive_opts;
        keepalive_opts.onoff = keepalive_;
        keepalive_opts.keepalivetime = keepalive_idle_ != -1 ? keepalive_idle_ * 1000 : 7200000;
        keepalive_opts.keepaliveinterval = keepalive_intvl_ != -1 ? keepalive_intvl_ * 1000 : 1000;
        DWORD num_bytes_returned;
        const int rc = WSAIoctl (s_, SIO_KEEPALIVE_VALS, &keepalive_opts, sizeof (keepalive_opts),
                                 NULL, 0, &num_bytes_returned, NULL, NULL);
        assert_success_or_recoverable (s_, rc);
        if (rc == SOCKET_ERROR)
            return rc;
    }
#else
#ifdef ZLINK_HAVE_SO_KEEPALIVE
    if (keepalive_ != -1) {
        int rc = setsockopt (s_, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char *> (&keepalive_),
                             sizeof (int));
        assert_success_or_recoverable (s_, rc);
        if (rc != 0)
            return rc;

#ifdef ZLINK_HAVE_TCP_KEEPCNT
        if (keepalive_cnt_ != -1) {
            int rc = setsockopt (s_, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_cnt_, sizeof (int));
            assert_success_or_recoverable (s_, rc);
            if (rc != 0)
                return rc;
        }
#endif // ZLINK_HAVE_TCP_KEEPCNT

#ifdef ZLINK_HAVE_TCP_KEEPIDLE
        if (keepalive_idle_ != -1) {
            int rc = setsockopt (s_, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle_, sizeof (int));
            assert_success_or_recoverable (s_, rc);
            if (rc != 0)
                return rc;
        }
#else // ZLINK_HAVE_TCP_KEEPIDLE
#ifdef ZLINK_HAVE_TCP_KEEPALIVE
        if (keepalive_idle_ != -1) {
            int rc = setsockopt (s_, IPPROTO_TCP, TCP_KEEPALIVE, &keepalive_idle_, sizeof (int));
            assert_success_or_recoverable (s_, rc);
            if (rc != 0)
                return rc;
        }
#endif // ZLINK_HAVE_TCP_KEEPALIVE
#endif // ZLINK_HAVE_TCP_KEEPIDLE

#ifdef ZLINK_HAVE_TCP_KEEPINTVL
        if (keepalive_intvl_ != -1) {
            int rc = setsockopt (s_, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_intvl_, sizeof (int));
            assert_success_or_recoverable (s_, rc);
            if (rc != 0)
                return rc;
        }
#endif // ZLINK_HAVE_TCP_KEEPINTVL
    }
#endif // ZLINK_HAVE_SO_KEEPALIVE
#endif // ZLINK_HAVE_WINDOWS

    return 0;
}

int zlink::tune_tcp_maxrt (fd_t sockfd_, int timeout_)
{
    if (timeout_ <= 0)
        return 0;

    LIBZLINK_UNUSED (sockfd_);

#if defined(ZLINK_HAVE_WINDOWS) && defined(TCP_MAXRT)
    // msdn says it's supported in >= Vista, >= Windows Server 2003
    timeout_ /= 1000; // in seconds
    const int rc = setsockopt (sockfd_, IPPROTO_TCP, TCP_MAXRT,
                               reinterpret_cast<char *> (&timeout_), sizeof (timeout_));
    assert_success_or_recoverable (sockfd_, rc);
    return rc;
#elif defined(TCP_USER_TIMEOUT)
    int rc = setsockopt (sockfd_, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout_, sizeof (timeout_));
    assert_success_or_recoverable (sockfd_, rc);
    return rc;
#else
    return 0;
#endif
}

void zlink::tcp_tune_loopback_fast_path (const fd_t socket_)
{
#if defined ZLINK_HAVE_WINDOWS && defined SIO_LOOPBACK_FAST_PATH
    int sio_loopback_fastpath = 1;
    DWORD number_of_bytes_returned = 0;

    const int rc =
      WSAIoctl (socket_, SIO_LOOPBACK_FAST_PATH, &sio_loopback_fastpath,
                sizeof sio_loopback_fastpath, NULL, 0, &number_of_bytes_returned, 0, 0);

    if (SOCKET_ERROR == rc) {
        const DWORD last_error = ::WSAGetLastError ();

        if (WSAEOPNOTSUPP == last_error) {
            // This system is not Windows 8 or Server 2012, and the call is not supported.
        } else {
            wsa_assert (false);
        }
    }
#else
    LIBZLINK_UNUSED (socket_);
#endif
}

void zlink::tune_tcp_busy_poll (fd_t socket_, int busy_poll_)
{
#if defined(ZLINK_HAVE_BUSY_POLL)
    if (busy_poll_ > 0) {
        const int rc = setsockopt (socket_, SOL_SOCKET, SO_BUSY_POLL,
                                   reinterpret_cast<char *> (&busy_poll_), sizeof (int));
        assert_success_or_recoverable (socket_, rc);
    }
#else
    LIBZLINK_UNUSED (socket_);
    LIBZLINK_UNUSED (busy_poll_);
#endif
}
