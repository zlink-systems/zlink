/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/transport/transport_connection.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace zlink::stream_connector::detail
{

struct websocket_endpoint_parts_t
{
    std::string host;
    std::string port;
    std::string target;
};

std::optional<websocket_endpoint_parts_t> parse_websocket_endpoint (const std::string &endpoint);
std::optional<websocket_endpoint_parts_t>
parse_websocket_secure_endpoint (const std::string &endpoint);
std::unique_ptr<stream_connection_t> connect_websocket (boost::asio::io_context &io_context,
                                                        const websocket_endpoint_parts_t &endpoint);
void connect_websocket_async (
  boost::asio::io_context &io_context,
  websocket_endpoint_parts_t endpoint,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback);
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
std::unique_ptr<stream_connection_t>
connect_websocket_secure (boost::asio::io_context &io_context,
                          const websocket_endpoint_parts_t &endpoint,
                          bool skip_server_certificate_validation);
void connect_websocket_secure_async (
  boost::asio::io_context &io_context,
  websocket_endpoint_parts_t endpoint,
  bool skip_server_certificate_validation,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback);
#endif

} // namespace zlink::stream_connector::detail
