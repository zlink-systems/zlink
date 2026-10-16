#pragma once
#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "server/shared/contracts.hpp"
namespace MyGame
{
namespace net = boost::asio;
namespace http = boost::beast::http;
using Tcp = net::ip::tcp;
struct Http
{
    static constexpr std::string_view scheme = "http://";
    static constexpr unsigned version = 11;
    static constexpr unsigned max_port = 65535;
};
inline Tcp::endpoint http_address (const std::string &endpoint)
{
    if (!endpoint.starts_with (Http::scheme))
        throw std::invalid_argument ("Expected http:// endpoint");
    const auto address = endpoint.substr (Http::scheme.size ());
    const auto colon = address.rfind (':');
    if (colon == std::string::npos)
        throw std::invalid_argument ("HTTP port required");
    const auto port = std::stoul (address.substr (colon + 1));
    if (port > Http::max_port)
        throw std::invalid_argument ("Invalid HTTP port");
    return {net::ip::make_address (address.substr (0, colon)), static_cast<unsigned short> (port)};
}
}
