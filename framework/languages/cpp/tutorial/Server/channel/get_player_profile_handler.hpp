#pragma once

#include "../../Shared/contracts.hpp"

// --8<-- [start:channel-request-handler]
// The return value is the reply. Nothing else has to be called to send it.
// request_type and reply_type are what the registration checks against.
class get_player_profile_handler_t
{
  public:
    using request_type = get_player_profile_t;
    using reply_type = player_profile_t;

    reply_type handle (
      const request_type &request)
    {
        return player_profile_t{
          request.player_id, "rookie", 1};
    }
};
// --8<-- [end:channel-request-handler]
