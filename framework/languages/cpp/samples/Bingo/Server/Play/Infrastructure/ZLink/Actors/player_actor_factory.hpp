/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "player_actor.hpp"

#include <utility>

namespace zlink::samples::bingo
{

struct player_actor_factory_t final
    : framework::actor_factory_t<player_actor_t>
{
    player_actor_t create (std::string actor_id) const
    {
        return create (std::move (actor_id), {});
    }

    player_actor_t create (std::string actor_id,
                           std::string display_name) const
    {
        if (display_name.empty ()) {
            display_name = actor_id;
        }
        player_actor_t player;
        player.actor_id = std::move (actor_id);
        player.display_name = std::move (display_name);
        return player;
    }

    framework::task_t<std::shared_ptr<player_actor_t>>
    create (actor_context_t context,
            std::stop_token) override
    {
        auto actor = std::make_shared<player_actor_t> (
          create (std::string (context.actor_ref ().actor_id ().value ()), {}));
        actor->set_actor_context (std::move (context));
        co_return actor;
    }
};

} // namespace zlink::samples::bingo
