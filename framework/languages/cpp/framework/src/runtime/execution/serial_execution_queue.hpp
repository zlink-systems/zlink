/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime
{

class serial_turn_handle_impl_t;

enum class serial_work_lane_t
{
    application,
    lifecycle
};

enum class spot_lane_execution_t
{
    entry,
    spot_wide,
    per_actor
};

enum class spot_lane_lifecycle_t
{
    active,
    return_wait,
    relocation_sealed
};

struct spot_lane_policy_t
{
    spot_lane_execution_t execution = spot_lane_execution_t::entry;
    spot_lane_lifecycle_t lifecycle = spot_lane_lifecycle_t::active;
};

enum class session_lane_lifecycle_t
{
    open,
    connection_closed
};

struct session_lane_policy_t
{
    session_lane_lifecycle_t lifecycle = session_lane_lifecycle_t::open;
};

struct actor_delivery_lane_policy_t
{
};

class serial_lane_policy_t
{
  public:
    using value_t = std::variant<spot_lane_policy_t,
                                 session_lane_policy_t,
                                 actor_delivery_lane_policy_t>;

    static serial_lane_policy_t entry_spot ()
    {
        return serial_lane_policy_t (
          spot_lane_policy_t{spot_lane_execution_t::entry});
    }
    static serial_lane_policy_t spot_wide ()
    {
        return serial_lane_policy_t (
          spot_lane_policy_t{spot_lane_execution_t::spot_wide});
    }
    static serial_lane_policy_t per_actor_spot ()
    {
        return serial_lane_policy_t (
          spot_lane_policy_t{spot_lane_execution_t::per_actor});
    }
    static serial_lane_policy_t session ()
    {
        return serial_lane_policy_t (session_lane_policy_t{});
    }
    static serial_lane_policy_t actor_delivery ()
    {
        return serial_lane_policy_t (actor_delivery_lane_policy_t{});
    }

    bool allows_turn_yield () const noexcept
    {
        const auto *spot = std::get_if<spot_lane_policy_t> (&_value);
        return spot
               && spot->execution == spot_lane_execution_t::spot_wide;
    }

    const value_t &value () const noexcept { return _value; }

  private:
    template<typename Policy>
    explicit serial_lane_policy_t (Policy policy) : _value (policy)
    {
    }

    value_t _value;
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
    // A record already accepted by the receiving owner mailbox transfers that
    // exact reservation into this queue without a second capacity decision.
    // The callback releases the mailbox side only after queue accounting has
    // committed. It is consumed by one queue boundary and is not propagated to
    // an upper Spot execution-gate turn.
    std::function<void ()> transfer_owner_reservation;
};

using serial_submission_id_t = std::uint64_t;

enum class serial_cancel_submission_outcome_t
{
    queued_cancelled,
    active_cancel_requested,
    already_terminal
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
                                       serial_lane_policy_t policy =
                                         serial_lane_policy_t::actor_delivery ());
    serial_execution_queue_t (offload_executor_t &executor,
                              serial_execution_queue_options_t options,
                              error_handler_t error_handler = {},
                              serial_lane_policy_t policy =
                                serial_lane_policy_t::actor_delivery ());
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
    result_t<serial_submission_id_t>
    try_post_cancellable_async (std::string name,
                                async_work_t work,
                                std::function<void ()> cancel,
                                serial_work_options_t options = {});
    serial_cancel_submission_outcome_t
    cancel_submission (serial_submission_id_t submission_id) noexcept;
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
    result_t<std::shared_ptr<detail::deferred_barrier_t>>
    reserve_handoff_barrier (std::string name);
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
    bool allows_yield () const noexcept
    {
        return _lane_policy.allows_turn_yield ();
    }

  private:
    friend class serial_turn_handle_impl_t;

    struct work_item_t
    {
        std::string name;
        async_work_t work;
        serial_work_lane_t lane = serial_work_lane_t::application;
        std::size_t byte_cost = fixed_work_byte_cost;
        bool after_active_phase = false;
        serial_submission_id_t submission_id = 0;
        std::function<void ()> cancel;
        std::shared_ptr<serial_turn_handle_impl_t> turn;
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

    struct active_turn_t
    {
        serial_submission_id_t submission_id = 0;
        std::shared_ptr<serial_turn_handle_impl_t> turn;
        std::function<void ()> cancel;
        bool cancel_requested = false;
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
                         serial_work_options_t options,
                         serial_submission_id_t submission_id = 0,
                         std::function<void ()> cancel = {});
    std::shared_ptr<serial_turn_handle_impl_t>
    create_turn (const std::string &name,
                 serial_work_lane_t lane,
                 bool after_active_phase);
    void activate_turn_locked (work_item_t &item) noexcept;
    bool has_ready_locked () const noexcept;
    work_item_t take_next_locked ();
    void report_deferred_error (const std::string &name,
                                const std::exception_ptr &error) const noexcept;

    offload_executor_t &_executor;
    const serial_execution_queue_options_t _options;
    const serial_lane_policy_t _lane_policy;
    error_handler_t _error_handler;
    mutable std::mutex _mutex;
    std::condition_variable _empty;
    std::condition_variable _capacity_changed;
    lane_state_t _application;
    lane_state_t _lifecycle;
    std::vector<deferred_work_t> _deferred_after_active;
    std::optional<active_turn_t> _active_turn;
    std::optional<serial_work_lane_t> _active_lane;
    std::size_t _active_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point> _claim_started_at;
    std::size_t _lifecycle_streak = 0;
    bool _lifecycle_debt = false;
    bool _closed = false;
    bool _drain_scheduled = false;
    bool _draining = false;
    std::size_t _active = 0;
    serial_submission_id_t _next_submission_id = 1;
};

} // namespace zlink::framework::runtime
