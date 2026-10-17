#pragma once
#include "contracts.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
namespace MyGame
{
namespace net = boost::asio;
namespace http = boost::beast::http;
using Tcp = net::ip::tcp;
inline std::pair<std::string, std::string> http_address (const std::string &endpoint)
{
    constexpr std::string_view scheme = "http://";
    const auto separator = endpoint.rfind (':');
    if (!endpoint.starts_with (scheme) || separator < scheme.size ()
        || separator + 1 == endpoint.size ())
        throw std::invalid_argument ("HTTP endpoint requires http://host:port");
    return {endpoint.substr (scheme.size (), separator - scheme.size ()),
            endpoint.substr (separator + 1)};
}
inline std::vector<WorldInfo> fetch_worlds (const std::string &endpoint)
{
    const auto [host, port] = http_address (endpoint);
    net::io_context io;
    Tcp::resolver resolver (io);
    boost::beast::tcp_stream stream (io);
    stream.expires_after (Limits::packet);
    stream.connect (resolver.resolve (host, port));
    constexpr unsigned http_version = 11;
    http::request<http::empty_body> request{http::verb::get, Names::worlds_path, http_version};
    request.set (http::field::host, host);
    http::write (stream, request);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read (stream, buffer, response);
    if (response.result () != http::status::ok)
        throw std::runtime_error ("World list HTTP request failed");
    return nlohmann::json::parse (response.body ()).get<std::vector<WorldInfo>> ();
}
}
