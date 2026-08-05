/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>
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

enum class client_server_server_state_t
{
    configured,
    connecting,
    ready,
    draining,
    disconnected,
    rejected
};

struct client_server_server_snapshot_t
{
    zlink::routing_id_t server_rid;
    std::uint64_t lifecycle_generation = 0;
    int weight = 0;
    bool ready = false;
    client_server_server_state_t state =
      client_server_server_state_t::configured;
    std::string descriptor_source;
    std::optional<std::string> last_failure;

    friend bool operator== (const client_server_server_snapshot_t &,
                            const client_server_server_snapshot_t &) = default;
};

struct client_server_channel_snapshot_t
{
    std::string channel_name;
    client_server_role_t local_role = client_server_role_t::client;
    bool selectable = false;
    int ready_server_count = 0;
    int connection_intent_count = 0;
    int pending_request_count = 0;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at{};
    std::vector<client_server_server_snapshot_t> servers;
    location_runtime_snapshot_t location;
};

struct client_server_runtime_event_t
{
    std::string identifier;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point timestamp{};
    std::string channel_name;
    std::optional<zlink::routing_id_t> server_rid;
    std::optional<std::uint64_t> lifecycle_generation;
    std::optional<int> weight;
    std::optional<bool> ready;
    std::optional<client_server_server_state_t> state;
    std::optional<std::string> reason;
};

class client_server_runtime_t
{
  public:
    virtual ~client_server_runtime_t () = default;

    virtual client_server_channel_snapshot_t snapshot (
      std::string channel_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t> observe (
      std::string channel_name,
      std::size_t capacity,
      std::function<void (
        const observed_status_t<client_server_runtime_event_t> &)> observer) = 0;
    virtual bool is_ready (std::string channel_name) const = 0;
};

} // namespace zlink::framework
