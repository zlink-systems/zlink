/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "../src/api/socket/socket_api_internal.hpp"
#include "../src/runtime/core/recv_internal.hpp"
#include "testutil_unity.hpp"
#include "certs/test_certs.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#if defined _WIN32
#include "utils/windows.hpp"
#include <direct.h>
#if defined _MSC_VER
#if defined ZLINK_HAVE_IPC
#include <afunix.h>
#endif
#include <crtdbg.h>
#pragma warning(disable : 4996)
// iphlpapi is needed for if_nametoindex (not on Windows XP)
#if _WIN32_WINNT > _WIN32_WINNT_WINXP
#pragma comment(lib, "iphlpapi")
#endif
#endif
#else
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/un.h>
#include <dirent.h>
#if defined(ZLINK_HAVE_AIX)
#include <sys/types.h>
#include <sys/socketvar.h>
#endif
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

const char *SEQ_END = (const char *) 1;

const char bounce_content[] = "12345678ABCDEFGH12345678abcdefgh";

static void send_bounce_msg (void *socket_)
{
    send_string_expect_success (socket_, bounce_content, ZLINK_SNDMORE);
    send_string_expect_success (socket_, bounce_content, 0);
}

static void recv_bounce_msg (void *socket_)
{
    zlink_msg_t msg;
    zlink_msg_init (&msg);

    socket_handle_t handle = as_socket_handle (socket_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::recv_msg_internal (handle.socket, &msg, 0));
    TEST_ASSERT_EQUAL_STRING (bounce_content, static_cast<const char *> (zlink_msg_data (&msg)));
    TEST_ASSERT_TRUE (test_msg_has_more (&msg));
    zlink_msg_close (&msg);

    zlink_msg_init (&msg);
    TEST_ASSERT_SUCCESS_ERRNO (zlink::recv_msg_internal (handle.socket, &msg, 0));
    TEST_ASSERT_EQUAL_STRING (bounce_content, static_cast<const char *> (zlink_msg_data (&msg)));
    TEST_ASSERT_FALSE (test_msg_has_more (&msg));
    zlink_msg_close (&msg);
}

void bounce (void *server_, void *client_)
{
    //  Send message from client to server
    send_bounce_msg (client_);

    //  Receive message at server side and
    //  check that message is still the same
    recv_bounce_msg (server_);

    //  Send two parts back to client
    send_bounce_msg (server_);

    //  Receive the two parts at the client side
    recv_bounce_msg (client_);
}

static void send_bounce_msg_may_fail (void *socket_)
{
    int timeout = 250;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (int)));
    int rc = zlink_send (socket_, bounce_content, 32, ZLINK_SNDMORE);
    TEST_ASSERT_TRUE ((rc == 32) || ((rc == -1) && (errno == EAGAIN)));
    rc = zlink_send (socket_, bounce_content, 32, 0);
    TEST_ASSERT_TRUE ((rc == 32) || ((rc == -1) && (errno == EAGAIN)));
}

static void recv_bounce_msg_fail (void *socket_)
{
    int timeout = 250;
    socket_handle_t handle = as_socket_handle (socket_);
    TEST_ASSERT_TRUE (
      zlink::wait_socket_events_internal (handle.socket, 1, timeout) <= 0);
}

void expect_bounce_fail (void *server_, void *client_)
{
    //  Send message from client to server
    send_bounce_msg_may_fail (client_);

    //  Receive message at server side (should not succeed)
    recv_bounce_msg_fail (server_);

    //  Send message from server to client to test other direction
    //  If connection failed, send may block, without a timeout
    send_bounce_msg_may_fail (server_);

    //  Receive message at client side (should not succeed)
    recv_bounce_msg_fail (client_);
}

char *s_recv (void *socket_)
{
    char buffer[256];
    socket_handle_t handle = as_socket_handle (socket_);
    int size = zlink::recv_buffer_internal (handle.socket, buffer, 255, 0);
    if (size == -1)
        return NULL;
    if (size > 255)
        size = 255;
    buffer[size] = 0;
    return strdup (buffer);
}

void s_send_seq (void *socket_, ...)
{
    va_list ap;
    va_start (ap, socket_);
    const char *data = va_arg (ap, const char *);
    while (true) {
        const char *prev = data;
        data = va_arg (ap, const char *);
        bool end = data == SEQ_END;

        if (!prev) {
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_send (socket_, static_cast<const void *> (NULL), 0, end ? 0 : ZLINK_SNDMORE));
        } else {
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_send (socket_, prev, strlen (prev) + 1, end ? 0 : ZLINK_SNDMORE));
        }
        if (end)
            break;
    }
    va_end (ap);
}

void s_recv_seq (void *socket_, ...)
{
    zlink_msg_t msg;
    zlink_msg_init (&msg);

    int more;

    va_list ap;
    va_start (ap, socket_);
    const char *data = va_arg (ap, const char *);

    while (true) {
        socket_handle_t handle = as_socket_handle (socket_);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink::recv_msg_internal (handle.socket, &msg, 0));

        if (!data)
            TEST_ASSERT_EQUAL_INT (0, zlink_msg_size (&msg));
        else
            TEST_ASSERT_EQUAL_STRING (data, (const char *) zlink_msg_data (&msg));

        data = va_arg (ap, const char *);
        bool end = data == SEQ_END;

        more = test_msg_has_more (&msg) ? 1 : 0;

        TEST_ASSERT_TRUE (!more == end);
        if (end)
            break;
    }
    va_end (ap);

    zlink_msg_close (&msg);
}

void close_zero_linger (void *socket_)
{
    int linger = 0;
    int rc = zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    TEST_ASSERT_TRUE (rc == 0 || errno == ETERM);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}

void setup_test_environment (int timeout_seconds_)
{
    // Disable output buffering to prevent hang when stdout is not a TTY
    setvbuf (stdout, NULL, _IONBF, 0);
    setvbuf (stderr, NULL, _IONBF, 0);

#if defined _WIN32
#if defined _MSC_VER
    _set_abort_behavior (0, _WRITE_ABORT_MSG);
    _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#else
#if defined ZLINK_HAVE_CYGWIN
    // abort test after 121 seconds
    alarm (121);
#else
#if !defined ZLINK_DISABLE_TEST_TIMEOUT
    // abort test after timeout_seconds_ seconds
    alarm (timeout_seconds_);
#endif
#endif
#endif
#if defined __MVS__
    // z/OS UNIX System Services: Ignore SIGPIPE during test runs, as a
    // workaround for no SO_NOGSIGPIPE socket option.
    signal (SIGPIPE, SIG_IGN);
#endif
}

void msleep (int milliseconds_)
{
#ifdef ZLINK_HAVE_WINDOWS
    Sleep (milliseconds_);
#else
    usleep (static_cast<useconds_t> (milliseconds_) * 1000);
#endif
}

int is_ipv6_available ()
{
#if defined(ZLINK_HAVE_WINDOWS) && (_WIN32_WINNT < 0x0600)
    return 0;
#else
    int rc, ipv6 = 1;
    struct sockaddr_in6 test_addr;

    memset (&test_addr, 0, sizeof (test_addr));
    test_addr.sin6_family = AF_INET6;
    inet_pton (AF_INET6, "::1", &(test_addr.sin6_addr));

    fd_t fd = socket (AF_INET6, SOCK_STREAM, IPPROTO_IP);
    if (fd == retired_fd)
        ipv6 = 0;
    else {
#ifdef ZLINK_HAVE_WINDOWS
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &ipv6, sizeof (int));
        rc = setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &ipv6, sizeof (int));
        if (rc == SOCKET_ERROR)
            ipv6 = 0;
        else {
            rc = bind (fd, (struct sockaddr *) &test_addr, sizeof (test_addr));
            if (rc == SOCKET_ERROR)
                ipv6 = 0;
        }
#else
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &ipv6, sizeof (int));
        rc = setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6, sizeof (int));
        if (rc != 0)
            ipv6 = 0;
        else {
            rc = bind (fd, reinterpret_cast<struct sockaddr *> (&test_addr), sizeof (test_addr));
            if (rc != 0)
                ipv6 = 0;
        }
#endif
        close (fd);
    }

    return ipv6;
#endif // _WIN32_WINNT < 0x0600
}

int test_inet_pton (int af_, const char *src_, void *dst_)
{
#if defined(ZLINK_HAVE_WINDOWS) && (_WIN32_WINNT < 0x0600)
    if (af_ == AF_INET) {
        struct in_addr *ip4addr = (struct in_addr *) dst_;

        ip4addr->s_addr = inet_addr (src_);

        //  INADDR_NONE is -1 which is also a valid representation for IP
        //  255.255.255.255
        if (ip4addr->s_addr == INADDR_NONE && strcmp (src_, "255.255.255.255") != 0) {
            return 0;
        }

        //  Success
        return 1;
    } else {
        //  Not supported.
        return 0;
    }
#else
    return inet_pton (af_, src_, dst_);
#endif
}

sockaddr_in bind_bsd_socket (int socket_)
{
    struct sockaddr_in saddr;
    memset (&saddr, 0, sizeof (saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT >= 0x0600)
    saddr.sin_port = 0;
#else
    saddr.sin_port = htons (PORT_6);
#endif

    TEST_ASSERT_SUCCESS_RAW_ERRNO (bind (socket_, (struct sockaddr *) &saddr, sizeof (saddr)));

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT >= 0x0600)
    socklen_t saddr_len = sizeof (saddr);
    TEST_ASSERT_SUCCESS_RAW_ERRNO (getsockname (socket_, (struct sockaddr *) &saddr, &saddr_len));
#endif

    return saddr;
}

fd_t connect_socket (const char *endpoint_, const int af_, const int protocol_)
{
    struct sockaddr_storage addr;
    //  OSX is very opinionated and wants the size to match the AF family type
    socklen_t addr_len;
    const fd_t s_pre = socket (af_, SOCK_STREAM, protocol_ == IPPROTO_TCP ? IPPROTO_TCP : 0);
#ifdef ZLINK_HAVE_WINDOWS
    TEST_ASSERT_NOT_EQUAL (INVALID_SOCKET, s_pre);
#else
    TEST_ASSERT_NOT_EQUAL (-1, s_pre);
#endif

    if (af_ == AF_INET || af_ == AF_INET6) {
        const char *port = strrchr (endpoint_, ':') + 1;
        char address[MAX_SOCKET_STRING];
        // getaddrinfo does not like [x:y::z]
        if (*strchr (endpoint_, '/') + 2 == '[') {
            strcpy (address, strchr (endpoint_, '[') + 1);
            address[strlen (address) - strlen (port) - 2] = '\0';
        } else {
            strcpy (address, strchr (endpoint_, '/') + 2);
            address[strlen (address) - strlen (port) - 1] = '\0';
        }

        struct addrinfo *in, hint;
        memset (&hint, 0, sizeof (struct addrinfo));
        hint.ai_flags = AI_NUMERICSERV;
        hint.ai_family = af_;
        hint.ai_socktype = SOCK_STREAM;
        hint.ai_protocol = IPPROTO_TCP;

        TEST_ASSERT_SUCCESS_RAW_ZERO_ERRNO (getaddrinfo (address, port, &hint, &in));
        TEST_ASSERT_NOT_NULL (in);
        memcpy (&addr, in->ai_addr, in->ai_addrlen);
        addr_len = (socklen_t) in->ai_addrlen;
        freeaddrinfo (in);
    } else {
#if defined(ZLINK_HAVE_IPC)
        //  Cannot cast addr as gcc 4.4 will fail with strict aliasing errors
        (*(struct sockaddr_un *) &addr).sun_family = AF_UNIX;
        strcpy ((*(struct sockaddr_un *) &addr).sun_path, endpoint_);
        addr_len = sizeof (struct sockaddr_un);
#else
        return retired_fd;
#endif
    }

    TEST_ASSERT_SUCCESS_RAW_ERRNO (connect (s_pre, (struct sockaddr *) &addr, addr_len));

    return s_pre;
}

fd_t bind_socket_resolve_port (
  const char *address_, const char *port_, char *my_endpoint_, const int af_, const int protocol_)
{
    struct sockaddr_storage addr;
    //  OSX is very opinionated and wants the size to match the AF family type
    socklen_t addr_len;
    const fd_t s_pre = socket (af_, SOCK_STREAM, protocol_ == IPPROTO_TCP ? IPPROTO_TCP : 0);
#ifdef ZLINK_HAVE_WINDOWS
    TEST_ASSERT_NOT_EQUAL (INVALID_SOCKET, s_pre);
#else
    TEST_ASSERT_NOT_EQUAL (-1, s_pre);
#endif

    if (af_ == AF_INET || af_ == AF_INET6) {
#ifdef ZLINK_HAVE_WINDOWS
        const char flag = '\1';
#elif defined ZLINK_HAVE_VXWORKS
        char flag = '\1';
#else
        int flag = 1;
#endif
        struct addrinfo *in, hint;
        memset (&hint, 0, sizeof (struct addrinfo));
        hint.ai_flags = AI_NUMERICSERV;
        hint.ai_family = af_;
        hint.ai_socktype = SOCK_STREAM;
        hint.ai_protocol = IPPROTO_TCP;

        TEST_ASSERT_SUCCESS_RAW_ERRNO (
          setsockopt (s_pre, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof (int)));
        TEST_ASSERT_SUCCESS_RAW_ZERO_ERRNO (getaddrinfo (address_, port_, &hint, &in));
        TEST_ASSERT_NOT_NULL (in);
        memcpy (&addr, in->ai_addr, in->ai_addrlen);
        addr_len = (socklen_t) in->ai_addrlen;
        freeaddrinfo (in);
    } else {
#if defined(ZLINK_HAVE_IPC)
        //  Cannot cast addr as gcc 4.4 will fail with strict aliasing errors
        (*(struct sockaddr_un *) &addr).sun_family = AF_UNIX;
        addr_len = sizeof (struct sockaddr_un);
        const std::string ipc_path = make_random_ipc_path ();
        strcpy ((*(struct sockaddr_un *) &addr).sun_path, ipc_path.c_str ());
        memcpy (my_endpoint_, "ipc://", 7);
        strcat (my_endpoint_, ipc_path.c_str ());

#else
        return retired_fd;
#endif
    }

    TEST_ASSERT_SUCCESS_RAW_ERRNO (bind (s_pre, (struct sockaddr *) &addr, addr_len));
    TEST_ASSERT_SUCCESS_RAW_ERRNO (listen (s_pre, SOMAXCONN));

    if (af_ == AF_INET || af_ == AF_INET6) {
        addr_len = sizeof (struct sockaddr_storage);
        TEST_ASSERT_SUCCESS_RAW_ERRNO (getsockname (s_pre, (struct sockaddr *) &addr, &addr_len));
        snprintf (my_endpoint_, 6 + strlen (address_) + 7 * sizeof (char), "%s://%s:%u",
                  protocol_ == IPPROTO_TCP   ? "tcp"
                  : protocol_ == IPPROTO_WSS ? "wss"
                                             : "ws",
                  address_,
                  af_ == AF_INET ? ntohs ((*(struct sockaddr_in *) &addr).sin_port)
                                 : ntohs ((*(struct sockaddr_in6 *) &addr).sin6_port));
    }

    return s_pre;
}

bool streq (const char *lhs_, const char *rhs_)
{
    return strcmp (lhs_, rhs_) == 0;
}

bool strneq (const char *lhs_, const char *rhs_)
{
    return strcmp (lhs_, rhs_) != 0;
}

std::string make_random_ipc_path ()
{
#ifdef ZLINK_HAVE_WINDOWS
    char path[MAX_PATH] = "";
    TEST_ASSERT_SUCCESS_RAW_ERRNO (tmpnam_s (path));
    TEST_ASSERT_SUCCESS_RAW_ERRNO (_mkdir (path));
    strcat (path, "/ipc");
    return std::string (path);
#else
    char path[PATH_MAX] = "";
    const char *tmpdir = getenv ("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    const int n = snprintf (path, sizeof (path), "%s/zlinkXXXXXX", tmpdir);
    if (n <= 0 || static_cast<size_t> (n) >= sizeof (path))
        strcpy (path, "/tmp/zlinkXXXXXX");
    const int fd = mkstemp (path);
    TEST_ASSERT_TRUE (fd != -1);
    close (fd);
    unlink (path);
    return std::string (path);
#endif
}

std::string make_test_temp_dir (const char *prefix_)
{
#ifdef ZLINK_HAVE_WINDOWS
    LIBZLINK_UNUSED (prefix_);
    char path[MAX_PATH] = "";
    TEST_ASSERT_SUCCESS_RAW_ERRNO (tmpnam_s (path));
    TEST_ASSERT_SUCCESS_RAW_ERRNO (_mkdir (path));
    return std::string (path);
#else
    char path[PATH_MAX] = "";
    const char *tmpdir = getenv ("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    const int n =
      snprintf (path, sizeof (path), "%s/%sXXXXXX", tmpdir, prefix_ ? prefix_ : "zlink");
    if (n <= 0 || static_cast<size_t> (n) >= sizeof (path))
        strcpy (path, "/tmp/zlinkXXXXXX");
    char *dir = mkdtemp (path);
    TEST_ASSERT_NOT_NULL (dir);
    return std::string (dir);
#endif
}

static bool write_pem_file (const std::string &path_, const char *pem_)
{
    FILE *fp = fopen (path_.c_str (), "wb");
    if (!fp)
        return false;
    const size_t len = strlen (pem_);
    const size_t written = fwrite (pem_, 1, len, fp);
    fclose (fp);
    return written == len;
}

tls_test_files_t make_tls_test_files ()
{
    tls_test_files_t files;
    files.dir = make_test_temp_dir ("zlink_tls_");

    files.ca_cert = files.dir + "/ca.crt";
    files.server_cert = files.dir + "/server.crt";
    files.server_key = files.dir + "/server.key";

    TEST_ASSERT_TRUE (write_pem_file (files.ca_cert, zlink::test_certs::ca_cert_pem));
    TEST_ASSERT_TRUE (write_pem_file (files.server_cert, zlink::test_certs::server_cert_pem));
    TEST_ASSERT_TRUE (write_pem_file (files.server_key, zlink::test_certs::server_key_pem));

    return files;
}

void cleanup_tls_test_files (const tls_test_files_t &files_)
{
    remove (files_.ca_cert.c_str ());
    remove (files_.server_cert.c_str ());
    remove (files_.server_key.c_str ());
#ifdef ZLINK_HAVE_WINDOWS
    _rmdir (files_.dir.c_str ());
#else
    rmdir (files_.dir.c_str ());
#endif
}

#if defined _WIN32
int fuzzer_corpus_encode (const char *dirname, uint8_t ***data, size_t **len, size_t *num_cases)
{
    (void) dirname;
    (void) data;
    (void) len;
    (void) num_cases;

    return -1;
}

#else

int fuzzer_corpus_encode (const char *dirname, uint8_t ***data, size_t **len, size_t *num_cases)
{
    TEST_ASSERT_NOT_NULL (dirname);
    TEST_ASSERT_NOT_NULL (data);
    TEST_ASSERT_NOT_NULL (len);

    struct dirent *ent;
    DIR *dir = opendir (dirname);
    if (!dir)
        return -1;

    *len = NULL;
    *data = NULL;
    *num_cases = 0;

    while ((ent = readdir (dir)) != NULL) {
        if (!strcmp (ent->d_name, ".") || !strcmp (ent->d_name, ".."))
            continue;

        char *filename = (char *) malloc (strlen (dirname) + strlen (ent->d_name) + 2);
        TEST_ASSERT_NOT_NULL (filename);
        strcpy (filename, dirname);
        strcat (filename, "/");
        strcat (filename, ent->d_name);
        FILE *f = fopen (filename, "r");
        free (filename);
        if (!f)
            continue;

        fseek (f, 0, SEEK_END);
        size_t file_len = ftell (f);
        fseek (f, 0, SEEK_SET);
        if (file_len == 0) {
            fclose (f);
            continue;
        }

        *len = (size_t *) realloc (*len, (*num_cases + 1) * sizeof (size_t));
        TEST_ASSERT_NOT_NULL (*len);
        *(*len + *num_cases) = file_len;
        *data = (uint8_t **) realloc (*data, (*num_cases + 1) * sizeof (uint8_t *));
        TEST_ASSERT_NOT_NULL (*data);
        *(*data + *num_cases) = (uint8_t *) malloc (file_len * sizeof (uint8_t));
        TEST_ASSERT_NOT_NULL (*(*data + *num_cases));
        size_t read_bytes = 0;
        read_bytes = fread (*(*data + *num_cases), 1, file_len, f);
        TEST_ASSERT_EQUAL (file_len, read_bytes);
        (*num_cases)++;

        fclose (f);
    }

    closedir (dir);

    return 0;
}
#endif
