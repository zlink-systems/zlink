/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/*
 * Deterministic, lossless endpoint-string normalization.
 *
 * Implements doc/plan/endpoint-notation-policy.ko.md §2.2-2.4 for C++:
 * lowercase scheme, lowercase host, unify IPv6 literals to bracket
 * notation (zone id preserved verbatim), decimal ports with leading
 * zeros stripped, trailing slash removed from the path, and the whole
 * string trimmed of surrounding whitespace.
 *
 * This is intentionally NOT a validator: `normalize_endpoint` never
 * throws. Input it cannot confidently re-shape (no scheme, an unknown
 * or non-authority scheme such as "ipc", or an authority it cannot
 * parse without guessing) is returned with only whitespace trimmed and
 * (when a scheme is present) the scheme lowercased -- callers that need
 * to reject malformed endpoints keep doing so where they already do
 * (e.g. transport_endpoint_t::parse, advertised_tcp_endpoint's wildcard
 * guards).
 *
 * Per §2.3 this belongs at write time: call it once when an endpoint is
 * constructed (advertised endpoint assembly) or accepted from outside
 * the process (application-supplied bind/remote endpoints, peer
 * descriptor endpoints read off the wire, Location Store rows). Callers
 * elsewhere keep comparing normalized strings with plain `==`.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace zlink::framework::runtime::transport
{

namespace detail
{

inline bool ascii_is_space (char character) noexcept
{
    return std::isspace (static_cast<unsigned char> (character)) != 0;
}

inline std::string_view trim_ascii_whitespace (std::string_view value) noexcept
{
    std::size_t begin = 0;
    while (begin < value.size () && ascii_is_space (value[begin]))
        ++begin;
    std::size_t end = value.size ();
    while (end > begin && ascii_is_space (value[end - 1]))
        --end;
    return value.substr (begin, end - begin);
}

inline std::string lowercase_ascii (std::string_view value)
{
    std::string result (value);
    std::transform (result.begin (), result.end (), result.begin (),
                    [] (unsigned char character) {
                        return static_cast<char> (std::tolower (character));
                    });
    return result;
}

inline bool is_authority_scheme (std::string_view scheme_lower) noexcept
{
    return scheme_lower == "tcp" || scheme_lower == "tls" || scheme_lower == "ws"
           || scheme_lower == "wss";
}

inline bool all_ascii_digits (std::string_view value) noexcept
{
    return !value.empty ()
           && std::all_of (value.begin (), value.end (),
                           [] (unsigned char character) { return std::isdigit (character) != 0; });
}

inline std::string strip_leading_zero_digits (std::string_view port)
{
    std::size_t start = 0;
    while (start + 1 < port.size () && port[start] == '0')
        ++start;
    return std::string (port.substr (start));
}

inline std::string strip_trailing_slashes (std::string_view path)
{
    std::size_t end = path.size ();
    while (end > 0 && path[end - 1] == '/')
        --end;
    return std::string (path.substr (0, end));
}

/* True when `host` needs bracket notation to be an unambiguous TCP/IP
 * authority host (i.e. it contains a ':' and is not already bracketed). */
inline bool looks_like_bare_ipv6 (std::string_view host) noexcept
{
    return !host.empty () && host.front () != '['
           && host.find (':') != std::string_view::npos;
}

} // namespace detail

/* Wraps `host` in "[...]" when it is an unbracketed IPv6-shaped literal
 * (contains ':'), otherwise returns it unchanged. Zone ids (the "%eth0"
 * suffix) travel inside the brackets untouched. Shared by every endpoint
 * construction seam so IPv6 formatting is identical everywhere a TCP
 * endpoint string is assembled. */
inline std::string bracket_ipv6_host (std::string_view host)
{
    if (!detail::looks_like_bare_ipv6 (host))
        return std::string (host);
    std::string bracketed;
    bracketed.reserve (host.size () + 2);
    bracketed += '[';
    bracketed += host;
    bracketed += ']';
    return bracketed;
}

/* Normalizes an endpoint string per the policy above. Total and
 * non-throwing: unparseable input is trimmed (and scheme-lowercased, if
 * a scheme is present) and returned as-is rather than corrupted. */
inline std::string normalize_endpoint (std::string_view raw) noexcept
{
    const auto trimmed = detail::trim_ascii_whitespace (raw);
    if (trimmed.empty ())
        return std::string (trimmed);

    const auto scheme_end = trimmed.find ("://");
    if (scheme_end == std::string_view::npos)
        return std::string (trimmed);

    const auto scheme_lower = detail::lowercase_ascii (trimmed.substr (0, scheme_end));
    const auto rest = trimmed.substr (scheme_end + 3);

    if (!detail::is_authority_scheme (scheme_lower)) {
        /* Non-authority scheme (e.g. "ipc" -- a filesystem path follows,
         * not a host) or an unknown scheme: only the scheme casing is a
         * safe, lossless normalization. The remainder is opaque. */
        std::string result;
        result.reserve (scheme_lower.size () + 3 + rest.size ());
        result += scheme_lower;
        result += "://";
        result += rest;
        return result;
    }

    const auto authority_end = rest.find_first_of ("/?#");
    const auto authority =
      authority_end == std::string_view::npos ? rest : rest.substr (0, authority_end);
    const auto remainder =
      authority_end == std::string_view::npos ? std::string_view{} : rest.substr (authority_end);

    std::string_view user_info;
    std::string_view host_port = authority;
    if (const auto at = authority.rfind ('@'); at != std::string_view::npos) {
        user_info = authority.substr (0, at + 1);
        host_port = authority.substr (at + 1);
    }

    std::string_view host_raw;
    std::string_view port;
    bool is_ipv6 = false;

    if (!host_port.empty () && host_port.front () == '[') {
        const auto close = host_port.find (']');
        if (close == std::string_view::npos) {
            /* Malformed bracket: refuse to guess, return the trimmed
             * original untouched. */
            return std::string (trimmed);
        }
        host_raw = host_port.substr (1, close - 1);
        is_ipv6 = true;
        const auto after = host_port.substr (close + 1);
        if (!after.empty ()) {
            if (after.front () != ':' || !detail::all_ascii_digits (after.substr (1)))
                return std::string (trimmed);
            port = after.substr (1);
        }
    } else {
        const auto colon_count =
          static_cast<std::size_t> (std::count (host_port.begin (), host_port.end (), ':'));
        if (colon_count == 0) {
            host_raw = host_port;
        } else if (colon_count == 1) {
            const auto colon = host_port.find (':');
            const auto candidate_port = host_port.substr (colon + 1);
            if (detail::all_ascii_digits (candidate_port)) {
                host_raw = host_port.substr (0, colon);
                port = candidate_port;
            } else {
                /* A single colon but no valid port after it -- not a
                 * shape this function can safely re-normalize. Refuse
                 * to guess (§2.4) and hand back the trimmed original. */
                return std::string (trimmed);
            }
        } else {
            /* 2+ colons with no brackets: an unbracketed IPv6 literal.
             * lastIndexOf(':')-style port splitting is exactly what §2.4
             * forbids, so no port is extracted here -- only bracketing. */
            host_raw = host_port;
            is_ipv6 = true;
        }
    }

    std::string host_normalized;
    if (is_ipv6) {
        const auto zone = host_raw.find ('%');
        const auto ip_part = zone == std::string_view::npos ? host_raw : host_raw.substr (0, zone);
        host_normalized = detail::lowercase_ascii (ip_part);
        if (zone != std::string_view::npos)
            host_normalized += std::string (host_raw.substr (zone));
    } else {
        host_normalized = detail::lowercase_ascii (host_raw);
    }

    const auto port_normalized =
      port.empty () ? std::string{} : detail::strip_leading_zero_digits (port);

    const auto qpos = remainder.find_first_of ("?#");
    const auto path = qpos == std::string_view::npos ? remainder : remainder.substr (0, qpos);
    const auto tail = qpos == std::string_view::npos ? std::string_view{} : remainder.substr (qpos);
    const auto path_normalized = detail::strip_trailing_slashes (path);

    std::string result;
    result.reserve (scheme_lower.size () + authority.size () + remainder.size () + 8);
    result += scheme_lower;
    result += "://";
    result += user_info;
    if (is_ipv6) {
        result += '[';
        result += host_normalized;
        result += ']';
    } else {
        result += host_normalized;
    }
    if (!port_normalized.empty ()) {
        result += ':';
        result += port_normalized;
    }
    result += path_normalized;
    result += tail;
    return result;
}

} // namespace zlink::framework::runtime::transport
