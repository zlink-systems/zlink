/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/http_client/contracts/client.hpp>

#include <boost/beast/http/verb.hpp>

#include <string>
#include <string_view>

namespace zlink::http_client::detail
{

inline constexpr unsigned http_11_version = 11;
inline constexpr std::string_view http_url_prefix = "http://";
inline constexpr std::string_view https_url_prefix = "https://";
inline constexpr const char *http_default_port = "80";
inline constexpr const char *https_default_port = "443";

enum class http_scheme_t
{
    plain,
    secure
};

struct parsed_url_t
{
    http_scheme_t scheme;
    std::string host;
    std::string port;
    std::string target_prefix;
};

struct hop_target_t
{
    http_scheme_t scheme;
    std::string host;
    std::string port;
    std::string target;
};

parsed_url_t parse_base_url (const std::string &url);
std::string make_target (const parsed_url_t &url, const std::string &path);
boost::beast::http::verb to_beast_method (http_method_t method);
bool is_redirect_status (int status);
bool same_origin (const hop_target_t &left, const hop_target_t &right);
hop_target_t resolve_location (const hop_target_t &current, const std::string &location);

} // namespace zlink::http_client::detail
