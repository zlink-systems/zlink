/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/locations/spot_kind.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace zlink::framework
{

enum class route_kind_t
{
    invalid = 0,
    actor_session = 1,
    spot_name = 2,
    framework_route = 3
};

enum class location_kind_t
{
    invalid = 0,
    peer = 1,
    spot = 2,
    actor = 3,
    route = 4
};

struct spot_location_key_t
{
    std::string spot_id;
};

struct actor_location_key_t
{
    std::string mesh_name;
    std::string actor_id;
};

struct route_location_key_t
{
    route_kind_t route_kind = route_kind_t::invalid;
    std::string route_key;
};

struct spot_location_t
{
    std::string mesh_name;
    std::string spot_id;
    std::uint64_t spot_generation = 0;
    std::optional<std::string> spot_type;
    zlink::routing_id_t node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    zlink::spot_kind spot_kind = zlink::spot_kind::invalid;
    std::optional<std::string> route_endpoint;
    std::string owner_id;
    std::int64_t generation = 0;
    std::chrono::system_clock::time_point updated_at{};
};

struct actor_location_t
{
    std::string mesh_name;
    std::string actor_id;
    std::string actor_type;
    std::optional<actor_ref_t> actor_ref;
    zlink::routing_id_t owner_node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    std::uint64_t owner_node_generation = 0;
    std::string spot_id;
    std::uint64_t spot_generation = 0;
    zlink::spot_kind spot_kind = zlink::spot_kind::invalid;
    std::uint64_t membership_epoch = 0;
    std::string owner_id;
    std::chrono::system_clock::time_point updated_at{};
};

} // namespace zlink::framework
