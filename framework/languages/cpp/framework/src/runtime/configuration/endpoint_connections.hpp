/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <zlink/framework/contracts/configuration/endpoint_connections.hpp>

#include <functional>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

/* Runtime-only access to a public endpoint_connections_t handle: the live
 * socket owner attaches connect/disconnect callbacks (replaying the configured
 * set), and options apply() freezes discovery-mode roles. */
class endpoint_connections_runtime_t
{
  public:
    static void attach (endpoint_connections_t &connections,
                        std::function<void (const std::string &)> connect,
                        std::function<void (const std::string &)> disconnect);
    static void freeze (endpoint_connections_t &connections);
    static void thaw (endpoint_connections_t &connections);
    static std::vector<std::string> snapshot (const endpoint_connections_t &connections);
};

} // namespace zlink::framework::detail
