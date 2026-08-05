/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Test suite for the ASIO SSL infrastructure
 *
 * These tests verify that the Boost.Asio SSL layer works correctly
 * for encrypted TCP connections. The actual ZLINK SSL integration will
 * use ssl_transport_t which wraps these primitives.
 *
 * NOTE: These tests require OpenSSL and are only compiled when
 * ZLINK_HAVE_ASIO_SSL is defined.
 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <thread>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

//  Include test certificates (embedded PEM strings)
#include "certs/test_certs.hpp"


void setUp ()
{
}

void tearDown ()
{
}

//  Test 1: SSL context creation
void test_ssl_context_creation ()
{
    //  Test TLS 1.2 server context creation
    ssl::context server_ctx (ssl::context::tlsv12_server);

    //  Test TLS 1.2 client context creation
    ssl::context client_ctx (ssl::context::tlsv12_client);

    //  Test that we can set verification mode
    client_ctx.set_verify_mode (ssl::verify_none);
    server_ctx.set_verify_mode (ssl::verify_none);

    //  If we get here, context creation succeeded
    TEST_PASS ();
}

//  Test 2: SSL certificate loading from PEM buffer
void test_ssl_certificate_loading ()
{
    ssl::context server_ctx (ssl::context::tlsv12_server);

    //  Try to load the server certificate
    try {
        server_ctx.use_certificate_chain (net::buffer (
          zlink::test_certs::server_cert_pem, strlen (zlink::test_certs::server_cert_pem)));

        server_ctx.use_private_key (net::buffer (zlink::test_certs::server_key_pem,
                                                 strlen (zlink::test_certs::server_key_pem)),
                                    ssl::context::pem);

        TEST_PASS ();
    }
    catch (const boost::system::system_error &e) {
        //  If certificate loading failed, report it
        char msg[256];
        snprintf (msg, sizeof (msg), "Certificate loading failed: %s", e.what ());
        TEST_FAIL_MESSAGE (msg);
    }
}

//  Test 3: SSL stream creation
void test_ssl_stream_creation ()
{
    net::io_context io_context;
    ssl::context ssl_ctx (ssl::context::tlsv12_client);

    //  Create SSL stream
    ssl::stream<tcp::socket> ssl_stream (io_context, ssl_ctx);

    //  Check that the lowest layer (TCP socket) exists
    TEST_ASSERT_FALSE (ssl_stream.lowest_layer ().is_open ());

    TEST_PASS ();
}

//  Test 4: TCP connection without SSL (baseline test)
void test_tcp_connection_baseline ()
{
    net::io_context io_context;

    //  Create work guard to keep io_context running
    auto work = net::make_work_guard (io_context);

    //  Create acceptor on ephemeral port
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    //  Server socket
    tcp::socket server_socket (io_context);

    //  Client socket
    tcp::socket client_socket (io_context);

    //  State tracking
    std::atomic<bool> all_done{false};
    std::atomic<int> completed{0};
    boost::system::error_code accept_ec, connect_ec;

    //  Async accept
    acceptor.async_accept (server_socket, [&] (const boost::system::error_code &ec) {
        accept_ec = ec;
        if (++completed >= 2) {
            all_done = true;
            work.reset ();
        }
    });

    //  Async connect
    client_socket.async_connect (endpoint, [&] (const boost::system::error_code &ec) {
        connect_ec = ec;
        if (++completed >= 2) {
            all_done = true;
            work.reset ();
        }
    });

    //  Run io_context in a separate thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        work.reset ();
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "TCP connection timed out");
    TEST_ASSERT_FALSE_MESSAGE (accept_ec.failed (), accept_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (connect_ec.failed (), connect_ec.message ().c_str ());
    TEST_ASSERT_TRUE (server_socket.is_open ());
    TEST_ASSERT_TRUE (client_socket.is_open ());
}

//  Test 5: SSL handshake with self-signed certificates
void test_ssl_handshake ()
{
    net::io_context io_context;

    //  Create work guard to keep io_context running
    auto work = net::make_work_guard (io_context);

    //  Create SSL contexts
    ssl::context server_ssl_ctx (ssl::context::tlsv12_server);
    ssl::context client_ssl_ctx (ssl::context::tlsv12_client);

    //  Configure server context with test certificates
    try {
        server_ssl_ctx.use_certificate_chain (net::buffer (
          zlink::test_certs::server_cert_pem, strlen (zlink::test_certs::server_cert_pem)));
        server_ssl_ctx.use_private_key (net::buffer (zlink::test_certs::server_key_pem,
                                                     strlen (zlink::test_certs::server_key_pem)),
                                        ssl::context::pem);
    }
    catch (const std::exception &e) {
        char msg[256];
        snprintf (msg, sizeof (msg), "Server certificate loading failed: %s", e.what ());
        TEST_FAIL_MESSAGE (msg);
        return;
    }

    //  Client: disable verification for self-signed certs
    client_ssl_ctx.set_verify_mode (ssl::verify_none);

    //  Create acceptor
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    //  Server SSL stream
    ssl::stream<tcp::socket> server_stream (io_context, server_ssl_ctx);

    //  Client SSL stream
    ssl::stream<tcp::socket> client_stream (io_context, client_ssl_ctx);

    //  State tracking
    std::atomic<bool> all_done{false};
    std::atomic<int> stage{0};
    boost::system::error_code server_hs_ec, client_hs_ec;

    //  Set up the chain of operations
    //  1. Accept TCP connection
    acceptor.async_accept (
      server_stream.lowest_layer (), [&] (const boost::system::error_code &ec) {
          if (ec) {
              all_done = true;
              work.reset ();
              return;
          }

          //  2. Server: SSL handshake
          server_stream.async_handshake (ssl::stream_base::server,
                                         [&] (const boost::system::error_code &ec) {
                                             server_hs_ec = ec;
                                             if (++stage >= 2) {
                                                 all_done = true;
                                                 work.reset ();
                                             }
                                         });
      });

    //  1. Client: TCP connect
    client_stream.lowest_layer ().async_connect (
      endpoint, [&] (const boost::system::error_code &ec) {
          if (ec) {
              all_done = true;
              work.reset ();
              return;
          }

          //  2. Client: SSL handshake
          client_stream.async_handshake (ssl::stream_base::client,
                                         [&] (const boost::system::error_code &ec) {
                                             client_hs_ec = ec;
                                             if (++stage >= 2) {
                                                 all_done = true;
                                                 work.reset ();
                                             }
                                         });
      });

    //  Run io_context in a separate thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        work.reset ();
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "SSL handshake timed out");
    TEST_ASSERT_FALSE_MESSAGE (server_hs_ec.failed (), server_hs_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (client_hs_ec.failed (), client_hs_ec.message ().c_str ());

    //  Note: Don't call sync shutdown - it can block
    //  Let the streams destruct naturally
}

//  Test 6: ZLINK tls:// end-to-end
void test_zlink_tls_pair ()
{
#if defined ZLINK_HAVE_TLS
    setup_test_context ();
    const tls_test_files_t files = make_tls_test_files ();

    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (), files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));

    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (server, "tls://127.0.0.1:*", endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "hello", 0);
    recv_string_expect_success (server, "hello", 0);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
    cleanup_tls_test_files (files);
    teardown_test_context ();
#else
    TEST_IGNORE_MESSAGE ("TLS not available");
#endif
}

//  Test 6: Multiple SSL contexts can coexist
void test_multiple_ssl_contexts ()
{
    ssl::context ctx1 (ssl::context::tlsv12_server);
    ssl::context ctx2 (ssl::context::tlsv12_client);
    ssl::context ctx3 (ssl::context::tlsv12_server);

    //  Each context should be independent
    ctx1.set_verify_mode (ssl::verify_none);
    ctx2.set_verify_mode (ssl::verify_peer);
    ctx3.set_verify_mode (ssl::verify_fail_if_no_peer_cert);

    TEST_PASS ();
}

//  Test 7: SSL context reuse across multiple streams
void test_ssl_context_reuse ()
{
    net::io_context io_context;
    ssl::context ssl_ctx (ssl::context::tlsv12_client);
    ssl_ctx.set_verify_mode (ssl::verify_none);

    //  Create multiple streams using same context
    ssl::stream<tcp::socket> stream1 (io_context, ssl_ctx);
    ssl::stream<tcp::socket> stream2 (io_context, ssl_ctx);
    ssl::stream<tcp::socket> stream3 (io_context, ssl_ctx);

    //  All streams should be independent but share context
    TEST_ASSERT_FALSE (stream1.lowest_layer ().is_open ());
    TEST_ASSERT_FALSE (stream2.lowest_layer ().is_open ());
    TEST_ASSERT_FALSE (stream3.lowest_layer ().is_open ());

    TEST_PASS ();
}

//  Test 8: SSL encrypted data exchange
void test_ssl_data_exchange ()
{
    net::io_context io_context;

    //  Create work guard to keep io_context running
    auto work = net::make_work_guard (io_context);

    //  Create SSL contexts
    ssl::context server_ssl_ctx (ssl::context::tlsv12_server);
    ssl::context client_ssl_ctx (ssl::context::tlsv12_client);

    //  Configure server context with test certificates
    try {
        server_ssl_ctx.use_certificate_chain (net::buffer (
          zlink::test_certs::server_cert_pem, strlen (zlink::test_certs::server_cert_pem)));
        server_ssl_ctx.use_private_key (net::buffer (zlink::test_certs::server_key_pem,
                                                     strlen (zlink::test_certs::server_key_pem)),
                                        ssl::context::pem);
    }
    catch (const std::exception &e) {
        char msg[256];
        snprintf (msg, sizeof (msg), "Server certificate loading failed: %s", e.what ());
        TEST_FAIL_MESSAGE (msg);
        return;
    }

    //  Client: disable verification for self-signed certs
    client_ssl_ctx.set_verify_mode (ssl::verify_none);

    //  Create acceptor
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    //  Server SSL stream
    ssl::stream<tcp::socket> server_stream (io_context, server_ssl_ctx);

    //  Client SSL stream
    ssl::stream<tcp::socket> client_stream (io_context, client_ssl_ctx);

    //  Test data
    const char *test_msg = "Hello SSL!";
    char recv_buf[64] = {0};
    std::size_t bytes_read = 0;

    //  State tracking
    std::atomic<bool> all_done{false};
    boost::system::error_code write_ec, read_ec;

    //  Set up the chain of operations
    acceptor.async_accept (
      server_stream.lowest_layer (), [&] (const boost::system::error_code &ec) {
          if (ec) {
              all_done = true;
              work.reset ();
              return;
          }

          //  Server: SSL handshake, then read
          server_stream.async_handshake (
            ssl::stream_base::server, [&] (const boost::system::error_code &ec) {
                if (ec) {
                    all_done = true;
                    work.reset ();
                    return;
                }

                //  Server reads data
                server_stream.async_read_some (
                  net::buffer (recv_buf, sizeof (recv_buf)),
                  [&] (const boost::system::error_code &ec, std::size_t n) {
                      read_ec = ec;
                      bytes_read = n;
                      all_done = true;
                      work.reset ();
                  });
            });
      });

    //  Client: TCP connect, handshake, then write
    client_stream.lowest_layer ().async_connect (
      endpoint, [&] (const boost::system::error_code &ec) {
          if (ec) {
              all_done = true;
              work.reset ();
              return;
          }

          client_stream.async_handshake (
            ssl::stream_base::client, [&] (const boost::system::error_code &ec) {
                if (ec) {
                    all_done = true;
                    work.reset ();
                    return;
                }

                //  Client writes data
                net::async_write (
                  client_stream, net::buffer (test_msg, strlen (test_msg)),
                  [&] (const boost::system::error_code &ec, std::size_t) { write_ec = ec; });
            });
      });

    //  Run io_context in a separate thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        work.reset ();
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "SSL data exchange timed out");
    TEST_ASSERT_FALSE_MESSAGE (write_ec.failed (), write_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (read_ec.failed (), read_ec.message ().c_str ());
    TEST_ASSERT_EQUAL_UINT64 (strlen (test_msg), bytes_read);
    TEST_ASSERT_EQUAL_STRING (test_msg, recv_buf);

    //  Note: Don't call sync shutdown - it can block
    //  Let the streams destruct naturally
}

#else // !ZLINK_IOTHREAD_POLLER_USE_ASIO || !ZLINK_HAVE_ASIO_SSL

void setUp ()
{
}

void tearDown ()
{
}

void test_asio_ssl_not_enabled ()
{
    //  Skip tests when Asio SSL is not enabled
    TEST_IGNORE_MESSAGE ("Asio SSL not enabled, skipping tests");
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL
    RUN_TEST (test_ssl_context_creation);
    RUN_TEST (test_ssl_certificate_loading);
    RUN_TEST (test_ssl_stream_creation);
    RUN_TEST (test_tcp_connection_baseline);
    RUN_TEST (test_ssl_handshake);
    RUN_TEST (test_multiple_ssl_contexts);
    RUN_TEST (test_ssl_context_reuse);
    RUN_TEST (test_ssl_data_exchange);
    RUN_TEST (test_zlink_tls_pair);
#else
    RUN_TEST (test_asio_ssl_not_enabled);
#endif

    return UNITY_END ();
}
