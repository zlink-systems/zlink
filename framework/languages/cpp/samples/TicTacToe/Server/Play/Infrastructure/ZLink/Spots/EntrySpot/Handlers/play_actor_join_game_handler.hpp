/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_entry_spot.hpp"

namespace zlink::samples::tictactoe
{

// --8<-- [start:doc-join-defer]
// C++은 handler class 대신 Entry Spot member 함수다.
inline task_t<void>
tictactoe_entry_spot_t::join_game (player_actor_t &actor,
                                   message_context_t &,
                                   const join_game_req_t &request)
{
    /* 공통 sample spec §13: JoinSpot payload에는 인증 때 actor에 설정한 PlayerInfo가 들어가고,
     * owner room Spot이 level 조건을 확인한다. */
    const auto payload = tictactoe_game_join_req_t{request.room_id, actor.require_player ()};
    actor.track_deferred_join (request.room_id);
    actor.context ().join_spot (request.room_id, payload).defer ();
    co_return;
}
// --8<-- [end:doc-join-defer]

} // namespace zlink::samples::tictactoe
