/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/logging.hpp>

#include "runtime/dispatch/offload_executor.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

class diagnostic_event_sink_t
{
  public:
    static void append_field (std::vector<log_field_t> &fields, const char *key, std::string value)
    {
        fields.push_back (log_field_t{key, std::move (value)});
    }

    static void log_if_configured (const std::optional<logger_t<>> &logger,
                                   log_level_t level,
                                   std::string_view message,
                                   std::vector<log_field_t> fields) noexcept
    {
        if (logger)
            logger->log_with_fields (level, std::string (message), std::move (fields));
    }

    template <typename TObserver,
              typename TCallback,
              typename TEvent,
              typename TObserverInvoke,
              typename TCallbackInvoke,
              typename TFailure,
              typename TDropped>
    static void deliver_observer (std::shared_ptr<TObserver> observer,
                                  TCallback callback,
                                  TEvent event,
                                  TObserverInvoke invoke_observer,
                                  TCallbackInvoke invoke_callback,
                                  TFailure record_failure,
                                  TDropped record_dropped) noexcept
    {
        if (!observer && !callback) {
            return;
        }
        if (!observer_executor ().try_submit (
              [observer = std::move (observer), callback = std::move (callback),
               event = std::move (event), invoke_observer = std::move (invoke_observer),
               invoke_callback = std::move (invoke_callback),
               record_failure = std::move (record_failure)] () mutable {
                  try {
                      if (observer) {
                          invoke_observer (*observer, event);
                          return;
                      }
                      if (callback) {
                          invoke_callback (callback, event);
                      }
                  }
                  catch (...) {
                      record_failure ();
                  }
              })) {
            record_dropped ();
        }
    }

  private:
    struct observer_executor_holder_t
    {
        std::unique_ptr<runtime::offload_executor_t> executor =
          std::make_unique<runtime::offload_executor_t> (1, 1024);

        ~observer_executor_holder_t () { executor.reset (); }
    };

    static runtime::offload_executor_t &observer_executor ()
    {
        static observer_executor_holder_t holder;
        return *holder.executor;
    }
};

} // namespace zlink::framework::detail
