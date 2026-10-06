/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime
{

class serial_turn_handle_impl_t;
struct serial_deferred_slot_t;
struct serial_turn_chain_t;

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
    using value_t =
      std::variant<spot_lane_policy_t, session_lane_policy_t, actor_delivery_lane_policy_t>;

    static serial_lane_policy_t entry_spot ()
    {
        return serial_lane_policy_t (spot_lane_policy_t{spot_lane_execution_t::entry});
    }
    static serial_lane_policy_t spot_wide ()
    {
        return serial_lane_policy_t (spot_lane_policy_t{spot_lane_execution_t::spot_wide});
    }
    static serial_lane_policy_t per_actor_spot ()
    {
        return serial_lane_policy_t (spot_lane_policy_t{spot_lane_execution_t::per_actor});
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
        return spot && spot->execution == spot_lane_execution_t::spot_wide;
    }

    const value_t &value () const noexcept { return _value; }

  private:
    template <typename Policy> explicit serial_lane_policy_t (Policy policy) : _value (policy) {}

    value_t _value;
};

struct serial_execution_queue_options_t
{
    std::chrono::milliseconds owner_time_budget = dispatch_limits::owner_time_budget;
    std::size_t lifecycle_burst_limit = dispatch_limits::lifecycle_burst_limit;
};

struct serial_work_options_t
{
    serial_work_lane_t lane = serial_work_lane_t::application;
    // Retained for diagnostics and ownership-transfer accounting only; bytes
    // never participate in Framework queue admission.
    std::size_t byte_cost = dispatch_limits::fixed_work_byte_cost;
    // A record claimed from the receiving owner mailbox keeps that claim until
    // the serial queue owns the record.
    std::function<void ()> transfer_owner_reservation;
    // Ordinary Actor ingress opt-in for the relocation admission fence
    // (spot-actor membership §"Defer() 뒤 source seal 전 message"). While an
    // Actor handoff barrier is reserved on this queue, a message that already
    // passed the transfer coordinator's not_moving admission must NOT be
    // allowed to land behind the barrier: it would run on the old owner after
    // capture and be lost from the authority state. Refusing the post here --
    // under the same mutex that orders the barrier's own enqueue -- is the
    // linearization point that makes coordinator admission and Actor FIFO
    // admission one atomic decision. The caller then re-admits the packet into
    // the coordinator backlog so it travels with the commit.
    // Never set on relocation-owned work (transfer_owner_reservation) or on
    // the Spot execution-gate hop; it is a positive opt-in from the one
    // ordinary Actor dispatch call site.
    bool refuse_when_actor_handoff_fenced = false;
    // Written (true) at the refusal site so the caller can distinguish a fence
    // refusal from a capacity/closed refusal. Only dereferenced inside the
    // synchronous post call.
    bool *actor_handoff_fence_refused = nullptr;
    // A lifecycle item that waits outside a user callback returns its turn, so
    // runnable application work proceeds (handler turn and execution gate §7).
    // A relocation readiness boundary instead holds application jobs until it
    // completes (Spot model §5.1).
    bool holds_application_while_waiting = false;
    // application dispatcher가 소유한 원본 메시지를 그대로 보존한다.
    std::shared_ptr<const void> retained_message;
};

using serial_submission_id_t = std::uint64_t;

enum class serial_cancel_submission_outcome_t
{
    queued_cancelled,
    active_cancel_requested,
    already_terminal
};

class serial_execution_queue_t : public std::enable_shared_from_this<serial_execution_queue_t>
{
  public:
    static constexpr std::size_t fixed_work_byte_cost = dispatch_limits::fixed_work_byte_cost;
    using error_handler_t = std::function<void (const std::string &, const std::exception_ptr &)>;
    using async_completion_t = std::function<void (std::function<void ()>)>;
    using async_work_t = std::function<void (async_completion_t)>;

    serial_execution_queue_t (
      offload_executor_t &executor,
      serial_execution_queue_options_t options = {},
      error_handler_t error_handler = {},
      serial_lane_policy_t policy = serial_lane_policy_t::actor_delivery ());
    ~serial_execution_queue_t ();

    serial_execution_queue_t (const serial_execution_queue_t &) = delete;
    serial_execution_queue_t &operator= (const serial_execution_queue_t &) = delete;

    bool try_post (std::string name, std::function<void ()> work);
    bool try_post (std::string name, std::function<void ()> work, serial_work_options_t options);
    bool try_post_async (std::string name, async_work_t work);
    bool try_post_async (std::string name, async_work_t work, serial_work_options_t options);
    result_t<serial_submission_id_t>
    try_post_cancellable_async (std::string name,
                                async_work_t work,
                                std::function<void ()> cancel,
                                serial_work_options_t options = {});
    task_t<serial_cancel_submission_outcome_t>
    cancel_submission (serial_submission_id_t submission_id);
    bool post_async_wait (std::string name,
                          async_work_t work,
                          std::function<bool ()> stop_requested = {});
    bool post_async_wait (std::string name,
                          async_work_t work,
                          serial_work_options_t options,
                          std::function<bool ()> stop_requested = {});
    bool try_post_deferred (std::string name, std::function<void ()> work);
    result_t<std::shared_ptr<detail::deferred_barrier_t>>
    reserve_barrier_next (std::string name, bool holds_application_while_waiting = false);
    result_t<std::shared_ptr<detail::deferred_barrier_t>>
    reserve_handoff_barrier (std::string name);
    void post (std::string name, std::function<void ()> work);
    void post (std::string name, std::function<void ()> work, serial_work_options_t options);
    void post_async (std::string name, async_work_t work);
    void post_async (std::string name, async_work_t work, serial_work_options_t options);
    void run (std::string name, std::function<void ()> work);
    void drain ();
    void close ();
    void cancel_waits () noexcept;
    void cancel_pending () noexcept;

    std::size_t pending_count () const;
    std::size_t pending_count (serial_work_lane_t lane) const;
    std::size_t pending_bytes () const;
    bool closed () const;
    /* Spot address messaging §7 step 3 under this queue's mutex: the first
     * retained message waiting behind a Close, or none, in which case the
     * queue closes in the same decision and refuses later work. */
    task_t<std::shared_ptr<const void>> first_pending_message_or_close ();
    task_t<std::vector<std::shared_ptr<const void>>> pending_messages () const;
    bool allows_yield () const noexcept { return _lane_policy.allows_turn_yield (); }
    // Installs the SpotWide owner before an Actor mailbox accepts work.
    void attach_spot_gate (std::shared_ptr<serial_execution_queue_t> gate);

  private:
    friend class serial_turn_handle_impl_t;

    struct work_item_t
    {
        std::string name;
        async_work_t work;
        serial_work_lane_t lane = serial_work_lane_t::application;
        std::size_t byte_cost = fixed_work_byte_cost;
        serial_submission_id_t submission_id = 0;
        std::function<void ()> cancel;
        std::shared_ptr<serial_turn_handle_impl_t> turn;
        bool cancel_requested = false;
        std::optional<std::function<void ()>> suspended_completion;
        bool holds_application_while_waiting = false;
        // The handler this item continues after a Yield; empty otherwise.
        std::shared_ptr<serial_turn_chain_t> chain;
        // The deferred-work position this item holds; empty otherwise.
        std::shared_ptr<serial_deferred_slot_t> slot;
        std::shared_ptr<const void> retained_message;
        std::shared_ptr<serial_execution_queue_t> actor_mailbox;
    };

    struct publication_node_t
    {
        std::atomic<publication_node_t *> next{nullptr};
        std::optional<work_item_t> item;
        std::function<void ()> control;
    };

    struct item_origin_t
    {
        std::shared_ptr<serial_turn_chain_t> chain;
        std::shared_ptr<serial_deferred_slot_t> slot;
    };

    struct lane_state_t
    {
        std::deque<work_item_t> queue;
        std::atomic<std::size_t> messages{0};
        std::atomic<std::size_t> bytes{0};
    };

    struct active_turn_t
    {
        serial_submission_id_t submission_id = 0;
        std::shared_ptr<serial_turn_handle_impl_t> turn;
        std::function<void ()> cancel;
        async_work_t ready_continuation;
        bool cancel_requested = false;
    };

    bool schedule_drain_locked ();
    bool publish_locked (std::unique_ptr<publication_node_t> node);
    void import_publications ();
    void import_publications_until (publication_node_t *cut);
    void run_shared_gate ();
    void release_shared_gate ();
    bool submit_control (std::function<void ()> control);
    template <typename T, typename TControl> task_t<T> control_async (TControl control)
    {
        auto completion = std::make_shared<task_completion_source_t<T>> ();
        auto task = completion->task ();
        if (!submit_control ([retained = weak_from_this ().lock (), completion,
                              control = std::move (control)] () mutable {
                try {
                    if constexpr (std::is_void_v<T>) {
                        control ();
                        completion->complete (result_t<void>::success ());
                    } else {
                        completion->complete (result_t<T>::success (control ()));
                    }
                }
                catch (const framework_exception_t &error) {
                    completion->complete (detail::result_access_t::failure<T> (error));
                }
                catch (const std::exception &error) {
                    completion->complete (result_t<T>::failure (
                      framework_error_kind_t::internal_failure, error.what ()));
                }
            }))
            completion->complete (result_t<T>::failure (framework_error_kind_t::shutting_down,
                                                        "shared Spot gate executor is stopping"));
        return task;
    }
    void notify_control (std::string name, std::function<void ()> control);
    serial_cancel_submission_outcome_t
    cancel_submission_owned (serial_submission_id_t submission_id) noexcept;
    std::shared_ptr<const void> first_pending_message_or_close_owned ();
    std::vector<std::shared_ptr<const void>> pending_messages_owned () const;
    void cancel_waits_owned () noexcept;
    void cancel_pending_owned () noexcept;
    void hold_lifecycle_owned (std::shared_ptr<serial_turn_chain_t> chain);
    void release_lifecycle_hold_owned (const serial_turn_chain_t *chain);
    void schedule_after_settle_owned ();
    bool resume_suspended_owned (const std::shared_ptr<serial_turn_handle_impl_t> &turn,
                                 std::function<void ()> work);
    void connect_actor_head (const std::shared_ptr<serial_execution_queue_t> &actor);
    void disconnect_actor_head (const std::shared_ptr<serial_execution_queue_t> &actor);
    void execute_actor_head (work_item_t spot_item);
    bool owns_shared_gate () const noexcept;
    void close_locked ();
    void drain_loop ();
    void execute_item (work_item_t item);
    void complete_one (std::string name,
                       std::function<void ()> completion,
                       bool allow_inline_claim = true);
    void complete_turn (std::string name,
                        std::function<void ()> completion,
                        const std::shared_ptr<serial_turn_handle_impl_t> &turn,
                        bool allow_inline_claim);
    void suspend_lifecycle (work_item_t item);
    task_t<bool> try_resume_suspended (const std::shared_ptr<serial_turn_handle_impl_t> &turn,
                                       std::function<void ()> work);
    lane_state_t &lane_locked (serial_work_lane_t lane) noexcept;
    const lane_state_t &lane_locked (serial_work_lane_t lane) const noexcept;
    bool enqueue_locked (std::string name,
                         async_work_t work,
                         serial_work_options_t options,
                         serial_submission_id_t submission_id = 0,
                         std::function<void ()> cancel = {},
                         item_origin_t origin = {});
    /* Accepts the FIFO position of work a handler defers to its terminal
     * (handler turn and execution gate §5, §7). The item is registered in the
     * lifecycle lane now and stays unrunnable until the handler terminal fills
     * or discards it. */
    bool enqueue_deferred_slot (std::string name, std::shared_ptr<serial_deferred_slot_t> slot);
    /* A lifecycle operation that released its turn (Yield) keeps the lifecycle
     * lane until its terminal (§7): only its own continuation runs there. */
    bool try_post_continuation (std::string name,
                                async_work_t work,
                                serial_work_lane_t lane,
                                std::shared_ptr<serial_turn_chain_t> chain);
    void hold_lifecycle (std::shared_ptr<serial_turn_chain_t> chain);
    void release_lifecycle_hold (std::shared_ptr<serial_turn_chain_t> chain);
    void schedule_after_settle ();
    std::shared_ptr<serial_turn_handle_impl_t>
    create_turn (const std::string &name, serial_work_lane_t lane, item_origin_t origin);
    void activate_turn_locked (work_item_t &item) noexcept;
    std::deque<work_item_t>::iterator next_lifecycle_locked () noexcept;
    bool has_queued_locked () const noexcept;
    bool has_ready_locked () noexcept;
    bool application_ready_locked () const noexcept;
    work_item_t take_next_locked (std::optional<serial_work_lane_t> forced_lane = {});
    serial_work_lane_t select_next_lane_locked () noexcept;
    void report_deferred_error (const std::string &name,
                                const std::exception_ptr &error) const noexcept;

    offload_executor_t &_executor;
    const serial_execution_queue_options_t _options;
    const serial_lane_policy_t _lane_policy;
    error_handler_t _error_handler;
    mutable std::mutex _mutex;
    std::condition_variable _empty;
    lane_state_t _application;
    lane_state_t _lifecycle;
    // P changes tail; the shared-gate consumer changes cursor. The cursor
    // node is retained while the producer tail can still point to it.
    publication_node_t *_publication_tail = nullptr;
    publication_node_t *_publication_cursor = nullptr;
    std::shared_ptr<serial_execution_queue_t> _spot_gate;
    enum class gate_state_t : unsigned char
    {
        idle,
        scheduled,
        running
    };
    // idle -> scheduled is the single uncontended successful gate CAS.
    std::atomic<gate_state_t> _gate_state{gate_state_t::idle};
    std::shared_ptr<serial_turn_chain_t> _lifecycle_hold;
    std::optional<active_turn_t> _active_turn;
    std::optional<work_item_t> _suspended_lifecycle;
    std::optional<serial_work_lane_t> _active_lane;
    std::size_t _active_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point> _claim_started_at;
    std::size_t _lifecycle_streak = 0;
    bool _lifecycle_debt = false;
    std::atomic_bool _closed{false};
    bool _drain_scheduled = false;
    bool _draining = false;
    std::size_t _active = 0;
    serial_submission_id_t _next_submission_id = 1;
    // Raised strictly before an Actor handoff barrier is enqueued and lowered
    // when that barrier is reached or cancelled. Read while _mutex is held, so
    // a post that observes zero is provably ordered ahead of the barrier.
    // Held behind a shared_ptr because a reserved barrier can outlive the queue
    // (an erased Actor queue) and still has to lower the fence it raised.
    const std::shared_ptr<std::atomic<std::size_t>> _actor_handoff_fence_depth =
      std::make_shared<std::atomic<std::size_t>> (0);
};

} // namespace zlink::framework::runtime
