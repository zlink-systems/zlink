/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/transport/endpoint_notation.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::runtime::transport
{

namespace detail
{

struct ipv6_component_t
{
    int groups = 0;
    bool all_zero = false;
};

inline std::optional<ipv6_component_t>
parse_ipv6_component (std::string_view component,
                      bool allow_ipv4_tail) noexcept
{
    if (component.empty ())
        return std::nullopt;
    if (component.find ('.') != std::string_view::npos) {
        if (!allow_ipv4_tail)
            return std::nullopt;
        int octets = 0;
        bool all_zero = true;
        std::size_t start = 0;
        while (start <= component.size ()) {
            const auto end = component.find ('.', start);
            const auto length = end == std::string_view::npos
                                  ? component.size () - start
                                  : end - start;
            if (length == 0 || length > 3)
                return std::nullopt;
            unsigned value = 0;
            for (std::size_t index = start; index < start + length; ++index) {
                const auto character = component[index];
                if (character < '0' || character > '9')
                    return std::nullopt;
                value = value * 10u
                        + static_cast<unsigned> (character - '0');
            }
            if (value > 255u)
                return std::nullopt;
            all_zero = all_zero && value == 0;
            ++octets;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return octets == 4
                 ? std::optional<ipv6_component_t>{{2, all_zero}}
                 : std::nullopt;
    }
    if (component.size () > 4)
        return std::nullopt;
    unsigned value = 0;
    for (const auto character : component) {
        unsigned digit = 0;
        if (character >= '0' && character <= '9')
            digit = static_cast<unsigned> (character - '0');
        else if (character >= 'a' && character <= 'f')
            digit = static_cast<unsigned> (character - 'a') + 10u;
        else if (character >= 'A' && character <= 'F')
            digit = static_cast<unsigned> (character - 'A') + 10u;
        else
            return std::nullopt;
        value = (value << 4u) | digit;
    }
    return ipv6_component_t{1, value == 0};
}

inline bool parse_ipv6_side (std::string_view side,
                             bool allow_ipv4_tail,
                             int &groups,
                             bool &all_zero) noexcept
{
    if (side.empty ())
        return true;
    std::size_t start = 0;
    while (start <= side.size ()) {
        const auto end = side.find (':', start);
        const auto component = side.substr (
          start,
          end == std::string_view::npos ? side.size () - start
                                         : end - start);
        const auto parsed = parse_ipv6_component (
          component,
          allow_ipv4_tail && end == std::string_view::npos);
        if (!parsed)
            return false;
        groups += parsed->groups;
        all_zero = all_zero && parsed->all_zero;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

inline bool is_zero_ipv6 (std::string_view host) noexcept
{
    if (host.size () >= 2 && host.front () == '[' && host.back () == ']')
        host = host.substr (1, host.size () - 2);
    if (const auto scope = host.find ('%'); scope != std::string_view::npos) {
        if (scope == 0 || scope + 1 >= host.size ())
            return false;
        host = host.substr (0, scope);
    }
    if (host.empty () || host.find (':') == std::string_view::npos
        || host.find ('[') != std::string_view::npos
        || host.find (']') != std::string_view::npos)
        return false;

    const auto compression = host.find ("::");
    if (compression != std::string_view::npos
        && host.find ("::", compression + 2) != std::string_view::npos)
        return false;

    int groups = 0;
    bool all_zero = true;
    if (compression == std::string_view::npos) {
        if (!parse_ipv6_side (host, true, groups, all_zero))
            return false;
        return groups == 8 && all_zero;
    }

    if (!parse_ipv6_side (host.substr (0, compression), false, groups, all_zero)
        || !parse_ipv6_side (host.substr (compression + 2), true, groups, all_zero))
        return false;
    return groups < 8 && all_zero;
}

} // namespace detail

inline bool is_wildcard_host (std::string_view host) noexcept
{
    return host == "0.0.0.0" || host == "*" || detail::is_zero_ipv6 (host);
}

/* Resolves the endpoint that a remote process uses after a listener has bound.
 * The caller supplies the listener kind only for a useful configuration error;
 * endpoint parsing and IPv6 formatting remain in this one runtime seam. */
inline std::string advertised_tcp_endpoint (
  std::string bound_endpoint,
  const std::optional<std::string> &advertise_host,
  std::string_view listener_kind)
{
    const auto port_separator = bound_endpoint.rfind (':');
    if (!bound_endpoint.starts_with ("tcp://")
        || port_separator == std::string::npos || port_separator < 6) {
        if (!advertise_host)
            return normalize_endpoint (bound_endpoint);
        throw std::invalid_argument (
          std::string (listener_kind) + " advertise host requires a TCP bind endpoint");
    }

    const auto bound_host = std::string_view (bound_endpoint).substr (
      6, port_separator - 6);
    if (!advertise_host && is_wildcard_host (bound_host)) {
        throw std::invalid_argument (
          std::string (listener_kind) + " wildcard bind host requires an advertise host");
    }
    if (!advertise_host)
        return normalize_endpoint (bound_endpoint);

    const auto &configured_host = *advertise_host;
    if (is_wildcard_host (configured_host))
        throw std::invalid_argument (
          std::string (listener_kind)
          + " advertise host must be a remotely reachable, non-wildcard host");
    const auto host = bracket_ipv6_host (configured_host);
    return normalize_endpoint ("tcp://" + host + bound_endpoint.substr (port_separator));
}

} // namespace zlink::framework::runtime::transport
