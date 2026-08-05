/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_game_spot.hpp"

namespace zlink::samples::tictactoe
{

// --8<-- [start:doc-actor-packet-handler]
// C++은 handler class 대신 Spot member 함수다. 첫 인자가 이 메시지를 받은 Actor다.
inline task_t<place_mark_res_t>
tictactoe_game_spot_t::place_mark (const player_actor_t &actor,
                                   const message_context_t &context,
                                   const place_mark_req_t &request)
{
    if (context.packet_name.empty ()) {
        throw std::runtime_error ("packet name is required");
    }
    auto state = match ().place (actor.actor_id, request);
    game_state_notify_t state_notify{state.room_id, state.next_turn, state};
    co_await publisher.publish (state_notify, actor.actor_id);
    if (state.status == tictactoe_status_t::won || state.status == tictactoe_status_t::draw) {
        co_await publish_win_milestone (actor, state);
    }
    co_return place_mark_res_t{state};
}
// --8<-- [end:doc-actor-packet-handler]

} // namespace zlink::samples::tictactoe
