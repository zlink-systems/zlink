/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_entry_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<observe_bingo_events_res_t>
bingo_entry_spot_t::observe_bingo_events (
  player_actor_t &actor,
  message_context_t &,
  const observe_bingo_events_req_t &request)
{
    const auto display_name =
      actor.display_name.empty () ? actor.actor_id : actor.display_name;
    const auto observer_id = observer_room_id (request.room_id, actor.actor_id);
    const bingo_room_settings_payload_t payload{
      "Bingo Observer " + actor.actor_id,
      bingo_sample_modes_t::two_player, 0, 75, "Observer", request.room_id};
    co_await _context.manager ()
      .get_or_create (observer_id, sample_names_t::room_spot)
      .creation_request (payload)
      .submit ();
    const auto join_request = bingo_room_join_req_t{
      request.room_id, actor.actor_id, display_name, true};
    actor.context ().join_spot (observer_id, join_request).defer ();
    co_return observe_bingo_events_res_t{true};
}

} // namespace zlink::samples::bingo
