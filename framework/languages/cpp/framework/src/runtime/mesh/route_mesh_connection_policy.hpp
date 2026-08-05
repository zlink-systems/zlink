/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_records.hpp>

namespace zlink::framework::runtime::mesh
{

inline bool route_mesh_connection_not_required (
  object_role_t local_role,
  bool local_has_server_channel,
  object_role_t remote_role,
  bool remote_has_server_channel) noexcept
{
    return local_role == object_role_t::client
           && !local_has_server_channel
           && remote_role == object_role_t::client
           && !remote_has_server_channel;
}

inline bool route_mesh_connection_not_required (
  const mesh_node_descriptor_t &local,
  const mesh_node_descriptor_t &remote) noexcept
{
    return route_mesh_connection_not_required (
      local.object_role, !local.channel_weights.empty (),
      remote.object_role, !remote.channel_weights.empty ());
}

} // namespace zlink::framework::runtime::mesh
