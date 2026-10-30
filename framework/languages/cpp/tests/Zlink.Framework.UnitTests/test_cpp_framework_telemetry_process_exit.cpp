/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/diagnostics/message_flow_tracer.hpp"
#include <zlink/framework/contracts/configuration/app.hpp>

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>

namespace
{
using namespace zlink::framework;
// Constructed before main first initializes the process telemetry owner.
app_t static_app;

struct observation_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
};
} // namespace

int main (int argc, char **argv)
{
    const bool blocked = argc == 2 && std::string_view (argv[1]) == "--blocked-provider";
    auto observation = std::make_shared<observation_t> ();
    auto local_app = blocked ? std::make_unique<app_t> () : nullptr;
    auto &app = blocked ? *local_app : static_app;
    auto &options = app.add_zlink_framework ().configure_dispatch ();
    options.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      options, [observation, blocked] (const message_flow_event_t &) {
          std::unique_lock lock (observation->mutex);
          observation->entered = true;
          observation->changed.notify_all ();
          if (blocked)
              observation->changed.wait (lock, [] { return false; });
      });
    detail::message_flow_tracer_t (options).trace (
      message_flow_event_t{.outcome = message_flow_outcome_t::received,
                           .surface = dispatch_error_surface_t::channel,
                           .message_kind = dispatch_message_kind_t::request,
                           .packet_name = std::string ("process-exit")});
    {
        std::unique_lock lock (observation->mutex);
        // CTest owns the existing process timeout; no separate timing budget.
        observation->changed.wait (lock, [&] { return observation->entered; });
    }
    local_app.reset ();
    std::cout << "provider_entered=1 host_destroyed=" << blocked << " returning_main=1"
              << std::endl;
    return 0;
}
