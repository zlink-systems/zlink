/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/configuration.hpp"

#include <zlink/framework.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace atd = zlink::framework::e2e::automatic_turn_dispatch::server;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace
{

struct external_api_options_t
{
    std::string http_endpoint;

    static external_api_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.http_endpoint = section.require ("httpEndpoint")};
    }
};

struct listen_address_t
{
    std::string host;
    std::uint16_t port;
};

listen_address_t parse_http_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "http://";
    if (!endpoint.starts_with (prefix)) {
        throw std::runtime_error ("external-api httpEndpoint must start with http://");
    }
    const auto address = endpoint.substr (prefix.size ());
    const auto separator = address.rfind (':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= address.size ()) {
        throw std::runtime_error ("external-api httpEndpoint must contain host and port");
    }
    const auto port = std::stoi (address.substr (separator + 1));
    if (port < 1 || port > 65535) {
        throw std::runtime_error ("external-api httpEndpoint port is out of range");
    }
    return {.host = address.substr (0, separator),
            .port = static_cast<std::uint16_t> (port)};
}

std::map<std::string, std::string> parse_query (std::string_view target)
{
    std::map<std::string, std::string> result;
    const auto question = target.find ('?');
    if (question == std::string_view::npos) {
        return result;
    }
    auto remaining = target.substr (question + 1);
    while (!remaining.empty ()) {
        const auto ampersand = remaining.find ('&');
        const auto pair = remaining.substr (0, ampersand);
        const auto equal = pair.find ('=');
        if (equal != std::string_view::npos) {
            result.emplace (std::string (pair.substr (0, equal)),
                            std::string (pair.substr (equal + 1)));
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix (ampersand + 1);
    }
    return result;
}

void serve_connection (tcp::socket socket)
{
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    beast::error_code read_error;
    http::read (socket, buffer, request, read_error);
    if (read_error) {
        return;
    }
    const auto target = std::string_view (request.target ().data (), request.target ().size ());
    http::response<http::string_body> response{http::status::ok, request.version ()};
    response.set (http::field::content_type, "application/json");
    response.keep_alive (false);
    if (target.starts_with ("/health")) {
        response.body () = R"({"status":"ready","role":"external-api"})";
    }
    else if (target.starts_with ("/delay?")) {
        const auto query = parse_query (target);
        const auto delay_ms = std::stoi (query.at ("delayMs"));
        std::this_thread::sleep_for (std::chrono::milliseconds (delay_ms));
        response.body () = "{\"request_id\":\"" + query.at ("requestId")
                           + "\",\"marker\":\"" + query.at ("marker") + "\"}";
    }
    else {
        response.result (http::status::not_found);
        response.body () = R"({"error":"not-found"})";
    }
    response.prepare_payload ();
    http::write (socket, response);
    beast::error_code ignored;
    socket.shutdown (tcp::socket::shutdown_send, ignored);
}

} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options =
      atd::read_role_options<external_api_options_t> (app, argc, argv, "external-api");
    const auto endpoint = parse_http_endpoint (options.http_endpoint);
    asio::io_context io;
    tcp::acceptor acceptor (io, {asio::ip::make_address (endpoint.host), endpoint.port});
    for (;;) {
        auto socket = acceptor.accept ();
        std::thread ([socket = std::move (socket)] () mutable {
            try {
                serve_connection (std::move (socket));
            }
            catch (...) {
                // A malformed probe or disconnected caller only ends this connection.
            }
        }).detach ();
    }
}
