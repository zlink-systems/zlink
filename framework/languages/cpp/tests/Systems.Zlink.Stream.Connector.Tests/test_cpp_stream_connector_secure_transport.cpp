/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <openssl/crypto.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <zlink/json.hpp>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include "../support/stream_connector_cases.hpp"

#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
struct openssl_thread_cleanup_t
{
    ~openssl_thread_cleanup_t () { OPENSSL_thread_stop (); }
};
#endif
} // namespace

namespace stream_connector_tests
{
int verify_websocket_and_tls_transport (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime)
{
    boost::asio::io_context websocket_io;
    boost::asio::ip::tcp::acceptor websocket_acceptor (
      websocket_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto websocket_endpoint = std::string ("ws://127.0.0.1:")
                                    + std::to_string (websocket_acceptor.local_endpoint ().port ())
                                    + "/stream";
    std::atomic_bool websocket_send_seen{false};
    joining_thread_t websocket_server_thread ([&websocket_acceptor, &websocket_send_seen] {
        boost::asio::ip::tcp::socket socket (websocket_acceptor.get_executor ());
        websocket_acceptor.accept (socket);
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket (
          std::move (socket));
        websocket.accept ();
        boost::beast::flat_buffer buffer;
        websocket.read (buffer);
        auto frame_text = boost::beast::buffers_to_string (buffer.data ());
        if (auto frame = try_read_server_frame (frame_text)) {
            websocket_send_seen =
              frame->header.kind == zlink::stream_connector::message_kind_t::send
              && frame->header.name == login_request_t::packet_name && websocket.got_binary ();
        }
        boost::system::error_code ignored;
        websocket.close (boost::beast::websocket::close_code::normal, ignored);
    });

    zlink::stream_connector::connector_options_t websocket_options;
    websocket_options.endpoint = websocket_endpoint;
    websocket_options.transport = zlink::stream_connector::transport_t::websocket;
    auto websocket_connector =
      zlink::stream_connector::connector_factory_t::create (websocket_options);
    if (!websocket_connector.connect ()) {
        return fail (47);
    }
    websocket_connector.send (login_request_t{}).submit ();
    const auto websocket_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!websocket_send_seen && std::chrono::steady_clock::now () < websocket_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    websocket_connector.close ();
    websocket_server_thread.join ();
    if (!websocket_send_seen) {
        return fail (49);
    }

#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
    boost::asio::io_context tls_io;
    boost::asio::ip::tcp::acceptor tls_acceptor (tls_io,
                                                 {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto tls_endpoint =
      std::string ("tls://localhost:") + std::to_string (tls_acceptor.local_endpoint ().port ());
    std::atomic_bool tls_send_seen{false};
    joining_thread_t tls_server_thread ([&tls_acceptor, &tls_send_seen] {
        openssl_thread_cleanup_t openssl_cleanup;
        boost::asio::ssl::context tls_context (boost::asio::ssl::context::tls_server);
        tls_context.use_certificate_chain_file (ZLINK_STREAM_CONNECTOR_TEST_CERT);
        tls_context.use_private_key_file (ZLINK_STREAM_CONNECTOR_TEST_KEY,
                                          boost::asio::ssl::context::pem);
        boost::asio::ip::tcp::socket socket (tls_acceptor.get_executor ());
        tls_acceptor.accept (socket);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream (std::move (socket),
                                                                       tls_context);
        boost::system::error_code error;
        stream.handshake (boost::asio::ssl::stream_base::server, error);
        if (error) {
            return;
        }
        std::string buffer;
        std::array<char, 1024> chunk{};
        while (!tls_send_seen) {
            const auto read = stream.read_some (boost::asio::buffer (chunk), error);
            if (error) {
                return;
            }
            buffer.append (chunk.data (), read);
            if (auto frame = try_read_server_frame (buffer)) {
                tls_send_seen = frame->header.kind == zlink::stream_connector::message_kind_t::send
                                && frame->header.name == login_request_t::packet_name;
            }
        }
        stream.shutdown (error);
    });

    zlink::stream_connector::connector_options_t tls_options;
    tls_options.endpoint = tls_endpoint;
    tls_options.transport = zlink::stream_connector::transport_t::tls;
    tls_options.skip_server_certificate_validation = true;
    auto tls_connector = zlink::stream_connector::connector_factory_t::create (tls_options);
    if (!tls_connector.connect ()) {
        return fail (50);
    }
    tls_connector.send (login_request_t{}).submit ();
    const auto tls_send_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!tls_send_seen && std::chrono::steady_clock::now () < tls_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    tls_connector.close ();
    tls_server_thread.join ();
    if (!tls_send_seen) {
        return fail (52);
    }

    boost::asio::io_context wss_io;
    boost::asio::ip::tcp::acceptor wss_acceptor (wss_io,
                                                 {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto wss_endpoint = std::string ("wss://localhost:")
                              + std::to_string (wss_acceptor.local_endpoint ().port ()) + "/stream";
    std::atomic_bool wss_send_seen{false};
    joining_thread_t wss_server_thread ([&wss_acceptor, &wss_send_seen] {
        openssl_thread_cleanup_t openssl_cleanup;
        boost::asio::ssl::context tls_context (boost::asio::ssl::context::tls_server);
        tls_context.use_certificate_chain_file (ZLINK_STREAM_CONNECTOR_TEST_CERT);
        tls_context.use_private_key_file (ZLINK_STREAM_CONNECTOR_TEST_KEY,
                                          boost::asio::ssl::context::pem);
        boost::asio::ip::tcp::socket socket (wss_acceptor.get_executor ());
        wss_acceptor.accept (socket);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream (std::move (socket),
                                                                           tls_context);
        boost::system::error_code error;
        tls_stream.handshake (boost::asio::ssl::stream_base::server, error);
        if (error) {
            return;
        }
        boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>
          websocket (std::move (tls_stream));
        websocket.accept ();
        boost::beast::flat_buffer buffer;
        websocket.read (buffer);
        auto frame_text = boost::beast::buffers_to_string (buffer.data ());
        if (auto frame = try_read_server_frame (frame_text)) {
            wss_send_seen = frame->header.kind == zlink::stream_connector::message_kind_t::send
                            && frame->header.name == login_request_t::packet_name
                            && websocket.got_binary ();
        }
        websocket.close (boost::beast::websocket::close_code::normal, error);
    });

    zlink::stream_connector::connector_options_t wss_options;
    wss_options.endpoint = wss_endpoint;
    wss_options.transport = zlink::stream_connector::transport_t::websocket_secure;
    wss_options.skip_server_certificate_validation = true;
    auto wss_connector = zlink::stream_connector::connector_factory_t::create (wss_options);
    if (!wss_connector.connect ()) {
        return fail (53);
    }
    wss_connector.send (login_request_t{}).submit ();
    const auto wss_send_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!wss_send_seen && std::chrono::steady_clock::now () < wss_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    wss_connector.close ();
    wss_server_thread.join ();
    if (!wss_send_seen) {
        return fail (55);
    }
#endif

    return verify_reconnect_and_endpoint_scheme (context, connector, runtime, websocket_endpoint);
}
} // namespace stream_connector_tests
