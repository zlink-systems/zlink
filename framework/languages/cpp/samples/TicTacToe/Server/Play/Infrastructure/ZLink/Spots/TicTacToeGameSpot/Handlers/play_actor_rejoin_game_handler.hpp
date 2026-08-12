/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_game_spot.hpp"

namespace zlink::samples::tictactoe
{

inline void tictactoe_game_spot_t::confirm_joined_game (
  const player_actor_t &actor,
  const message_context_t &,
  const join_game_msg_t &request)
{
    const auto &state = match ().snapshot ();
    if (request.room_id != state.room_id) {
        throw std::runtime_error ("JoinGameMsg room id does not match the joined room.");
    }
    if (actor.actor_id != state.x_actor_id && actor.actor_id != state.o_actor_id) {
        throw std::runtime_error ("JoinGameMsg actor is not a room member.");
    }
    actor.push (join_game_notify_t{state});
}

} // namespace zlink::samples::tictactoe
