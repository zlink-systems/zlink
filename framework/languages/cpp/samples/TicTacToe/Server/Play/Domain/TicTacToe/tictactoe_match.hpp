/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../../Shared/Contracts/messages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::samples::tictactoe
{

class tictactoe_match_t
{
  public:
    explicit tictactoe_match_t (
      std::string room_id,
      std::chrono::steady_clock::duration turn_timeout = std::chrono::seconds (15)) :
        _state{.room_id = std::move (room_id)}, _turn_timeout (turn_timeout)
    {
    }

    tictactoe_game_join_res_t
    evaluate_join (const std::string &actor_id, const std::string &room_id) const
    {
        if (room_id != _state.room_id) {
            throw std::runtime_error ("room id mismatch");
        }
        if (actor_id.empty ()) {
            throw std::runtime_error ("actor id must not be empty");
        }
        auto projected = _state;
        if (!projected.x_actor_id) {
            projected.x_actor_id = actor_id;
            projected.next_turn = tictactoe_marks_t::x;
            projected.status = tictactoe_status_t::waiting_for_players;
            return {std::move (projected)};
        }
        if (actor_id == projected.x_actor_id) {
            return {std::move (projected)};
        }
        if (!projected.o_actor_id) {
            projected.o_actor_id = actor_id;
            projected.status = tictactoe_status_t::in_progress;
            return {std::move (projected)};
        }
        if (actor_id == projected.o_actor_id) {
            return {std::move (projected)};
        }
        throw std::runtime_error ("match already has two players");
    }

    tictactoe_game_join_res_t join (const std::string &actor_id, const std::string &room_id)
    {
        const auto previous_status = _state.status;
        auto response = evaluate_join (actor_id, room_id);
        _state = response.state;
        if (previous_status != tictactoe_status_t::in_progress
            && _state.status == tictactoe_status_t::in_progress) {
            reset_turn_deadline ();
        }
        return response;
    }

    tictactoe_state_t place (const std::string &actor_id, const place_mark_req_t &request)
    {
        if (_state.status != tictactoe_status_t::in_progress) {
            throw std::runtime_error ("match is not playing");
        }
        const auto &next_actor_id =
          _state.next_turn == tictactoe_marks_t::x ? _state.x_actor_id : _state.o_actor_id;
        if (!next_actor_id || actor_id != *next_actor_id) {
            throw std::runtime_error ("not actor turn");
        }
        if (request.cell < 0 || request.cell >= 9
            || _state.board[static_cast<std::size_t> (request.cell)] != '.') {
            throw std::runtime_error ("invalid move");
        }

        const char mark = actor_id == _state.x_actor_id ? tictactoe_marks_t::x[0]
                                                        : tictactoe_marks_t::o[0];
        _state.board[static_cast<std::size_t> (request.cell)] = mark;
        _state.last_move_cell = request.cell;
        _state.last_move_actor_id = actor_id;
        if (has_winner (mark)) {
            _state.status = tictactoe_status_t::won;
            _state.winner = actor_id;
        } else if (_state.board.find ('.') == std::string::npos) {
            _state.status = tictactoe_status_t::draw;
        } else {
            _state.next_turn = actor_id == _state.x_actor_id ? tictactoe_marks_t::o
                                                             : tictactoe_marks_t::x;
            reset_turn_deadline ();
        }
        return _state;
    }

    bool tick ()
    {
        if (_state.status != tictactoe_status_t::in_progress || !_turn_deadline
            || std::chrono::steady_clock::now () < *_turn_deadline) {
            return false;
        }
        const auto &timed_out_actor = _state.next_turn == tictactoe_marks_t::x
                                        ? _state.x_actor_id
                                        : _state.o_actor_id;
        if (!timed_out_actor) {
            throw std::logic_error ("active turn is missing its player actor");
        }
        _state.status = tictactoe_status_t::turn_timed_out;
        _state.winner = timed_out_actor == _state.x_actor_id ? _state.o_actor_id
                                                              : _state.x_actor_id;
        _state.next_turn.clear ();
        _state.last_move_actor_id = *timed_out_actor;
        _state.last_move_cell.reset ();
        _turn_deadline.reset ();
        return true;
    }

    void ensure_can_leave (const std::string &actor_id) const
    {
        if (_state.status != tictactoe_status_t::won
            && _state.status != tictactoe_status_t::draw
            && _state.status != tictactoe_status_t::turn_timed_out) {
            throw std::runtime_error ("cannot leave before the game reaches a final state");
        }
        if (actor_id != _state.x_actor_id && actor_id != _state.o_actor_id) {
            throw std::runtime_error ("only a game participant can leave the room");
        }
    }

    const tictactoe_state_t &snapshot () const noexcept { return _state; }

  private:
    void reset_turn_deadline ()
    {
        _turn_deadline = std::chrono::steady_clock::now () + _turn_timeout;
    }

    bool has_winner (char mark) const
    {
        static constexpr std::array<std::array<int, 3>, 8> lines{{
          {{0, 1, 2}},
          {{3, 4, 5}},
          {{6, 7, 8}},
          {{0, 3, 6}},
          {{1, 4, 7}},
          {{2, 5, 8}},
          {{0, 4, 8}},
          {{2, 4, 6}},
        }};
        return std::any_of (lines.begin (), lines.end (), [&] (const auto &line) {
            return _state.board[static_cast<std::size_t> (line[0])] == mark
                   && _state.board[static_cast<std::size_t> (line[1])] == mark
                   && _state.board[static_cast<std::size_t> (line[2])] == mark;
        });
    }

    tictactoe_state_t _state;
    std::chrono::steady_clock::duration _turn_timeout;
    std::optional<std::chrono::steady_clock::time_point> _turn_deadline;
};

} // namespace zlink::samples::tictactoe
