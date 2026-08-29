/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/execution/serial_execution_queue.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>
#include <memory>
#include <vector>

namespace zlink::framework::runtime
{

namespace
{

serial_execution_queue_options_t legacy_options (std::size_t capacity)
{
    serial_execution_queue_options_t options;
    options.application_message_capacity = capacity;
    return options;
}

std::size_t normalized_byte_cost (serial_work_options_t options) noexcept
{
    return options.byte_cost == 0 ? serial_execution_queue_t::fixed_work_byte_cost
                                  : options.byte_cost;
}

} // namespace

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

    void reached (serial_execution_queue_t::async_completion_t complete)
    {
        std::function<void ()> work;
        async_work_t async_work;
        {
            std::lock_guard lock (_mutex);
            if (_reached)
                return;
            _reached = true;
            _complete = std::move (complete);
            if (_state == state_t::pending)
                return;
            work = _state == state_t::activated
                     ? std::move (_work)
                     : std::function<void ()>{};
            async_work = _state == state_t::activated ? std::move (_async_work) : async_work_t{};
            complete = std::move (_complete);
        }
        finish (std::move (complete), std::move (work), std::move (async_work), _release);
    }

    result_t<void> activate (std::function<void ()> work) override
    {
        if (!work) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Deferred Actor join barrier work is empty");
        }
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending) {
                return result_t<void>::failure (
                  framework_error_kind_t::invalid_operation,
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
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Deferred Actor join barrier async work is empty");
        }
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending) {
                return result_t<void>::failure (
                  framework_error_kind_t::invalid_operation,
                  "Deferred Actor join barrier is already terminal");
            }
            _state = state_t::activated;
            _async_work = std::move (work);
            if (!_reached)
                return result_t<void>::success ();
            complete = std::move (_complete);
            work = std::move (_async_work);
        }
        finish (std::move (complete), {}, std::move (work), _release);
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
        try {
            finish (std::move (complete), {}, {}, {});
        }
        catch (...) {
        }
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
        return [once, on_terminal = std::move (on_terminal)] {
            if (once->exchange (true, std::memory_order_acq_rel))
                return;
            try {
                on_terminal ();
            }
            catch (...) {
            }
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
            auto completion = std::make_shared<serial_execution_queue_t::async_completion_t> (
              std::move (complete));
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
    std::function<void ()> _work;
    async_work_t _async_work;
    std::function<void ()> _release;
};

class serial_turn_handle_impl_t final : public detail::serial_turn_t,
                                       public std::enable_shared_from_this<serial_turn_handle_impl_t>
{
  public:
    serial_turn_handle_impl_t (serial_execution_queue_t &queue,
                              std::string name,
                              serial_work_lane_t lane,
                              bool after_active_phase,
                              serial_execution_queue_t::async_completion_t complete) :
        _queue (queue), _name (std::move (name)), _lane (lane),
        _after_active_phase (after_active_phase), _complete (std::move (complete))
    {
    }

    bool release () override
    {
        return finish ([] {});
    }

    bool released () const override
    {
        std::lock_guard lock (_mutex);
        return _released;
    }

    detail::task_scheduler_t resume_scheduler () override
    {
        return [self = shared_from_this ()] (std::function<void ()> work) mutable {
            auto continuation = std::move (work);
            if (self->_queue.try_post_async (self->_name + "-await-resume",
                                             [work = continuation] (auto complete) mutable {
                                                 try {
                                                     work ();
                                                 }
                                                 catch (...) {
                                                 }
                                                 complete ([] {});
                                                 },
                                                 serial_work_options_t{
                                                   self->_lane,
                                                   serial_execution_queue_t::fixed_work_byte_cost})) {
                return;
            }
            if (continuation) {
                // The continuation must not resume on the producer's stack when
                // the owner queue is full. The executor fallback lets await_resume
                // observe CapacityExceeded on a separate scheduling turn.
                auto continuation_state =
                  std::make_shared<std::function<void ()>> (
                    std::move (continuation));
                if (!self->_queue._executor.try_submit_internal (
                  [work = continuation_state] () mutable {
                      detail::set_serial_resume_failure (
                        framework_error_kind_t::capacity_exceeded,
                        "serial execution queue could not reserve the continuation turn");
                      try {
                          (*work) ();
                      }
                      catch (...) {
                      }
                      (void) detail::take_serial_resume_failure ();
                  })) {
                    // The executor is stopping. The queue is terminal as
                    // well, so resume only to deliver the terminal error;
                    // this is not an inline fallback for a full queue.
                    detail::set_serial_resume_failure (
                      framework_error_kind_t::shutting_down,
                      "serial execution queue executor is stopping");
                    try {
                        (*continuation_state) ();
                    }
                    catch (...) {
                    }
                    (void) detail::take_serial_resume_failure ();
                }
            }
        };
    }

    bool belongs_to (const void *owner) const noexcept override { return owner == &_queue; }
    bool is_after_active_phase () const noexcept override { return _after_active_phase; }
    bool allows_yield () const noexcept override { return _queue.allows_yield (); }

    result_t<void> defer (std::function<void ()> work,
                          std::function<void ()> cancel) override
    {
        if (!work) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Deferred Actor join work is empty");
        }
        std::lock_guard lock (_mutex);
        if (_released) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Actor join defer requires an open Framework handler turn");
        }
        if (_deferred.size () >= 64) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "A Framework handler may defer at most 64 Actor joins");
        }
        _deferred.push_back (
          deferred_work_t{std::move (work), std::move (cancel)});
        return result_t<void>::success ();
    }

    void cancel_deferred () noexcept override
    {
        std::vector<deferred_work_t> deferred;
        {
            std::lock_guard lock (_mutex);
            deferred = std::move (_deferred);
            _deferred.clear ();
        }
        cancel_entries (deferred);
    }

    bool complete (std::function<void ()> completion)
    {
        return finish (std::move (completion));
    }

  private:
    struct deferred_work_t
    {
        std::function<void ()> activate;
        std::function<void ()> cancel;
    };

    bool finish (std::function<void ()> completion)
    {
        serial_execution_queue_t::async_completion_t complete;
        std::vector<deferred_work_t> deferred;
        {
            std::lock_guard lock (_mutex);
            if (_released) {
                return false;
            }
            _released = true;
            complete = std::move (_complete);
            deferred = std::move (_deferred);
        }
        if (!complete) {
            return false;
        }
        complete ([this, completion = std::move (completion),
                   deferred = std::move (deferred)] () mutable {
            try {
                if (completion)
                    completion ();
            }
            catch (...) {
                cancel_entries (deferred);
                throw;
            }
            if (deferred.empty ()) {
                return;
            }
            auto entries = std::make_shared<std::vector<deferred_work_t>> (
              std::move (deferred));
            if (!_queue.try_post_deferred (
                  _name + "-deferred",
                  [entries] () mutable {
                      for (auto &entry : *entries) {
                          if (entry.activate)
                              entry.activate ();
                      }
                  })) {
                cancel_entries (*entries);
            }
        });
        return true;
    }

    static void cancel_entries (std::vector<deferred_work_t> &entries) noexcept
    {
        for (auto &entry : entries) {
            try {
                if (entry.cancel)
                    entry.cancel ();
            }
            catch (...) {
            }
        }
    }

    serial_execution_queue_t &_queue;
    std::string _name;
    serial_work_lane_t _lane = serial_work_lane_t::application;
    bool _after_active_phase = false;
    mutable std::mutex _mutex;
    serial_execution_queue_t::async_completion_t _complete;
    std::vector<deferred_work_t> _deferred;
    bool _released = false;
};

serial_execution_queue_t::serial_execution_queue_t (offload_executor_t &executor,
                                                    std::size_t capacity,
                                                    error_handler_t error_handler,
                                                    serial_lane_policy_t policy) :
    serial_execution_queue_t (executor, legacy_options (capacity),
                              std::move (error_handler), std::move (policy))
{
}

serial_execution_queue_t::serial_execution_queue_t (
  offload_executor_t &executor,
  serial_execution_queue_options_t options,
  error_handler_t error_handler,
  serial_lane_policy_t policy) :
    _executor (executor), _options (options),
    _lane_policy (std::move (policy)),
    _error_handler (std::move (error_handler)),
    _application {{}, {}, 0, 0, options.application_message_capacity,
                  options.application_byte_capacity},
    _lifecycle {{}, {}, 0, 0, options.lifecycle_message_capacity,
                options.lifecycle_byte_capacity}
{
    if (_options.application_message_capacity == 0
        || _options.application_byte_capacity == 0
        || _options.lifecycle_message_capacity == 0
        || _options.lifecycle_byte_capacity == 0
        || _options.lifecycle_burst_limit == 0
        || _options.owner_time_budget < std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("serial execution queue limits are invalid");
    }
}

serial_execution_queue_t::~serial_execution_queue_t ()
{
    close ();
    drain ();
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
    return try_post_async (std::move (name), [work = std::move (work)] (auto complete) mutable {
        try {
            work ();
            complete ([] {});
        }
        catch (...) {
            auto error = std::current_exception ();
            complete ([error] { std::rethrow_exception (error); });
        }
    }, std::move (options));
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
        if (_closed || !can_enqueue_locked (options)) {
            return false;
        }
        accepted = enqueue_locked (std::move (name), std::move (work), std::move (options));
    }
    if (accepted && transfer_owner_reservation) {
        try {
            transfer_owner_reservation ();
        }
        catch (...) {
        }
    }
    return accepted;
}

result_t<serial_submission_id_t>
serial_execution_queue_t::try_post_cancellable_async (
  std::string name,
  async_work_t work,
  std::function<void ()> cancel,
  serial_work_options_t options)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    if (!cancel) {
        throw std::invalid_argument (
          "serial execution queue cancellation is empty");
    }

    const auto transfer_owner_reservation = options.transfer_owner_reservation;
    serial_submission_id_t submission_id = 0;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed) {
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::shutting_down,
              "serial execution queue is closed");
        }
        if (!can_enqueue_locked (options)) {
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::capacity_exceeded,
              "serial execution queue is full");
        }
        if (_next_submission_id == 0) {
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::internal_failure,
              "serial execution queue submission identifiers are exhausted");
        }

        submission_id = _next_submission_id++;
        if (!enqueue_locked (std::move (name), std::move (work),
                             std::move (options), submission_id,
                             std::move (cancel))) {
            return result_t<serial_submission_id_t>::failure (
              framework_error_kind_t::shutting_down,
              "serial execution queue executor is stopping");
        }
    }
    if (transfer_owner_reservation) {
        try {
            transfer_owner_reservation ();
        }
        catch (...) {
        }
    }
    return result_t<serial_submission_id_t>::success (submission_id);
}

serial_cancel_submission_outcome_t
serial_execution_queue_t::cancel_submission (
  serial_submission_id_t submission_id) noexcept
{
    if (submission_id == 0)
        return serial_cancel_submission_outcome_t::already_terminal;

    std::optional<work_item_t> cancelled_item;
    std::function<void ()> active_cancel;
    serial_cancel_submission_outcome_t outcome =
      serial_cancel_submission_outcome_t::already_terminal;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        const auto unlink = [&] (lane_state_t &lane) {
            const auto unlink_from = [&] (auto &queue) {
                const auto item = std::find_if (
                  queue.begin (), queue.end (), [&] (const auto &candidate) {
                      return candidate.submission_id == submission_id;
                  });
                if (item == queue.end ())
                    return false;
                if (lane.messages > 0)
                    --lane.messages;
                if (item->byte_cost <= lane.bytes)
                    lane.bytes -= item->byte_cost;
                else
                    lane.bytes = 0;
                cancelled_item.emplace (std::move (*item));
                queue.erase (item);
                return true;
            };
            return unlink_from (lane.queue)
                   || unlink_from (lane.after_active_queue);
        };

        if (unlink (_application) || unlink (_lifecycle)) {
            outcome = serial_cancel_submission_outcome_t::queued_cancelled;
            _capacity_changed.notify_all ();
            if (!has_ready_locked () && _active == 0 && !_draining
                && !_drain_scheduled) {
                _empty.notify_all ();
            }
        } else {
            if (_active_turn
                && _active_turn->submission_id == submission_id
                && _active_turn->turn
                && !_active_turn->turn->released ()) {
                outcome =
                  serial_cancel_submission_outcome_t::active_cancel_requested;
                if (!_active_turn->cancel_requested) {
                    _active_turn->cancel_requested = true;
                    active_cancel = std::move (_active_turn->cancel);
                }
            }
        }
    }

    try {
        if (cancelled_item && cancelled_item->cancel)
            cancelled_item->cancel ();
        if (active_cancel)
            active_cancel ();
    }
    catch (...) {
    }
    return outcome;
}

bool serial_execution_queue_t::post_async_wait (std::string name,
                                                async_work_t work,
                                                std::function<bool ()> stop_requested)
{
    return post_async_wait (std::move (name), std::move (work), {},
                            std::move (stop_requested));
}

bool serial_execution_queue_t::post_async_wait (
  std::string name,
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
        _capacity_changed.wait (lock, [&] {
            return _closed || can_enqueue_locked (options)
                   || (stop_requested && stop_requested ());
        });
        if (_closed || (stop_requested && stop_requested ())) {
            return false;
        }
        accepted = enqueue_locked (std::move (name), std::move (work), std::move (options));
    }
    if (accepted && transfer_owner_reservation) {
        try {
            transfer_owner_reservation ();
        }
        catch (...) {
        }
    }
    return accepted;
}

bool serial_execution_queue_t::try_post_deferred (
  std::string name,
  std::function<void ()> work)
{
    if (!work) {
        throw std::invalid_argument ("serial execution queue work is empty");
    }
    std::lock_guard<std::mutex> lock (_mutex);
    const serial_work_options_t options{
      serial_work_lane_t::lifecycle, fixed_work_byte_cost};
    if (_closed || !can_enqueue_locked (options)) {
        return false;
    }
    _deferred_after_active.push_back (
      deferred_work_t{std::move (name), std::move (work), fixed_work_byte_cost});
    ++_lifecycle.messages;
    _lifecycle.bytes += fixed_work_byte_cost;
    return true;
}

result_t<std::shared_ptr<detail::deferred_barrier_t>>
serial_execution_queue_t::reserve_barrier_next (std::string name)
{
    auto barrier = std::make_shared<serial_deferred_barrier_t> ();
    const auto submission = try_post_cancellable_async (
          std::move (name),
          [barrier] (auto complete) mutable {
              barrier->reached (std::move (complete));
          },
          [barrier] { barrier->cancel (); },
          serial_work_options_t{serial_work_lane_t::lifecycle,
                                fixed_work_byte_cost});
    if (!submission) {
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          submission.error_kind (),
          "Deferred Actor join target queue is full or closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (
      std::move (barrier));
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
               && !depth_owner->compare_exchange_weak (
                 depth, depth - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    };
    auto barrier = std::make_shared<serial_deferred_barrier_t> (lower_fence);
    const auto submission = try_post_cancellable_async (
          std::move (name),
          [barrier] (auto complete) mutable {
              barrier->reached (std::move (complete));
          },
          [barrier] { barrier->cancel (); },
          serial_work_options_t{serial_work_lane_t::application,
                                fixed_work_byte_cost});
    if (!submission) {
        lower_fence ();
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          submission.error_kind (),
          "Deferred Actor handoff barrier queue is full or closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (
      std::move (barrier));
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
        throw std::runtime_error ("serial execution queue is full or closed");
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
        throw std::runtime_error ("serial execution queue is full or closed");
    }
}

void serial_execution_queue_t::run (std::string name, std::function<void ()> work)
{
    post (std::move (name), std::move (work));
    drain ();
}

void serial_execution_queue_t::drain ()
{
    std::unique_lock<std::mutex> lock (_mutex);
    _empty.wait (
      lock, [&] {
          return !has_ready_locked () && _active == 0 && !_draining
                 && !_drain_scheduled;
      });
}

void serial_execution_queue_t::close ()
{
    std::lock_guard<std::mutex> lock (_mutex);
    _closed = true;
    if (!has_ready_locked () && _active == 0 && !_draining && !_drain_scheduled) {
        _empty.notify_all ();
    }
    _capacity_changed.notify_all ();
}

void serial_execution_queue_t::cancel_pending ()
{
    std::vector<work_item_t> cancelled_items;
    std::vector<std::function<void ()>> active_cancellations;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _closed = true;
        cancelled_items.reserve (
          _application.queue.size ()
          + _application.after_active_queue.size ()
          + _lifecycle.queue.size ()
          + _lifecycle.after_active_queue.size ());
        const auto clear_queued = [&cancelled_items] (lane_state_t &lane) {
            std::size_t queued_bytes = 0;
            for (auto &item : lane.queue) {
                queued_bytes += item.byte_cost;
                cancelled_items.push_back (std::move (item));
            }
            for (auto &item : lane.after_active_queue) {
                queued_bytes += item.byte_cost;
                cancelled_items.push_back (std::move (item));
            }
            const auto queued_messages =
              lane.queue.size () + lane.after_active_queue.size ();
            lane.messages -= queued_messages;
            lane.bytes = queued_bytes <= lane.bytes ? lane.bytes - queued_bytes : 0;
            lane.queue.clear ();
            lane.after_active_queue.clear ();
        };
        clear_queued (_application);
        clear_queued (_lifecycle);
        for (const auto &deferred : _deferred_after_active) {
            if (_lifecycle.messages > 0)
                --_lifecycle.messages;
            if (deferred.byte_cost <= _lifecycle.bytes)
                _lifecycle.bytes -= deferred.byte_cost;
            else
                _lifecycle.bytes = 0;
        }
        _deferred_after_active.clear ();
        if (_active_turn && _active_turn->turn
            && !_active_turn->turn->released ()) {
            if (_active_turn->submission_id != 0
                && !_active_turn->cancel_requested) {
                _active_turn->cancel_requested = true;
                if (_active_turn->cancel) {
                    active_cancellations.push_back (
                      std::move (_active_turn->cancel));
                }
            }
        }
        _capacity_changed.notify_all ();
    }
    for (auto &item : cancelled_items) {
        try {
            if (item.cancel)
                item.cancel ();
        }
        catch (...) {
        }
    }
    for (auto &cancel : active_cancellations) {
        try {
            cancel ();
        }
        catch (...) {
        }
    }
}

std::size_t serial_execution_queue_t::pending_count () const
{
    std::lock_guard<std::mutex> lock (_mutex);
    return _application.messages + _lifecycle.messages;
}

std::size_t serial_execution_queue_t::pending_count (serial_work_lane_t lane) const
{
    std::lock_guard<std::mutex> lock (_mutex);
    return lane_locked (lane).messages;
}

std::size_t serial_execution_queue_t::pending_bytes () const
{
    std::lock_guard<std::mutex> lock (_mutex);
    return _application.bytes + _lifecycle.bytes;
}

bool serial_execution_queue_t::closed () const
{
    std::lock_guard<std::mutex> lock (_mutex);
    return _closed;
}

bool serial_execution_queue_t::schedule_drain_locked ()
{
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

bool serial_execution_queue_t::can_enqueue_locked (
  const serial_work_options_t &options) const noexcept
{
    if (options.transfer_owner_reservation) {
        return true;
    }
    const auto &lane = lane_locked (options.lane);
    const auto bytes = normalized_byte_cost (options);
    return lane.messages < lane.message_capacity
           && lane.bytes <= lane.byte_capacity
           && bytes <= lane.byte_capacity - lane.bytes;
}

bool serial_execution_queue_t::enqueue_locked (std::string name,
                                               async_work_t work,
                                               serial_work_options_t options,
                                               serial_submission_id_t submission_id,
                                               std::function<void ()> cancel)
{
    const auto bytes = normalized_byte_cost (options);
    auto &lane = lane_locked (options.lane);
    auto turn = create_turn (name, options.lane, false);
    lane.queue.push_back (
      work_item_t{std::move (name), std::move (work), options.lane, bytes,
                  false, submission_id, std::move (cancel), std::move (turn)});
    ++lane.messages;
    lane.bytes += bytes;
    if (schedule_drain_locked ())
        return true;
    lane.queue.pop_back ();
    --lane.messages;
    lane.bytes -= bytes;
    return false;
}

bool serial_execution_queue_t::has_ready_locked () const noexcept
{
    return !_application.queue.empty () || !_application.after_active_queue.empty ()
           || !_lifecycle.queue.empty () || !_lifecycle.after_active_queue.empty ();
}

std::shared_ptr<serial_turn_handle_impl_t>
serial_execution_queue_t::create_turn (const std::string &name,
                                       serial_work_lane_t lane,
                                       bool after_active_phase)
{
    return std::make_shared<serial_turn_handle_impl_t> (
      *this, name, lane, after_active_phase,
      [this, name] (std::function<void ()> completion) mutable {
          bool queue_closed = false;
          {
              std::lock_guard<std::mutex> lock (_mutex);
              queue_closed = _closed;
          }
          if (queue_closed) {
              complete_one (std::move (name), std::move (completion), false);
              return;
          }
          auto completion_state =
            std::make_shared<std::pair<std::string, std::function<void ()>>> (
              std::move (name), std::move (completion));
          if (!_executor.try_submit_internal (
                [this, completion_state] () mutable {
                    complete_one (std::move (completion_state->first),
                                  std::move (completion_state->second), true);
                })) {
              complete_one (std::move (completion_state->first),
                            std::move (completion_state->second), false);
          }
      });
}

void serial_execution_queue_t::activate_turn_locked (work_item_t &item) noexcept
{
    assert (!_active_turn.has_value ());
    assert (item.turn);
    _active_turn.emplace ();
    _active_turn->submission_id = item.submission_id;
    _active_turn->turn = item.turn;
    _active_turn->cancel = std::move (item.cancel);
    _active_turn->cancel_requested = false;
    item.cancel = {};
}

serial_execution_queue_t::work_item_t
serial_execution_queue_t::take_next_locked ()
{
    const bool application_ready = !_application.queue.empty ();
    const bool application_after_active_ready =
      !_application.after_active_queue.empty ();
    const bool lifecycle_after_active_ready =
      !_lifecycle.after_active_queue.empty ();
    const bool lifecycle_ready =
      lifecycle_after_active_ready || !_lifecycle.queue.empty ();
    const bool any_application_ready = application_ready || application_after_active_ready;
    serial_work_lane_t selected_lane = serial_work_lane_t::application;
    if (lifecycle_ready && !any_application_ready) {
        selected_lane = serial_work_lane_t::lifecycle;
    } else if (lifecycle_ready && any_application_ready) {
        const bool lifecycle_must_yield =
          _lifecycle_debt
          || _lifecycle_streak >= _options.lifecycle_burst_limit;
        if (!lifecycle_must_yield)
            selected_lane = serial_work_lane_t::lifecycle;
    }

    auto &lane = lane_locked (selected_lane);
    auto &queue = !lane.after_active_queue.empty () ? lane.after_active_queue : lane.queue;
    const bool after_active_phase = !lane.after_active_queue.empty ();
    auto item = std::move (queue.front ());
    queue.pop_front ();
    assert (item.after_active_phase == after_active_phase);
    activate_turn_locked (item);
    if (selected_lane == serial_work_lane_t::lifecycle) {
        ++_lifecycle_streak;
        if (application_ready
            && _lifecycle_streak >= _options.lifecycle_burst_limit)
            _lifecycle_debt = true;
    } else {
        _lifecycle_streak = 0;
        _lifecycle_debt = false;
    }
    return item;
}

void serial_execution_queue_t::report_deferred_error (
  const std::string &name,
  const std::exception_ptr &error) const noexcept
{
    if (!_error_handler)
        return;
    try {
        _error_handler (name, error);
    }
    catch (...) {
    }
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
    if (!turn) {
        report_deferred_error (
          name,
          std::make_exception_ptr (
            std::logic_error ("serial execution queue turn was not registered")));
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
    }
    catch (...) {
        const auto error = std::current_exception ();
        if (!turn->complete ([error] { std::rethrow_exception (error); }))
            report_deferred_error (name, error);
    }
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

    std::vector<deferred_work_t> deferred_after_active;
    work_item_t next_item;
    bool execute_next = false;
    {
        std::lock_guard<std::mutex> lock (_mutex);
        deferred_after_active = std::move (_deferred_after_active);
        _deferred_after_active.clear ();

        // A deferred operation is already accounted for in the lifecycle
        // lane. Requeue it in the after-active phase after the current turn
        // releases the queue. Executing it inline here lets the operation
        // synchronously wait for this same queue and deadlock, while placing
        // it behind a deferred barrier would leave that barrier waiting for
        // its activation.
        for (auto &deferred : deferred_after_active) {
            const auto deferred_name = deferred.name;
            try {
                auto work = std::move (deferred.work);
                work_item_t item{
                  deferred.name,
                  [work = std::move (work)] (auto complete) mutable {
                      try {
                          if (work)
                              work ();
                          complete ([] {});
                      }
                      catch (...) {
                          const auto error = std::current_exception ();
                          complete ([error] {
                              std::rethrow_exception (error);
                          });
                      }
                  },
                  serial_work_lane_t::lifecycle,
                  deferred.byte_cost,
                  true};
                item.turn = create_turn (
                  item.name, serial_work_lane_t::lifecycle, true);
                _lifecycle.after_active_queue.push_back (std::move (item));
            }
            catch (...) {
                // The reservation belongs to this deferred entry. If the
                // queue node cannot be allocated, release that reservation
                // and report the failure without disturbing other entries.
                if (_lifecycle.messages > 0)
                    --_lifecycle.messages;
                if (deferred.byte_cost <= _lifecycle.bytes)
                    _lifecycle.bytes -= deferred.byte_cost;
                else
                    _lifecycle.bytes = 0;
                report_deferred_error (deferred_name, std::current_exception ());
            }
        }

        if (_active_turn
            && (!_active_turn->turn || _active_turn->turn->released ())) {
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
          && std::chrono::steady_clock::now () - *_claim_started_at
               >= _options.owner_time_budget;
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
        _capacity_changed.notify_all ();
    }
    if (execute_next)
        execute_item (std::move (next_item));
}

} // namespace zlink::framework::runtime
