/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Actors/player_actor.hpp"
#include "../../../../../Configuration/sample_names.hpp"
#include "../../../../Domain/Bingo/bingo_room_game.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class bingo_room_spot_t;

struct bingo_room_relocation_state_t
{
    bingo_room_state_t game;
    bool is_observer = false;
    std::string observed_room_id;
    bool cleanup_started = false;
};

inline void to_json (
  nlohmann::json &json,
  const bingo_room_relocation_state_t &value)
{
    json = {{"game", value.game},
            {"isObserver", value.is_observer},
            {"observedRoomId", value.observed_room_id},
            {"cleanupStarted", value.cleanup_started}};
}

inline void from_json (
  const nlohmann::json &json,
  bingo_room_relocation_state_t &value)
{
    value.game = json.value ("game", bingo_room_state_t{});
    value.is_observer = json.value ("isObserver", false);
    value.observed_room_id =
      json.value ("observedRoomId", std::string{});
    value.cleanup_started = json.value ("cleanupStarted", false);
}

class bingo_room_draw_timer_handler_t
{
  public:
    task_t<void> handle (bingo_room_spot_t &spot, const timer_tick_t &tick) const;
};

class bingo_room_spot_t : public spot_t<player_actor_t>
{
  public:
    bingo_room_spot_t () = default;

    explicit bingo_room_spot_t (std::string room_id) : _game (std::move (room_id)) {}

    explicit bingo_room_spot_t (spot_context_t context) :
        _context (std::move (context))
    {
    }

    spot_context_t &context () noexcept override { return *_context; }
    const spot_context_t &context () const noexcept override { return *_context; }

    void configure () override
    {
        _context->handlers ().add_actor_request<&bingo_room_spot_t::submit_card> ();
        _context->handlers ().add_actor_request<&bingo_room_spot_t::observe_events> ();
        _context->handlers ().add_actor_request<&bingo_room_spot_t::stop_observing_events> ();
        _context->handlers ().add_subscribe<&bingo_room_spot_t::on_reward_acquired> (
          sample_names_t::reward_topic);
    }

    task_t<spot_create_response_t>
    on_create (const message_t &request) override
    {
        if (!request.empty ()) {
            auto settings = request.decode<bingo_room_settings_payload_t> ();
            _is_observer = settings.purpose == "Observer";
        _observed_room_id = settings.observed_room_id.value_or ("");
        }
        co_return spot_create_response_t::accept ();
    }

    task_t<void> on_initialize () override
    {
        using namespace std::chrono_literals;
        _draw_timer =
          _context->add_timer<bingo_room_draw_timer_handler_t> ("bingo-draw", 200ms);
        co_return;
    }

    task_t<void> on_closing (
      const spot_closing_context_t &,
      std::stop_token) override
    {
        _draw_timer.cancel ();
        co_return;
    }

    task_t<void> on_relocation_ready_completed (
      const spot_relocation_ready_completion_t &completion) override
    {
        std::cout << "bingo room relocation-ready completed room="
                  << _context->spot_id ()
                  << " outcome="
                  << (completion.outcome
                            == spot_relocation_ready_outcome_t::relocated
                          ? "relocated"
                          : "continued")
                  << '\n';
        co_return;
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const message_t &request_message) override
    {
        auto request = request_message.decode<bingo_room_join_req_t> ();
        const auto joined_actor_id =
          actor_id.empty () ? request.actor_id : std::string (actor_id);
        if (request.observe_only) {
            if (!_is_observer || request.room_id != _observed_room_id) {
                throw std::runtime_error ("observe-only actor can join only its observer room");
            }
            _pending_joins[joined_actor_id] = request;
            co_return spot_actor_join_result_t::accept (bingo_room_join_res_t{
              bingo_room_state_t{request.room_id, bingo_room_status_t::running}});
        }
        if (_is_observer) {
            throw std::runtime_error ("player actor cannot join an observer room");
        }
        auto projected = _game;
        projected.set_room_id_if_empty (request.room_id);
        projected.join (joined_actor_id, request.display_name);
        _pending_joins[joined_actor_id] = request;
        co_return spot_actor_join_result_t::accept (
          bingo_room_join_res_t{projected.snapshot ()});
    }

    observe_bingo_events_res_t observe_events (
      const player_actor_t &actor,
      const message_context_t &context,
      const observe_bingo_events_req_t &request);

    task_t<stop_observing_bingo_events_res_t> stop_observing_events (
      const player_actor_t &actor,
      const message_context_t &context,
      const stop_observing_bingo_events_req_t &request);

    task_t<submit_bingo_card_res_t> submit_card (
      const player_actor_t &actor,
      const message_context_t &context,
      const submit_bingo_card_req_t &request);

    task_t<void> on_actor_joined (player_actor_t &actor) override
    {
        const auto pending = _pending_joins.find (actor.actor_id);
        if (pending == _pending_joins.end ()) {
            throw std::runtime_error ("accepted bingo actor admission is missing");
        }
        const auto request = pending->second;
        _game.set_room_id_if_empty (request.room_id);
        if (request.observe_only) {
            _pending_joins.erase (pending);
            observers[actor.actor_id] = &actor;
            // Observer membership is complete, so this turn is a safe
            // application-signaled relocation boundary.
            _context->relocation_ready ().defer ();
        } else {
            get_player_record_res_t record;
            try {
                record = co_await _context->outbound ()
                           .request (sample_names_t::api_channel,
                                     get_player_record_req_t{actor.actor_id})
                           .yield<get_player_record_res_t> ();
            }
            catch (...) {
                _pending_joins.erase (actor.actor_id);
                throw;
            }
            const auto resumed = _pending_joins.find (actor.actor_id);
            if (resumed == _pending_joins.end () || resumed->second.room_id != request.room_id
                || !_game.can_accept_player ()) {
                _pending_joins.erase (actor.actor_id);
                (void) co_await _context->leave_actor (
                  actor_ref_for (actor), actor);
                co_return;
            }
            _pending_joins.erase (resumed);
            const auto display_name = actor.display_name.empty () ? request.display_name
                                                                  : actor.display_name;
            actors[actor.actor_id] = &actor;
            const auto joined = _game.join (actor.actor_id, display_name,
                                            record.wins, record.losses);
            send_to_players (joined.player_joined, actor.actor_id);
            if (joined.game_started) {
                send_to_players (*joined.game_started, actor.actor_id);
                actor.push (*joined.game_started);
            }
            std::cout << "bingo player record loaded actor=" << record.actor_id
                      << " wins=" << record.wins << " losses=" << record.losses << '\n';
        }
        co_return;
    }

    task_t<void> on_leave_actor (player_actor_t &actor) override
    {
        const auto player = actors.find (actor.actor_id);
        if (player != actors.end ()) {
            const auto final_state = _game.snapshot ();
            const auto won = std::find (final_state.winners.begin (), final_state.winners.end (),
                                        actor.actor_id)
                             != final_state.winners.end ();
            auto report_pending =
              _context->outbound ()
                .request (sample_names_t::api_channel,
                          report_bingo_result_req_t{final_state.room_id,
                                                    actor.actor_id,
                                                    won,
                                                    final_state.draw_seq})
                .yield<report_bingo_result_res_t> ();
            const auto record = co_await report_pending;
            std::cout << "bingo result reported room=" << final_state.room_id
                      << " actor=" << record.actor_id << " won=" << (won ? "true" : "false")
                      << " wins=" << record.wins << " losses=" << record.losses << '\n';
        }
        actors.erase (actor.actor_id);
        observers.erase (actor.actor_id);
        _game.leave (actor.actor_id);
        if (actors.empty () && observers.empty ()) {
            (void) co_await _context->close ();
        }
        co_return;
    }

    task_t<void> on_disconnect_actor (const player_actor_t &actor)
    {
        actor.mark_disconnected ();
        co_return;
    }

    const bingo_room_state_t &snapshot () const noexcept { return _game.snapshot (); }

    bingo_room_relocation_state_t relocation_state () const
    {
        return {_game.snapshot (), _is_observer, _observed_room_id,
                cleanup_started};
    }

    void restore_relocation_state (
      bingo_room_relocation_state_t state)
    {
        _game.restore (std::move (state.game));
        _is_observer = state.is_observer;
        _observed_room_id = std::move (state.observed_room_id);
        cleanup_started = state.cleanup_started;
    }

    void record_observer_returned_to_entry_spot (const player_actor_t &actor) const
    {
        std::cout << "observer returned to entry spot actor=" << actor.actor_id << '\n';
    }

  private:
    friend class bingo_room_draw_timer_handler_t;

    task_t<void> handle_draw_tick (const timer_tick_t &);

    void publish_reward (const number_drawn_notify_t &drawn)
    {
        if (drawn.state.winners.empty ()) {
            return;
        }
        const auto reward_event = bingo_reward_acquired_event_t{
            drawn.state.room_id,
            drawn.state.winners.front (),
            drawn.state.draw_seq,
            bingo_reward_items_t::golden_dauber_id,
            bingo_reward_items_t::golden_dauber_name,
            bingo_reward_items_t::legendary_rarity
        };
        _context->publish (sample_names_t::reward_topic, reward_event).submit ();
    }

    template <typename TNotify>
    void send_to_players (const TNotify &notify, const std::string &excluded_actor_id = {})
    {
        for (auto &[actor_id, actor] : actors) {
            if (!excluded_actor_id.empty () && actor_id == excluded_actor_id) {
                continue;
            }
            actor->push (notify);
        }
    }

    task_t<void> on_reward_acquired (const bingo_reward_acquired_event_t &event);

    task_t<void> leave_finished_actors ()
    {
        if (cleanup_started || snapshot ().winners.empty ()) {
            co_return;
        }
        cleanup_started = true;
        std::vector<player_actor_t *> leaving;
        for (auto &[_, actor] : actors) {
            leaving.push_back (actor);
        }
        for (auto *actor : leaving) {
            actor->mark_for_destroy_after_room_leave ();
            const auto before = actor_ref_for (*actor);
            (void) co_await _context->leave_actor (before, *actor);
        }
        co_return;
    }

    static actor_ref_t actor_ref_for (const player_actor_t &actor)
    {
        return actor.context ().actor_ref ();
    }

    std::optional<spot_context_t> _context;
    bingo_room_game_t _game;
    std::map<std::string, player_actor_t *> actors;
    std::map<std::string, player_actor_t *> observers;
    std::map<std::string, bingo_room_join_req_t> _pending_joins;
    bool _is_observer = false;
    std::string _observed_room_id;
    bool cleanup_started = false;
    framework::timer_t _draw_timer;
};

} // namespace zlink::samples::bingo

#include "Handlers/bingo_reward_acquired_event_handler.hpp"
#include "Handlers/bingo_room_draw_timer_handler.hpp"
#include "Handlers/observe_bingo_events_handler.hpp"
#include "Handlers/stop_observing_bingo_events_handler.hpp"
#include "Handlers/submit_bingo_card_handler.hpp"
