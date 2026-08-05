/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/eventing/health.hpp>
#include <zlink/framework/contracts/http/http.hpp>

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>

#include <string>

namespace zlink::framework::runtime
{
namespace beast = boost::beast;
namespace http = beast::http;

class offload_executor_t;

struct parsed_http_endpoint_t
{
    std::string scheme;
    std::string host;
    std::string port;
};

parsed_http_endpoint_t parse_http_endpoint (const std::string &uri);

http::response<http::string_body>
handle_http_request (const http_options_snapshot_t &options,
                     service_provider_t &services,
                     health_builder_t &health,
                     offload_executor_t &handler_executor,
                     const http::request<http::string_body> &request);

http::response<http::string_body> make_http_status_response (http::status status,
                                                             unsigned version,
                                                             std::string body,
                                                             bool keep_alive);

http::response<http::string_body> make_http_parser_error_response (beast::error_code ec,
                                                                   unsigned version);

} // namespace zlink::framework::runtime
