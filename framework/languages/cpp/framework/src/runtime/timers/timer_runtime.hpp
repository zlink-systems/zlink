/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/timers/timer.hpp>
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"

#include <atomic>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace zlink::framework::detail
{

class timer_resource_t
{
  public:
    virtual ~timer_resource_t () = default;
    virtual result_t<void> cancel () noexcept = 0;
};

class core_timer_resource_t final : public timer_resource_t
{
  public:
    result_t<void> cancel () noexcept override;

    core_timer_drain_loop_t &loop () noexcept { return _loop; }

  private:
    core_timer_drain_loop_t _loop;
};

class timer_state_t
{
  public:
    static constexpr std::size_t observation_history_limit = 256;
    using handler_invoker_t = std::function<task_t<zlink::message_t> (
      void *, void *, serializer_registry_t &, const timer_tick_t &)>;

    std::string name;
    std::chrono::milliseconds period{0};
    timer_options_t options;
    std::type_index handler_type{typeid (void)};
    std::weak_ptr<void> handler_instance;
    handler_invoker_t handler_invoker;
    std::unique_ptr<timer_resource_t> native_timer;
    std::shared_ptr<runtime::serial_execution_queue_t> serial_queue;
    std::uint64_t delivery_index = 0;
    std::uint64_t last_scheduled_index = 0;
    std::atomic_bool disposed{false};
    bool running = false;
    mutable std::mutex mutex;
    std::shared_ptr<task_completion_source_t<void>> cancel_completion;
    std::optional<result_t<void>> cleanup_result;
    bool cancel_completed = false;
    bool pending_fire = false;
    std::uint64_t pending_fire_count = 0;
    std::deque<timer_tick_t> delivered_ticks;
    std::deque<timer_failure_event_t> failure_events;
};

struct timer_test_access_t
{
    static timer_t create (std::shared_ptr<timer_state_t> state)
    {
        return timer_t (std::move (state));
    }

    static void finish_callback (const std::shared_ptr<timer_state_t> &state);
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
                                 const std::shared_ptr<runtime::serial_execution_queue_t> &queue,
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
