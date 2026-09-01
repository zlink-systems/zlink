/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/Contracts/messages.hpp"

#include <map>
#include <mutex>
#include <string>

namespace zlink::samples::bingo
{

class bingo_player_record_store_t
{
  public:
    get_player_record_res_t get (const std::string &actor_id)
    {
        std::lock_guard lock (_mutex);
        auto [record, _] = _records.try_emplace (actor_id, player_record_t{actor_id, 0, 0});
        get_player_record_res_t response;
        response.set_actor_id (record->second.actor_id);
        response.set_wins (record->second.wins);
        response.set_losses (record->second.losses);
        return response;
    }

    report_bingo_result_res_t report (const std::string &actor_id, bool won)
    {
        std::lock_guard lock (_mutex);
        auto [record, _] = _records.try_emplace (actor_id, player_record_t{actor_id, 0, 0});
        if (won) {
            ++record->second.wins;
        } else {
            ++record->second.losses;
        }
        report_bingo_result_res_t response;
        response.set_actor_id (record->second.actor_id);
        response.set_wins (record->second.wins);
        response.set_losses (record->second.losses);
        return response;
    }

  private:
    struct player_record_t
    {
        std::string actor_id;
        int wins = 0;
        int losses = 0;
    };

    std::mutex _mutex;
    std::map<std::string, player_record_t> _records;
};

} // namespace zlink::samples::bingo
