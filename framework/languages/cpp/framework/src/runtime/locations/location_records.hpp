/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <zlink/framework/contracts/locations/rows.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework
{

// Framework-private records encoded into the opaque Location Store SPI.
// Providers only receive store_key_t and byte payloads.

struct mesh_node_descriptor_t
{
    std::string mesh_name;
    zlink::routing_id_t rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::string endpoint;
    std::optional<std::string> entry_spot_id;
    std::map<std::string, int> channel_weights;
    std::int64_t application_version = 0;
    std::vector<object_capability_t> object_capabilities;
    object_role_t object_role = object_role_t::none;
    int placement_weight = 100;
    placement_capacity_t capacity{};
    activation_concurrency_t activation_concurrency{};
    std::optional<std::string> maintenance_wave;
    framework_runtime_state_t state =
      framework_runtime_state_t::preparing;
    std::string security_identity;
    std::string owner_id;
    std::int64_t lease_generation = 0;
    std::chrono::system_clock::time_point updated_at{};
};

struct mesh_node_descriptor_key_t
{
    std::string mesh_name;
    zlink::routing_id_t rid =
      zlink::routing_id_t::from (std::uint32_t{0});
};

struct client_server_server_descriptor_t
{
    std::string channel_name;
    zlink::routing_id_t server_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::string endpoint;
    int weight = 100;
    framework_runtime_state_t state =
      framework_runtime_state_t::preparing;
    std::string security_identity;
    std::string owner_id;
    std::int64_t lease_generation = 0;
    std::chrono::system_clock::time_point updated_at{};
};

struct client_server_server_descriptor_key_t
{
    std::string channel_name;
    zlink::routing_id_t server_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
};

struct fanout_publisher_descriptor_t
{
    std::string channel_name;
    zlink::routing_id_t publisher_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::string endpoint;
    framework_runtime_state_t state =
      framework_runtime_state_t::preparing;
    std::string security_identity;
    std::string owner_id;
    std::int64_t lease_generation = 0;
    std::chrono::system_clock::time_point updated_at{};
};

struct fanout_publisher_descriptor_key_t
{
    std::string channel_name;
    zlink::routing_id_t publisher_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
};

} // namespace zlink::framework
