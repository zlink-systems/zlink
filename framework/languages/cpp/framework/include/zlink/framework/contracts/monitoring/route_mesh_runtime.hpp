/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

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

enum class mesh_node_state_t
{
    starting,
    ready,
    degraded,
    stopping,
    stopped,
    failed
};

enum class topology_reason_t
{
    runtime_not_ready,
    no_ready_peer,
    no_ready_target,
    location_unavailable,
    capacity_exceeded,
    draining,
    internal_failure
};

enum class peer_state_t
{
    connecting,
    ready,
    draining,
    not_connected,
    not_required
};

struct mesh_peer_snapshot_t
{
    zlink::routing_id_t node_rid;
    peer_state_t state = peer_state_t::not_connected;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_channel_snapshot_t
{
    std::string channel_name;
    bool is_ready = false;
    std::uint32_t ready_target_count = 0;
};

struct mesh_placement_snapshot_t
{
    bool is_available = false;
    std::uint32_t active_actor_count = 0;
    std::uint32_t active_spot_count = 0;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_node_snapshot_t
{
    std::string mesh_name;
    mesh_node_state_t state;
    bool is_ready = false;
    std::uint32_t ready_peer_count = 0;
    std::vector<mesh_channel_snapshot_t> channels;
    std::vector<mesh_peer_snapshot_t> peers;
    mesh_placement_snapshot_t placement;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class mesh_runtime_observation_t
{
  public:
    virtual ~mesh_runtime_observation_t () = default;
    virtual void close () = 0;
};

class route_mesh_runtime_t
{
  public:
    virtual mesh_node_snapshot_t snapshot (std::string mesh_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t>
    observe (std::string mesh_name,
             std::size_t capacity,
             std::function<void (
               const observed_status_t<mesh_node_snapshot_t> &)> observer) = 0;
    virtual bool is_ready (std::string mesh_name) const = 0;
};

} // namespace zlink::framework
