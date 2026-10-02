/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_game_spot.hpp"

namespace zlink::samples::tictactoe
{

// --8<-- [start:doc-ttt-leave-game]
inline void tictactoe_game_spot_t::leave_game (const player_actor_t &actor,
                                               const message_context_t &,
                                               const leave_game_msg_t &request)
{
    if (request.room_id != match ().snapshot ().room_id) {
        throw std::runtime_error ("LeaveGameMsg room id does not match the joined room.");
    }
    match ().ensure_can_leave (actor.actor_id);
    std::cout << "actor: LeaveGameMsg received. actor=" << actor.actor_id
              << ", roomId=" << request.room_id << std::endl;
    actor.mark_for_destroy_after_room_leave ();
    (void) _context.leave_actor (const_cast<player_actor_t &> (actor));
}
// --8<-- [end:doc-ttt-leave-game]

} // namespace zlink::samples::tictactoe
