/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/execution/serial_execution_queue.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>
#include <memory>
#include <future>
#include <vector>

namespace zlink::framework::runtime
{

namespace
{

thread_local serial_execution_queue_t *current_shared_gate = nullptr;

std::size_t normalized_byte_cost (serial_work_options_t options) noexcept
{
    return options.byte_cost == 0 ? serial_execution_queue_t::fixed_work_byte_cost
                                  : options.byte_cost;
}

} // namespace

/* The queue position of work a handler deferred to its terminal. It is
 * pending until that terminal fills it (activate) or drops it (discard); the
 * queue does not take a pending slot (handler turn and execution gate §5, §7). */
struct serial_deferred_slot_t
{
    enum class state_t
    {
        pending,
        filled,
        dropped
    };

    std::mutex mutex;
    state_t state = state_t::pending;
    std::function<void ()> work;

    bool pending ()
    {
        std::lock_guard lock (mutex);
        return state == state_t::pending;
    }
};

/* One handler across its Yield continuations. Deferred work belongs to the
 * handler, so its terminal -- not a Yield -- settles it. */
struct serial_turn_chain_t
{
    struct entry_t
    {
        std::shared_ptr<serial_deferred_slot_t> slot;
        std::function<void ()> activate;
        std::function<void ()> cancel;
        bool runs_after_failure = false;
    };

    std::mutex mutex;
    std::vector<entry_t> deferred;

    std::vector<entry_t> take ()
    {
        std::lock_guard lock (mutex);
        return std::exchange (deferred, {});
    }
};

class serial_deferred_barrier_t final : public detail::deferred_barrier_t
{
  public:
    serial_deferred_barrier_t () = default;
    // on_terminal lowers the owning queue's Actor handoff fence. Its lifetime
    // must cover the whole window in which the transfer coordinator blocks
    // dispatch for this Actor, so it fires when the barrier turn's work has
    // finished (the relocation is over) or when the reservation is rolled back
    // -- never merely when the queue reaches the barrier, which would reopen
    // the same late-admission window while the move is still in flight.
    explicit serial_deferred_barrier_t (std::function<void ()> on_terminal) :
        _release (make_release (std::move (on_terminal)))
    {
    }

    // Set before the barrier is queued: the item keeps its turn while it waits.
    void hold_application () noexcept { _holds_application = true; }

    void reached (serial_execution_queue_t::async_completion_t complete)
    {
        std::function<void ()> work;
        async_work_t async_work;
        const auto turn = detail::capture_current_serial_turn ();
        {
            std::lock_guard lock (_mutex);
            if (_reached)
                return;
            _reached = true;
            _turn = turn;
            if (turn)
                _resume = turn->resume_scheduler ();
            _complete = std::move (complete);
            if (_state == state_t::pending)
                return;
            work = _state == state_t::activated ? std::move (_work) : std::function<void ()>{};
            async_work = _state == state_t::activated ? std::move (_async_work) : async_work_t{};
            complete = std::move (_complete);
        }
        if (!turn) {
            complete ([release = _release] {
                const release_scope_t released{release};
                throw std::logic_error ("lifecycle barrier reached outside its serial turn");
            });
            return;
        }
        finish (std::move (complete), std::move (work), std::move (async_work), _release);
    }

    result_t<void> activate (std::function<void ()> work) override
    {
        if (!work) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Deferred Actor join barrier work is empty");
        }
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending) {
                return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                "Deferred Actor join barrier is already terminal");
            }
            _state = state_t::activated;
            _work = std::move (work);
            if (!_reached)
                return result_t<void>::success ();
            complete = std::move (_complete);
            work = std::move (_work);
        }
        finish (std::move (complete), std::move (work), {}, _release);
        return result_t<void>::success ();
    }

    result_t<void> activate_async (async_work_t work) override
    {
        if (!work) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Deferred Actor join barrier async work is empty");
        }
        serial_execution_queue_t::async_completion_t complete;
        detail::task_scheduler_t resume;
        std::shared_ptr<detail::serial_turn_t> turn;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending) {
                return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                "Deferred Actor join barrier is already terminal");
            }
            _state = state_t::activated;
            _async_work = std::move (work);
            if (!_reached)
                return result_t<void>::success ();
            complete = std::move (_complete);
            work = std::move (_async_work);
            resume = _resume;
            turn = _turn;
        }
        if (_holds_application) {
            // The item kept its turn while it waited, so it finishes on it now.
            finish (std::move (complete), {}, std::move (work), _release);
            return result_t<void>::success ();
        }
        if (resume) {
            resume ([complete = std::move (complete), work = std::move (work), release = _release,
                     turn = std::move (turn)] () mutable {
                const auto active = detail::capture_current_serial_turn ();
                const auto failure = detail::take_serial_resume_failure ();
                if (failure || !active || active != turn || active->released ()) {
                    complete ([release, failure] {
                        const release_scope_t released{release};
                        throw framework_exception_t (
                          failure ? failure->kind : framework_error_kind_t::shutting_down,
                          failure ? failure->message
                                  : "lifecycle barrier lost its retained serial turn");
                    });
                    return;
                }
                finish (std::move (complete), {}, std::move (work), std::move (release));
            });
        } else {
            complete ([release = _release] {
                const release_scope_t released{release};
                throw std::logic_error ("lifecycle barrier has no retained serial turn");
            });
            return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                            "lifecycle barrier has no retained serial turn");
        }
        return result_t<void>::success ();
    }

    void cancel () noexcept override
    {
        // A rolled-back reservation releases the coordinator's source hold
        // right away, so the queue must stop fencing ordinary ingress here and
        // not wait for the queue to reach the (now inert) barrier item.
        if (_release)
            _release ();
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending)
                return;
            _state = state_t::cancelled;
            if (!_reached)
                return;
            complete = std::move (_complete);
        }
        finish (std::move (complete), {}, {}, {});
    }

  private:
    enum class state_t
    {
        pending,
        activated,
        cancelled
    };

    // Idempotent and copyable so the queue completion callbacks can own it
    // even after the barrier object itself is gone.
    static std::function<void ()> make_release (std::function<void ()> on_terminal)
    {
        if (!on_terminal)
            return {};
        auto once = std::make_shared<std::atomic_bool> (false);
        return [once, on_terminal = std::move (on_terminal)] () noexcept {
            if (once->exchange (true, std::memory_order_acq_rel))
                return;
            on_terminal ();
        };
    }

    struct release_scope_t
    {
        std::function<void ()> release;
        ~release_scope_t ()
        {
            if (release)
                release ();
        }
    };

    static void finish (serial_execution_queue_t::async_completion_t complete,
                        std::function<void ()> work,
                        async_work_t async_work,
                        std::function<void ()> release)
    {
        if (!complete) {
            if (release)
                release ();
            return;
        }
        if (async_work) {
            auto completion =
              std::make_shared<serial_execution_queue_t::async_completion_t> (std::move (complete));
            try {
                async_work ([completion, release] (result_t<void> result) mutable {
                    (*completion) ([result = std::move (result), release] () mutable {
                        const release_scope_t released{release};
                        result.value ();
                    });
                });
            }
            catch (...) {
                const auto error = std::current_exception ();
                (*completion) ([error, release] {
                    const release_scope_t released{release};
                    std::rethrow_exception (error);
                });
            }
            return;
        }
        complete ([work = std::move (work), release] () mutable {
            const release_scope_t released{release};
            if (work)
                work ();
        });
    }

    std::mutex _mutex;
    state_t _state = state_t::pending;
    bool _reached = false;
    serial_execution_queue_t::async_completion_t _complete;
    detail::task_scheduler_t _resume;
    bool _holds_application = false;
    std::shared_ptr<detail::serial_turn_t> _turn;
    std::function<void ()> _work;
    async_work_t _async_work;
    std::function<void ()> _release;
};

class serial_turn_handle_impl_t final
    : public detail::serial_turn_t,
      public std::enable_shared_from_this<serial_turn_handle_impl_t>
{
  public:
    serial_turn_handle_impl_t (serial_execution_queue_t &queue,
                               std::string name,
                               serial_work_lane_t lane,
                               bool after_active_phase,
                               std::shared_ptr<serial_turn_chain_t> chain) :
        _queue (queue),
        _name (std::move (name)),
        _lane (lane),
        _after_active_phase (after_active_phase),
        _chain (std::move (chain))
    {
    }

    void set_completion (serial_execution_queue_t::async_completion_t complete)
    {
        _complete = std::move (complete);
    }

    /* Yield: the queue turn ends, the handler does not. Its deferred work and,
     * in the lifecycle lane, its lane position pass to the continuation. */
    bool release () override
    {
        const bool released = finish ([] {}, false);
        if (released && _queue._lane_policy.allows_turn_yield () && current_shared_gate == &_queue)
            _queue.release_shared_gate ();
        return released;
    }

    bool released () const override
    {
        std::lock_guard lock (_mutex);
        return _released;
    }

    std::stop_token wait_cancellation () const override
    {
        std::lock_guard lock (_mutex);
        if (!_wait_stop.stop_possible ())
            _wait_stop = std::stop_source{};
        return _wait_stop.get_token ();
    }
    void cancel_waits () noexcept override
    {
        try {
            auto owner = [&] {
                std::lock_guard lock (_mutex);
                if (!_wait_stop.stop_possible ())
                    _wait_stop = std::stop_source{};
                return _wait_stop;
            }();
            owner.request_stop ();
        }
        catch (...) {
            _queue.report_deferred_error (_name, std::current_exception ());
        }
    }
    detail::task_scheduler_t resume_scheduler () override
    {
        return [self = shared_from_this ()] (std::function<void ()> work) mutable {
            auto continuation = std::move (work);
            if (self->_lane == serial_work_lane_t::lifecycle
                && self->_queue.try_resume_suspended (self, continuation))
                return;
            if (self->_queue.try_post_continuation (
                  self->_name + "-await-resume",
                  [work = continuation] (auto complete) mutable {
                      work ();
                      complete ([] {});
                  },
                  self->_lane, self->chain ())) {
                return;
            }
            // The continuation cannot reach the queue: the handler ends here.
            self->end_chain (false);
            if (continuation) {
                auto continuation_state =
                  std::make_shared<std::function<void ()>> (std::move (continuation));
                if (!self->_queue._executor.try_submit_internal (
                      [work = continuation_state] () mutable {
                          detail::set_serial_resume_failure (
                            framework_error_kind_t::shutting_down,
                            "serial execution queue executor is stopping");
                          (*work) ();
                          (void) detail::take_serial_resume_failure ();
                      })) {
                    detail::set_serial_resume_failure (
                      framework_error_kind_t::shutting_down,
                      "serial execution queue executor is stopping");
                    (*continuation_state) ();
                    (void) detail::take_serial_resume_failure ();
                }
            }
        };
    }

    bool belongs_to (const void *owner) const noexcept override { return owner == &_queue; }
    bool is_after_active_phase () const noexcept override { return _after_active_phase; }
    bool allows_yield () const noexcept override { return _queue.allows_yield (); }

    result_t<void> defer (std::function<void ()> work, std::function<void ()> cancel) override
    {
        return register_deferred (_name + "-deferred", std::move (work), std::move (cancel), false);
    }

    /* Registers work at its accepted lifecycle FIFO position now and settles
     * it at this turn's terminal (handler turn and execution gate §5, §7).
     * `runs_after_failure` keeps the work when the turn ends exceptionally;
     * otherwise that terminal discards it and runs `cancel`. */
    result_t<void> register_deferred (std::string name,
                                      std::function<void ()> work,
                                      std::function<void ()> cancel,
                                      bool runs_after_failure)
    {
        if (!work) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Deferred Actor join work is empty");
        }
        if (released ()) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Actor join defer requires an open Framework handler turn");
        }
        auto slot = std::make_shared<serial_deferred_slot_t> ();
        if (!_queue.enqueue_deferred_slot (std::move (name), slot)) {
            return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                            "serial execution queue is closed");
        }
        {
            const auto owner = chain ();
            std::lock_guard lock (owner->mutex);
            owner->deferred.push_back (serial_turn_chain_t::entry_t{
              std::move (slot), std::move (work), std::move (cancel), runs_after_failure});
        }
        return result_t<void>::success ();
    }

    /* The handler failed after a Yield: its dispatcher reports that terminal
     * here, because a continuation turn only ends a queue turn. */
    void cancel_deferred () noexcept override { settle (false); }

    bool complete (std::function<void ()> completion)
    {
        return finish (std::move (completion), true);
    }

  private:
    std::shared_ptr<serial_turn_chain_t> chain ()
    {
        std::lock_guard lock (_mutex);
        if (!_chain)
            _chain = std::make_shared<serial_turn_chain_t> ();
        return _chain;
    }

    bool finish (std::function<void ()> completion, bool handler_terminal)
    {
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_released) {
                return false;
            }
            _released = true;
            complete = std::move (_complete);
        }
        if (!complete) {
            return false;
        }
        if (!handler_terminal && _lane == serial_work_lane_t::lifecycle)
            _queue.hold_lifecycle (chain ());
        complete ([this, completion = std::move (completion), handler_terminal] () mutable {
            if (!handler_terminal) {
                if (completion)
                    completion ();
                return;
            }
            try {
                if (completion)
                    completion ();
            }
            catch (...) {
                end_chain (false);
                throw;
            }
            end_chain (true);
        });
        return true;
    }

    /* Handler terminal: settle the deferred work, then give up the lifecycle
     * lane position a Yield kept. */
    void end_chain (bool succeeded) noexcept
    {
        std::shared_ptr<serial_turn_chain_t> owner;
        {
            std::lock_guard lock (_mutex);
            owner = _chain;
        }
        if (!owner)
            return;
        settle (succeeded);
        _queue.release_lifecycle_hold (owner.get ());
    }

    /* Fill each accepted slot with its work, or drop it and run the
     * cancellation. A closed queue admits no activation. */
    void settle (bool succeeded) noexcept
    {
        std::shared_ptr<serial_turn_chain_t> owner;
        {
            std::lock_guard lock (_mutex);
            owner = _chain;
        }
        if (!owner)
            return;
        auto entries = owner->take ();
        if (entries.empty ())
            return;
        const bool open = !_queue.closed ();
        for (auto &entry : entries) {
            const bool activate = open && (succeeded || entry.runs_after_failure);
            bool stored = false;
            {
                std::lock_guard lock (entry.slot->mutex);
                if (entry.slot->state == serial_deferred_slot_t::state_t::pending) {
                    if (activate) {
                        entry.slot->work = std::move (entry.activate);
                        entry.slot->state = serial_deferred_slot_t::state_t::filled;
                        stored = true;
                    } else {
                        entry.slot->state = serial_deferred_slot_t::state_t::dropped;
                    }
                }
            }
            if (!stored) {
                if (entry.cancel)
                    entry.cancel ();
            }
        }
        _queue.schedule_after_settle ();
    }

    serial_execution_queue_t &_queue;
    std::string _name;
    serial_work_lane_t _lane = serial_work_lane_t::application;
    mutable std::stop_source _wait_stop{std::nostopstate};
    bool _after_active_phase = false;
    mutable std::mutex _mutex;
    std::shared_ptr<serial_turn_chain_t> _chain;
    serial_execution_queue_t::async_completion_t _complete;
    bool _released = false;
};

serial_execution_queue_t::serial_execution_queue_t (offload_executor_t &executor,
                                                    serial_execution_queue_options_t options,
                                                    error_handler_t error_handler,
                                                    serial_lane_policy_t policy) :
    _executor (executor),
    _options (options),
    _lane_policy (std::move (policy)),
    _error_handler (std::move (error_handler))
{
    if (_options.lifecycle_burst_limit == 0
        || _options.owner_time_budget < std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("serial execution queue limits are invalid");
    }
    if (_lane_policy.allows_turn_yield ())
        _publication_cursor = _publication_tail = new publication_node_t ();
}

serial_execution_queue_t::~serial_execution_queue_t ()
{
    close ();
    drain ();
    while (_publication_cursor) {
        auto *next = _publication_cursor->next.load (std::memory_order_acquire);
        delete _publication_cursor;
        _publication_cursor = next;
    }
}

bool serial_execution_queue_t::owns_shared_gate () const noexcept
{
    return _lane_policy.allows_turn_yield () || static_cast<bool> (_spot_gate);
}

void serial_execution_queue_t::attach_spot_gate (std::shared_ptr<serial_execution_queue_t> gate)
{
    if (!gate || !gate->_lane_policy.allows_turn_yield ())
        throw std::invalid_argument ("Actor mailbox requires a SpotWide gate");
    std::lock_guard lock (_mutex);
    if (_publication_cursor || _active != 0 || has_queued_locked ())
        throw std::logic_error ("Actor mailbox must be attached before admission");
    _publication_cursor = _publication_tail = new publication_node_t ();
    _spot_gate = std::move (gate);
}

bool serial_execution_queue_t::try_post (std::string name, std::function<void ()> work)
{
    return try_post (std::move (name), std::move (work), {});
}

bool serial_execution_queue_t::try_post (std::string name,
                                         std::function<void ()> work,
                                         serial_work_options_t options)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    return try_post_async (
      std::move (name),
      [work = std::move (work)] (auto complete) mutable {
          try {
              work ();
              complete ([] {});
          }
          catch (...) {
              auto error = std::current_exception ();
              complete ([error] { std::rethrow_exception (error); });
          }
      },
      std::move (options));
}

bool serial_execution_queue_t::try_post_async (std::string name, async_work_t work)
{
    return try_post_async (std::move (name), std::move (work), {});
}

bool serial_execution_queue_t::try_post_async (std::string name,
                                               async_work_t work,
                                               serial_work_options_t options)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    const auto transfer_owner_reservation = options.transfer_owner_reservation;
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (options.refuse_when_actor_handoff_fenced
            && _actor_handoff_fence_depth->load (std::memory_order_acquire) > 0) {
            /* An Actor handoff barrier is already reserved on this queue: this
             * ordinary ingress would land behind it and run on the old owner
             * after capture. Refuse so the caller re-admits it into the
             * transfer coordinator backlog (membership §903 capture set). */
            if (options.actor_handoff_fence_refused)
                *options.actor_handoff_fence_refused = true;
            return false;
        }
        if (_closed) {
            return false;
        }
        accepted = enqueue_locked (std::move (name), std::move (work), std::move (options));
    }
    if (accepted && transfer_owner_reservation) {
        [&] () noexcept { transfer_owner_reservation (); }();
    }
    return accepted;
}

result_t<serial_submission_id_t> serial_execution_queue_t::try_post_cancellable_async (
  std::string name, async_work_t work, std::function<void ()> cancel, serial_work_options_t options)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    if (!cancel) {
        throw std::invalid_argument ("serial execution queue cancellation is empty");
    }

    const auto transfer_owner_reservation = options.transfer_owner_reservation;
    serial_submission_id_t submission_id = 0;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed) {
            return result_t<serial_submission_id_t>::failure (framework_error_kind_t::shutting_down,
                                                              "serial execution queue is closed");
        }
        if (_next_submission_id == 0) {
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::internal_failure,
              "serial execution queue submission identifiers are exhausted");
        }

        submission_id = _next_submission_id++;
        if (!enqueue_locked (std::move (name), std::move (work), std::move (options), submission_id,
                             std::move (cancel))) {
            --_next_submission_id;
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::shutting_down, "serial execution queue executor is stopping");
        }
    }
    if (transfer_owner_reservation) {
        [&] () noexcept { transfer_owner_reservation (); }();
    }
    return result_t<serial_submission_id_t>::success (submission_id);
}

serial_cancel_submission_outcome_t
serial_execution_queue_t::cancel_submission (serial_submission_id_t submission_id) noexcept
{
    if (submission_id == 0)
        return serial_cancel_submission_outcome_t::already_terminal;
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        auto outcome = serial_cancel_submission_outcome_t::already_terminal;
        try {
            wait_for_control ([this, submission_id, &outcome] {
                import_publications ();
                outcome = cancel_submission (submission_id);
            });
        }
        catch (const std::runtime_error &) {
            report_deferred_error ("cancel-submission", std::current_exception ());
        }
        return outcome;
    }

    std::optional<work_item_t> cancelled_item;
    std::function<void ()> active_cancel;
    serial_cancel_submission_outcome_t outcome =
      serial_cancel_submission_outcome_t::already_terminal;
    {
        std::unique_lock<std::mutex> lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        const auto unlink = [&] (lane_state_t &lane) {
            const auto item =
              std::find_if (lane.queue.begin (), lane.queue.end (), [&] (const auto &candidate) {
                  return candidate.submission_id == submission_id;
              });
            if (item == lane.queue.end ())
                return false;
            if (lane.messages > 0)
                --lane.messages;
            if (owns_shared_gate ())
                lane.bytes.fetch_sub (item->byte_cost, std::memory_order_acq_rel);
            else if (item->byte_cost <= lane.bytes)
                lane.bytes -= item->byte_cost;
            else
                lane.bytes = 0;
            cancelled_item.emplace (std::move (*item));
            lane.queue.erase (item);
            if (owns_shared_gate ())
                lane.messages.notify_all ();
            return true;
        };

        if (unlink (_application) || unlink (_lifecycle)) {
            outcome = serial_cancel_submission_outcome_t::queued_cancelled;
            if (!has_queued_locked () && _active == 0 && !_draining && !_drain_scheduled) {
                _empty.notify_all ();
            }
        } else {
            if (_active_turn && _active_turn->submission_id == submission_id && _active_turn->turn
                && !_active_turn->turn->released ()) {
                outcome = serial_cancel_submission_outcome_t::active_cancel_requested;
                if (!_active_turn->cancel_requested) {
                    _active_turn->cancel_requested = true;
                    active_cancel = std::move (_active_turn->cancel);
                }
            } else if (_suspended_lifecycle && _suspended_lifecycle->submission_id == submission_id
                       && !_suspended_lifecycle->turn->released ()) {
                outcome = serial_cancel_submission_outcome_t::active_cancel_requested;
                if (!_suspended_lifecycle->cancel_requested) {
                    _suspended_lifecycle->cancel_requested = true;
                    active_cancel = std::move (_suspended_lifecycle->cancel);
                }
            }
        }
    }

    if (cancelled_item && cancelled_item->cancel)
        cancelled_item->cancel ();
    if (active_cancel)
        active_cancel ();
    if (_spot_gate && outcome == serial_cancel_submission_outcome_t::queued_cancelled)
        _spot_gate->disconnect_actor_head (shared_from_this ());
    if (_spot_gate)
        _spot_gate->connect_actor_head (shared_from_this ());
    return outcome;
}

bool serial_execution_queue_t::post_async_wait (std::string name,
                                                async_work_t work,
                                                std::function<bool ()> stop_requested)
{
    return post_async_wait (std::move (name), std::move (work), {}, std::move (stop_requested));
}

bool serial_execution_queue_t::post_async_wait (std::string name,
                                                async_work_t work,
                                                serial_work_options_t options,
                                                std::function<bool ()> stop_requested)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    const auto transfer_owner_reservation = options.transfer_owner_reservation;
    bool accepted = false;
    {
        std::unique_lock lock (_mutex);
        if (_closed || (stop_requested && stop_requested ())) {
            return false;
        }
        accepted = enqueue_locked (std::move (name), std::move (work), std::move (options));
    }
    if (accepted && transfer_owner_reservation) {
        [&] () noexcept { transfer_owner_reservation (); }();
    }
    return accepted;
}

bool serial_execution_queue_t::try_post_deferred (std::string name, std::function<void ()> work)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    // The caller owns the current turn of this queue; the work keeps its
    // accepted position and runs after that turn ends, whatever its outcome.
    auto *turn =
      dynamic_cast<serial_turn_handle_impl_t *> (detail::capture_current_serial_turn ().get ());
    if (turn == nullptr || !turn->belongs_to (this))
        return false;
    return static_cast<bool> (
      turn->register_deferred (std::move (name), std::move (work), {}, true));
}

bool serial_execution_queue_t::enqueue_deferred_slot (std::string name,
                                                      std::shared_ptr<serial_deferred_slot_t> slot)
{
    bool accepted;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed)
            return false;
        accepted = enqueue_locked (
          std::move (name),
          [slot] (auto complete) mutable {
              std::function<void ()> work;
              {
                  std::lock_guard slot_lock (slot->mutex);
                  work = std::move (slot->work);
              }
              try {
                  if (work)
                      work ();
                  complete ([] {});
              }
              catch (...) {
                  const auto error = std::current_exception ();
                  complete ([error] { std::rethrow_exception (error); });
              }
          },
          serial_work_options_t{serial_work_lane_t::lifecycle}, 0,
          [slot] {
              std::lock_guard slot_lock (slot->mutex);
              slot->state = serial_deferred_slot_t::state_t::dropped;
              slot->work = {};
          },
          item_origin_t{{}, slot});
    }
    return accepted;
}

bool serial_execution_queue_t::try_post_continuation (std::string name,
                                                      async_work_t work,
                                                      serial_work_lane_t lane,
                                                      std::shared_ptr<serial_turn_chain_t> chain)
{
    bool accepted;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed)
            return false;
        accepted = enqueue_locked (std::move (name), std::move (work), serial_work_options_t{lane},
                                   0, {}, item_origin_t{std::move (chain), {}});
    }
    return accepted;
}

void serial_execution_queue_t::hold_lifecycle (std::shared_ptr<serial_turn_chain_t> chain)
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        wait_for_control (
          [this, chain = std::move (chain)] () mutable { hold_lifecycle (std::move (chain)); });
        return;
    }
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    _lifecycle_hold = std::move (chain);
    if (_spot_gate)
        _spot_gate->connect_actor_head (shared_from_this ());
}

void serial_execution_queue_t::release_lifecycle_hold (const serial_turn_chain_t *chain)
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        wait_for_control ([this, chain] { release_lifecycle_hold (chain); });
        return;
    }
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    if (_lifecycle_hold.get () != chain)
        return;
    _lifecycle_hold.reset ();
    (void) schedule_drain_locked ();
}

void serial_execution_queue_t::schedule_after_settle ()
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        wait_for_control ([this] { schedule_after_settle (); });
        return;
    }
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    (void) schedule_drain_locked ();
}

result_t<std::shared_ptr<detail::deferred_barrier_t>>
serial_execution_queue_t::reserve_barrier_next (std::string name,
                                                bool holds_application_while_waiting)
{
    auto barrier = std::make_shared<serial_deferred_barrier_t> ();
    if (holds_application_while_waiting)
        barrier->hold_application ();
    const auto submission = try_post_cancellable_async (
      std::move (name),
      [barrier] (auto complete) mutable { barrier->reached (std::move (complete)); },
      [barrier] { barrier->cancel (); },
      [holds_application_while_waiting] {
          serial_work_options_t options{serial_work_lane_t::lifecycle};
          options.holds_application_while_waiting = holds_application_while_waiting;
          return options;
      }());
    if (!submission) {
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          submission.error_kind (), "Deferred Actor join target queue is closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (std::move (barrier));
}

result_t<std::shared_ptr<detail::deferred_barrier_t>>
serial_execution_queue_t::reserve_handoff_barrier (std::string name)
{
    /* Raise the ordinary-ingress fence BEFORE the barrier enters the queue.
     * A post that is accepted afterwards must have read the fence while
     * holding _mutex, so "accepted" implies it was enqueued ahead of this
     * barrier; anything that reads the raised fence is refused and re-admitted
     * into the transfer coordinator backlog by its caller. The conservative
     * direction (refused while the barrier is not yet enqueued) is safe: the
     * coordinator source reservation always precedes this call. */
    _actor_handoff_fence_depth->fetch_add (1, std::memory_order_acq_rel);
    auto lower_fence = [depth_owner = _actor_handoff_fence_depth] {
        auto depth = depth_owner->load (std::memory_order_acquire);
        while (depth > 0
               && !depth_owner->compare_exchange_weak (depth, depth - 1, std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        }
    };
    auto barrier = std::make_shared<serial_deferred_barrier_t> (lower_fence);
    barrier->hold_application ();
    const auto submission = try_post_cancellable_async (
      std::move (name),
      [barrier] (auto complete) mutable { barrier->reached (std::move (complete)); },
      [barrier] { barrier->cancel (); },
      [] {
          serial_work_options_t options{serial_work_lane_t::application};
          options.holds_application_while_waiting = true;
          return options;
      }());
    if (!submission) {
        lower_fence ();
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          submission.error_kind (), "Deferred Actor handoff barrier queue is closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (std::move (barrier));
}

void serial_execution_queue_t::post (std::string name, std::function<void ()> work)
{
    post (std::move (name), std::move (work), {});
}

void serial_execution_queue_t::post (std::string name,
                                     std::function<void ()> work,
                                     serial_work_options_t options)
{
    if (!try_post (std::move (name), std::move (work), std::move (options))) {
        throw std::runtime_error ("serial execution queue is closed or stopping");
    }
}

void serial_execution_queue_t::post_async (std::string name, async_work_t work)
{
    post_async (std::move (name), std::move (work), {});
}

void serial_execution_queue_t::post_async (std::string name,
                                           async_work_t work,
                                           serial_work_options_t options)
{
    if (!try_post_async (std::move (name), std::move (work), std::move (options))) {
        throw std::runtime_error ("serial execution queue is closed or stopping");
    }
}

void serial_execution_queue_t::run (std::string name, std::function<void ()> work)
{
    post (std::move (name), std::move (work));
    drain ();
}

void serial_execution_queue_t::drain ()
{
    if (owns_shared_gate ()) {
        for (;;) {
            const auto applications = _application.messages.load (std::memory_order_acquire);
            const auto lifecycle = _lifecycle.messages.load (std::memory_order_acquire);
            if (applications > 0)
                _application.messages.wait (applications, std::memory_order_acquire);
            else if (lifecycle > 0)
                _lifecycle.messages.wait (lifecycle, std::memory_order_acquire);
            else if (!_spot_gate) {
                const auto state = _gate_state.load (std::memory_order_acquire);
                if (state != gate_state_t::idle)
                    _gate_state.wait (state, std::memory_order_acquire);
                else {
                    std::lock_guard producer (_mutex);
                    if (_gate_state.load (std::memory_order_acquire) == gate_state_t::idle
                        && _application.messages.load (std::memory_order_acquire) == 0
                        && _lifecycle.messages.load (std::memory_order_acquire) == 0)
                        return;
                }
            } else {
                return;
            }
        }
    }
    std::unique_lock<std::mutex> lock (_mutex);
    _empty.wait (lock, [&] {
        return !has_queued_locked () && _active == 0 && !_draining && !_drain_scheduled;
    });
}

std::shared_ptr<const void> serial_execution_queue_t::first_pending_message_or_close ()
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        std::shared_ptr<const void> result;
        wait_for_control ([this, &result] { result = first_pending_message_or_close (); });
        return result;
    }
    if (owns_shared_gate ()) {
        for (;;) {
            publication_node_t *cut;
            {
                std::lock_guard producer (_mutex);
                cut = _publication_tail;
            }
            import_publications_until (cut);
            for (const auto &item : _application.queue)
                if (item.retained_message)
                    return item.retained_message;
            std::lock_guard producer (_mutex);
            if (_publication_tail == cut) {
                close_locked ();
                return {};
            }
        }
    }
    std::lock_guard lock (_mutex);
    for (const auto &item : _application.queue)
        if (item.retained_message)
            return item.retained_message;
    close_locked ();
    return {};
}

std::vector<std::shared_ptr<const void>> serial_execution_queue_t::pending_messages () const
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        std::vector<std::shared_ptr<const void>> result;
        const_cast<serial_execution_queue_t *> (this)->wait_for_control (
          [this, &result] { result = pending_messages (); });
        return result;
    }
    publication_node_t *cut = nullptr;
    if (owns_shared_gate ()) {
        std::lock_guard producer (_mutex);
        cut = _publication_tail;
    }
    if (owns_shared_gate ())
        const_cast<serial_execution_queue_t *> (this)->import_publications_until (cut);
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    std::vector<std::shared_ptr<const void>> messages;
    for (const auto &item : _application.queue)
        if (item.retained_message)
            messages.push_back (item.retained_message);
    return messages;
}

void serial_execution_queue_t::close ()
{
    std::lock_guard<std::mutex> lock (_mutex);
    close_locked ();
}

void serial_execution_queue_t::close_locked ()
{
    _closed = true;
    if (owns_shared_gate ())
        return;
    if (!has_queued_locked () && _active == 0 && !_draining && !_drain_scheduled) {
        _empty.notify_all ();
    }
}

void serial_execution_queue_t::cancel_waits () noexcept
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        try {
            wait_for_control ([this] { cancel_waits (); });
        }
        catch (const std::runtime_error &) {
            report_deferred_error ("cancel-waits", std::current_exception ());
        }
        return;
    }
    std::vector<std::shared_ptr<detail::serial_turn_t>> turns;
    {
        std::unique_lock lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        if (_active_turn && _active_turn->turn && !_active_turn->turn->released ())
            turns.push_back (_active_turn->turn);
        if (_suspended_lifecycle && _suspended_lifecycle->turn
            && !_suspended_lifecycle->turn->released ())
            turns.push_back (_suspended_lifecycle->turn);
    }
    for (const auto &turn : turns)
        turn->cancel_waits ();
}

void serial_execution_queue_t::cancel_pending () noexcept
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        close ();
        try {
            wait_for_control ([this] {
                import_publications ();
                cancel_pending ();
            });
        }
        catch (const std::runtime_error &) {
            report_deferred_error ("cancel-pending", std::current_exception ());
        }
        return;
    }
    std::vector<work_item_t> cancelled_items;
    std::vector<std::function<void ()>> active_cancellations;
    {
        std::unique_lock<std::mutex> lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        _closed = true;
        cancelled_items.reserve (_application.queue.size () + _lifecycle.queue.size ());
        const auto clear_queued = [this, &cancelled_items] (lane_state_t &lane) {
            std::size_t queued_bytes = 0;
            std::size_t removed = 0;
            const auto end = std::remove_if (
              lane.queue.begin (), lane.queue.end (),
              [this, &cancelled_items, &queued_bytes, &removed] (work_item_t &item) {
                  // Actor claims still drain through this gate.
                  // Their closed-Spot rejection or lifecycle
                  // terminal is decided by the Actor mailbox.
                  if (_lane_policy.allows_turn_yield () && item.actor_mailbox)
                      return false;
                  queued_bytes += item.byte_cost;
                  ++removed;
                  cancelled_items.push_back (std::move (item));
                  return true;
              });
            lane.queue.erase (end, lane.queue.end ());
            lane.messages -= removed;
            if (owns_shared_gate ())
                lane.bytes.fetch_sub (queued_bytes, std::memory_order_acq_rel);
            else
                lane.bytes = queued_bytes <= lane.bytes ? lane.bytes - queued_bytes : 0;
            if (owns_shared_gate ())
                lane.messages.notify_all ();
        };
        clear_queued (_application);
        clear_queued (_lifecycle);
        if (_active_turn && _active_turn->turn && !_active_turn->turn->released ()) {
            if (_active_turn->submission_id != 0 && !_active_turn->cancel_requested) {
                _active_turn->cancel_requested = true;
                if (_active_turn->cancel) {
                    active_cancellations.push_back (std::move (_active_turn->cancel));
                }
            }
        }
        if (_suspended_lifecycle && _suspended_lifecycle->submission_id != 0
            && !_suspended_lifecycle->cancel_requested
            && !_suspended_lifecycle->turn->released ()) {
            _suspended_lifecycle->cancel_requested = true;
            if (_suspended_lifecycle->cancel)
                active_cancellations.push_back (std::move (_suspended_lifecycle->cancel));
        }
    }
    cancel_waits ();
    for (auto &item : cancelled_items) {
        if (item.cancel)
            item.cancel ();
    }
    for (auto &cancel : active_cancellations)
        cancel ();
    if (_spot_gate)
        _spot_gate->disconnect_actor_head (shared_from_this ());
}

std::size_t serial_execution_queue_t::pending_count () const
{
    if (owns_shared_gate ())
        return _application.messages.load (std::memory_order_acquire)
               + _lifecycle.messages.load (std::memory_order_acquire);
    std::lock_guard<std::mutex> lock (_mutex);
    return _application.messages + _lifecycle.messages;
}

std::size_t serial_execution_queue_t::pending_count (serial_work_lane_t lane) const
{
    if (owns_shared_gate ())
        return lane_locked (lane).messages.load (std::memory_order_acquire);
    std::lock_guard<std::mutex> lock (_mutex);
    return lane_locked (lane).messages;
}

std::size_t serial_execution_queue_t::pending_bytes () const
{
    if (owns_shared_gate ())
        return _application.bytes.load (std::memory_order_acquire)
               + _lifecycle.bytes.load (std::memory_order_acquire);
    std::lock_guard<std::mutex> lock (_mutex);
    return _application.bytes + _lifecycle.bytes;
}

bool serial_execution_queue_t::closed () const
{
    if (owns_shared_gate ())
        return _closed.load (std::memory_order_acquire);
    std::lock_guard<std::mutex> lock (_mutex);
    return _closed;
}

bool serial_execution_queue_t::schedule_drain_locked ()
{
    if (_spot_gate) {
        if (current_shared_gate == _spot_gate.get ())
            _spot_gate->connect_actor_head (shared_from_this ());
        return true;
    }
    if (_lane_policy.allows_turn_yield ()) {
        auto idle = gate_state_t::idle;
        if (!_gate_state.compare_exchange_strong (idle, gate_state_t::scheduled,
                                                  std::memory_order_acq_rel))
            return true;
        if (_executor.try_submit_internal (
              [this, retained = weak_from_this ().lock ()] { run_shared_gate (); }))
            return true;
        _gate_state.store (gate_state_t::idle, std::memory_order_release);
        _gate_state.notify_all ();
        return false;
    }
    if (_drain_scheduled || _draining || !has_ready_locked ()) {
        return true;
    }
    if (!_claim_started_at)
        _claim_started_at = std::chrono::steady_clock::now ();
    _drain_scheduled = true;
    if (!_executor.try_submit_internal ([this] { drain_loop (); })) {
        _drain_scheduled = false;
        _claim_started_at.reset ();
        return false;
    }
    return true;
}

bool serial_execution_queue_t::publish_locked (std::unique_ptr<publication_node_t> node)
{
    auto *previous = _publication_tail;
    auto *published = node.release ();
    previous->next.store (published, std::memory_order_release);
    _publication_tail = published;
    const bool scheduled = _spot_gate ? _spot_gate->submit_control ([actor = shared_from_this ()] {
        actor->import_publications ();
        actor->_spot_gate->connect_actor_head (actor);
    })
                                      : schedule_drain_locked ();
    if (scheduled)
        return true;
    previous->next.store (nullptr, std::memory_order_release);
    _publication_tail = previous;
    delete published;
    return false;
}

void serial_execution_queue_t::import_publications ()
{
    import_publications_until (nullptr);
}

void serial_execution_queue_t::import_publications_until (publication_node_t *cut)
{
    while (_publication_cursor != cut) {
        auto *next = _publication_cursor->next.load (std::memory_order_acquire);
        if (!next)
            break;
        if (next->item) {
            lane_locked (next->item->lane).queue.push_back (std::move (*next->item));
            next->item.reset ();
        }
        auto control = std::move (next->control);
        delete _publication_cursor;
        _publication_cursor = next;
        if (control)
            control ();
        if (_lane_policy.allows_turn_yield () && current_shared_gate != this)
            break;
    }
}

bool serial_execution_queue_t::submit_control (std::function<void ()> control)
{
    auto *gate = _spot_gate ? _spot_gate.get () : this;
    if (current_shared_gate == gate) {
        control ();
        return true;
    }
    if (_spot_gate)
        return _spot_gate->submit_control (std::move (control));
    auto node = std::make_unique<publication_node_t> ();
    node->control = std::move (control);
    std::lock_guard lock (_mutex);
    return publish_locked (std::move (node));
}

void serial_execution_queue_t::wait_for_control (std::function<void ()> control)
{
    auto *gate = _spot_gate ? _spot_gate.get () : this;
    if (current_shared_gate == gate) {
        control ();
        return;
    }
    auto task = std::make_shared<std::packaged_task<void ()>> (std::move (control));
    auto result = task->get_future ();
    if (!gate->submit_control ([task] { (*task) (); }))
        throw std::runtime_error ("shared Spot gate executor is stopping");
    result.get ();
}

void serial_execution_queue_t::run_shared_gate ()
{
    _gate_state.store (gate_state_t::running, std::memory_order_release);
    auto *previous = std::exchange (current_shared_gate, this);
    for (;;) {
        import_publications ();
        if (current_shared_gate != this)
            break;
        if (_active == 0 && has_ready_locked () && _claim_started_at
            && _options.owner_time_budget.count () != 0
            && std::chrono::steady_clock::now () - *_claim_started_at
                 >= _options.owner_time_budget) {
            _claim_started_at.reset ();
            _gate_state.store (gate_state_t::scheduled, std::memory_order_release);
            if (_executor.try_submit_internal (
                  [this, retained = weak_from_this ().lock ()] { run_shared_gate (); })) {
                current_shared_gate = previous;
                return;
            }
            _gate_state.store (gate_state_t::running, std::memory_order_release);
        }
        if (_active == 0 && has_ready_locked ()) {
            if (!_claim_started_at)
                _claim_started_at = std::chrono::steady_clock::now ();
            auto item = take_next_locked ();
            ++_active;
            _active_lane = item.lane;
            _active_bytes = item.byte_cost;
            if (item.actor_mailbox)
                execute_actor_head (std::move (item));
            else
                execute_item (std::move (item));
            if (current_shared_gate != this)
                break;
            continue;
        }
        if (_active == 0)
            _claim_started_at.reset ();
        std::lock_guard producer (_mutex);
        if (_publication_cursor->next.load (std::memory_order_acquire))
            continue;
        _gate_state.store (gate_state_t::idle, std::memory_order_release);
        _gate_state.notify_all ();
        break;
    }
    current_shared_gate = previous;
}

void serial_execution_queue_t::release_shared_gate ()
{
    if (current_shared_gate != this || !_lane_policy.allows_turn_yield ())
        return;
    std::lock_guard producer (_mutex);
    current_shared_gate = nullptr;
    _gate_state.store (gate_state_t::idle, std::memory_order_release);
    _gate_state.notify_all ();
    if (has_ready_locked () || _publication_cursor->next.load (std::memory_order_acquire))
        (void) schedule_drain_locked ();
}

serial_execution_queue_t::lane_state_t &
serial_execution_queue_t::lane_locked (serial_work_lane_t lane) noexcept
{
    return lane == serial_work_lane_t::application ? _application : _lifecycle;
}

const serial_execution_queue_t::lane_state_t &
serial_execution_queue_t::lane_locked (serial_work_lane_t lane) const noexcept
{
    return lane == serial_work_lane_t::application ? _application : _lifecycle;
}

bool serial_execution_queue_t::enqueue_locked (std::string name,
                                               async_work_t work,
                                               serial_work_options_t options,
                                               serial_submission_id_t submission_id,
                                               std::function<void ()> cancel,
                                               item_origin_t origin)
{
    const auto bytes = normalized_byte_cost (options);
    auto &lane = lane_locked (options.lane);
    auto turn = create_turn (name, options.lane, origin);
    work_item_t item{std::move (name),
                     std::move (work),
                     options.lane,
                     bytes,
                     submission_id,
                     std::move (cancel),
                     std::move (turn),
                     false,
                     {},
                     options.holds_application_while_waiting,
                     std::move (origin.chain),
                     std::move (origin.slot),
                     std::move (options.retained_message)};
    if (owns_shared_gate ()) {
        auto node = std::make_unique<publication_node_t> ();
        node->item.emplace (std::move (item));
        ++lane.messages;
        lane.bytes += bytes;
        if (publish_locked (std::move (node)))
            return true;
        --lane.messages;
        lane.bytes -= bytes;
        return false;
    }
    lane.queue.push_back (std::move (item));
    ++lane.messages;
    lane.bytes += bytes;
    if (schedule_drain_locked ())
        return true;
    lane.queue.pop_back ();
    --lane.messages;
    lane.bytes -= bytes;
    return false;
}

bool serial_execution_queue_t::has_queued_locked () const noexcept
{
    return !_application.queue.empty () || !_lifecycle.queue.empty () || _suspended_lifecycle;
}

std::deque<serial_execution_queue_t::work_item_t>::iterator
serial_execution_queue_t::next_lifecycle_locked () noexcept
{
    auto &queue = _lifecycle.queue;
    if (_lifecycle_hold) {
        return std::find_if (queue.begin (), queue.end (), [this] (const work_item_t &item) {
            return item.chain == _lifecycle_hold;
        });
    }
    if (queue.empty () || (queue.front ().slot && queue.front ().slot->pending ()))
        return queue.end ();
    return queue.begin ();
}

bool serial_execution_queue_t::has_ready_locked () noexcept
{
    return application_ready_locked ()
           || (_suspended_lifecycle
               && (bool (_suspended_lifecycle->work)
                   || bool (_suspended_lifecycle->suspended_completion)))
           || (!_suspended_lifecycle && next_lifecycle_locked () != _lifecycle.queue.end ());
}

bool serial_execution_queue_t::application_ready_locked () const noexcept
{
    return !_application.queue.empty ()
           && (!_suspended_lifecycle || !_suspended_lifecycle->holds_application_while_waiting);
}

std::shared_ptr<serial_turn_handle_impl_t> serial_execution_queue_t::create_turn (
  const std::string &name, serial_work_lane_t lane, item_origin_t origin)
{
    auto turn = std::make_shared<serial_turn_handle_impl_t> (
      *this, name, lane, static_cast<bool> (origin.slot), std::move (origin.chain));
    turn->set_completion ([this, name, identity = std::weak_ptr<serial_turn_handle_impl_t> (turn)] (
                            std::function<void ()> completion) mutable {
        const auto held_turn = identity.lock ();
        if (!held_turn) {
            report_deferred_error (name, std::make_exception_ptr (std::logic_error (
                                           "serial execution queue completion lost its turn")));
            return;
        }
        const bool queue_closed = _closed.load (std::memory_order_acquire);
        if (owns_shared_gate ()) {
            auto state = std::make_shared<std::pair<std::string, std::function<void ()>>> (
              std::move (name), std::move (completion));
            if (!submit_control ([this, state, held_turn] () mutable {
                    complete_turn (std::move (state->first), std::move (state->second), held_turn,
                                   true);
                }))
                report_deferred_error (state->first,
                                       std::make_exception_ptr (std::runtime_error (
                                         "shared Spot gate completion submission failed")));
            return;
        }
        if (queue_closed) {
            complete_turn (std::move (name), std::move (completion), held_turn, false);
            return;
        }
        auto completion_state = std::make_shared<std::pair<std::string, std::function<void ()>>> (
          std::move (name), std::move (completion));
        if (!_executor.try_submit_internal ([this, completion_state, held_turn] () mutable {
                complete_turn (std::move (completion_state->first),
                               std::move (completion_state->second), held_turn, true);
            })) {
            complete_turn (std::move (completion_state->first),
                           std::move (completion_state->second), held_turn, false);
        }
    });
    return turn;
}

void serial_execution_queue_t::activate_turn_locked (work_item_t &item) noexcept
{
    assert (!_active_turn.has_value ());
    assert (item.turn);
    _active_turn.emplace ();
    _active_turn->submission_id = item.submission_id;
    _active_turn->turn = item.turn;
    _active_turn->cancel = std::move (item.cancel);
    _active_turn->cancel_requested = item.cancel_requested;
    item.cancel = {};
}

serial_work_lane_t serial_execution_queue_t::select_next_lane_locked () noexcept
{
    const bool application_ready = application_ready_locked ();
    const auto lifecycle_next = next_lifecycle_locked ();
    const bool lifecycle_ready =
      _suspended_lifecycle
        ? bool (_suspended_lifecycle->work) || bool (_suspended_lifecycle->suspended_completion)
        : lifecycle_next != _lifecycle.queue.end ();
    serial_work_lane_t selected_lane = serial_work_lane_t::application;
    if (lifecycle_ready && !application_ready) {
        selected_lane = serial_work_lane_t::lifecycle;
    } else if (lifecycle_ready && application_ready) {
        const bool lifecycle_must_yield =
          _lifecycle_debt || _lifecycle_streak >= _options.lifecycle_burst_limit;
        if (!lifecycle_must_yield)
            selected_lane = serial_work_lane_t::lifecycle;
    }

    return selected_lane;
}

serial_execution_queue_t::work_item_t
serial_execution_queue_t::take_next_locked (std::optional<serial_work_lane_t> forced_lane)
{
    const auto selected_lane = forced_lane.value_or (select_next_lane_locked ());
    const bool application_ready = application_ready_locked ();
    const auto lifecycle_next = next_lifecycle_locked ();
    work_item_t item;
    if (selected_lane == serial_work_lane_t::lifecycle && _suspended_lifecycle) {
        item = std::move (*_suspended_lifecycle);
        _suspended_lifecycle.reset ();
    } else {
        auto &queue = lane_locked (selected_lane).queue;
        const auto next =
          selected_lane == serial_work_lane_t::lifecycle ? lifecycle_next : queue.begin ();
        item = std::move (*next);
        queue.erase (next);
    }
    activate_turn_locked (item);
    if (selected_lane == serial_work_lane_t::lifecycle) {
        ++_lifecycle_streak;
        if (application_ready && _lifecycle_streak >= _options.lifecycle_burst_limit)
            _lifecycle_debt = true;
    } else {
        _lifecycle_streak = 0;
        _lifecycle_debt = false;
    }
    return item;
}

void serial_execution_queue_t::connect_actor_head (
  const std::shared_ptr<serial_execution_queue_t> &actor)
{
    assert (current_shared_gate == this);
    actor->import_publications ();
    if (actor->_active != 0 || !actor->has_ready_locked ())
        return;
    const auto lane = actor->select_next_lane_locked ();
    const auto already_queued = [&actor] (const lane_state_t &ready_lane) {
        return std::find_if (
                 ready_lane.queue.begin (), ready_lane.queue.end (),
                 [&actor] (const work_item_t &item) { return item.actor_mailbox == actor; })
               != ready_lane.queue.end ();
    };
    if (already_queued (lane_locked (lane)))
        return;
    if (already_queued (lane_locked (lane == serial_work_lane_t::application
                                       ? serial_work_lane_t::lifecycle
                                       : serial_work_lane_t::application)))
        disconnect_actor_head (actor);
    const auto bytes = fixed_work_byte_cost;
    work_item_t ready;
    ready.name = "spot-actor-head";
    ready.lane = lane;
    ready.byte_cost = bytes;
    ready.turn = create_turn (ready.name, lane, {});
    ready.actor_mailbox = actor;
    auto &destination = lane_locked (lane);
    destination.queue.push_back (std::move (ready));
    ++destination.messages;
    destination.bytes += bytes;
}

void serial_execution_queue_t::disconnect_actor_head (
  const std::shared_ptr<serial_execution_queue_t> &actor)
{
    assert (current_shared_gate == this);
    for (auto *lane : {&_application, &_lifecycle}) {
        const auto found =
          std::find_if (lane->queue.begin (), lane->queue.end (),
                        [&actor] (const work_item_t &item) { return item.actor_mailbox == actor; });
        if (found == lane->queue.end ())
            continue;
        lane->queue.erase (found);
        --lane->messages;
        lane->bytes.fetch_sub (fixed_work_byte_cost, std::memory_order_acq_rel);
        lane->messages.notify_all ();
    }
}

void serial_execution_queue_t::execute_actor_head (work_item_t spot_item)
{
    auto actor = spot_item.actor_mailbox;
    actor->import_publications ();
    if (!actor->has_ready_locked ()) {
        (void) spot_item.turn->complete ([] {});
        return;
    }
    if (actor->select_next_lane_locked () != spot_item.lane) {
        (void) spot_item.turn->complete ([this, actor] { connect_actor_head (actor); });
        return;
    }
    auto actor_item = std::make_shared<work_item_t> (actor->take_next_locked (spot_item.lane));
    ++actor->_active;
    actor->_active_lane = actor_item->lane;
    actor->_active_bytes = actor_item->byte_cost;
    if (actor_item->suspended_completion) {
        actor->complete_one (actor_item->name, std::move (*actor_item->suspended_completion));
        (void) spot_item.turn->complete ([] {});
        return;
    }
    auto spot_turn = spot_item.turn;
    auto actor_turn = actor_item->turn;
    spot_item.work = [actor, actor_item, spot_turn, actor_turn] (auto spot_complete) mutable {
        actor_item->work ([spot_complete = std::move (spot_complete), spot_turn,
                           actor_turn] (std::function<void ()> finish) mutable {
            if (spot_turn && spot_turn->released ()) {
                (void) actor_turn->complete (std::move (finish));
                return;
            }
            spot_complete ([actor_turn, finish = std::move (finish)] () mutable {
                (void) actor_turn->complete (std::move (finish));
            });
        });
        if (actor_item->lane == serial_work_lane_t::application
            && actor_item->holds_application_while_waiting && !actor_turn->released ()
            && !spot_turn->released ())
            (void) spot_turn->release ();
        if (actor_item->lane == serial_work_lane_t::lifecycle && !actor_turn->released ()
            && !spot_turn->released ()) {
            actor->suspend_lifecycle (std::move (*actor_item));
            (void) spot_turn->release ();
        }
    };
    execute_item (std::move (spot_item));
}

void serial_execution_queue_t::report_deferred_error (
  const std::string &name, const std::exception_ptr &error) const noexcept
{
    if (!_error_handler)
        return;
    _error_handler (name, error);
}

void serial_execution_queue_t::drain_loop ()
{
    work_item_t item;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _drain_scheduled = false;
        if (!has_ready_locked ()) {
            _draining = false;
            _claim_started_at.reset ();
            if (_active == 0)
                _empty.notify_all ();
            return;
        }
        _draining = true;
        item = take_next_locked ();
        ++_active;
        _active_lane = item.lane;
        _active_bytes = item.byte_cost;
    }

    execute_item (std::move (item));
}

void serial_execution_queue_t::execute_item (work_item_t item)
{
    auto name = item.name;
    auto turn = std::move (item.turn);
    if (item.suspended_completion) {
        complete_one (std::move (name), std::move (*item.suspended_completion), true);
        return;
    }
    if (!turn) {
        report_deferred_error (name, std::make_exception_ptr (std::logic_error (
                                       "serial execution queue turn was not registered")));
        complete_one (std::move (name), [] {}, false);
        return;
    }

    try {
        detail::serial_turn_scope_t scope (turn);
        std::weak_ptr<serial_turn_handle_impl_t> weak_turn = turn;
        item.work ([weak_turn] (std::function<void ()> completion) mutable {
            if (auto active_turn = weak_turn.lock ())
                (void) active_turn->complete (std::move (completion));
        });
        if (item.lane == serial_work_lane_t::lifecycle && !turn->released ()) {
            item.turn = turn;
            suspend_lifecycle (std::move (item));
        }
    }
    catch (...) {
        const auto error = std::current_exception ();
        if (item.actor_mailbox && item.actor_mailbox->_active_turn
            && item.actor_mailbox->_active_turn->turn)
            (void) item.actor_mailbox->_active_turn->turn->complete (
              [error] { std::rethrow_exception (error); });
        if (!turn->complete ([error] { std::rethrow_exception (error); }))
            report_deferred_error (name, error);
    }
}

void serial_execution_queue_t::suspend_lifecycle (work_item_t item)
{
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    if (!item.turn || item.turn->released ())
        return;
    assert (!_suspended_lifecycle && _active_lane == serial_work_lane_t::lifecycle);
    item.work = {};
    if (_active_turn) {
        item.work = std::move (_active_turn->ready_continuation);
        item.cancel = std::move (_active_turn->cancel);
        item.cancel_requested = _active_turn->cancel_requested;
    }
    _suspended_lifecycle = std::move (item);
    _active_turn.reset ();
    _active_lane.reset ();
    _active_bytes = 0;
    --_active;
    if (!owns_shared_gate ())
        _draining = false;
    (void) schedule_drain_locked ();
}

bool serial_execution_queue_t::try_resume_suspended (
  const std::shared_ptr<serial_turn_handle_impl_t> &turn, std::function<void ()> work)
{
    if (owns_shared_gate () && current_shared_gate != (_spot_gate ? _spot_gate.get () : this)) {
        bool resumed = false;
        wait_for_control ([this, turn, &work, &resumed] {
            resumed = try_resume_suspended (turn, std::move (work));
        });
        return resumed;
    }
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    if (_suspended_lifecycle && _suspended_lifecycle->turn == turn) {
        _suspended_lifecycle->work = [work = std::move (work)] (auto) mutable { work (); };
        (void) schedule_drain_locked ();
        return true;
    }
    if (_active_lane == serial_work_lane_t::lifecycle && _active_turn && _active_turn->turn == turn
        && !turn->released ()) {
        _active_turn->ready_continuation = [work = std::move (work)] (auto) mutable { work (); };
        return true;
    }
    return false;
}

void serial_execution_queue_t::complete_turn (
  std::string name,
  std::function<void ()> completion,
  const std::shared_ptr<serial_turn_handle_impl_t> &turn,
  bool allow_inline_claim)
{
    {
        std::unique_lock lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        if (_suspended_lifecycle && _suspended_lifecycle->turn == turn) {
            _suspended_lifecycle->suspended_completion = std::move (completion);
            (void) schedule_drain_locked ();
            return;
        }
    }
    complete_one (std::move (name), std::move (completion), allow_inline_claim);
}

void serial_execution_queue_t::complete_one (std::string name,
                                             std::function<void ()> completion,
                                             bool allow_inline_claim)
{
    try {
        if (completion) {
            completion ();
        }
    }
    catch (...) {
        report_deferred_error (name, std::current_exception ());
    }
    if (owns_shared_gate ()) {
        assert (current_shared_gate == (_spot_gate ? _spot_gate.get () : this));
        if (_active_turn && (!_active_turn->turn || _active_turn->turn->released ()))
            _active_turn.reset ();
        if (_active_lane) {
            auto &lane = lane_locked (*_active_lane);
            if (lane.messages > 0)
                --lane.messages;
            lane.bytes.fetch_sub (_active_bytes, std::memory_order_acq_rel);
            lane.messages.notify_all ();
        }
        _active_lane.reset ();
        _active_bytes = 0;
        assert (_active > 0);
        --_active;
        if (_spot_gate)
            _spot_gate->connect_actor_head (shared_from_this ());
        return;
    }
    work_item_t next_item;
    bool execute_next = false;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_active_turn && (!_active_turn->turn || _active_turn->turn->released ())) {
            _active_turn.reset ();
        }
        if (_active_lane) {
            auto &lane = lane_locked (*_active_lane);
            if (lane.messages > 0)
                --lane.messages;
            if (_active_bytes <= lane.bytes)
                lane.bytes -= _active_bytes;
            else
                lane.bytes = 0;
        }
        _active_lane.reset ();
        _active_bytes = 0;
        --_active;
        const bool claim_expired =
          _options.owner_time_budget.count () != 0 && _claim_started_at
          && std::chrono::steady_clock::now () - *_claim_started_at >= _options.owner_time_budget;
        if (has_ready_locked ()) {
            if (allow_inline_claim && !claim_expired) {
                _draining = true;
                next_item = take_next_locked ();
                ++_active;
                _active_lane = next_item.lane;
                _active_bytes = next_item.byte_cost;
                execute_next = true;
            } else {
                _draining = false;
                if (claim_expired)
                    _claim_started_at.reset ();
                schedule_drain_locked ();
            }
        } else {
            _draining = false;
            _claim_started_at.reset ();
            if (_active == 0)
                _empty.notify_all ();
        }
    }
    if (execute_next)
        execute_item (std::move (next_item));
}

} // namespace zlink::framework::runtime
