/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../bingo_player_record_store.hpp"

#include <zlink/framework.hpp>

#include <utility>

namespace zlink::samples::bingo
{

using namespace framework;

class report_bingo_result_handler_t
{
  public:
    using request_type = report_bingo_result_req_t;
    using reply_type = report_bingo_result_res_t;
    using dependency_types = dependency_list_t<bingo_player_record_store_t>;
    static constexpr const char *topic_name = "ReportBingoResult";

    report_bingo_result_handler_t (bingo_player_record_store_t &records,
                                   logger_t<> logger = {}) :
        _records (records), _logger (std::move (logger))
    {
    }

    report_bingo_result_res_t handle (const report_bingo_result_req_t &request)
    {
        auto record = _records.report (request.actor_id, request.won);
        _logger.info ("api bingo result reported",
                      {{"room_id", request.room_id},
                       {"actor_id", request.actor_id},
                       {"won", request.won ? "true" : "false"},
                       {"final_draw_seq", std::to_string (request.final_draw_seq)},
                       {"wins", std::to_string (record.wins)},
                       {"losses", std::to_string (record.losses)}});
        return record;
    }

  private:
    bingo_player_record_store_t &_records;
    logger_t<> _logger;
};

} // namespace zlink::samples::bingo
