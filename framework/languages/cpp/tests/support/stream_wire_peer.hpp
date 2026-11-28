/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "loopback_tcp_endpoint.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/websocket.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework::tests
{
namespace stream_wire
{
namespace asio = boost::asio;
namespace websocket = boost::beast::websocket;
using asio::ip::tcp;
using namespace std::chrono_literals;

inline void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

struct wire_frame_t
{
    detail::stream_header_t header;
    std::string payload;
};

enum class wire_read_deadline_t
{
    response,
    enclosing_test
};

class wire_peer_t
{
  public:
    explicit wire_peer_t (const std::string &endpoint,
                          wire_read_deadline_t read_deadline = wire_read_deadline_t::response) :
        _read_deadline (read_deadline)
    {
        const auto port = endpoint_port (endpoint);
        socket ().connect ({asio::ip::address_v4::loopback (), port});
        _websocket = endpoint.starts_with ("ws:");
        if (_websocket) {
            _transport.handshake ("127.0.0.1:" + std::to_string (port), "/");
            _transport.binary (true);
        }
    }

    void send (const detail::stream_runtime_t &runtime,
               const detail::stream_header_t &header,
               std::string payload,
               bool malformed_header = false)
    {
        auto bytes =
          runtime.encode_frame (header, zlink::message_t::from (std::move (payload))).value ();
        if (malformed_header)
            bytes.at (6) = 0;
        if (_websocket)
            _transport.write (asio::buffer (bytes));
        else
            asio::write (socket (), asio::buffer (bytes));
    }

    wire_frame_t read (const detail::stream_runtime_t &runtime)
    {
        if (_websocket) {
            boost::beast::flat_buffer received;
            run_read ([&] (auto completed) { _transport.async_read (received, completed); });
            const auto bytes = received.data ();
            const auto *begin = static_cast<const std::uint8_t *> (bytes.data ());
            require (bytes.size () >= 6, "truncated STREAM WebSocket frame");
            std::vector<std::uint8_t> response (begin, begin + bytes.size ());
            const auto header_size = parse_header_size (response);
            require (6 + header_size <= response.size (), "invalid STREAM WebSocket header length");
            const auto header_end = response.begin () + 6 + header_size;
            auto header = runtime.decode_header ({response.begin () + 6, header_end}).value ();
            return {std::move (header), {header_end, response.end ()}};
        }
        const auto prefix = read_exact (6);
        const auto header_size = parse_header_size (prefix);
        std::size_t payload_size = 0;
        for (std::size_t index = 2; index != prefix.size (); ++index)
            payload_size = (payload_size << 8) | prefix[index];
        require (payload_size <= 64 * 1024, "oversized test response");
        auto header = runtime.decode_header (read_exact (header_size)).value ();
        auto payload = read_exact (payload_size);
        return {std::move (header), {payload.begin (), payload.end ()}};
    }

    void close () noexcept
    {
        boost::system::error_code ignored;
        socket ().close (ignored);
    }

  private:
    static std::size_t parse_header_size (const std::vector<std::uint8_t> &prefix)
    {
        return (static_cast<std::size_t> (prefix[0]) << 8) | prefix[1];
    }

    template <typename Start> void run_read (Start start)
    {
        boost::system::error_code result;
        bool finished = false;
        start ([&] (boost::system::error_code error, std::size_t) {
            result = error;
            finished = true;
        });
        _io.restart ();
        if (_read_deadline == wire_read_deadline_t::enclosing_test)
            _io.run ();
        else
            _io.run_for (3s);
        if (!finished) {
            close ();
            _io.restart ();
            _io.run ();
            throw std::runtime_error ("timed out waiting for STREAM response");
        }
        if (result)
            throw boost::system::system_error (result);
    }

    std::vector<std::uint8_t> read_exact (std::size_t size)
    {
        std::vector<std::uint8_t> bytes (size);
        run_read (
          [&] (auto completed) { asio::async_read (socket (), asio::buffer (bytes), completed); });
        return bytes;
    }

    tcp::socket &socket () { return _transport.next_layer (); }

    asio::io_context _io;
    websocket::stream<tcp::socket> _transport{_io};
    bool _websocket = false;
    wire_read_deadline_t _read_deadline;
};

} // namespace stream_wire
} // namespace zlink::framework::tests
