/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/sample_names.hpp"
#include "../../../Shared/Contracts/messages.hpp"

#include <string>

namespace zlink::samples::tictactoe
{

// --8<-- [start:doc-request-handler]
class authenticate_player_handler_t
{
  public:
    using request_type = authenticate_player_req_t;
    using reply_type = authenticate_player_res_t;
    static constexpr const char *topic_name = "AuthenticatePlayer";

    authenticate_player_res_t handle (const authenticate_player_req_t &request)
    {
        if (request.access_token.empty ()) {
            return {false, {}, "actor id must not be empty"};
        }
        const int wins = request.access_token == sample_names_t::x_actor_id ? 99 : 0;
        return {true,
                {request.access_token, request.access_token, sample_names_t::required_level, wins},
                ""};
    }
};
// --8<-- [end:doc-request-handler]

} // namespace zlink::samples::tictactoe
