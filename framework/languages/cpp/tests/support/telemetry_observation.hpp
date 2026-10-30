/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace zlink::framework::test
{

template <typename Event> struct telemetry_observations_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;

    void record (const Event &event)
    {
        std::lock_guard lock (mutex);
        events.push_back (event);
        changed.notify_all ();
    }

    std::vector<Event> snapshot ()
    {
        std::lock_guard lock (mutex);
        return events;
    }

    void clear ()
    {
        std::lock_guard lock (mutex);
        events.clear ();
    }
};

// The independent registration bounds preceding FIFO deliveries without adding
// the acknowledgment to the collector under test or retiring its registration.
inline void acknowledge_delivery (
  std::chrono::steady_clock::time_point deadline =
    std::chrono::steady_clock::now () + std::chrono::milliseconds (500))
{
    struct acknowledgment_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool acknowledged = false;
    };
    auto ack = std::make_shared<acknowledgment_t> ();
    dispatch_options_t barrier;
    barrier.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      barrier, [ack] (const message_flow_event_t &) {
          std::lock_guard lock (ack->mutex);
          ack->acknowledged = true;
          ack->changed.notify_all ();
      });
    detail::message_flow_tracer_t (barrier).trace (message_flow_event_t{
      .outcome = message_flow_outcome_t::received,
      .surface = dispatch_error_surface_t::channel,
      .message_kind = dispatch_message_kind_t::send});
    std::unique_lock lock (ack->mutex);
    if (!ack->changed.wait_until (lock, deadline, [&] { return ack->acknowledged; }))
        throw std::runtime_error ("telemetry record acknowledgment timed out");
}

} // namespace zlink::framework::test
