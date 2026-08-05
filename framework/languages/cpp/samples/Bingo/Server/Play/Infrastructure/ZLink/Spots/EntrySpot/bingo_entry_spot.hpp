/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Actors/player_actor.hpp"
#include "../../../../../Configuration/sample_names.hpp"
#include "../../../../../Configuration/sample_topology.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class bingo_entry_spot_t : public entry_spot_t<player_actor_t>
{
  public:
    bingo_entry_spot_t (entry_spot_context_t context, sample_topology_t topology) :
        _context (std::move (context)), _topology (std::move (topology))
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&bingo_entry_spot_t::match_bingo> ();
        _context.handlers ().add_actor_request<&bingo_entry_spot_t::observe_bingo_events> ();
    }

    task_t<observe_bingo_events_res_t>
    observe_bingo_events (player_actor_t &actor,
                          message_context_t &context,
                          const observe_bingo_events_req_t &request);

    task_t<match_bingo_res_t> match_bingo (player_actor_t &actor,
                                           message_context_t &context,
                                           const match_bingo_req_t &request);

    task_t<actor_create_response_t>
    on_create_actor (
      player_actor_t &actor,
      const message_t &create_request) override
    {
        const auto request = create_request.decode<ensure_player_actor_req_t> ();
        actor.display_name =
          request.display_name.empty () ? request.actor_id : request.display_name;
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
        joined_actor_ids.push_back (actor.actor_id);
        if (!actor.destroy_after_entry_spot_join) {
            co_return;
        }
        const auto actor_id = actor.actor_id;
        std::cout << "entry spot: actor destroy requested. actor=" << actor_id << std::endl;
        co_await _context.destroy_actor (actor);
        std::cout << "entry spot: actor destroy completed. actor=" << actor_id << std::endl;
    }

    task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        joined_actor_ids.erase (
          std::remove (joined_actor_ids.begin (), joined_actor_ids.end (), actor.actor_id),
          joined_actor_ids.end ());
        co_return;
    }

    task_t<void> on_disconnect_actor (player_actor_t &actor) override
    {
        actor.mark_disconnected ();
        co_return;
    }

    std::vector<std::string> created_actor_ids;
    std::vector<std::string> joined_actor_ids;

  private:
    static spot_id_t observer_room_id (const std::string &room_id,
                                       const std::string &actor_id)
    {
        return "observe:" + room_id + ":" + actor_id;
    }

    static actor_ref_t actor_ref_for (const player_actor_t &actor)
    {
        return actor.context ().actor_ref ();
    }

    entry_spot_context_t _context;
    sample_topology_t _topology;
};

} // namespace zlink::samples::bingo

#include "Handlers/match_bingo_actor_handler.hpp"
#include "Handlers/observe_bingo_events_handler.hpp"
