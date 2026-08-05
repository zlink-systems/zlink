/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../tictactoe_entry_spot.hpp"

namespace zlink::samples::tictactoe
{

inline observe_milestone_res_t
tictactoe_entry_spot_t::observe_milestone (const player_actor_t &actor,
                                           message_context_t &,
                                           const observe_milestone_req_t &)
{
    observers[actor.actor_id] = const_cast<player_actor_t *> (&actor);
    return {true};
}

} // namespace zlink::samples::tictactoe
