/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/stream_connector.hpp>
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include <boost/asio.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

namespace control_contract
{
namespace sc = zlink::stream_connector;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

inline std::vector<std::uint8_t>
frame (std::string name, std::vector<std::uint8_t> payload = {}, bool invalid_header = false)
{
    sc::detail::stream_header_t header;
    header.kind = sc::message_kind_t::control;
    header.codec = sc::codec_t::raw;
    header.name = std::move (name);
    auto encoded = sc::detail::header_codec_t{}.encode (header).value ();
    if (invalid_header)
        encoded[2] = static_cast<std::uint8_t> (sc::codec_t::json);
    return sc::detail::frame_codec_t::encode (encoded, payload, sc::connector_options_t{}).value ();
}

inline std::vector<std::uint8_t> barrier ()
{
    sc::detail::stream_header_t header;
    header.kind = sc::message_kind_t::send;
    header.codec = sc::codec_t::raw;
    header.name = "control.barrier";
    return sc::detail::frame_codec_t::encode (sc::detail::header_codec_t{}.encode (header).value (),
                                              {}, sc::connector_options_t{})
      .value ();
}

inline boost::system::error_code
read_exact (tcp::socket &socket,
            asio::mutable_buffer bytes,
            std::chrono::steady_clock::time_point expires = std::chrono::steady_clock::now ()
                                                            + std::chrono::seconds (5))
{
    if (std::chrono::steady_clock::now () >= expires)
        return asio::error::timed_out;
    auto &io = static_cast<asio::io_context &> (socket.get_executor ().context ());
    io.restart ();
    asio::steady_timer deadline (io, expires);
    boost::system::error_code result;
    deadline.async_wait ([&] (auto error) {
        if (!error)
            socket.cancel ();
    });
    asio::async_read (socket, bytes, [&] (auto error, auto) {
        result = error;
        deadline.cancel ();
    });
    io.run ();
    return result;
}

inline bool empty_pong (tcp::socket &socket)
{
    const auto expected = frame ("$zlink.heartbeat.pong");
    std::vector<std::uint8_t> bytes (expected.size ());
    const auto error = read_exact (socket, asio::buffer (bytes));
    if (error || bytes != expected) {
        std::cout << "pong read error=" << error.message () << " bytes=";
        for (const auto byte : bytes)
            std::cout << static_cast<unsigned> (byte) << ',';
        std::cout << '\n';
    }
    return !error && bytes == expected;
}

// A raw client must answer the server's periodic ping while awaiting a test response.
// Every read in this operation shares the original five-second deadline.
inline std::pair<boost::system::error_code, std::vector<std::uint8_t>>
read_server_frame (tcp::socket &socket)
{
    const auto expires = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    for (;;) {
        std::array<std::uint8_t, 6> prefix{};
        if (auto error = read_exact (socket, asio::buffer (prefix), expires))
            return {error, {}};
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8) | prefix[5];
        if (!sc::detail::frame_codec_t::validate_receive_frame_size (header_size, payload_size,
                                                                     sc::connector_options_t{}))
            return {asio::error::message_size, {}};
        std::vector<std::uint8_t> bytes (6 + header_size + payload_size);
        std::copy (prefix.begin (), prefix.end (), bytes.begin ());
        if (auto error =
              read_exact (socket, asio::buffer (bytes.data () + 6, bytes.size () - 6), expires))
            return {error, {}};
        auto header = sc::detail::header_codec_t{}.decode (
          std::vector<std::uint8_t> (bytes.begin () + 6, bytes.begin () + 6 + header_size));
        if (!header)
            return {asio::error::invalid_argument, {}};
        if (header.value ().kind == sc::message_kind_t::control
            && header.value ().name == "$zlink.heartbeat.ping") {
            const auto pong = frame ("$zlink.heartbeat.pong");
            boost::system::error_code error;
            asio::write (socket, asio::buffer (pong), error);
            if (error)
                return {error, {}};
            continue;
        }
        return {{}, std::move (bytes)};
    }
}

inline bool server_peer_heartbeat_contract ()
{
    asio::io_context io;
    tcp::acceptor listener (io, {asio::ip::address_v4::loopback (), 0});
    tcp::socket sender (io), receiver (io);
    sender.connect (listener.local_endpoint ());
    listener.accept (receiver);
    auto bytes = frame ("$zlink.heartbeat.ping");
    for (const auto &next :
         {frame ("$zlink.heartbeat.ping", {255, 1, 2}), frame ("$zlink.heartbeat.pong")})
        bytes.insert (bytes.end (), next.begin (), next.end ());
    asio::write (sender, asio::buffer (bytes));
    const auto response = read_server_frame (receiver);
    return !response.first && response.second == frame ("$zlink.heartbeat.pong")
           && empty_pong (sender) && empty_pong (sender);
}

// The same wire cases exercise the C++ connector and the Unreal FIFO adapter.
// The adapter owns creation/dispatch; protocol decisions remain in the connector.
template <class WithConnector> int connector_cases (WithConnector with_connector)
{
    int failures = 0;
    auto check = [&] (bool ok, const char *label) {
        std::cout << label << ": " << (ok ? "PASS" : "FAIL") << '\n';
        failures += !ok;
    };
    asio::io_context io;
    tcp::acceptor acceptor (io, {asio::ip::address_v4::loopback (), 0});
    sc::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    with_connector (options, [&] (sc::connector_t &connector) {
        check (static_cast<bool> (connector.connect ()), "control connector connects");
        tcp::socket peer (io);
        acceptor.accept (peer);
        std::size_t callbacks = 0;
        auto unknown = connector.template on<sc::packet_t> ("$zlink.future",
                                                            [&] (const auto &) { ++callbacks; });
        auto pong = connector.template on<sc::packet_t> ("$zlink.heartbeat.pong",
                                                         [&] (const auto &) { ++callbacks; });
        std::ostringstream logs;
        auto *previous = std::cerr.rdbuf (logs.rdbuf ());
        std::vector<std::uint8_t> batch;
        for (int i = 0; i < 1000; ++i) {
            auto bytes = frame ("$zlink.future", {255, 1, 2});
            batch.insert (batch.end (), bytes.begin (), bytes.end ());
        }
        auto bytes = frame ("$zlink.heartbeat.pong", {1, 2, 3});
        batch.insert (batch.end (), bytes.begin (), bytes.end ());
        bytes = barrier ();
        batch.insert (batch.end (), bytes.begin (), bytes.end ());
        asio::write (peer, asio::buffer (batch));
        const auto ignored_received = connector.wait_for ("control.barrier");
        connector.send (sc::packet_t{.name = "control.barrier"}).submit ();
        const auto expected = barrier ();
        std::vector<std::uint8_t> outgoing (expected.size ());
        const bool ignored_pong =
          !read_exact (peer, asio::buffer (outgoing)) && outgoing == expected;
        batch = frame ("$zlink.heartbeat.ping", {255, 1, 2});
        bytes = barrier ();
        batch.insert (batch.end (), bytes.begin (), bytes.end ());
        asio::write (peer, asio::buffer (batch));
        const auto received = connector.wait_for ("control.barrier");
        const bool answered = empty_pong (peer);
        connector.send (sc::packet_t{.name = "control.barrier"}).submit ();
        const bool single_pong =
          !read_exact (peer, asio::buffer (outgoing)) && outgoing == expected;
        connector.dispatch ();
        std::cerr.rdbuf (previous);
        check (ignored_received && received
                 && connector.state () == sc::connection_state_t::connected,
               "unknown control and nonempty pong preserve connection");
        check (answered && single_pong,
               "unknown heartbeat version receives exactly one empty pong");
        check (ignored_pong, "nonempty pong and unknown controls send no extra reply");
        check (callbacks == 0, "ignored controls never reach packet callbacks");
        check (logs.str ().empty (), "1000 control frames produce zero default logs");
        connector.close ();
    });
    struct invalid_t
    {
        const char *name;
        std::vector<std::uint8_t> payload;
        bool header = false;
    };
    for (const auto &invalid :
         {invalid_t{"future", {}}, invalid_t{"session-closing", {1, 5, 0, 1}},
          invalid_t{"$zlink.actor.bound", {1, 0, 1, 2, 'x'}},
          invalid_t{"$zlink.actor.unbound", {1, 0}}, invalid_t{"$zlink.future", {}, true}}) {
        with_connector (options, [&] (sc::connector_t &connector) {
            check (static_cast<bool> (connector.connect ()), "invalid case connects");
            tcp::socket peer (io);
            acceptor.accept (peer);
            auto bytes = frame (invalid.name, invalid.payload, invalid.header);
            const auto after = barrier ();
            bytes.insert (bytes.end (), after.begin (), after.end ());
            asio::write (peer, asio::buffer (bytes));
            const auto received = connector.wait_for ("control.barrier");
            connector.dispatch ();
            check (!received && connector.state () != sc::connection_state_t::connected
                     && connector.close_reason () == sc::close_reason_t::protocol_error,
                   invalid.name);
            connector.close ();
        });
    }
    return failures;
}
} // namespace control_contract
