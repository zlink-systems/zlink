/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <map>
#include <string>

namespace zlink::http_client
{

inline constexpr int version_major = 0;
inline constexpr int version_minor = 3;
inline constexpr int version_patch = 1;

enum class http_method_t
{
    get,
    post,
    put,
    delete_,
    patch,
    head,
    options
};

template <typename T> struct http_response_t
{
    int status = 0;
    std::map<std::string, std::string> headers;
    T body;
    std::string raw_body;
};

struct raw_http_response_t
{
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

} // namespace zlink::http_client
