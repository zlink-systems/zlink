/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

class serial_turn_handle_impl_t;

enum class serial_work_lane_t
{
    application,
    lifecycle
};

struct serial_execution_queue_options_t
{
    std::size_t application_message_capacity =
      dispatch_limits::application_mailbox_messages;
    std::size_t application_byte_capacity =
      dispatch_limits::application_mailbox_bytes;
    std::size_t lifecycle_message_capacity =
      dispatch_limits::control_mailbox_messages;
    std::size_t lifecycle_byte_capacity =
      dispatch_limits::control_mailbox_bytes;
    std::chrono::milliseconds owner_time_budget =
      dispatch_limits::owner_time_budget;
    std::size_t lifecycle_burst_limit =
      dispatch_limits::lifecycle_burst_limit;
};

struct serial_work_options_t
{
    serial_work_lane_t lane = serial_work_lane_t::application;
    // This is the complete reservation cost: payload, metadata, envelope and
    // queue-node overhead. A zero value uses the fixed minimum reservation.
    std::size_t byte_cost = dispatch_limits::fixed_work_byte_cost;
};

class serial_execution_queue_t
{
  public:
    static constexpr std::size_t fixed_work_byte_cost =
      dispatch_limits::fixed_work_byte_cost;
    using error_handler_t = std::function<void (const std::string &, const std::exception_ptr &)>;
    using async_completion_t = std::function<void (std::function<void ()>)>;
    using async_work_t = std::function<void (async_completion_t)>;

    explicit serial_execution_queue_t (offload_executor_t &executor,
                                       std::size_t capacity =
                                         dispatch_limits::application_mailbox_messages,
                                       error_handler_t error_handler = {},
                                       bool allow_yield = false);
    serial_execution_queue_t (offload_executor_t &executor,
                              serial_execution_queue_options_t options,
                              error_handler_t error_handler = {},
                              bool allow_yield = false);
    ~serial_execution_queue_t ();

    serial_execution_queue_t (const serial_execution_queue_t &) = delete;
    serial_execution_queue_t &operator= (const serial_execution_queue_t &) = delete;

    bool try_post (std::string name, std::function<void ()> work);
    bool try_post (std::string name,
                   std::function<void ()> work,
                   serial_work_options_t options);
    bool try_post_async (std::string name, async_work_t work);
    bool try_post_async (std::string name,
                         async_work_t work,
                         serial_work_options_t options);
    bool post_async_wait (std::string name,
                          async_work_t work,
                          std::function<bool ()> stop_requested = {});
    bool post_async_wait (std::string name,
                          async_work_t work,
                          serial_work_options_t options,
                          std::function<bool ()> stop_requested = {});
    bool try_post_deferred (std::string name, std::function<void ()> work);
    result_t<std::shared_ptr<detail::deferred_barrier_t>>
    reserve_barrier_next (std::string name);
    void post (std::string name, std::function<void ()> work);
    void post (std::string name,
               std::function<void ()> work,
               serial_work_options_t options);
    void post_async (std::string name, async_work_t work);
    void post_async (std::string name,
                     async_work_t work,
                     serial_work_options_t options);
    void run (std::string name, std::function<void ()> work);
    void drain ();
    void close ();
    void cancel_pending ();

    std::size_t pending_count () const;
    std::size_t pending_count (serial_work_lane_t lane) const;
    std::size_t pending_bytes () const;
    bool closed () const;
    bool allows_yield () const noexcept { return _allow_yield; }

  private:
    friend class serial_turn_handle_impl_t;

    struct work_item_t
    {
        std::string name;
        async_work_t work;
        serial_work_lane_t lane = serial_work_lane_t::application;
        std::size_t byte_cost = fixed_work_byte_cost;
        bool after_active_phase = false;
    };

    struct lane_state_t
    {
        std::deque<work_item_t> queue;
        // Work registered by the current turn is released as a dedicated
        // after-active phase. It must run before a reserved deferred barrier
        // can be reached, but it is still a normal serial turn rather than
        // inline execution inside completion.
        std::deque<work_item_t> after_active_queue;
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::size_t message_capacity = 0;
        std::size_t byte_capacity = 0;
    };

    struct deferred_work_t
    {
        std::string name;
        std::function<void ()> work;
        std::size_t byte_cost = fixed_work_byte_cost;
    };

    bool schedule_drain_locked ();
    void drain_loop ();
    void execute_item (work_item_t item);
    void complete_one (std::string name,
                       std::function<void ()> completion,
                       bool allow_inline_claim = true);
    lane_state_t &lane_locked (serial_work_lane_t lane) noexcept;
    const lane_state_t &lane_locked (serial_work_lane_t lane) const noexcept;
    bool can_enqueue_locked (const serial_work_options_t &options) const noexcept;
    bool enqueue_locked (std::string name,
                         async_work_t work,
                         serial_work_options_t options);
    bool has_ready_locked () const noexcept;
    work_item_t take_next_locked ();
    void report_deferred_error (const std::string &name,
                                const std::exception_ptr &error) const noexcept;

    offload_executor_t &_executor;
    const serial_execution_queue_options_t _options;
    const bool _allow_yield;
    error_handler_t _error_handler;
    mutable std::mutex _mutex;
    std::condition_variable _empty;
    std::condition_variable _capacity_changed;
    lane_state_t _application;
    lane_state_t _lifecycle;
    std::vector<deferred_work_t> _deferred_after_active;
    std::vector<std::shared_ptr<detail::serial_turn_t>> _active_turns;
    std::optional<serial_work_lane_t> _active_lane;
    std::size_t _active_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point> _claim_started_at;
    std::size_t _lifecycle_streak = 0;
    bool _lifecycle_debt = false;
    bool _closed = false;
    bool _drain_scheduled = false;
    bool _draining = false;
    std::size_t _active = 0;
};

} // namespace zlink::framework::runtime
