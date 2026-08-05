/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>
#include <string>
#if defined(ZLINK_HAVE_IPC)
#include <stdio.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void *create_sync_socket (int type_)
{
    void *socket = zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    return socket;
}

void close_sync_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}
}

static bool is_transport_available (const char *transport_)
{
    //  TCP and inproc are always available (core transports)
    if (strcmp (transport_, "tcp") == 0 || strcmp (transport_, "inproc") == 0)
        return true;

        //  IPC is available on Unix-like systems
#ifdef ZLINK_HAVE_IPC
    if (strcmp (transport_, "ipc") == 0)
        return true;
#endif

    //  WebSocket and TLS transports are optional and reported by zlink_has()
    return zlink_has (transport_) != 0;
}

static bool is_tls_transport (const char *transport_)
{
    return strcmp (transport_, "tls") == 0 || strcmp (transport_, "wss") == 0;
}

static void configure_tls (void *server_, void *client_, const tls_test_files_t &files_)
{
    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client_, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_CERT, files_.server_cert.c_str (), files_.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server_, ZLINK_OPT_TLS_KEY, files_.server_key.c_str (), files_.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client_, ZLINK_OPT_TLS_CA, files_.ca_cert.c_str (),
                                                 files_.ca_cert.size ()));
    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client_, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
}

static void bind_endpoint (void *socket_,
                           const char *transport_,
                           const char *inproc_name_,
                           char *endpoint_,
                           size_t endpoint_len_)
{
    if (strcmp (transport_, "inproc") == 0) {
        snprintf (endpoint_, endpoint_len_, "inproc://%s", inproc_name_);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (socket_, endpoint_));
        return;
    }

    if (strcmp (transport_, "tcp") == 0) {
        test_bind (socket_, "tcp://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "ipc") == 0) {
        test_bind (socket_, "ipc://*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "ws") == 0) {
        test_bind (socket_, "ws://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "wss") == 0) {
        test_bind (socket_, "wss://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    if (strcmp (transport_, "tls") == 0) {
        test_bind (socket_, "tls://127.0.0.1:*", endpoint_, endpoint_len_);
        return;
    }

    TEST_FAIL_MESSAGE ("unknown transport");
}

static void run_pair (const char *transport_)
{
    if (!is_transport_available (transport_))
        TEST_IGNORE_MESSAGE ("transport not available");

    void *server = create_sync_socket (ZLINK_SOCKET_PAIR);
    void *client = create_sync_socket (ZLINK_SOCKET_PAIR);

    tls_test_files_t tls_files;
    if (is_tls_transport (transport_)) {
        tls_files = make_tls_test_files ();
        configure_tls (server, client, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (server, transport_, "matrix_pair", endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    msleep (SETTLE_TIME);

    send_string_expect_success (client, "pair-hello", 0);
    recv_string_expect_success (server, "pair-hello", 0);
    send_string_expect_success (server, "pair-ack", 0);
    recv_string_expect_success (client, "pair-ack", 0);

    close_sync_socket (client);
    close_sync_socket (server);
    if (is_tls_transport (transport_))
        cleanup_tls_test_files (tls_files);
}

static void run_pubsub (const char *transport_)
{
    if (!is_transport_available (transport_))
        TEST_IGNORE_MESSAGE ("transport not available");

    void *pub = create_sync_socket (ZLINK_SOCKET_PUB);
    void *sub = create_sync_socket (ZLINK_SOCKET_SUB);

    tls_test_files_t tls_files;
    if (is_tls_transport (transport_)) {
        tls_files = make_tls_test_files ();
        configure_tls (pub, sub, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (pub, transport_, "matrix_pubsub", endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    msleep (SETTLE_TIME);

    // Prime subscription propagation before checking the real payload.
    send_string_expect_success (pub, "warmup", 0);
    msleep (SETTLE_TIME);
    recv_string_expect_success (sub, "warmup", 0);
    send_string_expect_success (pub, "pubsub-hello", 0);
    recv_string_expect_success (sub, "pubsub-hello", 0);

    close_sync_socket (sub);
    close_sync_socket (pub);
    if (is_tls_transport (transport_))
        cleanup_tls_test_files (tls_files);
}

static void run_router_dealer (const char *transport_)
{
    if (!is_transport_available (transport_))
        TEST_IGNORE_MESSAGE ("transport not available");

    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "DEALER1", 7));

    tls_test_files_t tls_files;
    if (is_tls_transport (transport_)) {
        tls_files = make_tls_test_files ();
        configure_tls (router, dealer, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (router, transport_, "matrix_router_dealer", endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    send_string_expect_success (dealer, "dealer-msg", 0);

    recv_string_expect_success (router, "DEALER1", 0);
    recv_string_expect_success (router, "dealer-msg", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "DEALER1", 7, ZLINK_SNDMORE));
    send_string_expect_success (router, "router-reply", 0);
    recv_string_expect_success (dealer, "router-reply", 0);

    close_sync_socket (dealer);
    close_sync_socket (router);
    if (is_tls_transport (transport_))
        cleanup_tls_test_files (tls_files);
}

static void run_router_router (const char *transport_)
{
    if (!is_transport_available (transport_))
        TEST_IGNORE_MESSAGE ("transport not available");

    void *server = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *client = create_sync_socket (ZLINK_SOCKET_ROUTER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "SERVER", 6));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "CLIENT", 6));

    tls_test_files_t tls_files;
    if (is_tls_transport (transport_)) {
        tls_files = make_tls_test_files ();
        configure_tls (server, client, tls_files);
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_endpoint (server, transport_, "matrix_router_router", endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    msleep (SETTLE_TIME);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "SERVER", 6, ZLINK_SNDMORE));
    send_string_expect_success (client, "router-msg", 0);

    recv_string_expect_success (server, "CLIENT", 0);
    recv_string_expect_success (server, "router-msg", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (server, "CLIENT", 6, ZLINK_SNDMORE));
    send_string_expect_success (server, "router-reply", 0);

    recv_string_expect_success (client, "SERVER", 0);
    recv_string_expect_success (client, "router-reply", 0);

    close_sync_socket (client);
    close_sync_socket (server);
    if (is_tls_transport (transport_))
        cleanup_tls_test_files (tls_files);
}

static void test_transport_matrix (const char *transport_)
{
    fprintf (stderr, "Testing transport: %s\n", transport_);
    fflush (stderr);

    run_pair (transport_);
    fprintf (stderr, "  PAIR complete\n");
    fflush (stderr);

    run_pubsub (transport_);
    fprintf (stderr, "  PUB/SUB complete\n");
    fflush (stderr);

    run_router_dealer (transport_);
    fprintf (stderr, "  ROUTER/DEALER complete\n");
    fflush (stderr);

    run_router_router (transport_);
    fprintf (stderr, "  ROUTER/ROUTER complete\n");
    fflush (stderr);
}

void test_matrix_tcp ()
{
    test_transport_matrix ("tcp");
}

void test_matrix_inproc ()
{
    test_transport_matrix ("inproc");
}

void test_matrix_ipc ()
{
    test_transport_matrix ("ipc");
}

void test_ipc_regular_file_bind_does_not_unlink ()
{
#if defined(ZLINK_HAVE_IPC)
    if (!is_transport_available ("ipc"))
        TEST_IGNORE_MESSAGE ("ipc is not available");

    std::string path = make_random_ipc_path ();
    FILE *file = fopen (path.c_str (), "w");
    TEST_ASSERT_NOT_NULL (file);
    TEST_ASSERT_EQUAL_INT (0, fclose (file));

    const std::string endpoint = std::string ("ipc://") + path;
    void *server = create_sync_socket (ZLINK_SOCKET_PAIR);
    errno = 0;
    const int rc = zlink_bind (server, endpoint.c_str ());
    const int saved_errno = errno;
    close_sync_socket (server);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_ADDR_IN_USE, rc);
    TEST_ASSERT_EQUAL_INT (EADDRINUSE, saved_errno);
    TEST_ASSERT_EQUAL_INT (0, access (path.c_str (), F_OK));
    unlink (path.c_str ());
#else
    TEST_IGNORE_MESSAGE ("ipc is not available");
#endif
}

void test_ipc_overlong_endpoint_does_not_unlink ()
{
#if defined(ZLINK_HAVE_IPC)
    if (!is_transport_available ("ipc"))
        TEST_IGNORE_MESSAGE ("ipc is not available");

    std::string path = "/tmp/zlink-ipc-preserve-";
    path += std::to_string (static_cast<unsigned long long> (getpid ()));
    path += "-";
    path.append (96, 'a');

    FILE *file = fopen (path.c_str (), "w");
    TEST_ASSERT_NOT_NULL (file);
    TEST_ASSERT_EQUAL_INT (0, fclose (file));

    const std::string endpoint = std::string ("ipc://") + path;
    void *server = create_sync_socket (ZLINK_SOCKET_PAIR);
    errno = 0;
    const int rc = zlink_bind (server, endpoint.c_str ());
    const int saved_errno = errno;
    close_sync_socket (server);

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_INTERNAL_ERROR, rc);
    TEST_ASSERT_EQUAL_INT (ENAMETOOLONG, saved_errno);
    TEST_ASSERT_EQUAL_INT (0, access (path.c_str (), F_OK));
    unlink (path.c_str ());
#else
    TEST_IGNORE_MESSAGE ("ipc is not available");
#endif
}

void test_matrix_ws ()
{
    test_transport_matrix ("ws");
}

void test_matrix_wss ()
{
    test_transport_matrix ("wss");
}

void test_matrix_tls ()
{
    test_transport_matrix ("tls");
}

int main ()
{
    // DEALER/ROUTER now establish two physical transport connections. The
    // encrypted matrix therefore performs twice as many TLS handshakes.
    setup_test_environment (120);

    UNITY_BEGIN ();
    RUN_TEST (test_matrix_tcp);
    RUN_TEST (test_matrix_inproc);
    RUN_TEST (test_matrix_ipc);
    RUN_TEST (test_ipc_regular_file_bind_does_not_unlink);
    RUN_TEST (test_ipc_overlong_endpoint_does_not_unlink);
    RUN_TEST (test_matrix_ws);
    RUN_TEST (test_matrix_wss);
    RUN_TEST (test_matrix_tls);
    return UNITY_END ();
}
