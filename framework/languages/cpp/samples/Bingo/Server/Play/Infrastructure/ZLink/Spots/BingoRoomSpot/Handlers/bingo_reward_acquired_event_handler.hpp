/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<void>
bingo_room_spot_t::on_reward_acquired (const bingo_reward_acquired_event_t &event)
{
    if (!_is_observer || event.room_id != _observed_room_id) {
        co_return;
    }
    for (auto &[_, actor] : observers) {
        const auto notify = bingo_reward_announced_notify_t{
          event.room_id, event.actor_id, event.draw_seq, event.item_id,
          event.item_name, event.rarity};
        actor->push (notify);
    }
    // The observer event has been submitted to every current participant.
    // This turn can be used as an application-signaled relocation boundary.
    _context->relocation_ready ().defer ();
    co_return;
}

} // namespace zlink::samples::bingo
