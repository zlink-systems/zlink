/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "telemetry_observation.hpp"
#include "runtime/diagnostics/dispatch_events.hpp"
#include <mutex>
#include <vector>

namespace framework_channel_messaging_fixture
{
inline std::vector<zlink::framework::message_dispatch_error_event_t>
wait_dispatch_errors (std::vector<zlink::framework::message_dispatch_error_event_t> &events,
                      std::mutex &mutex)
{
    // The barrier orders preceding FIFO deliveries; the test runner bounds the wait (#5358).
    zlink::framework::test::acknowledge_delivery ();
    std::lock_guard lock (mutex);
    return events;
}

inline void
clear_dispatch_errors (std::vector<zlink::framework::message_dispatch_error_event_t> &events,
                       std::mutex &mutex)
{
    (void) wait_dispatch_errors (events, mutex);
    std::lock_guard lock (mutex);
    events.clear ();
}
} // namespace framework_channel_messaging_fixture
