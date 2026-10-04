/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector.hpp>
#include <boost/asio.hpp>
#include <barrier>
#include <chrono>
#include <iostream>
#include <future>
#include <string>
#include <thread>

#ifdef ZLINK_CONNECTOR_LIFECYCLE_TLS
#include <boost/asio/ssl.hpp>
#include <openssl/crypto.h>
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_GODOT
#include <zlink_godot_stream_connector.hpp>
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_AXMOL
#include <zlink_axmol_stream_connector.hpp>
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_UNREAL
#include <ZLinkStreamConnector.h>
#endif

namespace
{
bool require (bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}

int main (int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "closed";
    namespace sc = zlink::stream_connector;
    if (mode == "refused") {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor reservation (io);
        reservation.open (boost::asio::ip::tcp::v4 ());
        reservation.bind ({boost::asio::ip::address_v4::loopback (), 0});
        sc::connector_options_t options;
        options.endpoint =
          "tcp://127.0.0.1:" + std::to_string (reservation.local_endpoint ().port ());
        options.connect_timeout = std::chrono::seconds (3);
        options.reconnect.enabled = false;
        auto connector = sc::connector_factory_t::create (options);
        const auto started = std::chrono::steady_clock::now ();
        const auto result = connector.connect ();
        const auto elapsed = std::chrono::steady_clock::now () - started;
        if (result.error ()) {
            std::cerr << "refused code=" << static_cast<int> (result.error ()->code)
                      << " error=" << result.error ()->message << " elapsed_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ()
                      << '\n';
        }
        return require (!result && result.error_code () == sc::error_code_t::disconnected
                          && elapsed < options.connect_timeout,
                        "F23: connection refusal must be Disconnected before the connect deadline")
                 ? 0
                 : 1;
    }
    if (mode == "close_connect") {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor (io, {boost::asio::ip::address_v4::loopback (), 0});
        std::promise<void> release;
        auto released = release.get_future ();
        std::thread server ([&] {
            boost::asio::ip::tcp::socket socket (io);
            acceptor.accept (socket);
            released.wait ();
        });
        sc::connector_options_t options;
        options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
        options.dispatch_mode = sc::dispatch_mode_t::manual;
        options.heartbeat.enabled = false;
        options.reconnect.enabled = false;
        auto connector = sc::connector_factory_t::create (options);
        bool callback_seen = false;
        bool connect_rejected = false;
        auto subscription =
          connector.on_connection_state_changed ([&] (const sc::connection_state_changed_t &event) {
              if (event.current == sc::connection_state_t::connected) {
                  callback_seen = true;
                  connector.close ();
                  const auto result = connector.connect ();
                  connect_rejected =
                    !result && result.error_code () == sc::error_code_t::disconnected;
              }
          });
        const auto result = connector.connect ();
        connector.dispatch ();
        connector.close ();
        release.set_value ();
        server.join ();
        return require (result && callback_seen && connect_rejected,
                        "F16: connect after a callback requests close must be rejected")
                 ? 0
                 : 1;
    }
    if (mode == "timeout") {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor (io, {boost::asio::ip::address_v4::loopback (), 0});
        std::promise<void> release;
        auto released = release.get_future ();
        std::thread server ([&] {
            boost::asio::ip::tcp::socket socket (io);
            acceptor.accept (socket);
            released.wait ();
        });
        sc::connector_options_t options;
        options.endpoint =
          "ws://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ()) + "/stream";
        options.connect_timeout = std::chrono::milliseconds (50);
        options.reconnect.enabled = false;
        auto connector = sc::connector_factory_t::create (options);
        const auto started = std::chrono::steady_clock::now ();
        const auto result = connector.connect ();
        const auto elapsed = std::chrono::steady_clock::now () - started;
        release.set_value ();
        server.join ();
        if (result.error ()) {
            std::cerr << "timeout code=" << static_cast<int> (result.error ()->code)
                      << " error=" << result.error ()->message << " elapsed_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ()
                      << '\n';
        }
        return require (!result && result.error_code () == sc::error_code_t::connect_timeout
                          && elapsed < std::chrono::milliseconds (250),
                        "F23: the connect deadline must preserve ConnectTimeout")
                 ? 0
                 : 1;
    }
    if (mode == "race") {
        sc::connector_options_t options;
        options.endpoint = "tcp://127.0.0.1:1";
        options.reconnect.enabled = false;
        auto connector = sc::connector_factory_t::create (options);
        std::barrier start (2);
        std::thread connect ([&] {
            for (int attempt = 0; attempt != 128; ++attempt) {
                start.arrive_and_wait ();
                (void) connector.connect ();
                start.arrive_and_wait ();
            }
        });
        std::thread send ([&] {
            for (int attempt = 0; attempt != 128; ++attempt) {
                start.arrive_and_wait ();
                for (int packet = 0; packet != 32; ++packet) {
                    connector.send (sc::packet_t{.name = "race", .payload = {1}}).submit ();
                }
                start.arrive_and_wait ();
            }
        });
        connect.join ();
        send.join ();
        connector.close ();
        return require (connector.state () == sc::connection_state_t::closed,
                        "F16: concurrent connect and send must preserve lifecycle ownership")
                 ? 0
                 : 1;
    }
    if (mode == "closed") {
        sc::connector_options_t options;
        options.endpoint = "tcp://127.0.0.1:1";
        auto connector = sc::connector_factory_t::create (options);
        connector.close ();
        const auto result = connector.connect ();
        return require (!result && result.error_code () == sc::error_code_t::disconnected
                          && connector.state () == sc::connection_state_t::closed,
                        "F16: Closed must remain terminal")
                 ? 0
                 : 1;
    }
#ifdef ZLINK_CONNECTOR_LIFECYCLE_TLS
    if (mode == "tls") {
        // Process cleanup must be safe after the last connector and TLS
        // fixture have been destroyed, including the connector's workers.
        struct openssl_cleanup_t
        {
            ~openssl_cleanup_t () { OPENSSL_cleanup (); }
        } cleanup;
        boost::asio::io_context io;
        boost::asio::ssl::context context (boost::asio::ssl::context::tls_server);
        context.use_certificate_chain_file (ZLINK_STREAM_CONNECTOR_TEST_CERT);
        context.use_private_key_file (ZLINK_STREAM_CONNECTOR_TEST_KEY,
                                      boost::asio::ssl::context::pem);
        boost::asio::ip::tcp::acceptor acceptor (io, {boost::asio::ip::address_v4::loopback (), 0});
        const auto port = acceptor.local_endpoint ().port ();
        std::thread server ([&] {
            boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream (io, context);
            boost::system::error_code error;
            acceptor.accept (stream.next_layer (), error);
            if (!error) {
                stream.handshake (boost::asio::ssl::stream_base::server, error);
            }
        });
        sc::connector_options_t options;
        options.endpoint = "tls://127.0.0.1:" + std::to_string (port);
        options.transport = sc::transport_t::tls;
        options.connect_timeout = std::chrono::seconds (3);
        options.reconnect.enabled = false;
        auto connector = sc::connector_factory_t::create (options);
        const auto result = connector.connect ();
        server.join ();
        return require (!result && result.error_code () == sc::error_code_t::tls_validation_failed
                          && connector.state () == sc::connection_state_t::disconnected,
                        "F23: untrusted native TLS certificate must preserve TlsValidationFailed")
                 ? 0
                 : 1;
    }
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_GODOT
    if (mode == "godot") {
        zlink::godot_stream_connector::stream_connector_t connector;
        connector.connect ("invalid-endpoint");
        return require (connector.state ()
                          == zlink::godot_stream_connector::connection_state_t::created,
                        "F24: Godot must retain core Created on configuration failure")
                 ? 0
                 : 1;
    }
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_AXMOL
    if (mode == "axmol") {
        zlink::axmol_stream_connector::stream_connector_t connector;
        connector.connect ("invalid-endpoint");
        return require (connector.state ()
                          == zlink::axmol_stream_connector::connection_state_t::created,
                        "F24: Axmol must retain core Created on configuration failure")
                 ? 0
                 : 1;
    }
#endif
#ifdef ZLINK_CONNECTOR_LIFECYCLE_UNREAL
    if (mode == "unreal") {
        UZLinkStreamConnector connector;
        connector.Connect ("invalid-endpoint");
        return require (connector.LastState () == EZLinkStreamConnectionState::Created,
                        "F24: Unreal must retain core Created on configuration failure")
                 ? 0
                 : 1;
    }
#endif
    return require (false, "unknown or unavailable test mode") ? 0 : 1;
}
