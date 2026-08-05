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
    static constexpr const char *topic_name = "AuthenticatePlayer";

    explicit authenticate_player_handler_t (logger_t<> logger = {}) : _logger (std::move (logger))
    {
    }

    authenticate_player_res_t handle (const authenticate_player_req_t &request)
    {
        if (request.access_token == bingo_sample_players_t::observer) {
            _logger.info ("authenticate observer", {{"actor_id", request.access_token}});
            return {true, request.access_token, "Observer", std::nullopt};
        }
        if (request.access_token.rfind ("player-", 0) != 0) {
            _logger.warn ("reject player authentication", {{"access_token", request.access_token}});
            return {false, std::nullopt, std::nullopt,
                    "access token must be a sample player id"};
        }
        _logger.info ("authenticate player", {{"actor_id", request.access_token}});
        return {true, request.access_token, "Player " + request.access_token.substr (7),
                std::nullopt};
    }

  private:
    logger_t<> _logger;
};

} // namespace zlink::samples::bingo
