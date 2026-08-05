/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_game_spot.hpp"

namespace zlink::samples::tictactoe
{

inline task_t<void> tictactoe_game_spot_t::handle_game_tick (const timer_tick_t &)
{
    if (!match ().tick ()) {
        co_return;
    }
    const auto &state = match ().snapshot ();
    const auto notify = game_state_notify_t{state.room_id, state.next_turn, state};
    co_await publisher.publish (notify);
    co_return;
}

inline task_t<void>
tictactoe_game_timer_handler_t::handle (tictactoe_game_spot_t &spot,
                                        const timer_tick_t &tick) const
{
    co_await spot.handle_game_tick (tick);
}

} // namespace zlink::samples::tictactoe
