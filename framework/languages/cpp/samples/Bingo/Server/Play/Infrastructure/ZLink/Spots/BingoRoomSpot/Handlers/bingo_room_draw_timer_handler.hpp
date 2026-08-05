/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_room_spot.hpp"

namespace zlink::samples::bingo
{

inline task_t<void> bingo_room_spot_t::handle_draw_tick (const timer_tick_t &)
{
    if (!_game.should_draw ()) {
        co_return;
    }
    const auto drawn = _game.draw_next ();
    if (!drawn) {
        co_return;
    }
    send_to_players (*drawn);
    if (drawn->state.status == bingo_room_status_t::finished) {
        send_to_players (game_ended_notify_t{drawn->state});
        publish_reward (*drawn);
        _draw_timer.cancel ();
        co_await leave_finished_actors ();
        // All completed-round work has been submitted. Framework may relocate
        // the room after this serial turn finishes.
        _context->relocation_ready ().defer ();
    }
}

inline task_t<void>
bingo_room_draw_timer_handler_t::handle (bingo_room_spot_t &spot,
                                         const timer_tick_t &tick) const
{
    co_await spot.handle_draw_tick (tick);
}

} // namespace zlink::samples::bingo
