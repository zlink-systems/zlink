/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/Contracts/messages.hpp"

#include <zlink/framework.hpp>

#include <utility>

namespace zlink::samples::bingo
{

using namespace framework;

class authenticate_player_handler_t
{
  public:
    using request_type = authenticate_player_req_t;
    using reply_type = authenticate_player_res_t;
    static constexpr const char *topic_name = "AuthenticatePlayerReq";

    explicit authenticate_player_handler_t (logger_t<> logger = {}) : _logger (std::move (logger))
    {
    }

    authenticate_player_res_t handle (const authenticate_player_req_t &request)
    {
        authenticate_player_res_t response;
        if (request.access_token () == bingo_sample_players_t::observer) {
            _logger.info ("authenticate observer", {{"actor_id", request.access_token ()}});
            response.set_accepted (true);
            response.set_actor_id (request.access_token ());
            response.set_display_name ("Observer");
            return response;
        }
        if (request.access_token ().rfind ("player-", 0) != 0) {
            _logger.warn ("reject player authentication",
                          {{"access_token", request.access_token ()}});
            response.set_reason ("access token must be a sample player id");
            return response;
        }
        _logger.info ("authenticate player", {{"actor_id", request.access_token ()}});
        response.set_accepted (true);
        response.set_actor_id (request.access_token ());
        response.set_display_name ("Player " + request.access_token ().substr (7));
        return response;
    }

  private:
    logger_t<> _logger;
};

} // namespace zlink::samples::bingo
