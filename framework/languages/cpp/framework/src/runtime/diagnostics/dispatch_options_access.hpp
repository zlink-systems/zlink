/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <functional>
#include <optional>
#include <utility>

namespace zlink::framework::detail
{

class dispatch_options_access_t
{
  public:
    static const std::optional<logger_t<>> &logger (
      const dispatch_options_t &options) noexcept
    {
        return options._diagnostics_logger;
    }

    static void set_logger (
      dispatch_options_t &options,
      std::optional<logger_t<>> logger)
    {
        options._diagnostics_logger = std::move (logger);
    }

    static void set_observer_for_tests (
      dispatch_options_t &options,
      std::function<void (const message_flow_event_t &)> observer)
    {
        options._message_flow_observer =
          [observer = std::move (observer)] (const void *event) {
              observer (*static_cast<const message_flow_event_t *> (event));
          };
    }

    static void observe (
      const dispatch_options_t &options,
      const message_flow_event_t &event)
    {
        if (options._message_flow_observer)
            options._message_flow_observer (&event);
    }
};

} // namespace zlink::framework::detail
