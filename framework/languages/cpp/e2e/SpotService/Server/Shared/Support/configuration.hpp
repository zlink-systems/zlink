/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

struct spot_tcp_endpoint_t
{
    std::string host;
    std::uint16_t port = 0;
};

inline spot_tcp_endpoint_t parse_spot_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (!endpoint.starts_with (prefix))
        throw std::invalid_argument ("Spot client/server endpoint must use tcp://");
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= prefix.size ()
        || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("Spot client/server endpoint must include host and port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("Spot client/server endpoint port is out of range");
    return {.host = endpoint.substr (prefix.size (), separator - prefix.size ()),
            .port = static_cast<std::uint16_t> (value)};
}

inline std::vector<std::string> split_endpoints (const std::string &text)
{
    std::vector<std::string> endpoints;
    std::stringstream input (text);
    std::string endpoint;
    while (std::getline (input, endpoint, ',')) {
        if (!endpoint.empty ()) {
            endpoints.push_back (endpoint);
        }
    }
    return endpoints;
}

inline void load_spot_service_config (zlink::framework::app_t &app,
                                      int argc,
                                      char **argv,
                                      const char *role)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error (std::string ("SpotService ") + role
                                  + " requires --config=<path>");
    }
    app.config ().load_json (*path);
}
