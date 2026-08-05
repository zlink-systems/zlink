/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/timers/timer.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>
#include "runtime/execution/serial_execution_queue.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <vector>

namespace zlink::framework::detail
{

class timer_state_t
{
  public:
    using handler_invoker_t = std::function<task_t<zlink::message_t> (
      void *, void *, serializer_registry_t &, const timer_tick_t &)>;

    std::string name;
    std::chrono::milliseconds period{0};
    timer_options_t options;
    std::type_index handler_type{typeid (void)};
    std::weak_ptr<void> handler_instance;
    handler_invoker_t handler_invoker;
    std::unique_ptr<zlink::timer_t> native_timer;
    std::shared_ptr<runtime::serial_execution_queue_t> lane;
    std::uint64_t delivery_index = 0;
    std::uint64_t last_scheduled_index = 0;
    bool disposed = false;
    bool running = false;
    mutable std::mutex mutex;
    bool pending_fire = false;
    std::uint64_t pending_fire_count = 0;
    std::vector<timer_tick_t> delivered_ticks;
    std::vector<timer_failure_event_t> failure_events;
};

class timer_runtime_t
{
  public:
    static timer_runtime_t from (spot_context_t &context);
    explicit timer_runtime_t (std::shared_ptr<spot_context_state_t> context);

    result_t<timer_tick_t>
    dispatch_fire_count (timer_t &timer,
                         std::uint64_t fire_count,
                         std::function<void (const timer_tick_t &)> handler = {}) const;

    static void post_fire_count (const std::shared_ptr<spot_context_state_t> &context,
                                 const std::shared_ptr<timer_state_t> &state,
                                 std::uint64_t fire_count);

    task_t<timer_tick_t> dispatch_fire_count_async (timer_t &timer, std::uint64_t fire_count) const;

    void cancel_all () const noexcept;
    static void cancel_all (spot_context_state_t &context) noexcept;
    std::vector<timer_failure_event_t> failure_events (const timer_t &timer) const;
    std::vector<timer_tick_t> delivered_ticks (const timer_t &timer) const;

  private:
    std::shared_ptr<spot_context_state_t> _context;
};

} // namespace zlink::framework::detail
