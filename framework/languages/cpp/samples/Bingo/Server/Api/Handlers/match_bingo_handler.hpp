/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/sample_names.hpp"
#include "../../../Shared/Contracts/messages.hpp"

#include <zlink/framework.hpp>

#include <utility>

namespace zlink::samples::bingo
{

using namespace framework;

class match_bingo_api_handler_t
{
  public:
    using request_type = match_bingo_api_req_t;
    using reply_type = match_bingo_api_res_t;
    static constexpr const char *topic_name = "MatchBingoApiReq";

    match_bingo_api_handler_t (route_client_t &routes,
                               spot_manager_t &spots,
                               logger_t<> logger = {}) :
        _routes (routes), _spots (spots), _logger (std::move (logger))
    {
    }

    task_t<match_bingo_api_res_t> handle (const match_bingo_api_req_t &request)
    {
        constexpr auto level_bucket = "1-10";
        reserve_bingo_room_req_t reserve_request;
        reserve_request.set_mode (request.mode ());
        reserve_request.set_actor_id (request.actor_id ());
        reserve_request.set_level_bucket (level_bucket);
        auto allocated = co_await _routes
                           .request_to_spot (spot_id_t (std::string ("match:") + level_bucket),
                                             std::move (reserve_request))
                           .instance_spot (sample_names_t::matchmaker_spot)
                           .in_mesh (sample_names_t::matchmaking_mesh)
                           .async<reserve_bingo_room_res_t> ();
        co_await _spots.get_or_create (spot_id_t (allocated.room_id ()), sample_names_t::room_spot)
          .in_mesh (sample_names_t::room_spot_mesh)
          .creation_request ([&allocated] {
              bingo_room_create_req_t create;
              *create.mutable_settings () = allocated.settings ();
              return create;
          }())
          .async ();
        _logger.info ("match bingo room", {{"actor_id", request.actor_id ()},
                                           {"room_id", allocated.room_id ()},
                                           {"mode", request.mode ()}});
        match_bingo_api_res_t response;
        response.set_room_id (allocated.room_id ());
        co_return response;
    }

  private:
    route_client_t &_routes;
    spot_manager_t &_spots;
    logger_t<> _logger;
};

} // namespace zlink::samples::bingo
