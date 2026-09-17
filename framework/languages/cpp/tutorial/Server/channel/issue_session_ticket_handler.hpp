#pragma once

#include "../../Shared/contracts.hpp"

// --8<-- [start:clientserver-handler]
// Written the same way as a RouteMesh handler. Only the registration differs.
class issue_session_ticket_handler_t
{
  public:
    using request_type = issue_session_ticket_t;
    using reply_type = session_ticket_t;

    reply_type handle (const request_type &request)
    {
        return session_ticket_t{"ticket-" + request.player_id};
    }
};
// --8<-- [end:clientserver-handler]
