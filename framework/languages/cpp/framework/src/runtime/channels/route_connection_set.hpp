/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <zlink/Contracts/Core/routing_id.hpp>

namespace zlink::framework::detail
{

class route_connection_set_t
{
  public:
    struct target_t
    {
        std::string endpoint;
        std::optional<zlink::routing_id_t> peer_rid;
    };

    bool connect (std::string endpoint);
    bool connect (zlink::routing_id_t peer_rid, std::string endpoint);
    bool disconnect (const std::string &endpoint);
    bool disconnect (const zlink::routing_id_t &peer_rid,
                     const std::string &endpoint);
    bool contains (const std::string &endpoint) const;
    std::vector<std::string> list () const;
    std::vector<target_t> targets () const;

  private:
    std::map<std::string, std::optional<zlink::routing_id_t>> _manual_connections;
};

} // namespace zlink::framework::detail
