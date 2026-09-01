/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_entry_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<observe_bingo_events_res_t> bingo_entry_spot_t::observe_bingo_events (
  player_actor_t &actor, message_context_t &, const observe_bingo_events_req_t &request)
{
    const auto display_name = actor.display_name.empty () ? actor.actor_id : actor.display_name;
    const auto observer_id = observer_room_id (request.room_id (), actor.actor_id);
    bingo_room_create_req_t payload;
    auto *settings = payload.mutable_settings ();
    settings->set_room_name ("Bingo Observer " + actor.actor_id);
    settings->set_mode (bingo_sample_modes_t::two_player);
    settings->set_required_players (0);
    settings->set_max_draw_number (75);
    settings->set_purpose ("Observer");
    settings->set_observed_room_id (request.room_id ());
    co_await _context.manager ()
      .get_or_create (observer_id, sample_names_t::room_spot)
      .creation_request (payload)
      .async ();
    bingo_room_join_req_t join_request;
    join_request.set_room_id (request.room_id ());
    join_request.set_actor_id (actor.actor_id);
    join_request.set_display_name (display_name);
    join_request.set_observe_only (true);
    actor.context ().join_spot (observer_id, join_request).defer ();
    observe_bingo_events_res_t response;
    response.set_subscribed (true);
    co_return response;
}

} // namespace zlink::samples::bingo
