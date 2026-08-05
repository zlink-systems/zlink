/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "bingo_game.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::samples::bingo
{

struct bingo_room_join_result_t
{
    player_joined_notify_t player_joined;
    std::optional<game_started_notify_t> game_started;
};

class bingo_room_game_t
{
  public:
    bingo_room_game_t () = default;

    explicit bingo_room_game_t (std::string room_id) : _state{std::move (room_id)} {}

    explicit bingo_room_game_t (bingo_room_state_t state) :
        _state (std::move (state)),
        _game (_state.last_drawn_number.value_or (0) + 1)
    {
    }

    void restore (bingo_room_state_t state)
    {
        _state = std::move (state);
        _game = bingo_game_t (_state.last_drawn_number.value_or (0) + 1);
    }

    bingo_room_join_result_t join (std::string actor_id,
                                   std::string display_name,
                                   int wins = 0,
                                   int losses = 0)
    {
        if (_state.players.size () >= 2) {
            throw std::runtime_error ("bingo room is full");
        }
        if (find_player (actor_id) != _state.players.end ()) {
            return {{_state.room_id, actor_id, display_name, 0, false, _state}, std::nullopt};
        }

        const bool is_host = _state.players.empty ();
        const int seat = static_cast<int> (_state.players.size ()) + 1;
        if (is_host) {
            _state.host_actor_id = actor_id;
        }
        _state.players.push_back (
          {actor_id, display_name, seat, is_host, {}, {}, 0, wins, losses});
        _state.can_start = _state.players.size () == 2;
        if (_state.can_start) {
            _state.status = bingo_room_status_t::running;
        }
        auto joined = player_joined_notify_t{_state.room_id, std::move (actor_id),
                                             std::move (display_name), seat, is_host, _state};
        auto started = _state.can_start
                         ? std::optional<game_started_notify_t> (game_started_notify_t{_state})
                         : std::nullopt;
        return {std::move (joined), std::move (started)};
    }

    submit_bingo_card_res_t submit_card (const std::string &actor_id,
                                         const std::vector<int> &numbers)
    {
        if (_state.status != bingo_room_status_t::running) {
            throw std::runtime_error ("bingo room is not running");
        }
        _game.submit_card (_state.players, actor_id, numbers);
        return {_state};
    }

    bool should_draw () const noexcept { return _game.all_cards_submitted (_state.players); }

    bool can_accept_player () const noexcept { return _state.players.size () < 2; }

    void set_room_id_if_empty (std::string room_id)
    {
        if (_state.room_id.empty ()) {
            _state.room_id = std::move (room_id);
        }
    }

    std::optional<number_drawn_notify_t> draw_next () { return _game.draw_next (_state); }

    bingo_room_state_t leave (const std::string &actor_id)
    {
        _state.players.erase (std::remove_if (_state.players.begin (), _state.players.end (),
                                              [&] (const auto &player) {
                                                  return player.actor_id == actor_id;
                                              }),
                              _state.players.end ());
        _state.can_start = _state.players.size () == 2;
        return _state;
    }

    const bingo_room_state_t &snapshot () const noexcept { return _state; }

  private:
    std::vector<bingo_player_state_t>::iterator find_player (const std::string &actor_id)
    {
        return std::find_if (_state.players.begin (), _state.players.end (), [&] (const auto &p) {
            return p.actor_id == actor_id;
        });
    }

    bingo_room_state_t _state;
    bingo_game_t _game;
};

} // namespace zlink::samples::bingo
