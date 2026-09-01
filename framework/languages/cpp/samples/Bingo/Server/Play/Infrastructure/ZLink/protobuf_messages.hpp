/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../../Shared/Contracts/messages.hpp"
#include "../../Domain/Bingo/bingo_room_game.hpp"

namespace zlink::samples::bingo
{

inline void write_message (const bingo_player_state_t &source, pb::BingoPlayerState &target)
{
    target.set_actor_id (source.actor_id);
    target.set_display_name (source.display_name);
    target.set_seat (source.seat);
    target.set_is_host (source.is_host);
    target.mutable_card ()->Assign (source.card.begin (), source.card.end ());
    target.mutable_marks ()->Assign (source.marks.begin (), source.marks.end ());
    target.set_completed_lines (source.completed_lines);
    target.set_wins (source.wins);
    target.set_losses (source.losses);
}

inline bingo_player_state_t read_message (const pb::BingoPlayerState &source)
{
    bingo_player_state_t target;
    target.actor_id = source.actor_id ();
    target.display_name = source.display_name ();
    target.seat = source.seat ();
    target.is_host = source.is_host ();
    target.card.assign (source.card ().begin (), source.card ().end ());
    target.marks.assign (source.marks ().begin (), source.marks ().end ());
    target.completed_lines = source.completed_lines ();
    target.wins = source.wins ();
    target.losses = source.losses ();
    return target;
}

inline void write_message (const bingo_room_state_t &source, pb::BingoRoomState &target)
{
    target.set_room_id (source.room_id);
    target.set_status (source.status);
    target.set_host_actor_id (source.host_actor_id);
    target.set_can_start (source.can_start);
    target.set_draw_seq (source.draw_seq);
    if (source.last_drawn_number) {
        target.set_last_drawn_number (*source.last_drawn_number);
    } else {
        target.clear_last_drawn_number ();
    }
    target.mutable_drawn_numbers ()->Assign (source.drawn_numbers.begin (),
                                             source.drawn_numbers.end ());
    target.clear_players ();
    for (const auto &player : source.players) {
        write_message (player, *target.add_players ());
    }
    target.mutable_winners ()->Assign (source.winners.begin (), source.winners.end ());
}

inline bingo_room_state_t read_message (const pb::BingoRoomState &source)
{
    bingo_room_state_t target;
    target.room_id = source.room_id ();
    target.status = source.status ();
    target.host_actor_id = source.host_actor_id ();
    target.can_start = source.can_start ();
    target.draw_seq = source.draw_seq ();
    target.last_drawn_number = source.has_last_drawn_number ()
                                 ? std::optional<int> (source.last_drawn_number ())
                                 : std::nullopt;
    target.drawn_numbers.assign (source.drawn_numbers ().begin (), source.drawn_numbers ().end ());
    target.players.reserve (source.players_size ());
    for (const auto &player : source.players ()) {
        target.players.push_back (read_message (player));
    }
    target.winners.assign (source.winners ().begin (), source.winners ().end ());
    return target;
}

inline player_joined_notify_t make_message (const bingo_player_joined_t &source)
{
    player_joined_notify_t target;
    target.set_room_id (source.room_id);
    target.set_actor_id (source.actor_id);
    target.set_display_name (source.display_name);
    target.set_seat (source.seat);
    target.set_is_host (source.is_host);
    write_message (source.state, *target.mutable_state ());
    return target;
}

inline game_started_notify_t make_game_started_message (const bingo_room_state_t &source)
{
    game_started_notify_t target;
    write_message (source, *target.mutable_state ());
    return target;
}

inline number_drawn_notify_t make_message (const bingo_number_drawn_t &source)
{
    number_drawn_notify_t target;
    target.set_room_id (source.room_id);
    target.set_draw_seq (source.draw_seq);
    target.set_number (source.number);
    write_message (source.state, *target.mutable_state ());
    return target;
}

inline game_ended_notify_t make_game_ended_message (const bingo_room_state_t &source)
{
    game_ended_notify_t target;
    write_message (source, *target.mutable_state ());
    return target;
}

} // namespace zlink::samples::bingo
