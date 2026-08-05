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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#else
#include "transports/tcp/tcp.hpp"
#ifdef ZLINK_HAVE_IPC
#include "transports/ipc/ipc_address.hpp"
#endif
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

#if defined ZLINK_HAVE_WINDOWS
static void tune_socket (const SOCKET socket_)
{
    BOOL tcp_nodelay = 1;
    const int rc = setsockopt (socket_, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<char *> (&tcp_nodelay), sizeof tcp_nodelay);
    wsa_assert (rc != SOCKET_ERROR);

    zlink::tcp_tune_loopback_fast_path (socket_);
}

static int make_fdpair_tcpip (zlink::fd_t *r_, zlink::fd_t *w_)
{
#if !defined _WIN32_WCE && !defined ZLINK_HAVE_WINDOWS_UWP
    //  Windows CE does not manage security attributes
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa;
    memset (&sd, 0, sizeof sd);
    memset (&sa, 0, sizeof sa);

    InitializeSecurityDescriptor (&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl (&sd, TRUE, 0, FALSE);

    sa.nLength = sizeof (SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
#endif

    //  This function has to be in a system-wide critical section so that
    //  two instances of the library don't accidentally create signaler
    //  crossing the process boundary.
    //  We'll use named event object to implement the critical section.
    //  Note that if the event object already exists, the CreateEvent requests
    //  EVENT_ALL_ACCESS access right. If this fails, we try to open
    //  the event object asking for SYNCHRONIZE access only.
    HANDLE sync = NULL;

    //  Create critical section only if using fixed signaler port
    //  Use problematic Event implementation for compatibility if using old port 5905.
    //  Otherwise use Mutex implementation.
    const int event_signaler_port = 5905;

    if (zlink::signaler_port == event_signaler_port) {
#if !defined _WIN32_WCE && !defined ZLINK_HAVE_WINDOWS_UWP
        sync = CreateEventW (&sa, FALSE, TRUE, L"Global\\zlink-signaler-port-sync");
#else
        sync = CreateEventW (NULL, FALSE, TRUE, L"Global\\zlink-signaler-port-sync");
#endif
        if (sync == NULL && GetLastError () == ERROR_ACCESS_DENIED)
            sync = OpenEventW (SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                               L"Global\\zlink-signaler-port-sync");

        win_assert (sync != NULL);
    } else if (zlink::signaler_port != 0) {
        wchar_t mutex_name[MAX_PATH];
#ifdef __MINGW32__
        _snwprintf (mutex_name, MAX_PATH, L"Global\\zlink-signaler-port-%d", zlink::signaler_port);
#else
        swprintf (mutex_name, MAX_PATH, L"Global\\zlink-signaler-port-%d", zlink::signaler_port);
#endif

#if !defined _WIN32_WCE && !defined ZLINK_HAVE_WINDOWS_UWP
        sync = CreateMutexW (&sa, FALSE, mutex_name);
#else
        sync = CreateMutexW (NULL, FALSE, mutex_name);
#endif
        if (sync == NULL && GetLastError () == ERROR_ACCESS_DENIED)
            sync = OpenMutexW (SYNCHRONIZE, FALSE, mutex_name);

        win_assert (sync != NULL);
    }

    //  Windows has no 'socketpair' function. CreatePipe is no good as pipe
    //  handles cannot be polled on. Here we create the socketpair by hand.
    *w_ = INVALID_SOCKET;
    *r_ = INVALID_SOCKET;

    //  Create listening socket.
    SOCKET listener;
    listener = zlink::open_socket (AF_INET, SOCK_STREAM, 0);
    wsa_assert (listener != INVALID_SOCKET);

    //  Set SO_REUSEADDR and TCP_NODELAY on listening socket.
    BOOL so_reuseaddr = 1;
    int rc = setsockopt (listener, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<char *> (&so_reuseaddr), sizeof so_reuseaddr);
    wsa_assert (rc != SOCKET_ERROR);

    tune_socket (listener);

    //  Init sockaddr to signaler port.
    struct sockaddr_in addr;
    memset (&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (zlink::signaler_port);

    //  Create the writer socket.
    *w_ = zlink::open_socket (AF_INET, SOCK_STREAM, 0);
    wsa_assert (*w_ != INVALID_SOCKET);

    if (sync != NULL) {
        //  Enter the critical section.
        const DWORD dwrc = WaitForSingleObject (sync, INFINITE);
        zlink_assert (dwrc == WAIT_OBJECT_0 || dwrc == WAIT_ABANDONED);
    }

    //  Bind listening socket to signaler port.
    rc = bind (listener, reinterpret_cast<const struct sockaddr *> (&addr), sizeof addr);

    if (rc != SOCKET_ERROR && zlink::signaler_port == 0) {
        //  Retrieve ephemeral port number
        int addrlen = sizeof addr;
        rc = getsockname (listener, reinterpret_cast<struct sockaddr *> (&addr), &addrlen);
    }

    //  Listen for incoming connections.
    if (rc != SOCKET_ERROR) {
        rc = listen (listener, 1);
    }

    //  Connect writer to the listener.
    if (rc != SOCKET_ERROR) {
        rc = connect (*w_, reinterpret_cast<struct sockaddr *> (&addr), sizeof addr);
    }

    //  Accept connection from writer.
    if (rc != SOCKET_ERROR) {
        //  Set TCP_NODELAY on writer socket.
        tune_socket (*w_);

        *r_ = accept (listener, NULL, NULL);
    }

    //  Send/receive large chunk to work around TCP slow start
    //  This code is a workaround for #1608
    if (*r_ != INVALID_SOCKET) {
        const size_t dummy_size = 1024 * 1024; //  1M to overload default receive buffer
        unsigned char *dummy = static_cast<unsigned char *> (malloc (dummy_size));
        wsa_assert (dummy);

        int still_to_send = static_cast<int> (dummy_size);
        int still_to_recv = static_cast<int> (dummy_size);
        while (still_to_send || still_to_recv) {
            int nbytes;
            if (still_to_send > 0) {
                nbytes = ::send (*w_, reinterpret_cast<char *> (dummy + dummy_size - still_to_send),
                                 still_to_send, 0);
                wsa_assert (nbytes != SOCKET_ERROR);
                still_to_send -= nbytes;
            }
            nbytes = ::recv (*r_, reinterpret_cast<char *> (dummy + dummy_size - still_to_recv),
                             still_to_recv, 0);
            wsa_assert (nbytes != SOCKET_ERROR);
            still_to_recv -= nbytes;
        }
        free (dummy);
    }

    //  Save errno if error occurred in bind/listen/connect/accept.
    int saved_errno = 0;
    if (*r_ == INVALID_SOCKET)
        saved_errno = WSAGetLastError ();

    //  We don't need the listening socket anymore. Close it.
    rc = closesocket (listener);
    wsa_assert (rc != SOCKET_ERROR);

    if (sync != NULL) {
        //  Exit the critical section.
        BOOL brc;
        if (zlink::signaler_port == event_signaler_port)
            brc = SetEvent (sync);
        else
            brc = ReleaseMutex (sync);
        win_assert (brc != 0);

        //  Release the kernel object
        brc = CloseHandle (sync);
        win_assert (brc != 0);
    }

    if (*r_ != INVALID_SOCKET) {
        zlink::make_socket_noninheritable (*r_);
        return 0;
    }
    //  Cleanup writer if connection failed
    if (*w_ != INVALID_SOCKET) {
        rc = closesocket (*w_);
        wsa_assert (rc != SOCKET_ERROR);
        *w_ = INVALID_SOCKET;
    }
    //  Set errno from saved value
    errno = zlink::wsa_error_to_errno (saved_errno);
    return -1;
}
#endif

int zlink::make_fdpair (fd_t *r_, fd_t *w_)
{
#if defined ZLINK_HAVE_EVENTFD
    int flags = 0;
#if defined ZLINK_HAVE_EVENTFD_CLOEXEC
    //  Setting this option result in sane behaviour when exec() functions
    //  are used. Old sockets are closed and don't block TCP ports, avoid
    //  leaks, etc.
    flags |= EFD_CLOEXEC;
#endif
    fd_t fd = eventfd (0, flags);
    if (fd == -1) {
        errno_assert (errno == ENFILE || errno == EMFILE);
        *w_ = *r_ = -1;
        return -1;
    }
    *w_ = *r_ = fd;
    return 0;


#elif defined ZLINK_HAVE_WINDOWS
#ifdef ZLINK_HAVE_IPC
    ipc_address_t address;
    std::string dirname, filename;
    sockaddr_un lcladdr;
    socklen_t lcladdr_len = sizeof lcladdr;
    int rc = 0;
    int saved_errno = 0;

    // It appears that a lack of runtime AF_UNIX support
    // can fail in more than one way.
    // At least: open_socket can fail or later in bind
    bool ipc_fallback_on_tcpip = true;

    //  Create a listening socket.
    const SOCKET listener = open_socket (AF_UNIX, SOCK_STREAM, 0);
    if (listener == retired_fd) {
        //  This may happen if the library was built on a system supporting AF_UNIX, but the system running doesn't support it.
        goto try_tcpip;
    }

    create_ipc_wildcard_address (dirname, filename);

    //  Initialise the address structure.
    rc = address.resolve (filename.c_str ());
    if (rc != 0) {
        goto error_closelistener;
    }

    //  Bind the socket to the file path.
    rc = bind (listener, const_cast<sockaddr *> (address.addr ()), address.addrlen ());
    if (rc != 0) {
        errno = wsa_error_to_errno (WSAGetLastError ());
        goto error_closelistener;
    }
    // if we got here, ipc should be working,
    // so raise any remaining errors
    ipc_fallback_on_tcpip = false;

    //  Listen for incoming connections.
    rc = listen (listener, 1);
    if (rc != 0) {
        errno = wsa_error_to_errno (WSAGetLastError ());
        goto error_closelistener;
    }

    rc = getsockname (listener, reinterpret_cast<struct sockaddr *> (&lcladdr), &lcladdr_len);
    wsa_assert (rc != -1);

    //  Create the client socket.
    *w_ = open_socket (AF_UNIX, SOCK_STREAM, 0);
    if (*w_ == -1) {
        errno = wsa_error_to_errno (WSAGetLastError ());
        goto error_closelistener;
    }

    //  Connect to the remote peer.
    rc = ::connect (*w_, reinterpret_cast<const struct sockaddr *> (&lcladdr), lcladdr_len);
    if (rc == -1) {
        goto error_closeclient;
    }

    *r_ = accept (listener, NULL, NULL);
    errno_assert (*r_ != -1);

    //  Close the listener socket, we don't need it anymore.
    rc = closesocket (listener);
    wsa_assert (rc == 0);

    //  Cleanup temporary socket file descriptor
    if (!filename.empty ()) {
        rc = ::unlink (filename.c_str ());
        if ((rc == 0) && !dirname.empty ()) {
            rc = ::rmdir (dirname.c_str ());
            dirname.clear ();
        }
        filename.clear ();
    }

    return 0;

error_closeclient:
    saved_errno = errno;
    rc = closesocket (*w_);
    wsa_assert (rc == 0);
    errno = saved_errno;

error_closelistener:
    saved_errno = errno;
    rc = closesocket (listener);
    wsa_assert (rc == 0);

    //  Cleanup temporary socket file descriptor
    if (!filename.empty ()) {
        rc = ::unlink (filename.c_str ());
        if ((rc == 0) && !dirname.empty ()) {
            rc = ::rmdir (dirname.c_str ());
            dirname.clear ();
        }
        filename.clear ();
    }

    // ipc failed due to lack of AF_UNIX support, fallback on tcpip
    if (ipc_fallback_on_tcpip) {
        goto try_tcpip;
    }

    errno = saved_errno;
    return -1;

try_tcpip:
    //  Try TCP/IP every time. The fallback depends on runtime platform state,
    //  so caching the decision would hide later environment changes.
#endif

    return make_fdpair_tcpip (r_, w_);
#elif defined ZLINK_HAVE_OPENVMS

    //  Whilst OpenVMS supports socketpair - it maps to AF_INET only.  Further,
    //  it does not set the socket options TCP_NODELAY and TCP_NODELACK which
    //  can lead to performance problems.
    //
    //  The bug will be fixed in V5.6 ECO4 and beyond.  In the meantime, we'll
    //  create the socket pair manually.
    struct sockaddr_in lcladdr;
    memset (&lcladdr, 0, sizeof lcladdr);
    lcladdr.sin_family = AF_INET;
    lcladdr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    lcladdr.sin_port = 0;

    int listener = open_socket (AF_INET, SOCK_STREAM, 0);
    errno_assert (listener != -1);

    int on = 1;
    int rc = setsockopt (listener, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    errno_assert (rc != -1);

    rc = setsockopt (listener, IPPROTO_TCP, TCP_NODELACK, &on, sizeof on);
    errno_assert (rc != -1);

    rc = bind (listener, (struct sockaddr *) &lcladdr, sizeof lcladdr);
    errno_assert (rc != -1);

    socklen_t lcladdr_len = sizeof lcladdr;

    rc = getsockname (listener, (struct sockaddr *) &lcladdr, &lcladdr_len);
    errno_assert (rc != -1);

    rc = listen (listener, 1);
    errno_assert (rc != -1);

    *w_ = open_socket (AF_INET, SOCK_STREAM, 0);
    errno_assert (*w_ != -1);

    rc = setsockopt (*w_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    errno_assert (rc != -1);

    rc = setsockopt (*w_, IPPROTO_TCP, TCP_NODELACK, &on, sizeof on);
    errno_assert (rc != -1);

    rc = connect (*w_, (struct sockaddr *) &lcladdr, sizeof lcladdr);
    errno_assert (rc != -1);

    *r_ = accept (listener, NULL, NULL);
    errno_assert (*r_ != -1);

    close (listener);

    return 0;
#elif defined ZLINK_HAVE_VXWORKS
    struct sockaddr_in lcladdr;
    memset (&lcladdr, 0, sizeof lcladdr);
    lcladdr.sin_family = AF_INET;
    lcladdr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    lcladdr.sin_port = 0;

    int listener = open_socket (AF_INET, SOCK_STREAM, 0);
    errno_assert (listener != -1);

    int on = 1;
    int rc = setsockopt (listener, IPPROTO_TCP, TCP_NODELAY, (char *) &on, sizeof on);
    errno_assert (rc != -1);

    rc = bind (listener, (struct sockaddr *) &lcladdr, sizeof lcladdr);
    errno_assert (rc != -1);

    socklen_t lcladdr_len = sizeof lcladdr;

    rc = getsockname (listener, (struct sockaddr *) &lcladdr, (int *) &lcladdr_len);
    errno_assert (rc != -1);

    rc = listen (listener, 1);
    errno_assert (rc != -1);

    *w_ = open_socket (AF_INET, SOCK_STREAM, 0);
    errno_assert (*w_ != -1);

    rc = setsockopt (*w_, IPPROTO_TCP, TCP_NODELAY, (char *) &on, sizeof on);
    errno_assert (rc != -1);

    rc = connect (*w_, (struct sockaddr *) &lcladdr, sizeof lcladdr);
    errno_assert (rc != -1);

    *r_ = accept (listener, NULL, NULL);
    errno_assert (*r_ != -1);

    close (listener);

    return 0;
#else
    // All other implementations support socketpair()
    int sv[2];
    int type = SOCK_STREAM;
    //  Setting this option result in sane behaviour when exec() functions
    //  are used. Old sockets are closed and don't block TCP ports, avoid
    //  leaks, etc.
#if defined ZLINK_HAVE_SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    int rc = socketpair (AF_UNIX, type, 0, sv);
    if (rc == -1) {
        errno_assert (errno == ENFILE || errno == EMFILE);
        *w_ = *r_ = -1;
        return -1;
    } else {
        make_socket_noninheritable (sv[0]);
        make_socket_noninheritable (sv[1]);

        *w_ = sv[0];
        *r_ = sv[1];
        return 0;
    }
#endif
}
