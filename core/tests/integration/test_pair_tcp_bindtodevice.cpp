/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#if defined(__linux__)
#include <dirent.h>
#include <cerrno>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#endif

SETUP_TEARDOWN_TESTCONTEXT

#if defined(__linux__) && defined(SO_BINDTODEVICE)
static int find_tcp_fd (int port_, bool listener_)
{
    DIR *directory = opendir ("/proc/self/fd");
    TEST_ASSERT_NOT_NULL (directory);
    int found = -1;
    for (struct dirent *entry = readdir (directory); entry;
         entry = readdir (directory)) {
        char *end = NULL;
        const long number = strtol (entry->d_name, &end, 10);
        if (*entry->d_name == '\0' || *end != '\0')
            continue;
        const int fd = static_cast<int> (number);
        struct sockaddr_in address;
        socklen_t length = sizeof address;
        const int rc = listener_ ? getsockname (fd, reinterpret_cast<sockaddr *> (&address), &length)
                                 : getpeername (fd, reinterpret_cast<sockaddr *> (&address), &length);
        if (rc != 0 || address.sin_family != AF_INET || ntohs (address.sin_port) != port_)
            continue;
        int accepting = 0;
        length = sizeof accepting;
        if (getsockopt (fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &length) == 0
            && static_cast<bool> (accepting) == listener_) {
            found = fd;
            break;
        }
    }
    closedir (directory);
    return found;
}

static void assert_device (int fd_, const char *expected_)
{
    TEST_ASSERT_TRUE (fd_ >= 0);
    char actual[IFNAMSIZ] = {};
    socklen_t length = sizeof actual;
    TEST_ASSERT_EQUAL_INT (0, getsockopt (fd_, SOL_SOCKET, SO_BINDTODEVICE, actual, &length));
    TEST_ASSERT_EQUAL_STRING (expected_, actual);
}

void test_bindtodevice_on_listener_and_connecter ()
{
    int probe = socket (AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE (probe >= 0);
    const int probe_rc = setsockopt (probe, SOL_SOCKET, SO_BINDTODEVICE, "lo", 2);
    const int probe_errno = errno;
    close (probe);
    if (probe_rc != 0 && (probe_errno == EPERM || probe_errno == EACCES || probe_errno == ENOPROTOOPT))
        TEST_IGNORE_MESSAGE ("SO_BINDTODEVICE requires permission or kernel support");
    TEST_ASSERT_EQUAL_INT (0, probe_rc);

    void *listener = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (listener, ZLINK_OPT_BINDTODEVICE, "lo", 2));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (listener, endpoint, sizeof endpoint);
    const int port = atoi (strrchr (endpoint, ':') + 1);
    assert_device (find_tcp_fd (port, true), "lo");

    void *connecter = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (connecter, ZLINK_OPT_BINDTODEVICE, "lo", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (connecter, endpoint));
    bounce (listener, connecter);
    assert_device (find_tcp_fd (port, false), "lo");

    test_context_socket_close (connecter);
    test_context_socket_close (listener);
}

void test_default_device_is_empty ()
{
    void *listener = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (listener, endpoint, sizeof endpoint);
    const int port = atoi (strrchr (endpoint, ':') + 1);
    assert_device (find_tcp_fd (port, true), "");
    test_context_socket_close (listener);
}
#else
void test_bindtodevice_unavailable ()
{
    TEST_IGNORE_MESSAGE ("SO_BINDTODEVICE is unavailable on this platform");
}
#endif

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#if defined(__linux__) && defined(SO_BINDTODEVICE)
    RUN_TEST (test_bindtodevice_on_listener_and_connecter);
    RUN_TEST (test_default_device_is_empty);
#else
    RUN_TEST (test_bindtodevice_unavailable);
#endif
    return UNITY_END ();
}
