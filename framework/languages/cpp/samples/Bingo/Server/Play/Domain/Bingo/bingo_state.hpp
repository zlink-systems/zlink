/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace zlink::samples::bingo
{

struct bingo_player_state_t
{
    std::string actor_id;
    std::string display_name;
    int seat = 0;
    bool is_host = false;
    std::vector<int> card;
    std::vector<bool> marks;
    int completed_lines = 0;
    int wins = 0;
    int losses = 0;
};

struct bingo_room_state_t
{
    std::string room_id;
    std::string status = "WaitingForPlayers";
    std::string host_actor_id;
    bool can_start = false;
    int draw_seq = 0;
    std::optional<int> last_drawn_number;
    std::vector<int> drawn_numbers;
    std::vector<bingo_player_state_t> players;
    std::vector<std::string> winners;
};

struct bingo_number_drawn_t
{
    std::string room_id;
    int draw_seq = 0;
    int number = 0;
    bingo_room_state_t state;
};

} // namespace zlink::samples::bingo
