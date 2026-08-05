/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_player_record_store.hpp"

#include <zlink/framework.hpp>

#include <utility>

namespace zlink::samples::bingo
{

using namespace framework;

class get_player_record_handler_t
{
  public:
    using request_type = get_player_record_req_t;
    using reply_type = get_player_record_res_t;
    using dependency_types = dependency_list_t<bingo_player_record_store_t>;
    static constexpr const char *topic_name = "GetPlayerRecord";

    get_player_record_handler_t (bingo_player_record_store_t &records, logger_t<> logger = {}) :
        _records (records), _logger (std::move (logger))
    {
    }

    get_player_record_res_t handle (const get_player_record_req_t &request)
    {
        auto record = _records.get (request.actor_id);
        _logger.info ("api player record loaded",
                      {{"actor_id", record.actor_id},
                       {"wins", std::to_string (record.wins)},
                       {"losses", std::to_string (record.losses)}});
        return record;
    }

  private:
    bingo_player_record_store_t &_records;
    logger_t<> _logger;
};

} // namespace zlink::samples::bingo
