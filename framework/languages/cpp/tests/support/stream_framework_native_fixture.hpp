/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/streams/stream_runtime.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace zlink::framework::test::stream_framework
{
struct native_tcp_client_t
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
};

using native_tcp_client_ptr_t = std::unique_ptr<native_tcp_client_t>;

native_tcp_client_ptr_t connect_loopback (std::uint16_t port);

void close_native_client (native_tcp_client_ptr_t &client, bool shutdown_first = false);

std::vector<std::uint8_t>
make_native_stream_frame (const zlink::framework::detail::stream_runtime_t &runtime,
                          const zlink::framework::detail::stream_header_t &header,
                          const zlink::message_t &payload);

void send_native_bytes (const native_tcp_client_ptr_t &client,
                        const std::vector<std::uint8_t> &bytes,
                        std::size_t offset,
                        std::size_t length);

void send_native_bytes (const native_tcp_client_ptr_t &client,
                        const std::vector<std::uint8_t> &bytes);

std::optional<std::vector<std::uint8_t>>
read_until_peer_close (const native_tcp_client_ptr_t &socket_fd, std::chrono::seconds timeout);
} // namespace zlink::framework::test::stream_framework
