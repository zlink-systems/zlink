/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_entry_spot.hpp"

namespace zlink::samples::tictactoe
{

// --8<-- [start:doc-ttt-milestone-handler]
inline void
tictactoe_entry_spot_t::on_player_win_milestone (const player_win_milestone_event_t &event)
{
    // --8<-- [start:doc-ttt-milestone-notify]
    for (auto &[_, actor] : observers) {
        const auto notify = win_milestone_notify_t{
          event.room_id, event.actor_id, event.display_name, event.wins};
        (void) actor->push (notify);
    }
    // --8<-- [end:doc-ttt-milestone-notify]
}
// --8<-- [end:doc-ttt-milestone-handler]

} // namespace zlink::samples::tictactoe
