/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::e2e::registry_messaging
{

struct tcp_endpoint_t
{
    std::string host;
    std::uint16_t port;
};

inline tcp_endpoint_t parse_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (!endpoint.starts_with (prefix)) {
        throw std::runtime_error ("ClientServer endpoint must use tcp://: " + endpoint);
    }
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= prefix.size ()
        || separator + 1 >= endpoint.size ()) {
        throw std::runtime_error (
          "ClientServer endpoint must contain a host and port: " + endpoint);
    }
    const auto port = std::stoul (endpoint.substr (separator + 1));
    if (port == 0 || port > 65535) {
        throw std::runtime_error (
          "ClientServer endpoint port is out of range: " + endpoint);
    }
    return {.host = endpoint.substr (prefix.size (), separator - prefix.size ()),
            .port = static_cast<std::uint16_t> (port)};
}

} // namespace zlink::framework::e2e::registry_messaging
