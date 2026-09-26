/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "runtime/diagnostics/listener_status_registry.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

namespace zlink::framework::tests
{
// A test listener binds "tcp://127.0.0.1:*" (or "tls://127.0.0.1:*"), so the
// port is chosen by the bind itself and no other process can take it between
// a reservation and the bind. The resolved endpoint is read afterwards.
inline constexpr const char *loopback_tcp_any_port = "tcp://127.0.0.1:*";

inline std::uint16_t endpoint_port (const std::string &endpoint_)
{
    const auto colon = endpoint_.rfind (':');
    assert (colon != std::string::npos);
    return static_cast<std::uint16_t> (std::stoul (endpoint_.substr (colon + 1)));
}

// The endpoint a started listener resolved, from its listener status.
inline std::string listener_endpoint (const runtime::listener_status_registry_t &listeners_,
                                      listener_kind_t kind_,
                                      const std::string &name_)
{
    const std::optional<listener_status_t> status = listeners_.find (kind_, name_);
    assert (status && !status->endpoint.empty ());
    return status->endpoint;
}
}
