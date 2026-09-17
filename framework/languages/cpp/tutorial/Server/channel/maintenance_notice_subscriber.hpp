#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

namespace fw = zlink::framework;

// --8<-- [start:fanout-handler]
// Receives what any publisher on this channel sends. The publisher does not
// know this node exists, so adding or removing a subscriber changes nothing
// there. event_type is what marks this as a fanout handler, and the topic it
// listens on defaults to that type's packet name.
class maintenance_notice_subscriber_t
{
  public:
    using event_type = maintenance_notice_t;

    explicit maintenance_notice_subscriber_t (
      fw::logger_t<maintenance_notice_subscriber_t> &logger) :
        _logger (logger)
    {
    }

    fw::task_t<void> handle (const event_type &event)
    {
        _logger.info ("maintenance notice: " + event.message);
        co_return;
    }

  private:
    fw::logger_t<maintenance_notice_subscriber_t> _logger;
};
// --8<-- [end:fanout-handler]
