/* SPDX-License-Identifier: Elastic-2.0 */
#include <boost/asio/write.hpp>
#include "stream_framework_native_fixture.hpp"

namespace zlink::framework::test::stream_framework
{
native_tcp_client_ptr_t connect_loopback (std::uint16_t port)
{
    auto client = std::make_unique<native_tcp_client_t> ();
    boost::system::error_code error;
    client->socket.connect ({boost::asio::ip::address_v4::loopback (), port}, error);
    if (error) {
        return nullptr;
    }
    return client;
}

void close_native_client (native_tcp_client_ptr_t &client, bool shutdown_first)
{
    if (!client) {
        return;
    }
    boost::system::error_code ignored;
    if (shutdown_first) {
        client->socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
    }
    client->socket.close (ignored);
    client.reset ();
}

std::vector<std::uint8_t>
make_native_stream_frame (const zlink::framework::detail::stream_runtime_t &runtime,
                          const zlink::framework::detail::stream_header_t &header,
                          const zlink::message_t &payload)
{
    const auto encoded_header = runtime.encode_header (header);
    if (!encoded_header) {
        throw std::runtime_error ("failed to encode STREAM fairness frame header");
    }
    const auto payload_bytes = payload.to_bytes ();
    const auto header_size = encoded_header.value ().size ();
    std::vector<std::uint8_t> frame;
    frame.reserve (6 + header_size + payload_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
    frame.insert (frame.end (), encoded_header.value ().begin (), encoded_header.value ().end ());
    frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
    return frame;
}

void send_native_bytes (const native_tcp_client_ptr_t &client,
                        const std::vector<std::uint8_t> &bytes,
                        std::size_t offset,
                        std::size_t length)
{
    if (!client || offset > bytes.size () || length > bytes.size () - offset) {
        throw std::runtime_error ("invalid STREAM fairness frame range");
    }
    boost::system::error_code error;
    const auto sent = boost::asio::write (
      client->socket, boost::asio::buffer (bytes.data () + offset, length), error);
    if (error || sent != length) {
        throw std::runtime_error ("failed to send STREAM fairness frame");
    }
}

void send_native_bytes (const native_tcp_client_ptr_t &client,
                        const std::vector<std::uint8_t> &bytes)
{
    send_native_bytes (client, bytes, 0, bytes.size ());
}

/* Reads until the peer closes the connection. Returns std::nullopt when the
 * deadline passes before EOF (the self-deadlock symptom: the host never
 * disconnects the peer). */
std::optional<std::vector<std::uint8_t>>
read_until_peer_close (const native_tcp_client_ptr_t &socket_fd, std::chrono::seconds timeout)
{
    if (!socket_fd) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> collected;
    boost::system::error_code error;
    socket_fd->socket.non_blocking (true, error);
    if (error) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    for (;;) {
        if (std::chrono::steady_clock::now () >= deadline) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 4096> chunk{};
        const auto received = socket_fd->socket.read_some (boost::asio::buffer (chunk), error);
        if (!error) {
            collected.insert (collected.end (), chunk.data (), chunk.data () + received);
            continue;
        }
        if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset) {
            return collected;
        }
        if (error != boost::asio::error::would_block && error != boost::asio::error::try_again) {
            return collected;
        }
        error.clear ();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
}
} // namespace zlink::framework::test::stream_framework
