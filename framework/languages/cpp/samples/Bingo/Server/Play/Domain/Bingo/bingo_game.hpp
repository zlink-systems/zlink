/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "bingo_card.hpp"
#include "../../../../Shared/Contracts/messages.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::samples::bingo
{

class bingo_game_t
{
  public:
    explicit bingo_game_t (int next_draw = 1) :
        _next_draw (std::max (1, next_draw))
    {
    }

    void submit_card (std::vector<bingo_player_state_t> &players,
                      const std::string &actor_id,
                      const std::vector<int> &numbers)
    {
        auto &player = find_player (players, actor_id);
        if (!player.card.empty ()) {
            throw std::runtime_error ("bingo card has already been submitted");
        }
        const bingo_card_t card (numbers);
        player.card.assign (card.numbers ().begin (), card.numbers ().end ());
        player.marks.assign (card.marks ().begin (), card.marks ().end ());
        player.completed_lines = card.completed_lines ();
    }

    bool all_cards_submitted (const std::vector<bingo_player_state_t> &players) const noexcept
    {
        return players.size () == 2
               && std::all_of (players.begin (), players.end (), [] (const auto &player) {
                      return player.card.size () == bingo_card_t::cell_count;
                  });
    }

    std::optional<number_drawn_notify_t> draw_next (bingo_room_state_t &state)
    {
        if (state.status != bingo_room_status_t::running || _next_draw > 15
            || !state.winners.empty ()) {
            return std::nullopt;
        }

        const int number = _next_draw++;
        state.drawn_numbers.push_back (number);
        state.last_drawn_number = number;
        state.draw_seq += 1;

        for (auto &player : state.players) {
            bingo_card_t card (player.card);
            for (const auto drawn : state.drawn_numbers) {
                card.mark (drawn);
            }
            player.marks.assign (card.marks ().begin (), card.marks ().end ());
            player.completed_lines = card.completed_lines ();
            if (player.completed_lines > 0
                && std::find (state.winners.begin (), state.winners.end (), player.actor_id)
                     == state.winners.end ()) {
                state.winners.push_back (player.actor_id);
            }
        }

        if (!state.winners.empty () || _next_draw > 15) {
            state.status = bingo_room_status_t::finished;
        }
        return number_drawn_notify_t{state.room_id, state.draw_seq, number, state};
    }

  private:
    static bingo_player_state_t &find_player (std::vector<bingo_player_state_t> &players,
                                              const std::string &actor_id)
    {
        auto player = std::find_if (players.begin (), players.end (), [&] (const auto &entry) {
            return entry.actor_id == actor_id;
        });
        if (player == players.end ()) {
            throw std::runtime_error ("bingo player is not in the room");
        }
        return *player;
    }

    int _next_draw = 1;
};

} // namespace zlink::samples::bingo
