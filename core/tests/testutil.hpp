/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __TESTUTIL_HPP_INCLUDED__
#define __TESTUTIL_HPP_INCLUDED__

#include "test_platform.hpp"
#include <zlink.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

//  For AF_INET and IPPROTO_TCP
#if defined _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if defined(__MINGW32__)
#include <unistd.h>
#endif
#include <process.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

//  This defines the settle time used in tests; raise this if we
//  get test failures on slower systems due to binds/connects not
//  settled. Tested to work reliably at 1 msec on a fast PC.
#define SETTLE_TIME 300 //  In msec
//  Commonly used buffer size for ZLINK_OPT_LAST_ENDPOINT
//  this used to be sizeof ("tcp://[::ffff:127.127.127.127]:65536"), but this
//  may be too short for ipc wildcard binds, e.g.
#define MAX_SOCKET_STRING 256

//  We need to test codepaths with non-random bind ports. Use a per-process
//  offset so parallel test runs do not collide.
static inline int test_port_offset ()
{
    static int offset = -1;
    if (offset < 0) {
        const char *env = getenv ("ZLINK_TEST_PORT_OFFSET");
        if (env && *env) {
            offset = atoi (env);
        } else {
#if defined ZLINK_HAVE_WINDOWS
            offset = (_getpid () % 1000) * 10;
#else
            offset = (getpid () % 1000) * 10;
#endif
        }
    }
    return offset;
}

static inline int test_port (int base_)
{
    return base_ + test_port_offset ();
}

static inline const char *endpoint_0 ()
{
    static char buf[64];
    snprintf (buf, sizeof (buf), "tcp://127.0.0.1:%d", test_port (5555));
    return buf;
}

static inline const char *endpoint_1 ()
{
    static char buf[64];
    snprintf (buf, sizeof (buf), "tcp://127.0.0.1:%d", test_port (5556));
    return buf;
}

static inline const char *endpoint_2 ()
{
    static char buf[64];
    snprintf (buf, sizeof (buf), "tcp://127.0.0.1:%d", test_port (5557));
    return buf;
}

static inline const char *endpoint_3 ()
{
    static char buf[64];
    snprintf (buf, sizeof (buf), "tcp://127.0.0.1:%d", test_port (5558));
    return buf;
}

#define ENDPOINT_0 (endpoint_0 ())
#define ENDPOINT_1 (endpoint_1 ())
#define ENDPOINT_2 (endpoint_2 ())
#define ENDPOINT_3 (endpoint_3 ())
#define PORT_6 (test_port (5561))

#undef NDEBUG

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef ZLINK_HAVE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX // Macros min(a,b) and max(a,b)
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <stdexcept>
inline int close (SOCKET socket_)
{
    return closesocket (socket_);
}
typedef int socket_size_t;
inline const char *as_setsockopt_opt_t (const void *opt)
{
    return static_cast<const char *> (opt);
}
#else
typedef size_t socket_size_t;
inline const void *as_setsockopt_opt_t (const void *opt_)
{
    return opt_;
}
#endif

typedef zlink_fd_t fd_t;
#ifdef _WIN32
static const fd_t retired_fd = INVALID_SOCKET;
#else
static const fd_t retired_fd = -1;
#endif

//  In MSVC prior to v14, snprintf is not available
//  The closest implementation is the _snprintf_s function
#if defined _MSC_VER && _MSC_VER < 1900
#define snprintf(buffer_, count_, format_, ...)                                                    \
    _snprintf_s (buffer_, count_, _TRUNCATE, format_, __VA_ARGS__)
#endif

#define LIBZLINK_UNUSED(object) (void) object

//  Bounce a message from client to server and back
//  For REQ/REP or DEALER/DEALER pairs only
void bounce (void *server_, void *client_);

//  Same as bounce, but expect messages to never arrive
//  for security or subscriber reasons.
void expect_bounce_fail (void *server_, void *client_);

//  Receive 0MQ string from socket and convert into C string
//  Caller must free returned string. Returns NULL if the context
//  is being terminated.
char *s_recv (void *socket_);

bool streq (const char *lhs, const char *rhs);
bool strneq (const char *lhs, const char *rhs);

extern const char *SEQ_END;

std::string make_random_ipc_path ();
std::string make_test_temp_dir (const char *prefix_);

//  Temporary TLS test files (cert/key/CA) created from embedded PEMs.
struct tls_test_files_t
{
    std::string dir;
    std::string ca_cert;
    std::string server_cert;
    std::string server_key;
};

tls_test_files_t make_tls_test_files ();
void cleanup_tls_test_files (const tls_test_files_t &files_);

//  Sends a message composed of frames that are C strings or null frames.
//  The list must be terminated by SEQ_END.
//  Example: s_send_seq (req, "ABC", 0, "DEF", SEQ_END);

void s_send_seq (void *socket_, ...);

//  Receives message a number of frames long and checks that the frames have
//  the given data which can be either C strings or 0 for a null frame.
//  The list must be terminated by SEQ_END.
//  Example: s_recv_seq (rep, "ABC", 0, "DEF", SEQ_END);

void s_recv_seq (void *socket_, ...);


//  Sets a zero linger period on a socket and closes it.
void close_zero_linger (void *socket_);

//  Setups the test environment. Must be called at the beginning of each test
//  executable. On POSIX systems, it sets an alarm to the specified number of
//  seconds, after which the test will be killed. Set to 0 to disable this
//  timeout.
void setup_test_environment (int timeout_seconds_ = 60);

//  Provide portable millisecond sleep
//  http://www.cplusplus.com/forum/unices/60161/
//  http://en.cppreference.com/w/cpp/thread/sleep_for

void msleep (int milliseconds_);

enum
{
    zlink_test_poll_step_ms = 25
};

enum zlink_test_wait_step_result_t
{
    zlink_test_wait_retry,
    zlink_test_wait_done,
    zlink_test_wait_failed
};

template <typename Predicate> bool zlink_test_wait_until (int timeout_ms_, Predicate predicate_)
{
    const int attempts = timeout_ms_ / zlink_test_poll_step_ms;
    for (int i = 0; i < attempts; ++i) {
        if (predicate_ ())
            return true;
        msleep (zlink_test_poll_step_ms);
    }
    return false;
}

template <typename Predicate>
bool zlink_test_wait_until_step (int timeout_ms_, int step_ms_, Predicate predicate_)
{
    const int attempts = timeout_ms_ / step_ms_ + 1;
    for (int i = 0; i < attempts; ++i) {
        if (predicate_ ())
            return true;
        msleep (step_ms_);
    }
    return false;
}

template <typename Predicate>
bool zlink_test_wait_until_result (int timeout_ms_, Predicate predicate_)
{
    const int attempts = timeout_ms_ / zlink_test_poll_step_ms;
    for (int i = 0; i < attempts; ++i) {
        const zlink_test_wait_step_result_t result = predicate_ ();
        if (result == zlink_test_wait_done)
            return true;
        if (result == zlink_test_wait_failed)
            return false;
        msleep (zlink_test_poll_step_ms);
    }
    return false;
}

template <typename Predicate>
bool zlink_test_wait_until_result_step (int timeout_ms_, int step_ms_, Predicate predicate_)
{
    const int attempts = timeout_ms_ / step_ms_ + 1;
    for (int i = 0; i < attempts; ++i) {
        const zlink_test_wait_step_result_t result = predicate_ ();
        if (result == zlink_test_wait_done)
            return true;
        if (result == zlink_test_wait_failed)
            return false;
        msleep (step_ms_);
    }
    return false;
}

// check if IPv6 is available (0/false if not, 1/true if it is)
// only way to reliably check is to actually open a socket and try to bind it
int is_ipv6_available (void);


//  Wrapper around 'inet_pton' for systems that don't support it (e.g. Windows
//  XP)
int test_inet_pton (int af_, const char *src_, void *dst_);

//  Binds an ipv4 BSD socket to an ephemeral port, returns the compiled sockaddr
struct sockaddr_in bind_bsd_socket (int socket);

//  Some custom definitions for WebSocket testing.
#define IPPROTO_WS 10000
#define IPPROTO_WSS 10001

//  Connects a BSD socket to the ZLINK endpoint. Works with ipv4/ipv6/unix.
fd_t connect_socket (const char *endpoint_,
                     const int af_ = AF_INET,
                     const int protocol_ = IPPROTO_TCP);

//  Binds a BSD socket to an ephemeral port, returns the file descriptor.
//  The resulting ZLINK endpoint will be stored in my_endpoint, including the protocol
//  prefix, so ensure it is writable and of appropriate size.
//  Works with ipv4/ipv6/unix. With unix sockets address_/port_ can be empty and
//  my_endpoint_ will contain a random path.
fd_t bind_socket_resolve_port (const char *address_,
                               const char *port_,
                               char *my_endpoint_,
                               const int af_ = AF_INET,
                               const int protocol_ = IPPROTO_TCP);

int fuzzer_corpus_encode (const char *filename, uint8_t ***data, size_t **len, size_t *num_cases);

#endif
