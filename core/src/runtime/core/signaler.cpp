/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/poller.hpp"
#include "utils/polling_util.hpp"

#if defined ZLINK_POLL_BASED_ON_POLL
#if !defined ZLINK_HAVE_WINDOWS && !defined ZLINK_HAVE_AIX
#include <poll.h>
#endif
#elif defined ZLINK_POLL_BASED_ON_SELECT
#if defined ZLINK_HAVE_WINDOWS
#elif defined ZLINK_HAVE_HPUX
#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#elif defined ZLINK_HAVE_OPENVMS
#include <sys/types.h>
#include <sys/time.h>
#elif defined ZLINK_HAVE_VXWORKS
#include <sys/types.h>
#include <sys/time.h>
#include <sockLib.h>
#include <strings.h>
#else
#include <sys/select.h>
#endif
#endif

#include "core/signaler.hpp"
#include "utils/likely.hpp"
#include "utils/stdint.hpp"
#include "utils/config.hpp"
#include "utils/err.hpp"
#include "utils/fd.hpp"
#include "utils/ip.hpp"
#include "transports/tcp/tcp.hpp"

#if !defined ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#if !defined(ZLINK_HAVE_WINDOWS)
// Helper to sleep for specific number of milliseconds (or until signal)
//
static int sleep_ms (unsigned int ms_)
{
    if (ms_ == 0)
        return 0;
#if defined ZLINK_HAVE_ANDROID
    usleep (ms_ * 1000);
    return 0;
#elif defined ZLINK_HAVE_VXWORKS
    struct timespec ns_;
    ns_.tv_sec = ms_ / 1000;
    ns_.tv_nsec = ms_ % 1000 * 1000000;
    return nanosleep (&ns_, 0);
#else
    return usleep (ms_ * 1000);
#endif
}

// Helper to wait on close(), for non-blocking sockets, until it completes
// If EAGAIN is received, will sleep briefly (1-100ms) then try again, until
// the overall timeout is reached.
//
static int close_wait_ms (int fd_, unsigned int max_ms_ = 2000)
{
    unsigned int ms_so_far = 0;
    const unsigned int min_step_ms = 1;
    const unsigned int max_step_ms = 100;
    const unsigned int step_ms = std::min (std::max (min_step_ms, max_ms_ / 10), max_step_ms);

    int rc = 0; // do not sleep on first attempt
    do {
        if (rc == -1 && errno == EAGAIN) {
            sleep_ms (step_ms);
            ms_so_far += step_ms;
        }
        rc = close (fd_);
    } while (ms_so_far < max_ms_ && rc == -1 && errno == EAGAIN);

    return rc;
}
#endif

zlink::signaler_t::signaler_t () : signaler_t (false)
{
}

zlink::signaler_t::signaler_t (bool event_only_)
{
    _w = retired_fd;
    _r = retired_fd;
#ifdef ZLINK_HAVE_WINDOWS
    _signaled.store (false, std::memory_order_relaxed);
    _event_only = event_only_;
    _event = NULL;

    if (_event_only) {
        _event = CreateEventW (NULL, FALSE, FALSE, NULL);
        win_assert (_event != NULL);
    }
#endif

    //  Create the socketpair for signaling.
    if (
#ifdef ZLINK_HAVE_WINDOWS
      !_event_only &&
#endif
      make_fdpair (&_r, &_w) == 0) {
        unblock_socket (_w);
        unblock_socket (_r);
    }
#ifdef HAVE_FORK
    pid = getpid ();
#endif
}

// This might get run after some part of construction failed, leaving one or
// both of _r and _w retired_fd.
zlink::signaler_t::~signaler_t ()
{
#if defined ZLINK_HAVE_EVENTFD
    if (_r == retired_fd)
        return;
    int rc = close_wait_ms (_r);
    errno_assert (rc == 0);
#elif defined ZLINK_HAVE_WINDOWS
    if (_event != NULL) {
        const BOOL rc = CloseHandle (_event);
        win_assert (rc != 0);
        _event = NULL;
    }
    if (_event_only)
        return;
    if (_w != retired_fd) {
        const struct linger so_linger = {1, 0};
        int rc = setsockopt (_w, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *> (&so_linger),
                             sizeof so_linger);
        //  Only check shutdown if WSASTARTUP was previously done
        if (rc == 0 || WSAGetLastError () != WSANOTINITIALISED) {
            wsa_assert (rc != SOCKET_ERROR);
            rc = closesocket (_w);
            wsa_assert (rc != SOCKET_ERROR);
            if (_r == retired_fd)
                return;
            rc = closesocket (_r);
            wsa_assert (rc != SOCKET_ERROR);
        }
    }
#else
    if (_w != retired_fd) {
        int rc = close_wait_ms (_w);
        errno_assert (rc == 0);
    }
    if (_r != retired_fd) {
        int rc = close_wait_ms (_r);
        errno_assert (rc == 0);
    }
#endif
}

zlink::fd_t zlink::signaler_t::get_fd () const
{
    return _r;
}

#ifdef ZLINK_HAVE_WINDOWS
HANDLE zlink::signaler_t::get_handle () const
{
    return _event;
}

void zlink::signaler_t::reset_event ()
{
    if (_event != NULL) {
        _signaled.store (false, std::memory_order_release);
        const BOOL rc = ResetEvent (_event);
        win_assert (rc != 0);
    }
}
#endif

void zlink::signaler_t::send ()
{
#if defined HAVE_FORK
    if (unlikely (pid != getpid ())) {
        return; // do not send anything in forked child context
    }
#endif
#if defined ZLINK_HAVE_EVENTFD
    const uint64_t inc = 1;
    ssize_t sz;
    do {
        sz = write (_w, &inc, sizeof (inc));
    } while (sz == -1 && errno == EINTR);
    errno_assert (sz == sizeof (inc));
#elif defined ZLINK_HAVE_WINDOWS
    if (_event_only) {
        const BOOL rc = SetEvent (_event);
        win_assert (rc != 0);
        _signaled.store (true, std::memory_order_release);
        return;
    }
    const char dummy = 0;
    int nbytes;
    do {
        nbytes = ::send (_w, &dummy, sizeof (dummy), 0);
        wsa_assert (nbytes != SOCKET_ERROR);
        // wsa_assert does not abort on WSAEWOULDBLOCK. If we get this, we retry.
    } while (nbytes == SOCKET_ERROR);
    // Given the small size of dummy (should be 1) expect that send was able to send everything.
    zlink_assert (nbytes == sizeof (dummy));
#ifdef ZLINK_HAVE_WINDOWS
    _signaled.store (true, std::memory_order_release);
#endif
#elif defined ZLINK_HAVE_VXWORKS
    unsigned char dummy = 0;
    while (true) {
        ssize_t nbytes = ::send (_w, (char *) &dummy, sizeof (dummy), 0);
        if (unlikely (nbytes == -1 && errno == EINTR))
            continue;
#if defined(HAVE_FORK)
        if (unlikely (pid != getpid ())) {
            errno = EINTR;
            break;
        }
#endif
        zlink_assert (nbytes == sizeof dummy);
        break;
    }
#else
    unsigned char dummy = 0;
    while (true) {
        ssize_t nbytes = ::send (_w, &dummy, sizeof (dummy), 0);
        if (unlikely (nbytes == -1 && errno == EINTR))
            continue;
#if defined(HAVE_FORK)
        if (unlikely (pid != getpid ())) {
            errno = EINTR;
            break;
        }
#endif
        zlink_assert (nbytes == sizeof dummy);
        break;
    }
#endif
}

int zlink::signaler_t::wait (int timeout_) const
{
#ifdef HAVE_FORK
    if (unlikely (pid != getpid ())) {
        // we have forked and the file descriptor is closed. Emulate an interrupt
        // response.

        errno = EINTR;
        return -1;
    }
#endif

#ifdef ZLINK_HAVE_WINDOWS
    if (_event_only) {
        const DWORD wait_rc = WaitForSingleObject (
          _event, timeout_ < 0 ? INFINITE : static_cast<DWORD> (timeout_));
        if (wait_rc == WAIT_OBJECT_0)
            return 0;
        if (wait_rc == WAIT_TIMEOUT) {
            errno = EAGAIN;
            return -1;
        }
        errno = EINTR;
        return -1;
    }
    if (_signaled.load (std::memory_order_acquire))
        return 0;
#endif

#ifdef ZLINK_POLL_BASED_ON_POLL
    struct pollfd pfd;
    pfd.fd = _r;
    pfd.events = POLLIN;
    const int rc = poll (&pfd, 1, timeout_);
    if (unlikely (rc < 0)) {
        errno_assert (errno == EINTR);
        return -1;
    }
    if (unlikely (rc == 0)) {
        errno = EAGAIN;
        return -1;
    }
#ifdef HAVE_FORK
    if (unlikely (pid != getpid ())) {
        // we have forked and the file descriptor is closed. Emulate an interrupt
        // response.
        errno = EINTR;
        return -1;
    }
#endif
    zlink_assert (rc == 1);
    if (unlikely ((pfd.revents & POLLIN) == 0)) {
        if (pfd.revents
            & (POLLERR | POLLHUP
#if defined POLLNVAL
               | POLLNVAL
#endif
               )) {
            errno = EINTR;
            return -1;
        }
        errno = EINTR;
        return -1;
    }
    return 0;

#elif defined ZLINK_POLL_BASED_ON_SELECT

    optimized_fd_set_t fds (1);
    FD_ZERO (fds.get ());
    FD_SET (_r, fds.get ());
    struct timeval timeout;
    if (timeout_ >= 0) {
        timeout.tv_sec = timeout_ / 1000;
        timeout.tv_usec = timeout_ % 1000 * 1000;
    }
#ifdef ZLINK_HAVE_WINDOWS
    int rc = select (0, fds.get (), NULL, NULL, timeout_ >= 0 ? &timeout : NULL);
    wsa_assert (rc != SOCKET_ERROR);
#else
    int rc = select (_r + 1, fds.get (), NULL, NULL, timeout_ >= 0 ? &timeout : NULL);
    if (unlikely (rc < 0)) {
        errno_assert (errno == EINTR);
        return -1;
    }
#endif
    if (unlikely (rc == 0)) {
        errno = EAGAIN;
        return -1;
    }
    zlink_assert (rc == 1);
    return 0;

#else
#error
#endif
}

void zlink::signaler_t::recv ()
{
//  Attempt to read a signal.
#if defined ZLINK_HAVE_EVENTFD
    uint64_t dummy;
    ssize_t sz = read (_r, &dummy, sizeof (dummy));
    errno_assert (sz == sizeof (dummy));

    //  If we accidentally grabbed the next signal(s) along with the current
    //  one, return it back to the eventfd object.
    if (unlikely (dummy > 1)) {
        const uint64_t inc = dummy - 1;
        ssize_t sz2 = write (_w, &inc, sizeof (inc));
        errno_assert (sz2 == sizeof (inc));
        return;
    }

    zlink_assert (dummy == 1);
#else
    unsigned char dummy;
#if defined ZLINK_HAVE_WINDOWS
    if (_event_only) {
        _signaled.store (false, std::memory_order_release);
        const BOOL rc = ResetEvent (_event);
        win_assert (rc != 0);
        return;
    }
    _signaled.store (false, std::memory_order_release);
    const int nbytes = ::recv (_r, reinterpret_cast<char *> (&dummy), sizeof (dummy), 0);
    wsa_assert (nbytes != SOCKET_ERROR);
#elif defined ZLINK_HAVE_VXWORKS
    ssize_t nbytes = ::recv (_r, (char *) &dummy, sizeof (dummy), 0);
    errno_assert (nbytes >= 0);
#else
    ssize_t nbytes = ::recv (_r, &dummy, sizeof (dummy), 0);
    errno_assert (nbytes >= 0);
#endif
    zlink_assert (nbytes == sizeof (dummy));
    zlink_assert (dummy == 0);
#endif
}

int zlink::signaler_t::recv_failable ()
{
//  Attempt to read a signal.
#if defined ZLINK_HAVE_EVENTFD
    uint64_t dummy;
    ssize_t sz = read (_r, &dummy, sizeof (dummy));
    if (sz == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EBADF) {
            errno = EAGAIN;
            return -1;
        }
        errno_assert (errno == EAGAIN);
        return -1;
    }
    errno_assert (sz == sizeof (dummy));

    //  If we accidentally grabbed the next signal(s) along with the current
    //  one, return it back to the eventfd object.
    if (unlikely (dummy > 1)) {
        const uint64_t inc = dummy - 1;
        ssize_t sz2 = write (_w, &inc, sizeof (inc));
        errno_assert (sz2 == sizeof (inc));
        return 0;
    }

    zlink_assert (dummy == 1);

#else
    unsigned char dummy;
#if defined ZLINK_HAVE_WINDOWS
    if (_event_only) {
        if (!_signaled.exchange (false, std::memory_order_acq_rel)) {
            errno = EAGAIN;
            return -1;
        }
        const BOOL rc = ResetEvent (_event);
        win_assert (rc != 0);
        if (_signaled.load (std::memory_order_acquire))
            SetEvent (_event);
        return 0;
    }
    if (!_signaled.exchange (false, std::memory_order_acq_rel)) {
        errno = EAGAIN;
        return -1;
    }
    const int nbytes = ::recv (_r, reinterpret_cast<char *> (&dummy), sizeof (dummy), 0);
    if (nbytes == SOCKET_ERROR) {
        const int last_error = WSAGetLastError ();
        if (last_error == WSAEWOULDBLOCK) {
            _signaled.store (true, std::memory_order_release);
            errno = EAGAIN;
            return -1;
        }
        wsa_assert (last_error == WSAEWOULDBLOCK);
    }
#elif defined ZLINK_HAVE_VXWORKS
    ssize_t nbytes = ::recv (_r, (char *) &dummy, sizeof (dummy), 0);
    if (nbytes == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            errno = EAGAIN;
            return -1;
        }
        errno_assert (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
#else
    ssize_t nbytes = ::recv (_r, &dummy, sizeof (dummy), 0);
    if (nbytes == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            errno = EAGAIN;
            return -1;
        }
        errno_assert (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
#endif
    zlink_assert (nbytes == sizeof (dummy));
    zlink_assert (dummy == 0);
#endif
    return 0;
}

bool zlink::signaler_t::valid () const
{
#ifdef ZLINK_HAVE_WINDOWS
    if (_event_only)
        return _event != NULL;
#endif
    return _w != retired_fd;
}

#ifdef HAVE_FORK
void zlink::signaler_t::forked ()
{
    //  Close file descriptors created in the parent and create new pair
    close (_r);
    close (_w);
    make_fdpair (&_r, &_w);
}
#endif
