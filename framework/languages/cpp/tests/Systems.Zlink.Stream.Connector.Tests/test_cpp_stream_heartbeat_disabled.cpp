/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector.hpp>
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include <boost/asio.hpp>
#include <future>
#include <iostream>
#include <thread>

int main ()
{
    namespace asio = boost::asio;
    namespace connector = zlink::stream_connector;
    using tcp = asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor (io, {asio::ip::address_v4::loopback (), 0});
    connector::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto frame = [&options] (const char *name) {
        connector::detail::stream_header_t header;
        header.kind = connector::message_kind_t::control;
        header.codec = connector::codec_t::raw;
        header.name = name;
        const auto encoded = connector::detail::header_codec_t{}.encode (header);
        return connector::detail::frame_codec_t::encode (encoded.value (), {}, options).value ();
    };
    const auto ping = frame (connector::detail::heartbeat_ping_name);
    const auto expected_pong = frame (connector::detail::heartbeat_pong_name);
    std::promise<bool> completed;
    auto observed = completed.get_future ();
    std::thread peer ([&] {
        try {
            tcp::socket socket (io);
            acceptor.accept (socket);
            asio::write (socket, asio::buffer (ping));
            std::vector<std::uint8_t> inbound (expected_pong.size ());
            asio::steady_timer deadline (io, std::chrono::seconds (3));
            deadline.async_wait ([&socket] (const boost::system::error_code &error) {
                if (!error)
                    socket.close ();
            });
            asio::async_read (socket, asio::buffer (inbound),
                              [&] (const boost::system::error_code &error, std::size_t) {
                                  completed.set_value (!error && inbound == expected_pong);
                                  deadline.cancel ();
                              });
            io.run ();
        }
        catch (const std::exception &error) {
            std::cerr << "FAIL c peer: " << error.what () << '\n';
            completed.set_value (false);
        }
    });
    auto client = connector::connector_factory_t::create (options);
    const auto connected = client.connect ();
    if (!connected)
        acceptor.close ();
    peer.join ();
    const bool pong = observed.get ();
    (void) client.close ();
    if (!connected || !pong) {
        std::cerr << "FAIL c: heartbeat disabled must still answer ping with pong\n";
        return 1;
    }
    std::cout << "PASS c: heartbeat disabled answers ping with pong\n";
}
