/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/connector_runtime.hpp"
#include "runtime/transport/transport_connection.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <optional>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zlink::stream_connector::detail
{

struct endpoint_parts_t
{
    std::string host;
    std::string port;
};

std::unique_ptr<stream_connection_t> make_tcp_connection (boost::asio::io_context &io_context,
                                                          boost::asio::ip::tcp::socket socket);
std::optional<endpoint_parts_t> parse_tcp_endpoint (const std::string &endpoint);
std::optional<endpoint_parts_t> parse_tls_endpoint (const std::string &endpoint);
bool is_transport_connected (const connector_state_t &state);
std::vector<std::uint8_t> read_exact (connector_state_t &state, std::size_t size);
void write_bytes (connector_state_t &state, const std::vector<std::uint8_t> &bytes);
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
std::unique_ptr<stream_connection_t> connect_tls (boost::asio::io_context &io_context,
                                                  const endpoint_parts_t &endpoint,
                                                  bool skip_server_certificate_validation);
void connect_tls_async (
  boost::asio::io_context &io_context,
  endpoint_parts_t endpoint,
  bool skip_server_certificate_validation,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback);
#endif

} // namespace zlink::stream_connector::detail
