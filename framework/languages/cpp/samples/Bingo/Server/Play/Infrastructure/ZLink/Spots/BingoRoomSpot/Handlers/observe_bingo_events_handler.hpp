/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline observe_bingo_events_res_t bingo_room_spot_t::observe_events (
  const player_actor_t &actor,
  const message_context_t &,
  const observe_bingo_events_req_t &request)
{
    _game.set_room_id_if_empty (request.room_id);
    observers[actor.actor_id] = const_cast<player_actor_t *> (&actor);
    return {true};
}

} // namespace zlink::samples::bingo
