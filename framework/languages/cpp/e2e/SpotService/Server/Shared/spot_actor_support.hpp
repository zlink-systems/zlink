/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework.hpp>

#include <string>

namespace e2e = zlink::framework::e2e::spot_service;

namespace
{

zlink::framework::actor_ref_t to_actor_ref (const e2e::actor_ref_dto_t &actor)
{
    return zlink::framework::actor_ref_t (
      zlink::framework::actor_id_t (actor.actor_id), actor.generation, e2e::spot_mesh,
      zlink::framework::node_rid_t::from_string (actor.node_rid));
}

e2e::actor_ref_dto_t from_actor_ref (const zlink::framework::actor_ref_t &actor)
{
    return {.node_rid = std::string (actor.node_rid ().value ()),
            .actor_type = std::string (e2e::actor_type),
            .actor_id = std::string (actor.actor_id ().value ()),
            .generation = actor.object_generation ()};
}

std::string owner_for_key (const std::string &key)
{
    return e2e::owner_node_rid_for_key (key);
}

zlink::framework::spot_id_t user_spot_id (const std::string &key)
{
    return (e2e::user_spot_id_for_key (key));
}

} // namespace
