/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_entry_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<match_bingo_res_t> bingo_entry_spot_t::match_bingo (player_actor_t &actor,
                                                                  message_context_t &,
                                                                  const match_bingo_req_t &request)
{
    const auto display_name = actor.display_name.empty () ? actor.actor_id : actor.display_name;
    match_bingo_api_req_t match_request;
    match_request.set_actor_id (actor.actor_id);
    match_request.set_display_name (display_name);
    match_request.set_mode (request.mode ());
    auto matched = co_await _context.outbound ()
                     .request (sample_names_t::api_channel, match_request)
                     .submit<match_bingo_api_res_t> ();
    bingo_room_join_req_t join_request;
    join_request.set_room_id (matched.room_id ());
    join_request.set_actor_id (actor.actor_id);
    join_request.set_display_name (display_name);
    bingo_room_state_t initial_state;
    initial_state.room_id = matched.room_id ();
    initial_state.status = bingo_room_status_t::waiting;
    initial_state.host_actor_id = actor.actor_id;
    initial_state.players.push_back (bingo_player_state_t{actor.actor_id, display_name, 1, true});
    match_bingo_res_t response;
    response.set_room_id (matched.room_id ());
    write_message (initial_state, *response.mutable_state ());
    actor.context ().join_spot (matched.room_id (), join_request).defer ();
    co_return response;
}

} // namespace zlink::samples::bingo
