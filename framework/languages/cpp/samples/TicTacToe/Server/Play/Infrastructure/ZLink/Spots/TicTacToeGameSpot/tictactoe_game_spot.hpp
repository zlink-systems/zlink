/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Actors/player_actor.hpp"
#include "../../../../../Configuration/sample_names.hpp"
#include "Notifications/game_notification_publisher.hpp"
#include "../../../../Domain/TicTacToe/tictactoe_match.hpp"

#include <zlink/framework.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace zlink::samples::tictactoe
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class tictactoe_game_spot_t;

// --8<-- [start:doc-timer-handler]
// C++ timer만 별도 handler 타입이다. handle이 대상 Spot과 tick 둘을 받는다.
class tictactoe_game_timer_handler_t
{
  public:
    task_t<void> handle (tictactoe_game_spot_t &spot, const timer_tick_t &tick) const;
};
// --8<-- [end:doc-timer-handler]

class tictactoe_game_spot_t : public spot_t<player_actor_t>
{
  private:
    std::map<std::string, player_actor_t *> actors;
    game_notification_publisher_t publisher{actors};

  public:
    explicit tictactoe_game_spot_t (spot_context_t context) :
        _context (std::move (context))
    {
    }

    spot_context_t &context () noexcept override { return _context; }
    const spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&tictactoe_game_spot_t::place_mark> ();
        _context.handlers ().add_actor_send<&tictactoe_game_spot_t::leave_game> ();
    }

    task_t<spot_create_response_t> on_create (const message_t &) override
    {
        auto room_id = _context.spot_id ();
        if (const auto separator = room_id.rfind (':');
            separator != std::string::npos && separator + 1 < room_id.size ()) {
            room_id = room_id.substr (separator + 1);
        }
        _match.emplace (room_id);
        co_return spot_create_response_t::accept ();
    }

    task_t<void> on_initialize () override
    {
        using namespace std::chrono_literals;
        _game_timer =
          _context.add_timer<tictactoe_game_timer_handler_t> ("game-tick", 1s);
        co_return;
    }

    task_t<void> on_closing (
      const spot_closing_context_t &,
      std::stop_token) override
    {
        _game_timer.cancel ();
        co_return;
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const message_t &request_message) override
    {
        auto request = request_message.decode<tictactoe_game_join_req_t> ();
        if (request.player.actor_id.empty ()
            || request.player.level < sample_names_t::required_level) {
            co_return spot_actor_join_result_t::reject ();
        }
        auto response = match ().evaluate_join (std::string (actor_id), request.room_id);
        _pending_joins[std::string (actor_id)] = request;
        co_return spot_actor_join_result_t::accept (
          std::move (response));
    }

    task_t<place_mark_res_t> place_mark (const player_actor_t &actor,
                                         const message_context_t &context,
                                         const place_mark_req_t &request);

    void leave_game (const player_actor_t &actor,
                     const message_context_t &,
                     const leave_game_msg_t &request);

    task_t<void> on_actor_joined (player_actor_t &actor) override
    {
        const auto pending = _pending_joins.find (actor.actor_id);
        if (pending == _pending_joins.end ()) {
            throw std::runtime_error ("accepted tic-tac-toe actor admission is missing");
        }
        const auto request = pending->second;
        _pending_joins.erase (pending);
        players[actor.actor_id] = request.player;
        actor.apply_player (request.player);
        (void) match ().join (actor.actor_id, request.room_id);
        actors[actor.actor_id] = &actor;
        const auto &state = match ().snapshot ();
        player_joined_notify_t notify{
            state.room_id,
            actor.actor_id,
            players[actor.actor_id].display_name,
            players[actor.actor_id].level,
            actor.actor_id == state.x_actor_id ? tictactoe_marks_t::x : tictactoe_marks_t::o,
            state
        };
        co_await publisher.publish (notify, actor.actor_id);

        game_state_notify_t state_notify{state.room_id, state.next_turn, state};
        co_await publisher.publish (state_notify, actor.actor_id);
        co_return;
    }

    task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        actors.erase (actor.actor_id);
        players.erase (actor.actor_id);
        co_return;
    }

    task_t<void> on_disconnect_actor (player_actor_t &actor) override
    {
        actor.mark_disconnected ();
        co_return;
    }

  private:
    friend class tictactoe_game_timer_handler_t;

    task_t<void> handle_game_tick (const timer_tick_t &);

    tictactoe_match_t &match ()
    {
        if (!_match) {
            throw std::runtime_error ("tic-tac-toe match is not initialized");
        }
        return *_match;
    }

    const tictactoe_match_t &match () const
    {
        if (!_match) {
            throw std::runtime_error ("tic-tac-toe match is not initialized");
        }
        return *_match;
    }

    task_t<void>
    publish_win_milestone (const player_actor_t &actor,
                           const tictactoe_state_t &state)
    {
        if (state.status != tictactoe_status_t::won || state.winner != actor.actor_id) {
            co_return;
        }
        auto player = players[actor.actor_id];
        player.wins += 1;
        players[actor.actor_id] = player;
        actor.apply_player (player);
        if (player.wins == 100) {
            const auto milestone_event = player_win_milestone_event_t{
              state.room_id, player.actor_id, player.display_name, player.wins};
            co_await _context
              .publish (sample_names_t::player_milestone_topic,
                        milestone_event)
              .submit ();
        }
        co_return;
    }

    spot_context_t _context;
    std::optional<tictactoe_match_t> _match;
    std::map<std::string, player_info_t> players;
    std::map<std::string, tictactoe_game_join_req_t> _pending_joins;
    framework::timer_t _game_timer;
};

} // namespace zlink::samples::tictactoe

#include "Handlers/play_actor_leave_game_handler.hpp"
#include "Handlers/play_actor_place_mark_handler.hpp"
#include "Handlers/tictactoe_game_timer_handler.hpp"
