/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/options.hpp"
#include "protocol/zmp_control.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/wire.hpp"

#include "utils/config.hpp"

#if defined ZLINK_HAVE_WS && defined ZLINK_IOTHREAD_POLLER_USE_ASIO
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#if defined ZLINK_HAVE_WSS
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string.h>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

#if defined ZLINK_HAVE_WS
namespace
{
void exercise_request_reply (void *router_,
                             void *dealer_,
                             const char *endpoint_,
                             const char *request_payload_,
                             const char *reply_payload_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_, "zmp-ws-dealer", 13));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_, endpoint_));
    msleep (SETTLE_TIME * 5);

    zlink_msg_t request[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&request[0], strlen (request_payload_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&request[1], 12));
    memcpy (zlink_msg_data (&request[0]), request_payload_,
            strlen (request_payload_));
    memcpy (zlink_msg_data (&request[1]), "request-tail", 12);
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer_, NULL, &request[0],
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, 0, NULL,
                          NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer_, NULL, &request[1],
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 5000,
                          NULL, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);

    zlink_routing_id_t reply_rid;
    memset (&reply_rid, 0, sizeof (reply_rid));
    zlink_reply_token_t reply_token = 0;
    unsigned char retained_kind = 0;
    uint64_t retained_sequence = 0;
    for (size_t i = 0; i != 2; ++i) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t token = 0;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part (router_, &source_rid, &token, &part,
                                  &has_more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_NOT_EQUAL (0, token);
        if (i == 0) {
            reply_rid = *source_rid;
            reply_token = token;
            TEST_ASSERT_EQUAL_STRING_LEN (
              request_payload_,
              static_cast<const char *> (zlink_msg_data (&part)),
              strlen (request_payload_));
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
        } else {
            TEST_ASSERT_EQUAL_UINT64 (reply_token, token);
            TEST_ASSERT_EQUAL_STRING_LEN (
              "request-tail",
              static_cast<const char *> (zlink_msg_data (&part)), 12);
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
        }
        TEST_ASSERT_FALSE (
          reinterpret_cast<zlink::msg_t *> (&part)
            ->get_request_reply_metadata (&retained_kind, &retained_sequence));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    zlink_msg_t reply[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&reply[0], strlen (reply_payload_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&reply[1], 10));
    memcpy (zlink_msg_data (&reply[0]), reply_payload_, strlen (reply_payload_));
    memcpy (zlink_msg_data (&reply[1]), "reply-tail", 10);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, &reply_rid, reply_token, &reply[0],
                        ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, &reply_rid, reply_token, &reply[1],
                        ZLINK_PART_FINAL));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    zlink_recv_result_t completion_rc = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < 5000
                          && completion_rc == ZLINK_RECV_NO_DATA;
         ++attempt) {
        completion_rc = zlink_completion_recv (
          dealer_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (completion_rc == ZLINK_RECV_NO_DATA)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, completion_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      reply_payload_, static_cast<const char *> (
                        zlink_msg_data (&completion.reply_parts[0])),
      strlen (reply_payload_));
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&completion.reply_parts[0])
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    zlink_completion_close (&completion);
}

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
typedef net::ip::tcp raw_ws_tcp_t;
#if defined ZLINK_HAVE_WSS
typedef websocket::stream<net::ssl::stream<raw_ws_tcp_t::socket>>
  raw_wss_stream_t;
#endif

std::vector<unsigned char> make_zmp_data_frame (const char *payload_)
{
    const size_t payload_size = strlen (payload_);
    std::vector<unsigned char> frame (zlink::zmp_header_size + payload_size);
    frame[0] = zlink::zmp_magic;
    frame[1] = zlink::zmp_version;
    frame[2] = 0;
    frame[3] = zlink::zmp_kind_data;
    zlink::put_uint32 (&frame[4], static_cast<uint32_t> (payload_size));
    memcpy (&frame[zlink::zmp_header_size], payload_, payload_size);
    return frame;
}

template <typename websocket_stream_t>
void raw_ws_zmp_handshake (websocket_stream_t *client_)
{
    zlink::options_t options;
    options.type = ZLINK_CORE_SOCKET_PAIR;
    options.zmp_metadata = true;

    unsigned char hello[272];
    size_t hello_size = 0;
    zlink::zmp_control::build_hello_frame (
      options, hello, sizeof (hello), &hello_size);
    std::vector<unsigned char> ready;
    zlink::zmp_control::build_ready_frame (options, ready);

    boost::system::error_code ec;
    TEST_ASSERT_EQUAL_UINT64 (
      hello_size,
      client_->write_some (false, net::buffer (hello, hello_size), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    TEST_ASSERT_EQUAL_UINT64 (
      0, client_->write_some (true, net::const_buffer (), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());

    TEST_ASSERT_EQUAL_UINT64 (
      ready.size (),
      client_->write_some (false, net::buffer (ready), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    TEST_ASSERT_EQUAL_UINT64 (
      0, client_->write_some (true, net::const_buffer (), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());

    // HELLO and READY are adjacent ZMP frames in one bounded binary carrier
    // record. Draining it also proves the asynchronous server handshake made
    // forward progress before the application-byte assertions below.
    beast::flat_buffer record;
    client_->read (record, ec);
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    TEST_ASSERT_TRUE (client_->got_binary ());
}

void assert_pair_has_no_message (void *server_, int duration_ms_)
{
    unsigned char received[64];
    for (int waited = 0; waited < duration_ms_; waited += 5) {
        errno = 0;
        const int rc = zlink_recv (
          server_, received, sizeof (received), ZLINK_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        msleep (5);
    }
}

void send_invalid_middle_metadata_record (void *sender_, void *receiver_)
{
    zlink_msg_t first;
    zlink_msg_t invalid_one;
    zlink_msg_t invalid_two;
    zlink_msg_t final;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&first, 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&invalid_one, 7));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&invalid_two, 7));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&final, 5));
    memcpy (zlink_msg_data (&first), "first", 5);
    memcpy (zlink_msg_data (&invalid_one), "invalid", 7);
    memcpy (zlink_msg_data (&invalid_two), "invalid", 7);
    memcpy (zlink_msg_data (&final), "final", 5);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&invalid_one)
        ->set_request_reply_metadata (zlink::zmp_kind_request, 91));
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&invalid_two)
        ->set_request_reply_metadata (zlink::zmp_kind_reply, 92));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender_, &first, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender_, &invalid_one, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender_, &invalid_two, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender_, &final, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    // The first frame has already established encoder multipart state. A
    // batching fallback could discard both invalid continuations, encode the
    // ordinary final frame, and publish a spliced [first, final] record. The
    // message transport must instead close at the first invalid header.
    assert_pair_has_no_message (receiver_, 250);
}

void recv_pair_message_with_timeout (void *server_, const char *expected_)
{
    unsigned char received[64];
    int rc = -1;
    for (int waited = 0; waited < 5000 && rc == -1; waited += 5) {
        errno = 0;
        rc = zlink_recv (server_, received, sizeof (received), ZLINK_DONTWAIT);
        if (rc == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            msleep (5);
        }
    }
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (expected_)), rc);
    TEST_ASSERT_EQUAL_MEMORY (expected_, received, strlen (expected_));
}

template <typename websocket_stream_t>
void wait_for_ws_disconnect (websocket_stream_t *client_,
                             net::io_context *client_io_)
{
    beast::flat_buffer record;
    net::steady_timer deadline (*client_io_);
    bool timed_out = false;
    bool disconnected = false;
    std::function<void ()> read_next;

    read_next = [&] () {
        client_->async_read (
          record,
          [&] (const boost::system::error_code &ec,
              std::size_t bytes_transferred) {
              if (ec) {
                  disconnected = true;
                  deadline.cancel ();
                  return;
              }
              record.consume (bytes_transferred);
              read_next ();
          });
    };

    deadline.expires_after (std::chrono::seconds (2));
    deadline.async_wait ([&] (const boost::system::error_code &ec) {
        if (ec)
            return;
        timed_out = true;
        boost::system::error_code ignored;
        beast::get_lowest_layer (*client_).cancel (ignored);
    });
    read_next ();
    client_io_->restart ();
    client_io_->run ();

    TEST_ASSERT_FALSE_MESSAGE (timed_out,
                               "WS peer did not close invalid record");
    TEST_ASSERT_TRUE (disconnected);
}

template <typename websocket_stream_t>
void send_text_zmp_record (websocket_stream_t *client_,
                           const std::vector<unsigned char> &record_,
                           bool fragmented_)
{
    client_->text (true);
    boost::system::error_code ec;
    if (!fragmented_) {
        TEST_ASSERT_EQUAL_UINT64 (
          record_.size (), client_->write (net::buffer (record_), ec));
        TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
        return;
    }

    const size_t split = std::max<size_t> (1, record_.size () / 2);
    TEST_ASSERT_EQUAL_UINT64 (
      split,
      client_->write_some (
        false, net::buffer (&record_[0], split), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    TEST_ASSERT_EQUAL_UINT64 (
      record_.size () - split,
      client_->write_some (
        true, net::buffer (&record_[split], record_.size () - split), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
}

template <typename websocket_stream_t>
void assert_text_zmp_rejected (void *server_,
                               websocket_stream_t *client_,
                               net::io_context *client_io_,
                               bool handshake_first_,
                               bool fragmented_)
{
    if (handshake_first_) {
        client_->binary (true);
        raw_ws_zmp_handshake (client_);
        send_text_zmp_record (
          client_, make_zmp_data_frame (fragmented_ ? "fragmented-text"
                                                    : "text-data"),
          fragmented_);
    } else {
        zlink::options_t options;
        options.type = ZLINK_CORE_SOCKET_PAIR;
        options.zmp_metadata = true;
        unsigned char hello[272];
        size_t hello_size = 0;
        zlink::zmp_control::build_hello_frame (
          options, hello, sizeof (hello), &hello_size);
        const std::vector<unsigned char> hello_record (
          hello, hello + hello_size);
        send_text_zmp_record (client_, hello_record, fragmented_);
    }

    assert_pair_has_no_message (server_, 100);
    wait_for_ws_disconnect (client_, client_io_);
}
#endif
}

void test_zmp_ws_pair_message ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "ws://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "ws-zmp", 0);
    recv_string_expect_success (server, "ws-zmp", 0);

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
    send_invalid_middle_metadata_record (client, server);
#endif

    test_context_socket_close (client);
    test_context_socket_close (server);
}

void test_zmp_ws_request_reply ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "ws://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint,
                        &endpoint_len));
    exercise_request_reply (server, client, endpoint, "ws-request",
                            "ws-reply");

    test_context_socket_close (client);
    test_context_socket_close (server);
}

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
void test_zmp_ws_binary_record_is_a_byte_carrier ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "ws://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint,
                        &endpoint_len));
    unsigned int port = 0;
    TEST_ASSERT_EQUAL_INT (
      1, sscanf (endpoint, "ws://127.0.0.1:%u", &port));

    net::io_context client_io;
    raw_ws_tcp_t::resolver resolver (client_io);
    websocket::stream<raw_ws_tcp_t::socket> client (client_io);
    const std::string port_text = std::to_string (port);
    net::connect (client.next_layer (),
                  resolver.resolve ("127.0.0.1", port_text));
    client.binary (true);
    client.handshake ("127.0.0.1:" + port_text, "/");
    raw_ws_zmp_handshake (&client);

    boost::system::error_code ec;
    TEST_ASSERT_EQUAL_UINT64 (
      0, client.write (net::const_buffer (), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    assert_pair_has_no_message (server, 50);

    const char staged_payload[] = "staged";
    const std::vector<unsigned char> staged =
      make_zmp_data_frame (staged_payload);
    TEST_ASSERT_EQUAL_UINT64 (
      staged.size (),
      client.write_some (false, net::buffer (staged), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());

    // WebSocket fragmentation is transport-only. A complete ZMP frame is
    // publishable without waiting for the carrier record's FIN bit.
    recv_pair_message_with_timeout (server, staged_payload);

    TEST_ASSERT_EQUAL_UINT64 (
      0, client.write_some (true, net::const_buffer (), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());

    const std::vector<unsigned char> first = make_zmp_data_frame ("first");
    const std::vector<unsigned char> second = make_zmp_data_frame ("second");
    const size_t split = first.size () / 2;
    TEST_ASSERT_EQUAL_UINT64 (
      split, client.write (net::buffer (&first[0], split), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    assert_pair_has_no_message (server, 50);
    TEST_ASSERT_EQUAL_UINT64 (
      first.size () - split,
      client.write (net::buffer (&first[split], first.size () - split), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    recv_pair_message_with_timeout (server, "first");

    std::vector<unsigned char> batch;
    batch.reserve (first.size () + second.size ());
    batch.insert (batch.end (), first.begin (), first.end ());
    batch.insert (batch.end (), second.begin (), second.end ());
    TEST_ASSERT_EQUAL_UINT64 (
      batch.size (), client.write (net::buffer (batch), ec));
    TEST_ASSERT_FALSE_MESSAGE (ec.failed (), ec.message ().c_str ());
    recv_pair_message_with_timeout (server, "first");
    recv_pair_message_with_timeout (server, "second");

    boost::system::error_code ignored;
    client.next_layer ().close (ignored);
    test_context_socket_close (server);
}

void test_zmp_ws_rejects_text_hello_data_and_fragmented_data ()
{
    for (int case_index = 0; case_index != 3; ++case_index) {
        void *server = test_context_socket (ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (server);
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_bind (server, "ws://127.0.0.1:*"));

        char endpoint[256];
        size_t endpoint_len = sizeof (endpoint);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint,
                            &endpoint_len));
        unsigned int port = 0;
        TEST_ASSERT_EQUAL_INT (
          1, sscanf (endpoint, "ws://127.0.0.1:%u", &port));

        net::io_context client_io;
        raw_ws_tcp_t::resolver resolver (client_io);
        websocket::stream<raw_ws_tcp_t::socket> client (client_io);
        const std::string port_text = std::to_string (port);
        net::connect (client.next_layer (),
                      resolver.resolve ("127.0.0.1", port_text));
        client.handshake ("127.0.0.1:" + port_text, "/");

        assert_text_zmp_rejected (
          server, &client, &client_io, case_index != 0,
          case_index == 2);

        boost::system::error_code ignored;
        beast::get_lowest_layer (client).close (ignored);
        test_context_socket_close (server);
    }
}
#endif

#if defined ZLINK_HAVE_WSS
void test_zmp_wss_pair_message ()
{
    const tls_test_files_t files = make_tls_test_files ();

    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

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

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "wss://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "wss-zmp", 0);
    recv_string_expect_success (server, "wss-zmp", 0);

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
    send_invalid_middle_metadata_record (client, server);
#endif

    test_context_socket_close (client);
    test_context_socket_close (server);
    cleanup_tls_test_files (files);
}

void test_zmp_wss_request_reply ()
{
    const tls_test_files_t files = make_tls_test_files ();

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system,
                        sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_TLS_CERT,
                        files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (),
                        files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (),
                        files.ca_cert.size ()));

    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname,
                        strlen (hostname)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "wss://127.0.0.1:*"));

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint,
                        &endpoint_len));
    exercise_request_reply (server, client, endpoint, "wss-request",
                            "wss-reply");

    test_context_socket_close (client);
    test_context_socket_close (server);
    cleanup_tls_test_files (files);
}

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
void test_zmp_wss_rejects_text_hello_data_and_fragmented_data ()
{
    const tls_test_files_t files = make_tls_test_files ();
    for (int case_index = 0; case_index != 3; ++case_index) {
        void *server = test_context_socket (ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (server);
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_TLS_CERT,
                            files.server_cert.c_str (),
                            files.server_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_TLS_KEY,
                            files.server_key.c_str (),
                            files.server_key.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_bind (server, "wss://127.0.0.1:*"));

        char endpoint[256];
        size_t endpoint_len = sizeof (endpoint);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint,
                            &endpoint_len));
        unsigned int port = 0;
        TEST_ASSERT_EQUAL_INT (
          1, sscanf (endpoint, "wss://127.0.0.1:%u", &port));

        net::io_context client_io;
        net::ssl::context client_tls (net::ssl::context::tls_client);
        client_tls.set_verify_mode (net::ssl::verify_none);
        raw_ws_tcp_t::resolver resolver (client_io);
        raw_wss_stream_t client (client_io, client_tls);
        const std::string port_text = std::to_string (port);
        net::connect (beast::get_lowest_layer (client),
                      resolver.resolve ("127.0.0.1", port_text));
        client.next_layer ().handshake (net::ssl::stream_base::client);
        client.handshake ("127.0.0.1:" + port_text, "/");

        assert_text_zmp_rejected (
          server, &client, &client_io, case_index != 0,
          case_index == 2);

        boost::system::error_code ignored;
        beast::get_lowest_layer (client).close (ignored);
        test_context_socket_close (server);
    }
    cleanup_tls_test_files (files);
}
#endif
#endif // ZLINK_HAVE_WSS
#endif // ZLINK_HAVE_WS

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

#if defined ZLINK_HAVE_WS
    RUN_TEST (test_zmp_ws_pair_message);
    RUN_TEST (test_zmp_ws_request_reply);
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
    RUN_TEST (test_zmp_ws_binary_record_is_a_byte_carrier);
    RUN_TEST (test_zmp_ws_rejects_text_hello_data_and_fragmented_data);
#endif
#if defined ZLINK_HAVE_WSS
    RUN_TEST (test_zmp_wss_pair_message);
    RUN_TEST (test_zmp_wss_request_reply);
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
    RUN_TEST (test_zmp_wss_rejects_text_hello_data_and_fragmented_data);
#endif
#endif
#else
    TEST_IGNORE_MESSAGE ("WebSocket support not enabled");
#endif

    return UNITY_END ();
}
