/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<submit_bingo_card_res_t> bingo_room_spot_t::submit_card (
  const player_actor_t &actor,
  const message_context_t &context,
  const submit_bingo_card_req_t &request)
{
    if (context.packet_name.empty ()) {
        throw std::runtime_error ("packet name is required");
    }
    (void) _game.submit_card (actor.actor_id, request.card);
    co_return submit_bingo_card_res_t{snapshot ()};
}

} // namespace zlink::samples::bingo
