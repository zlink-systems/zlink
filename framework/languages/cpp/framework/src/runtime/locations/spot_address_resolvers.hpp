/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <runtime/locations/location_repository_values.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zlink::framework::runtime
{

/* Internal resolved address. Public direct messaging accepts only a global
 * SpotId; lifecycle and relocation code may retain this exact owner snapshot. */
struct spot_address_t
{
    std::string mesh_name;
    zlink::routing_id_t node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    std::string spot_id;
    std::uint64_t spot_generation = 0;
    std::string store_version;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    location_owner_token_t owner;
    std::uint64_t node_generation = 0;
};

class spot_address_resolver_t
{
  public:
    virtual ~spot_address_resolver_t () = default;
    virtual task_t<std::optional<spot_address_t>>
    resolve_spot_address (std::string mesh_name, std::string spot_id) = 0;
    virtual void invalidate_spot_address (std::string_view spot_id) = 0;
    virtual bool invalidate_spot_address_if_matches (
      std::string_view spot_id, const spot_address_t &expected)
    {
        (void) spot_id;
        (void) expected;
        return false;
    }
    virtual void invalidate_all_routes_after_store_recovery () = 0;
};

class actor_address_resolver_t
{
  public:
    virtual ~actor_address_resolver_t () = default;
    virtual task_t<std::optional<spot_address_t>>
    resolve_actor_address (std::string actor_id) = 0;
    virtual void invalidate_actor_address (std::string_view actor_id)
    {
        (void) actor_id;
    }
    virtual bool invalidate_actor_address_if_matches (
      std::string_view actor_id, const spot_address_t &expected)
    {
        (void) actor_id;
        (void) expected;
        return false;
    }
};

} // namespace zlink::framework::runtime
