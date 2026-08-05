/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Actors/player_actor.hpp"
#include "../../../../../Configuration/sample_names.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace zlink::samples::tictactoe
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

// --8<-- [start:doc-entry-spot]
class tictactoe_entry_spot_t : public entry_spot_t<player_actor_t>
{
  public:
    explicit tictactoe_entry_spot_t (entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_send<&tictactoe_entry_spot_t::join_game> ();
        _context.handlers ().add_actor_request<&tictactoe_entry_spot_t::observe_milestone> ();
        _context.handlers ().add_subscribe<&tictactoe_entry_spot_t::on_player_win_milestone> (
          sample_names_t::player_milestone_topic);
    }

    task_t<void> join_game (player_actor_t &actor,
                            message_context_t &,
                            const join_game_req_t &request);

    observe_milestone_res_t observe_milestone (const player_actor_t &actor,
                                               message_context_t &,
                                               const observe_milestone_req_t &);

    /* 공통 sample spec §13: 인증에서 받은 PlayerInfo가 actor 생성 payload로 들어오고,
     * actor는 그 값(display name/level/wins)을 그대로 보관한다. */
    task_t<actor_create_response_t>
    on_create_actor (
      player_actor_t &actor,
      const message_t &create_request) override
    {
        actor.apply_player (create_request.decode<player_info_t> ());
        created_actor_ids.push_back (actor.actor_id);
        co_return actor_create_response_t::accept ();
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const message_t &) override
    {
        co_return spot_actor_join_result_t::accept ();
    }

    task_t<void> on_actor_joined (player_actor_t &actor) override
    {
        actor_ids.push_back (actor.actor_id);
        if (!actor.destroy_after_entry_spot_join) {
            co_return;
        }
        std::cout << "entry spot: actor destroy requested. actor=" << actor.actor_id << std::endl;
        co_await _context.destroy_actor (actor);
        std::cout << "entry spot: actor destroy completed. actor=" << actor.actor_id << std::endl;
    }

    task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        actor_ids.erase (std::remove (actor_ids.begin (), actor_ids.end (), actor.actor_id),
                         actor_ids.end ());
        observers.erase (actor.actor_id);
        co_return;
    }

    task_t<void> on_disconnect_actor (player_actor_t &actor) override
    {
        actor.mark_disconnected ();
        observers.erase (actor.actor_id);
        co_return;
    }

    std::vector<std::string> created_actor_ids;
    std::vector<std::string> actor_ids;

  private:
    void on_player_win_milestone (const player_win_milestone_event_t &event);

    entry_spot_context_t _context;
    std::map<std::string, player_actor_t *> observers;
};
// --8<-- [end:doc-entry-spot]

} // namespace zlink::samples::tictactoe

#include "Handlers/play_actor_join_game_handler.hpp"
#include "Handlers/play_actor_observe_milestone_handler.hpp"
#include "Handlers/player_win_milestone_event_handler.hpp"
