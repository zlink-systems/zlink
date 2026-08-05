/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Actors/player_actor.hpp"
#include "../../../../../../../Shared/Contracts/messages.hpp"

#include <map>
#include <string>
#include <string_view>

namespace zlink::samples::tictactoe
{

class game_notification_publisher_t
{
  public:
    explicit game_notification_publisher_t (const std::map<std::string, player_actor_t *> &actors) :
        _actors (actors)
    {
    }

    template <typename TNotify>
    task_t<void>
    publish (const TNotify &notify,
             std::string_view excluded_actor_id = {}) const
    {
        for (const auto &[actor_id, actor] : _actors) {
            if (actor_id == excluded_actor_id || actor == nullptr) {
                continue;
            }
            co_await actor->context ().bound_session ().send (notify).submit ();
        }
        co_return;
    }

  private:
    const std::map<std::string, player_actor_t *> &_actors;
};

} // namespace zlink::samples::tictactoe
