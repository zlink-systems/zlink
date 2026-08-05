/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/ip.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"
#include "utils/config.hpp"
#include "core/address.hpp"


#if !defined ZLINK_HAVE_WINDOWS
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <unistd.h>

#include <vector>
#else
#include "transports/tcp/tcp.hpp"
#ifdef ZLINK_HAVE_IPC
#include "transports/ipc/ipc_address.hpp"
#endif

#include <direct.h>

#define rmdir _rmdir
#define unlink _unlink
#endif

#if defined ZLINK_HAVE_OPENVMS || defined ZLINK_HAVE_VXWORKS
#include <ioctl.h>
#endif

#if defined ZLINK_HAVE_VXWORKS
#include <unistd.h>
#include <sockLib.h>
#include <ioLib.h>
#endif

#if defined ZLINK_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifndef ZLINK_HAVE_WINDOWS
// Acceptable temporary directory environment variables
static const char *tmp_env_vars[] = {
  "TMPDIR", "TEMPDIR", "TMP",
  0 // Sentinel
};
#endif

zlink::fd_t zlink::open_socket (int domain_, int type_, int protocol_)
{
    int rc;

    //  Setting this option result in sane behaviour when exec() functions
    //  are used. Old sockets are closed and don't block TCP ports etc.
#if defined ZLINK_HAVE_SOCK_CLOEXEC
    type_ |= SOCK_CLOEXEC;
#endif

#if defined ZLINK_HAVE_WINDOWS && defined WSA_FLAG_NO_HANDLE_INHERIT
    // if supported, create socket with WSA_FLAG_NO_HANDLE_INHERIT, such that
    // the race condition in making it non-inheritable later is avoided
    const fd_t s = WSASocket (domain_, type_, protocol_, NULL, 0,
                              WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    const fd_t s = socket (domain_, type_, protocol_);
#endif
    if (s == retired_fd) {
#ifdef ZLINK_HAVE_WINDOWS
        errno = wsa_error_to_errno (WSAGetLastError ());
#endif
        return retired_fd;
    }

    make_socket_noninheritable (s);

    //  Socket is not yet connected so EINVAL is not a valid networking error
    rc = zlink::set_nosigpipe (s);
    errno_assert (rc == 0);

    return s;
}

void zlink::unblock_socket (fd_t s_)
{
#if defined ZLINK_HAVE_WINDOWS
    u_long nonblock = 1;
    const int rc = ioctlsocket (s_, FIONBIO, &nonblock);
    wsa_assert (rc != SOCKET_ERROR);
#elif defined ZLINK_HAVE_OPENVMS || defined ZLINK_HAVE_VXWORKS
    int nonblock = 1;
    int rc = ioctl (s_, FIONBIO, &nonblock);
    errno_assert (rc != -1);
#else
    int flags = fcntl (s_, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    int rc = fcntl (s_, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
#endif
}

void zlink::enable_ipv4_mapping (fd_t s_)
{
    LIBZLINK_UNUSED (s_);

#if defined IPV6_V6ONLY && !defined ZLINK_HAVE_OPENBSD && !defined ZLINK_HAVE_DRAGONFLY
#ifdef ZLINK_HAVE_WINDOWS
    DWORD flag = 0;
#else
    int flag = 0;
#endif
    const int rc =
      setsockopt (s_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char *> (&flag), sizeof (flag));
#ifdef ZLINK_HAVE_WINDOWS
    wsa_assert (rc != SOCKET_ERROR);
#else
    errno_assert (rc == 0);
#endif
#endif
}

int zlink::get_peer_ip_address (fd_t sockfd_, std::string &ip_addr_)
{
    struct sockaddr_storage ss;

    const zlink_socklen_t addrlen = get_socket_address (sockfd_, socket_end_remote, &ss);

    if (addrlen == 0) {
#ifdef ZLINK_HAVE_WINDOWS
        const int last_error = WSAGetLastError ();
        wsa_assert (last_error != WSANOTINITIALISED && last_error != WSAEFAULT
                    && last_error != WSAEINPROGRESS && last_error != WSAENOTSOCK);
#elif !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE
        errno_assert (errno != EBADF && errno != EFAULT && errno != ENOTSOCK);
#else
        errno_assert (errno != EFAULT && errno != ENOTSOCK);
#endif
        return 0;
    }

    char host[NI_MAXHOST];
    const int rc = getnameinfo (reinterpret_cast<struct sockaddr *> (&ss), addrlen, host,
                                sizeof host, NULL, 0, NI_NUMERICHOST);
    if (rc != 0)
        return 0;

    ip_addr_ = host;

    union
    {
        struct sockaddr sa;
        struct sockaddr_storage sa_stor;
    } u;

    u.sa_stor = ss;
    return static_cast<int> (u.sa.sa_family);
}

void zlink::set_ip_type_of_service (fd_t s_, int iptos_)
{
    int rc =
      setsockopt (s_, IPPROTO_IP, IP_TOS, reinterpret_cast<char *> (&iptos_), sizeof (iptos_));

#ifdef ZLINK_HAVE_WINDOWS
    wsa_assert (rc != SOCKET_ERROR);
#else
    errno_assert (rc == 0);
#endif

    //  Windows and Hurd do not support IPV6_TCLASS
#if !defined(ZLINK_HAVE_WINDOWS) && defined(IPV6_TCLASS)
    rc = setsockopt (s_, IPPROTO_IPV6, IPV6_TCLASS, reinterpret_cast<char *> (&iptos_),
                     sizeof (iptos_));

    //  If IPv6 is not enabled ENOPROTOOPT will be returned on Linux and
    //  EINVAL on OSX
    if (rc == -1) {
        errno_assert (errno == ENOPROTOOPT || errno == EINVAL);
    }
#endif
}

void zlink::set_socket_priority (fd_t s_, int priority_)
{
#ifdef ZLINK_HAVE_SO_PRIORITY
    int rc = setsockopt (s_, SOL_SOCKET, SO_PRIORITY, reinterpret_cast<char *> (&priority_),
                         sizeof (priority_));
    errno_assert (rc == 0);
#else
    LIBZLINK_UNUSED (s_);
    LIBZLINK_UNUSED (priority_);
#endif
}

int zlink::set_nosigpipe (fd_t s_)
{
#ifdef SO_NOSIGPIPE
    //  Make sure that SIGPIPE signal is not generated when writing to a
    //  connection that was already closed by the peer.
    //  As per POSIX spec, EINVAL will be returned if the socket was valid but
    //  the connection has been reset by the peer. Return an error so that the
    //  socket can be closed and the connection retried if necessary.
    int set = 1;
    int rc = setsockopt (s_, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof (int));
    if (rc != 0 && errno == EINVAL)
        return -1;
    errno_assert (rc == 0);
#else
    LIBZLINK_UNUSED (s_);
#endif

    return 0;
}

int zlink::bind_to_device (fd_t s_, const std::string &bound_device_)
{
#ifdef ZLINK_HAVE_SO_BINDTODEVICE
    int rc =
      setsockopt (s_, SOL_SOCKET, SO_BINDTODEVICE, bound_device_.c_str (), bound_device_.length ());
    if (rc != 0) {
        assert_success_or_recoverable (s_, rc);
        return -1;
    }
    return 0;

#else
    LIBZLINK_UNUSED (s_);
    LIBZLINK_UNUSED (bound_device_);

    errno = ENOTSUP;
    return -1;
#endif
}

bool zlink::initialize_network ()
{
#ifdef ZLINK_HAVE_WINDOWS
    //  Initialise Windows sockets. Note that WSAStartup can be called multiple
    //  times given that WSACleanup will be called for each WSAStartup.

    const WORD version_requested = MAKEWORD (2, 2);
    WSADATA wsa_data;
    const int rc = WSAStartup (version_requested, &wsa_data);
    zlink_assert (rc == 0);
    zlink_assert (LOBYTE (wsa_data.wVersion) == 2 && HIBYTE (wsa_data.wVersion) == 2);
#endif

    return true;
}

void zlink::shutdown_network ()
{
#ifdef ZLINK_HAVE_WINDOWS
    //  On Windows, uninitialise socket layer.
    const int rc = WSACleanup ();
    wsa_assert (rc != SOCKET_ERROR);
#endif

}

void zlink::make_socket_noninheritable (fd_t sock_)
{
#if defined ZLINK_HAVE_WINDOWS && !defined _WIN32_WCE && !defined ZLINK_HAVE_WINDOWS_UWP
    //  On Windows, preventing sockets to be inherited by child processes.
    const BOOL brc =
      SetHandleInformation (reinterpret_cast<HANDLE> (sock_), HANDLE_FLAG_INHERIT, 0);
    win_assert (brc);
#elif (!defined ZLINK_HAVE_SOCK_CLOEXEC || !defined HAVE_ACCEPT4) && defined FD_CLOEXEC
    //  If there 's no SOCK_CLOEXEC, let's try the second best option.
    //  Race condition can cause socket not to be closed (if fork happens
    //  between accept and this point).
    const int rc = fcntl (sock_, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#else
    LIBZLINK_UNUSED (sock_);
#endif
}

void zlink::assert_success_or_recoverable (zlink::fd_t s_, int rc_)
{
#ifdef ZLINK_HAVE_WINDOWS
    if (rc_ != SOCKET_ERROR) {
        return;
    }
#else
    if (rc_ != -1) {
        return;
    }
#endif

    //  Check whether an error occurred
    int err = 0;
#if defined ZLINK_HAVE_HPUX || defined ZLINK_HAVE_VXWORKS
    int len = sizeof err;
#else
    socklen_t len = sizeof err;
#endif

    const int rc = getsockopt (s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *> (&err), &len);

    //  Assert if the error was caused by 0MQ bug.
    //  Networking problems are OK. No need to assert.
#ifdef ZLINK_HAVE_WINDOWS
    zlink_assert (rc == 0);
    if (err != 0) {
        wsa_assert (err == WSAECONNREFUSED || err == WSAECONNRESET || err == WSAECONNABORTED
                    || err == WSAEINTR || err == WSAETIMEDOUT || err == WSAEHOSTUNREACH
                    || err == WSAENETUNREACH || err == WSAENETDOWN || err == WSAENETRESET
                    || err == WSAEACCES || err == WSAEINVAL || err == WSAEADDRINUSE);
    }
#else
    //  Following code should handle both Berkeley-derived socket
    //  implementations and Solaris.
    if (rc == -1)
        err = errno;
    if (err != 0) {
        errno = err;
        errno_assert (errno == ECONNREFUSED || errno == ECONNRESET || errno == ECONNABORTED
                      || errno == EINTR || errno == ETIMEDOUT || errno == EHOSTUNREACH
                      || errno == ENETUNREACH || errno == ENETDOWN || errno == ENETRESET
                      || errno == EINVAL);
    }
#endif
}

#ifdef ZLINK_HAVE_IPC

#if defined ZLINK_HAVE_WINDOWS
char *widechar_to_utf8 (const wchar_t *widestring)
{
    int nch, n;
    char *utf8 = 0;
    nch = WideCharToMultiByte (CP_UTF8, 0, widestring, -1, 0, 0, NULL, NULL);
    if (nch > 0) {
        utf8 = (char *) malloc ((nch + 1) * sizeof (char));
        n = WideCharToMultiByte (CP_UTF8, 0, widestring, -1, utf8, nch, NULL, NULL);
        utf8[nch] = 0;
    }
    return utf8;
}
#endif

int zlink::create_ipc_wildcard_address (std::string &path_, std::string &file_)
{
#if defined ZLINK_HAVE_WINDOWS
    wchar_t buffer[MAX_PATH];

    {
        const errno_t rc = _wtmpnam_s (buffer);
        errno_assert (rc == 0);
    }

    //  Use the wide-character CRT path to match the generated wide temp name.
    const int rc = _wmkdir (buffer);
    if (rc != 0) {
        return -1;
    }

    char *tmp = widechar_to_utf8 (buffer);
    if (tmp == 0) {
        return -1;
    }

    path_.assign (tmp);
    file_ = path_ + "/socket";

    free (tmp);
#else
    std::string tmp_path;

    // If TMPDIR, TEMPDIR, or TMP are available and are directories, create
    // the socket directory there.
    const char **tmp_env = tmp_env_vars;
    while (tmp_path.empty () && *tmp_env != 0) {
        const char *const tmpdir = getenv (*tmp_env);
        struct stat statbuf;

        // Confirm it is actually a directory before trying to use
        if (tmpdir != 0 && ::stat (tmpdir, &statbuf) == 0 && S_ISDIR (statbuf.st_mode)) {
            tmp_path.assign (tmpdir);
            if (*(tmp_path.rbegin ()) != '/') {
                tmp_path.push_back ('/');
            }
        }

        // Try the next environment variable
        ++tmp_env;
    }

    // If no env var is set, use a local ./tmp directory in the current
    // working directory. Create it if it doesn't exist.
    if (tmp_path.empty ()) {
        const char *local_tmp = "./tmp";
        struct stat statbuf;
        if (::stat (local_tmp, &statbuf) == 0) {
            if (!S_ISDIR (statbuf.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
        } else if (errno == ENOENT) {
            if (::mkdir (local_tmp, S_IRWXU) != 0)
                return -1;
        } else {
            return -1;
        }
        tmp_path.assign (local_tmp);
        tmp_path.push_back ('/');
    }

    // Append a directory name
    tmp_path.append ("tmpXXXXXX");

    // We need room for tmp_path + trailing NUL
    std::vector<char> buffer (tmp_path.length () + 1);
    memcpy (&buffer[0], tmp_path.c_str (), tmp_path.length () + 1);

#if defined HAVE_MKDTEMP
    // Create the directory.  POSIX requires that mkdtemp() creates the
    // directory with 0700 permissions, meaning the only possible race
    // with socket creation could be the same user.  However, since
    // each socket is created in a directory created by mkdtemp(), and
    // mkdtemp() guarantees a unique directory name, there will be no
    // collision.
    if (mkdtemp (&buffer[0]) == 0) {
        return -1;
    }

    path_.assign (&buffer[0]);
    file_ = path_ + "/socket";
#else
    LIBZLINK_UNUSED (path_);
    int fd = mkstemp (&buffer[0]);
    if (fd == -1)
        return -1;
    ::close (fd);

    file_.assign (&buffer[0]);
#endif
#endif

    return 0;
}
#endif
