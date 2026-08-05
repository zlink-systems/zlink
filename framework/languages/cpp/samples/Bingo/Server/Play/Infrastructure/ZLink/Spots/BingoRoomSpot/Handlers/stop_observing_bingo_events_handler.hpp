/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<stop_observing_bingo_events_res_t>
bingo_room_spot_t::stop_observing_events (
  const player_actor_t &actor,
  const message_context_t &,
  const stop_observing_bingo_events_req_t &request)
{
    if (!_is_observer) {
        throw std::runtime_error ("stop observing is valid only in an observer room");
    }
    if (request.room_id != _observed_room_id) {
        throw std::runtime_error ("stop observing room id does not match the subscription");
    }
    const auto observer = observers.find (actor.actor_id);
    if (observer == observers.end ()) {
        throw std::runtime_error ("actor has no observer subscription in this room");
    }
    co_await _context->leave_actor (
      actor_ref_for (actor), const_cast<player_actor_t &> (actor));
    record_observer_returned_to_entry_spot (actor);
    co_return stop_observing_bingo_events_res_t{true};
}

} // namespace zlink::samples::bingo
