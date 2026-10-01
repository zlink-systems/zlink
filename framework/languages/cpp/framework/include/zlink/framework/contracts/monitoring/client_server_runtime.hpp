/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework
{

enum class client_server_role_t
{
    client,
    server,
    client_and_server
};

struct client_server_target_snapshot_t
{
    zlink::routing_id_t node_rid;
    int weight = 0;
    peer_state_t state = peer_state_t::not_connected;
    std::optional<topology_reason_t> unavailable_reason;
};

struct client_server_channel_snapshot_t
{
    std::string channel_name;
    client_server_role_t local_role = client_server_role_t::client;
    topology_state_t state = topology_state_t::starting;
    bool is_ready = false;
    std::uint32_t ready_target_count = 0;
    std::vector<client_server_target_snapshot_t> targets;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at{};
};

class client_server_runtime_t
{
  public:
    virtual ~client_server_runtime_t () = default;

    virtual client_server_channel_snapshot_t snapshot (std::string channel_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t>
    observe (std::string channel_name,
             std::size_t capacity,
             std::function<void (const observed_status_t<client_server_channel_snapshot_t> &)>
               observer) = 0;
    virtual bool is_ready (std::string channel_name) const = 0;
};

} // namespace zlink::framework
