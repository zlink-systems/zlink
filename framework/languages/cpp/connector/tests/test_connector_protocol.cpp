/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector.hpp>

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace sc = zlink::stream_connector;
using tcp = boost::asio::ip::tcp;

int main (int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "named-response";
    const bool named = mode.starts_with ("named-");
    const bool remote_error = mode.ends_with ("error");
    boost::asio::io_context io;
    tcp::acceptor acceptor (io, {boost::asio::ip::address_v4::loopback (), 0});
    std::thread peer ([&] {
        tcp::socket socket (io);
        acceptor.accept (socket);
        std::array<std::uint8_t, 6> prefix{};
        boost::asio::read (socket, boost::asio::buffer (prefix));
        const std::size_t header_size = (prefix[0] << 8) | prefix[1];
        const std::size_t payload_size = (std::size_t (prefix[2]) << 24)
                                         | (std::size_t (prefix[3]) << 16)
                                         | (std::size_t (prefix[4]) << 8) | prefix[5];
        std::vector<std::uint8_t> request (header_size + payload_size);
        boost::asio::read (socket, boost::asio::buffer (request));
        // The peer preserves the public wire request sequence and varies only name_len.
        std::vector<std::uint8_t> header{0xF2, std::uint8_t (remote_error ? 4 : 3),
                                         std::uint8_t (remote_error ? 1 : 0), 1};
        header.insert (header.end (), request.begin () + 4, request.begin () + 12);
        header.push_back (named ? 1 : 0);
        if (named) {
            header.push_back ('x');
        }
        const std::string payload =
          remote_error ? R"({"code":"denied","message":"rejected"})" : "ok";
        std::vector<std::uint8_t> reply{0, std::uint8_t (header.size ()), 0, 0,
                                        0, std::uint8_t (payload.size ())};
        reply.insert (reply.end (), header.begin (), header.end ());
        reply.insert (reply.end (), payload.begin (), payload.end ());
        boost::system::error_code error;
        boost::asio::write (socket, boost::asio::buffer (reply), error);
        std::array<char, 64> drained{};
        while (!error) {
            socket.read_some (boost::asio::buffer (drained), error);
        }
    });

    sc::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.dispatch_mode = sc::dispatch_mode_t::immediate;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto connector = sc::connector_factory_t::create (options);
    std::promise<void> decode_error;
    auto observed = decode_error.get_future ();
    auto subscription = connector.on_error ([&] (const sc::error_t &error) {
        if (error.code == sc::error_code_t::frame_decode_failed) {
            decode_error.set_value ();
        }
    });
    const auto connected = connector.connect ();
    if (!connected) {
        connector.close ();
        acceptor.close ();
        peer.join ();
        std::cerr << "F20 fixture could not connect\n";
        return 1;
    }
    sc::packet_t packet;
    packet.name = "probe";
    packet.codec = sc::codec_t::raw;
    packet.payload = {'p'};
    const auto result = connector.request (std::move (packet))
                          .timeout (std::chrono::seconds (2))
                          .submit<std::vector<std::uint8_t>> ();
    bool passed;
    if (named) {
        passed = observed.wait_for (std::chrono::seconds (2)) == std::future_status::ready
                 && !result && connector.close_reason () == sc::close_reason_t::protocol_error;
    } else {
        passed = remote_error ? (!result && result.error_code () == sc::error_code_t::remote_error)
                              : (result && result.value () == std::vector<std::uint8_t>{'o', 'k'});
        passed = passed && connector.state () == sc::connection_state_t::connected;
    }
    std::cout << "F20 " << mode << ": request=" << (result ? "success" : "failure")
              << " error=" << (result.error_code () ? int (*result.error_code ()) : -1)
              << " close_reason="
              << (connector.close_reason () ? int (*connector.close_reason ()) : -1) << '\n';
    connector.close ();
    peer.join ();
    if (!passed) {
        std::cerr << "F20 " << mode << ": reply name contract was not enforced\n";
        return 1;
    }
    std::cout << "F20 " << mode << ": passed\n";
    return 0;
}
