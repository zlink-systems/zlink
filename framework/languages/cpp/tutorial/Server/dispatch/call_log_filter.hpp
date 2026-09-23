#pragma once

#include <zlink/framework.hpp>

#include <chrono>
#include <string>

namespace fw = zlink::framework;

// Runs around every handler this node receives, so the same logging is not
// repeated in each handler. Calling next() runs the handler; skipping it does
// not.
// --8<-- [start:filter-implementation]
class call_log_filter_t
{
  public:
    explicit call_log_filter_t (fw::logger_t<call_log_filter_t> &logger) : _logger (logger) {}

    fw::task_t<void> invoke (const fw::handler_filter_context_t &context, fw::handler_next_t next)
    {
        const auto started_at = std::chrono::steady_clock::now ();
        _logger.info ("dispatch start: " + context.packet_name);

        co_await next ();

        const auto elapsed = std::chrono::steady_clock::now () - started_at;
        _logger.info ("dispatch done: " + context.packet_name + " in "
                      + std::to_string (
                        std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ())
                      + "ms");
    }

  private:
    fw::logger_t<call_log_filter_t> _logger;
};
// --8<-- [end:filter-implementation]
