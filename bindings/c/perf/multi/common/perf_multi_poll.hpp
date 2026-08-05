#ifndef PERF_MULTI_POLL_HPP
#define PERF_MULTI_POLL_HPP

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <dlfcn.h>
#include <poll.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <climits>
#include <cerrno>

#if defined(_WIN32)
typedef unsigned int zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

#ifndef ZLINK_POLLIN
#define ZLINK_POLLIN 1
#endif
#ifndef ZLINK_POLLOUT
#define ZLINK_POLLOUT 2
#endif
#ifndef ZLINK_POLLERR
#define ZLINK_POLLERR 4
#endif
#ifndef ZLINK_POLLPRI
#define ZLINK_POLLPRI 8
#endif

#ifndef ZLINK_HAVE_POLLER
typedef struct zlink_pollitem_t
{
    void *socket;
    zlink_fd_t fd;
    short events;
    short revents;
} zlink_pollitem_t;
#endif

static const long PERF_AUX_POLL_WAIT_MS = 100;

inline int perf_idle_wait_ms (long timeout_)
{
    if (timeout_ <= 0)
        return 0;

#if defined(_WIN32)
    const DWORD wait_ms =
      static_cast<DWORD> (timeout_ > static_cast<long> (DWORD (-1)) ? DWORD (-1) : timeout_);
    ::Sleep (wait_ms);
    return 0;
#else
    const int wait_ms =
      timeout_ > static_cast<long> (INT_MAX) ? INT_MAX : static_cast<int> (timeout_);
    int rc = 0;
    do {
        rc = ::poll (NULL, 0, wait_ms);
    } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -1 : 0;
#endif
}

inline int perf_socket_poll (zlink_pollitem_t *items_, int nitems_, long timeout_)
{
    if (nitems_ < 0) {
        errno = EINVAL;
        return -1;
    }

    if (nitems_ == 0 || !items_)
        return perf_idle_wait_ms (timeout_);
    const int rc = ::zlink_poll (items_, nitems_, timeout_, NULL);
    if (rc < 0 && zlink_errno () == EAGAIN)
        return 0;
    return rc;
}

inline long perf_aux_poll_wait_ms ()
{
    return PERF_AUX_POLL_WAIT_MS;
}

#endif
