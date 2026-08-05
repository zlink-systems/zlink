/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/cookie_jar.hpp"

#include "runtime/text.hpp"

#include <algorithm>
#include <optional>

namespace zlink::http_client::detail
{

namespace
{

constexpr std::size_t max_cookies_per_host = 128;

bool path_matches_cookie (const std::string &request_path, const std::string &cookie_path)
{
    if (cookie_path.empty () || cookie_path == "/") {
        return true;
    }
    if (request_path == cookie_path) {
        return true;
    }
    if (request_path.rfind (cookie_path, 0) != 0) {
        return false;
    }
    return cookie_path.back () == '/' || request_path[cookie_path.size ()] == '/';
}

} // namespace

void cookie_jar_t::store (const std::string &host, const std::string &set_cookie_header)
{
    std::string_view remaining (set_cookie_header);
    const auto take_segment = [&remaining] () -> std::optional<std::string> {
        if (remaining.empty ()) {
            return std::nullopt;
        }
        const auto semicolon = remaining.find (';');
        auto segment = trim (remaining.substr (0, semicolon));
        remaining = semicolon == std::string_view::npos ? std::string_view ()
                                                        : remaining.substr (semicolon + 1);
        return segment;
    };

    const auto pair = take_segment ();
    if (!pair) {
        return;
    }
    const auto equals = pair->find ('=');
    if (equals == std::string::npos) {
        return;
    }
    cookie_t cookie{.host = host,
                    .name = trim (std::string_view (*pair).substr (0, equals)),
                    .value = trim (std::string_view (*pair).substr (equals + 1)),
                    .path = "/",
                    .secure = false};
    if (cookie.name.empty ()) {
        return;
    }

    bool expired = false;
    while (auto attribute = take_segment ()) {
        const auto attr_equals = attribute->find ('=');
        const auto name = attr_equals == std::string::npos
                            ? *attribute
                            : trim (std::string_view (*attribute).substr (0, attr_equals));
        const auto value = attr_equals == std::string::npos
                             ? std::string ()
                             : trim (std::string_view (*attribute).substr (attr_equals + 1));
        if (iequals (name, "path") && !value.empty ()) {
            cookie.path = value;
        } else if (iequals (name, "secure")) {
            cookie.secure = true;
        } else if (iequals (name, "max-age")) {
            try {
                expired = std::stol (value) <= 0;
            }
            catch (const std::exception &) {
            }
        }
    }

    const std::lock_guard<std::mutex> lock (_mutex);
    _cookies.erase (std::remove_if (_cookies.begin (), _cookies.end (),
                                    [&] (const cookie_t &existing) {
                                        return existing.host == host && existing.name == cookie.name
                                               && existing.path == cookie.path;
                                    }),
                    _cookies.end ());
    if (!expired) {
        _cookies.push_back (std::move (cookie));
        std::size_t cookies_for_host = 0;
        for (const auto &stored : _cookies) {
            if (stored.host == host) {
                ++cookies_for_host;
            }
        }
        while (cookies_for_host > max_cookies_per_host) {
            auto oldest = std::find_if (_cookies.begin (), _cookies.end (),
                                        [&] (const cookie_t &stored) {
                                            return stored.host == host;
                                        });
            if (oldest == _cookies.end ()) {
                break;
            }
            _cookies.erase (oldest);
            --cookies_for_host;
        }
    }
}

std::string
cookie_jar_t::header_for (const std::string &host, const std::string &path, bool secure) const
{
    const std::lock_guard<std::mutex> lock (_mutex);
    std::string header;
    for (const auto &cookie : _cookies) {
        if (cookie.host != host || (cookie.secure && !secure)
            || !path_matches_cookie (path, cookie.path)) {
            continue;
        }
        if (!header.empty ()) {
            header += "; ";
        }
        header += cookie.name + "=" + cookie.value;
    }
    return header;
}

} // namespace zlink::http_client::detail
