#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

namespace fw = zlink::framework;

// --8<-- [start:channel-send-handler]
// Receives a message sent with send. There is no return value, so the caller
// learns nothing about how it went.
class record_login_handler_t
{
  public:
    explicit record_login_handler_t (fw::logger_t<record_login_handler_t> &logger) :
        _logger (logger)
    {
    }

    fw::task_t<void> handle (const record_login_t &message)
    {
        _logger.info ("login recorded: " + message.player_id);
        co_return;
    }

  private:
    fw::logger_t<record_login_handler_t> _logger;
};
// --8<-- [end:channel-send-handler]
