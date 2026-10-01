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

struct fanout_channel_snapshot_t
{
    std::string channel_name;
    topology_state_t state = topology_state_t::starting;
    bool is_ready = false;
    std::uint32_t ready_publisher_count = 0;
    std::vector<mesh_peer_snapshot_t> publishers;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at{};
};

class fanout_runtime_observation_t
{
  public:
    virtual ~fanout_runtime_observation_t () = default;
    virtual void close () = 0;
};

class fanout_runtime_t
{
  public:
    virtual ~fanout_runtime_t () = default;

    virtual fanout_channel_snapshot_t snapshot (std::string channel_name) const = 0;
    virtual std::unique_ptr<fanout_runtime_observation_t> observe (
      std::string channel_name,
      std::size_t capacity,
      std::function<void (const observed_status_t<fanout_channel_snapshot_t> &)> observer) = 0;
};

} // namespace zlink::framework
