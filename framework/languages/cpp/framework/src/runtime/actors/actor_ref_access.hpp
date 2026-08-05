/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework::detail
{

/* Stable actor type is runtime placement metadata, not public ActorRef data. */
class actor_ref_access_t final
{
  public:
    static actor_ref_t make (node_rid_t node_rid,
                             std::string actor_type,
                             std::string actor_id,
                             std::uint64_t generation)
    {
        actor_ref_t result (actor_id_t (std::move (actor_id)), generation, {},
                            std::move (node_rid));
        result._actor_type = std::move (actor_type);
        return result;
    }

    static std::string_view actor_type (const actor_ref_t &actor) noexcept
    {
        return actor._actor_type;
    }

    static actor_ref_t with_actor_type (const actor_ref_t &actor,
                                        std::string actor_type)
    {
        actor_ref_t result = actor;
        result._actor_type = std::move (actor_type);
        return result;
    }

    static bool empty (const actor_ref_t &actor) noexcept
    {
        return actor._actor_id.value ().empty ();
    }
};

} // namespace zlink::framework::detail
