/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_handler_participant.hpp"

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

thread_local serial_execution_queue_t *current_shared_gate = nullptr;

std::size_t normalized_byte_cost (serial_work_options_t options) noexcept
{
    return options.byte_cost == 0 ? serial_execution_queue_t::fixed_work_byte_cost
                                  : options.byte_cost;
}

serial_execution_queue_t::cancellation_t adapt_cancellation (std::function<void ()> cancel)
{
    if (!cancel)
        return {};
    return [cancel = std::move (cancel)] (serial_cancel_submission_outcome_t) { cancel (); };
}

} // namespace

// Deferred operations and participants create these actual completion objects.
// An ordinary Yield keeps only its existing continuation chain.
struct serial_turn_completion_t
{
    task_completion_source_t<void> handler_terminal;
    using mailbox_release_t = std::function<result_t<void> (std::unique_lock<std::mutex>)>;
    using mailbox_continuation_t =
      std::function<void (mailbox_release_t, std::unique_lock<std::mutex>)>;
    mailbox_continuation_t mailbox_continuation;
};

/* One handler across its Yield continuations. Deferred work belongs to the
 * handler, so its terminal -- not a Yield -- settles it. */
struct serial_turn_chain_t
{
    explicit serial_turn_chain_t (serial_execution_queue_t &owner) : queue (owner) {}

    // A Yield keeps the same Actor FIFO; this is an existing queue capability.
    serial_execution_queue_t &queue;

    struct entry_t
    {
        std::shared_ptr<detail::deferred_barrier_t> barrier;
        std::function<task_t<void> ()> activate;
        std::function<void ()> cancel;
        bool runs_after_failure = false;
    };

    std::mutex mutex;
    std::vector<entry_t> deferred;
    std::shared_ptr<serial_turn_completion_t> completion;

    std::shared_ptr<serial_turn_completion_t> terminal ()
    {
        std::lock_guard lock (mutex);
        if (!completion)
            completion = std::make_shared<serial_turn_completion_t> ();
        return completion;
    }

    std::vector<entry_t> take ()
    {
        std::lock_guard lock (mutex);
        return std::exchange (deferred, {});
    }
};

class serial_handler_observer_t final : public serial_handler_participant_t
{
  public:
    explicit serial_handler_observer_t (std::shared_ptr<task_completion_source_t<void>> handler) :
        _handler (std::move (handler))
    {
    }

    task_t<void> terminal () override { return _release.task (); }
    task_t<void> run () override { return _handler->task (); }

    void cancel () noexcept override
    {
        _handler->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                     "Actor handler observation was cancelled"));
        _release.complete (result_t<void>::success ());
    }

  private:
    std::shared_ptr<task_completion_source_t<void>> _handler;
    task_completion_source_t<void> _release;
};

class serial_deferred_barrier_t final
    : public detail::deferred_barrier_t,
      public std::enable_shared_from_this<serial_deferred_barrier_t>
{
    using phase_t = std::function<void (result_t<void>)>;

  public:
    serial_deferred_barrier_t (std::function<void ()> cancel = {},
                               std::function<void (async_completion_t)> settle = {},
                               std::function<void ()> notify = {}) :
        _cancel (std::move (cancel)), _settle (std::move (settle)), _notify (std::move (notify))
    {
    }

    bool pending ()
    {
        std::lock_guard lock (_mutex);
        return _state == state_t::pending;
    }
    task_t<void> terminal () override { return _terminal.task (); }
    bool prepare_participants (serial_execution_queue_t::async_completion_t complete)
    {
        {
            std::lock_guard lock (_mutex);
            if (_state == state_t::pending) {
                _complete = std::move (complete);
                return true;
            }
        }
        reached (std::move (complete));
        return false;
    }

    void retain_participant (std::shared_ptr<serial_handler_participant_t> participant)
    {
        {
            std::lock_guard lock (_mutex);
            if (_state == state_t::pending) {
                _participants.push_back (std::move (participant));
                return;
            }
        }
        cancel_participants ({std::move (participant)});
    }

    task_t<void> acquire_participants (
      std::function<task_t<std::vector<std::shared_ptr<serial_execution_queue_t>>> ()> collect)
    {
        const auto turn = detail::capture_current_serial_turn ();
        auto queues =
          co_await detail::with_task_resume_scheduler (collect (), turn->resume_scheduler ());
        for (const auto &queue : queues) {
            auto reserved = queue->observe_handler_terminal ();
            if (!reserved)
                throw framework_exception_t (reserved.error_kind (),
                                             "Relocation participant queue is closed");
            retain_participant (std::move (reserved.value ()));
        }
        std::vector<std::shared_ptr<serial_handler_participant_t>> participants;
        {
            std::lock_guard lock (_mutex);
            participants = _participants;
        }
        for (const auto &participant : participants)
            co_await detail::with_task_resume_scheduler (participant->run (),
                                                         turn->resume_scheduler ());
    }

    void acquire (
      std::function<task_t<std::vector<std::shared_ptr<serial_execution_queue_t>>> ()> collect,
      std::function<void (const result_t<void> &)> completion)
    {
        const auto turn = detail::capture_current_serial_turn ();
        auto acquisition = std::make_shared<task_t<void>> (detail::with_task_resume_scheduler (
          acquire_participants (std::move (collect)), turn->resume_scheduler ()));
        {
            std::lock_guard lock (_mutex);
            _acquisition = acquisition;
        }
        detail::observe_task_completion (
          *acquisition, [self = shared_from_this (), acquisition,
                         completion = std::move (completion)] (const result_t<void> &result) {
              if (!result)
                  self->release_participants ();
              completion (result);
          });
    }

    bool cancelled ()
    {
        std::lock_guard lock (_mutex);
        return _state == state_t::cancelled;
    }

    void reached (serial_execution_queue_t::async_completion_t complete)
    {
        phase_t work;
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
            if (complete)
                _complete = std::move (complete);
            if (_state == state_t::pending) {
                work = std::move (_work);
            } else {
                work = _state == state_t::activated ? std::move (_work) : phase_t{};
                async_work =
                  _state == state_t::activated ? std::move (_async_work) : async_work_t{};
                complete = std::move (_complete);
            }
        }
        if (!complete) {
            if (work)
                work (turn ? result_t<void>::success ()
                           : result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                      "Lifecycle barrier has no retained turn"));
            return;
        }
        if (!turn) {
            complete (
              [] { throw std::logic_error ("lifecycle barrier reached outside its serial turn"); });
            return;
        }
        finish (std::move (complete), std::move (work), std::move (async_work));
    }

    task_t<void> run (std::function<task_t<void> ()> work) override
    {
        if (!work)
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                          "Lifecycle barrier phase is empty"));
        auto source = std::make_shared<task_completion_source_t<void>> ();
        auto owned_work = std::make_shared<std::function<task_t<void> ()>> (std::move (work));
        const auto invoke = [self = shared_from_this (), source,
                             owned_work] (result_t<void> admission) {
            if (!admission) {
                source->complete (std::move (admission));
                return;
            }
            std::shared_ptr<task_t<void>> acquisition;
            {
                std::lock_guard lock (self->_mutex);
                acquisition = self->_acquisition;
            }
            if (acquisition) {
                const auto &acquired = acquisition->result ();
                if (!acquired) {
                    source->complete (acquired);
                    return;
                }
            }
            if (const auto failure = detail::take_serial_resume_failure ()) {
                source->complete (result_t<void>::failure (failure->kind, failure->message));
                return;
            }
            try {
                auto pending = std::make_shared<task_t<void>> ((*owned_work) ());
                detail::observe_task_terminal (
                  *pending, [source, pending, owned_work] (const result_t<void> &result) {
                      source->complete (result);
                  });
            }
            catch (const framework_exception_t &error) {
                source->complete (detail::result_access_t::failure<void> (error));
            }
            catch (const std::exception &error) {
                source->complete (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                           error.what ()));
            }
        };
        detail::task_scheduler_t resume;
        std::shared_ptr<detail::serial_turn_t> turn;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending || _work)
                return task_t<void> (
                  result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                           "Lifecycle barrier phase is unavailable"));
            if (!_reached) {
                _work = invoke;
                return source->task ();
            }
            resume = _resume;
            turn = _turn;
        }
        if (resume)
            resume ([invoke, turn] {
                const auto active = detail::capture_current_serial_turn ();
                invoke (active && active == turn && !active->released ()
                          ? result_t<void>::success ()
                          : result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                     "Lifecycle barrier lost its retained turn"));
            });
        else
            source->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                       "Lifecycle barrier has no retained turn"));
        return source->task ();
    }

    result_t<void> activate (std::function<void ()> work) override
    {
        if (!work)
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Deferred Actor join barrier work is empty");
        return activate_async ([work = std::move (work)] (async_completion_t complete) {
            work ();
            complete (result_t<void>::success ());
        });
    }

    result_t<void> activate_async (async_work_t work) override
    {
        if (!work) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Deferred Actor join barrier async work is empty");
        }
        serial_execution_queue_t::async_completion_t complete;
        phase_t cancelled_phase;
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
            if (!_reached) {
                if (_complete) {
                    cancelled_phase = std::move (_work);
                }
            } else {
                complete = std::move (_complete);
                work = std::move (_async_work);
                resume = _resume;
                turn = _turn;
            }
        }
        if (_notify)
            _notify ();
        if (cancelled_phase)
            cancelled_phase (
              result_t<void>::failure (framework_error_kind_t::shutting_down,
                                       "Relocation readiness acquisition was cancelled"));
        if (!complete) {
            release_participants ();
            return result_t<void>::success ();
        }
        if (resume) {
            resume ([self = shared_from_this (), complete = std::move (complete),
                     work = std::move (work), turn = std::move (turn)] () mutable {
                const auto active = detail::capture_current_serial_turn ();
                const auto failure = detail::take_serial_resume_failure ();
                if (failure || !active || active != turn || active->released ()) {
                    self->finish (
                      std::move (complete),
                      [failure] (result_t<void>) {
                          throw framework_exception_t (
                            failure ? failure->kind : framework_error_kind_t::shutting_down,
                            failure ? failure->message
                                    : "lifecycle barrier lost its retained serial turn");
                      },
                      {});
                    return;
                }
                self->finish (std::move (complete), {}, std::move (work));
            });
        } else {
            finish (std::move (complete),
                    [] (result_t<void>) {
                        throw std::logic_error ("lifecycle barrier has no retained serial turn");
                    },
                    {});
            return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                            "lifecycle barrier has no retained serial turn");
        }
        return result_t<void>::success ();
    }

    void cancel () noexcept override
    {
        serial_execution_queue_t::async_completion_t complete;
        phase_t phase;
        {
            std::lock_guard lock (_mutex);
            if (_state != state_t::pending)
                return;
            _state = state_t::cancelled;
            _settle = {};
            phase = std::move (_work);
            complete = std::move (_complete);
        }
        if (_cancel)
            _cancel ();
        if (_notify)
            _notify ();
        if (phase)
            phase (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                            "Lifecycle barrier was cancelled"));
        finish (std::move (complete), {}, {});
    }

  private:
    enum class state_t
    {
        pending,
        activated,
        cancelled
    };

    static void cancel_participants (
      std::vector<std::shared_ptr<serial_handler_participant_t>> participants) noexcept
    {
        for (const auto &participant : participants)
            participant->cancel ();
    }

    void release_participants () noexcept
    {
        std::vector<std::shared_ptr<serial_handler_participant_t>> participants;
        {
            std::lock_guard lock (_mutex);
            participants = std::exchange (_participants, {});
        }
        cancel_participants (std::move (participants));
    }

    void finish (serial_execution_queue_t::async_completion_t complete,
                 phase_t work,
                 async_work_t async_work)
    {
        std::shared_ptr<task_t<void>> acquisition;
        {
            std::lock_guard lock (_mutex);
            acquisition = std::exchange (_acquisition, {});
        }
        if (!complete) {
            release_participants ();
            _terminal.complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                         "Lifecycle barrier was cancelled"));
            return;
        }
        // The readiness holder owns every acquired Actor boundary through the
        // same terminal as its root barrier, including asynchronous completion.
        auto release = [self = shared_from_this (), acquisition = std::move (acquisition),
                        complete = std::move (complete)] (std::function<void ()> terminal) mutable {
            self->release_participants ();
            acquisition.reset ();
            auto finish = [self, complete = std::move (complete),
                           terminal = std::move (terminal)] (result_t<void> settled) mutable {
                complete ([self, terminal = std::move (terminal), settled] {
                    try {
                        if (terminal)
                            terminal ();
                        settled.value ();
                    }
                    catch (...) {
                        self->_terminal.complete (detail::current_exception_result<void> ());
                        throw;
                    }
                    self->_terminal.complete (result_t<void>::success ());
                });
            };
            std::function<void (async_completion_t)> settle;
            {
                std::lock_guard lock (self->_mutex);
                settle = std::exchange (self->_settle, {});
            }
            if (settle)
                settle (std::move (finish));
            else
                finish (result_t<void>::success ());
        };
        if (async_work) {
            auto completion =
              std::make_shared<serial_execution_queue_t::async_completion_t> (std::move (release));
            try {
                async_work ([completion] (result_t<void> result) mutable {
                    (*completion) ([result = std::move (result)] () mutable { result.value (); });
                });
            }
            catch (...) {
                const auto error = std::current_exception ();
                (*completion) ([error] { std::rethrow_exception (error); });
            }
            return;
        }
        release ([work = std::move (work)] () mutable {
            if (work)
                work (result_t<void>::success ());
        });
    }

    std::function<void ()> _cancel;
    std::function<void (async_completion_t)> _settle;
    std::function<void ()> _notify;
    std::mutex _mutex;
    state_t _state = state_t::pending;
    bool _reached = false;
    serial_execution_queue_t::async_completion_t _complete;
    detail::task_scheduler_t _resume;
    task_completion_source_t<void> _terminal;
    std::shared_ptr<detail::serial_turn_t> _turn;
    phase_t _work;
    async_work_t _async_work;
    std::vector<std::shared_ptr<serial_handler_participant_t>> _participants;
    std::shared_ptr<task_t<void>> _acquisition;
};

class serial_turn_handle_impl_t final
    : public detail::serial_turn_t,
      public std::enable_shared_from_this<serial_turn_handle_impl_t>
{
    friend class serial_execution_queue_t;

  public:
    serial_turn_handle_impl_t (serial_execution_queue_t &queue,
                               std::string name,
                               serial_work_lane_t lane,
                               bool after_active_phase,
                               std::shared_ptr<serial_turn_chain_t> chain,
                               bool holds_application_while_waiting) :
        _queue (queue),
        _queue_owner (queue.weak_from_this ().lock ()),
        _name (std::move (name)),
        _lane (lane),
        _after_active_phase (after_active_phase),
        _chain (std::move (chain)),
        _holds_application_while_waiting (holds_application_while_waiting)
    {
    }

    bool holds_application_while_waiting () const noexcept
    {
        return _holds_application_while_waiting;
    }

    void set_holds_application_while_waiting (bool hold) noexcept
    {
        assert (current_shared_gate == &_queue);
        assert (detail::capture_current_serial_turn ().get () == this);
        _holds_application_while_waiting = hold;
    }

    void link_handler (const std::shared_ptr<serial_turn_handle_impl_t> &handler)
    {
        _handler = handler;
    }

    task_t<void> observe_handler ()
    {
        if (auto handler = _handler.lock ())
            return handler->chain ()->terminal ()->handler_terminal.task ();
        if (_queue.shared_spot_gate ())
            return task_t<void> (result_t<void>::success ());
        return chain ()->terminal ()->handler_terminal.task ();
    }

    void idle_handler_terminal ()
    {
        chain ()->terminal ()->handler_terminal.complete (result_t<void>::success ());
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
        return _phase == phase_t::released;
    }

    bool claim_yield () override
    {
        std::lock_guard lock (_mutex);
        if (_phase != phase_t::open || !_queue.allows_yield ())
            return false;
        _phase = phase_t::yield_admitted;
        return true;
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
            auto resume = [self, continuation] (const result_t<bool> &resumed) mutable {
                if (resumed && resumed.value ())
                    return;
                if (self->_queue.try_post_continuation (
                      self->_name + "-await-resume",
                      [self, work = continuation] (auto complete) mutable {
                          // Closing changes the wait result, not its right to run cleanup.
                          if (self->_queue.closed ())
                              detail::set_serial_resume_failure (
                                framework_error_kind_t::shutting_down,
                                detail::registered_execution_context_shutting_down_message);
                          try {
                              work ();
                          }
                          catch (...) {
                              (void) detail::take_serial_resume_failure ();
                              throw;
                          }
                          (void) detail::take_serial_resume_failure ();
                          complete ([] {});
                      },
                      self, self->_holds_application_while_waiting)) {
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
                                detail::registered_execution_context_shutting_down_message);
                              (*work) ();
                              (void) detail::take_serial_resume_failure ();
                          })) {
                        detail::set_serial_resume_failure (
                          framework_error_kind_t::shutting_down,
                          detail::registered_execution_context_shutting_down_message);
                        (*continuation_state) ();
                        (void) detail::take_serial_resume_failure ();
                    }
                }
            };
            if (self->_lane == serial_work_lane_t::lifecycle) {
                auto pending = std::make_shared<task_t<bool>> (
                  self->_queue.try_resume_suspended (self, continuation));
                detail::observe_task_terminal (
                  *pending, [pending, resume = std::move (resume)] (
                              const result_t<bool> &result) mutable { resume (result); });
            } else {
                resume (result_t<bool>::success (false));
            }
        };
    }

    bool belongs_to (const void *owner) const noexcept override { return owner == &_queue; }
    bool is_after_active_phase () const noexcept override { return _after_active_phase; }
    bool allows_yield () const noexcept override { return _queue.allows_yield (); }

    result_t<void> defer (std::function<task_t<void> ()> work,
                          std::function<void ()> cancel,
                          std::shared_ptr<detail::deferred_barrier_t> barrier) override
    {
        return register_deferred (_name + "-deferred", std::move (work), std::move (cancel), false,
                                  std::move (barrier));
    }

    /* Registers work at its accepted lifecycle FIFO position now and settles
     * it at this turn's terminal (handler turn and execution gate §5, §7).
     * `runs_after_failure` keeps the work when the turn ends exceptionally;
     * otherwise that terminal discards it and runs `cancel`. */
    result_t<void> register_deferred (std::string name,
                                      std::function<task_t<void> ()> work,
                                      std::function<void ()> cancel,
                                      bool runs_after_failure,
                                      std::shared_ptr<detail::deferred_barrier_t> barrier = {})
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
        const auto owner = chain ();
        if (!barrier) {
            auto reserved = owner->queue.reserve_deferred_barrier_next (std::move (name));
            if (!reserved)
                return result_t<void>::failure (reserved.error_kind (),
                                                "serial execution queue is closed");
            barrier = std::move (reserved.value ());
        }
        {
            (void) owner->terminal ();
            std::lock_guard lock (owner->mutex);
            owner->deferred.push_back (serial_turn_chain_t::entry_t{
              std::move (barrier), std::move (work), std::move (cancel), runs_after_failure});
        }
        return result_t<void>::success ();
    }

    /* The handler failed after a Yield: its dispatcher reports that terminal
     * here, because a continuation turn only ends a queue turn. */
    void cancel_deferred () noexcept override { end_chain (false); }

    bool complete (std::function<void ()> completion)
    {
        return finish (std::move (completion), true);
    }

    bool finish_handler (std::function<void ()> completion, bool succeeded) override
    {
        if (finish (completion, true, succeeded))
            return true;
        _queue.notify_control (
          "handler-terminal",
          [self = shared_from_this (), completion = std::move (completion), succeeded] () mutable {
              try {
                  if (completion)
                      completion ();
              }
              catch (...) {
                  self->end_chain_owned (false);
                  throw;
              }
              self->end_chain_owned (succeeded);
          });
        return false;
    }

    serial_work_lane_t lane () const noexcept { return _lane; }

    template <typename Completion> void release_mailbox_owned (Completion completion)
    {
        std::unique_lock claim (_queue._mutex, std::defer_lock);
        if (!_queue.owns_shared_gate ())
            claim.lock ();
        auto *terminal = _chain ? _chain->completion.get () : nullptr;
        if (!terminal || !terminal->mailbox_continuation) {
            completion (std::move (claim));
            return;
        }
        serial_turn_completion_t::mailbox_release_t finish =
          [completion = std::move (completion)] (
            std::unique_lock<std::mutex> claim) mutable -> result_t<void> {
            if constexpr (std::is_void_v<
                            std::invoke_result_t<Completion &, std::unique_lock<std::mutex>>>) {
                completion (std::move (claim));
                return result_t<void>::success ();
            } else {
                return completion (std::move (claim));
            }
        };
        auto continuation = std::exchange (terminal->mailbox_continuation, {});
        continuation (std::move (finish), std::move (claim));
    }

    void retain_until (task_t<void> terminal,
                       std::function<void (const result_t<void> &)> released = {})
    {
        if (auto handler = _handler.lock ())
            (void) handler->chain ()->terminal ();
        auto owner = chain ();
        auto completion = owner->terminal ();
        auto pending = std::make_shared<task_t<void>> (std::move (terminal));
        completion->mailbox_continuation =
          [self = shared_from_this (), owner, completion, pending, released = std::move (released),
           previous = std::move (completion->mailbox_continuation)] (
            serial_turn_completion_t::mailbox_release_t finish,
            std::unique_lock<std::mutex> claim) mutable {
              completion->mailbox_continuation = std::move (previous);
              if (claim.owns_lock ())
                  claim.unlock ();
              detail::observe_task_terminal (
                *pending, [self, pending, finish = std::move (finish),
                           released = std::move (released)] (const result_t<void> &) mutable {
                    self->release_mailbox (
                      [finish = std::move (finish), released = std::move (released)] (
                        std::unique_lock<std::mutex> claim) mutable {
                          auto result =
                            finish ? finish (std::move (claim)) : result_t<void>::success ();
                          if (released)
                              released (result);
                          return result;
                      });
                });
          };
    }

    void release_mailbox (serial_turn_completion_t::mailbox_release_t finish)
    {
        _queue.notify_control (
          "mailbox-terminal", [self = shared_from_this (), finish = std::move (finish)] () mutable {
              self->release_mailbox_owned (std::move (finish));
          });
    }

  private:
    std::shared_ptr<serial_turn_chain_t> chain ()
    {
        std::lock_guard lock (_mutex);
        if (!_chain) {
            const auto peer = _handler.lock ();
            auto &owner = peer && peer->_queue.shared_spot_gate () ? peer->_queue : _queue;
            _chain = std::make_shared<serial_turn_chain_t> (owner);
        }
        return _chain;
    }

    bool finish (std::function<void ()> completion, bool handler_terminal, bool succeeded = true)
    {
        serial_execution_queue_t::async_completion_t complete;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::released) {
                return false;
            }
            _phase = phase_t::released;
            complete = std::move (_complete);
        }
        if (!complete) {
            return false;
        }
        if (!handler_terminal && _lane == serial_work_lane_t::lifecycle)
            _queue.hold_lifecycle (chain ());
        complete (
          [this, completion = std::move (completion), handler_terminal, succeeded] () mutable {
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
              end_chain (succeeded);
          });
        return true;
    }

    /* Handler terminal: settle the deferred work, then give up the lifecycle
     * lane position a Yield kept. */
    void end_chain (bool succeeded) noexcept
    {
        _queue.notify_control ("handler-terminal", [self = shared_from_this (), succeeded] {
            self->end_chain_owned (succeeded);
        });
    }

    void end_chain_owned (bool succeeded) noexcept
    {
        std::shared_ptr<serial_turn_chain_t> owner;
        {
            std::lock_guard lock (_mutex);
            owner = _chain;
        }
        if (!owner)
            return;
        settle_owned (succeeded);
        std::shared_ptr<serial_turn_completion_t> completion;
        {
            std::lock_guard lock (owner->mutex);
            completion = owner->completion;
        }
        if (completion)
            completion->handler_terminal.complete (result_t<void>::success ());
        _queue.release_lifecycle_hold (std::move (owner));
    }

    /* The Actor FIFO barrier owns activation and terminal completion.
     * A failed handler cancels its reserved barrier before later work runs. */
    void settle (bool succeeded) noexcept
    {
        _queue.notify_control ("deferred-settle", [self = shared_from_this (), succeeded] {
            self->settle_owned (succeeded);
        });
    }

    void settle_owned (bool succeeded) noexcept
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
            if (open && (succeeded || entry.runs_after_failure)) {
                auto activated = entry.barrier->activate_async (
                  [work = std::move (entry.activate)] (
                    detail::deferred_barrier_t::async_completion_t complete) mutable {
                      auto pending = std::make_shared<task_t<void>> (work ());
                      detail::observe_task_terminal (
                        *pending,
                        [pending, work = std::move (work), complete = std::move (complete)] (
                          const result_t<void> &result) mutable { complete (result); });
                  });
                if (!activated) {
                    entry.barrier->cancel ();
                    if (entry.cancel)
                        entry.cancel ();
                }
            } else {
                entry.barrier->cancel ();
                if (entry.cancel)
                    entry.cancel ();
            }
        }
    }

    std::weak_ptr<serial_turn_handle_impl_t> _handler;
    serial_execution_queue_t &_queue;
    // Completion can outlive removal from the Actor executor snapshot.
    std::shared_ptr<serial_execution_queue_t> _queue_owner;
    std::string _name;
    serial_work_lane_t _lane = serial_work_lane_t::application;
    mutable std::stop_source _wait_stop{std::nostopstate};
    bool _after_active_phase = false;
    mutable std::mutex _mutex;
    std::shared_ptr<serial_turn_chain_t> _chain;
    bool _holds_application_while_waiting = false;
    serial_execution_queue_t::async_completion_t _complete;
    enum class phase_t : unsigned char
    {
        open,
        yield_admitted,
        released
    };
    phase_t _phase = phase_t::open;
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
    return _lane_policy.allows_turn_yield () || shared_spot_gate ();
}

serial_execution_queue_t *serial_execution_queue_t::shared_spot_gate () const noexcept
{
    return _spot_gate && _spot_gate->_lane_policy.allows_turn_yield () ? _spot_gate.get ()
                                                                       : nullptr;
}

void serial_execution_queue_t::set_application_start_policy (application_start_policy_t policy)
{
    notify_control ("application-start-policy", [this, policy = std::move (policy)] () mutable {
        std::unique_lock lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        _application_start_policy = std::move (policy);
        (void) schedule_drain_locked ();
    });
}

void serial_execution_queue_t::attach_spot_gate (std::shared_ptr<serial_execution_queue_t> gate)
{
    if (!gate)
        throw std::invalid_argument ("Actor mailbox requires its Spot owner");
    std::lock_guard lock (_mutex);
    if (_publication_cursor || _active != 0 || has_queued_locked ())
        throw std::logic_error ("Actor mailbox must be attached before admission");
    if (gate->_lane_policy.allows_turn_yield ())
        _publication_cursor = _publication_tail = new publication_node_t ();
    _spot_gate = std::move (gate);
}

void serial_execution_queue_t::accept_application_owned (work_item_t &item)
{
    if (item.lane != serial_work_lane_t::application || !item.application_record)
        return;
    if (item.application_record->saved_work ()) {
        item.application_record->admission.take_over ();
    } else {
        // The preceding durable payload owner also takes the next accepted
        // application record; ordinary initial supply remains unchanged.
        const auto tail = _application_tail_record.lock ();
        if (tail && tail->admission.durable ())
            item.application_record->admission.take_over ();
    }
    _application_tail_record = item.application_record;
    // The private FIFO now owns the accepted body and its position. Its M
    // terminal retains that owner, independently of the receiving native claim.
    item.application_record->release_native_claim ();
}

void serial_execution_queue_t::refresh_application_tail_locked ()
{
    const auto tail =
      std::find_if (_application.queue.rbegin (), _application.queue.rend (),
                    [] (const auto &item) { return bool (item.application_record); });
    _application_tail_record = tail == _application.queue.rend ()
                                 ? std::weak_ptr<serial_application_record_t>{}
                                 : tail->application_record;
}

void serial_execution_queue_t::refresh_application_barriers_locked ()
{
    // Only FIFO barrier mutations and their activate/cancel notification rebuild
    // this derived index; application readiness never traverses lifecycle work.
    _application_barriers = std::count_if (
      _lifecycle.queue.begin (), _lifecycle.queue.end (), [this] (const work_item_t &item) {
          return item.barrier && (_spot_gate || !item.barrier->pending ());
      });
}

void serial_execution_queue_t::prepare_application_head_locked ()
{
    const auto next = next_application_locked ();
    if (next == _application.queue.end ())
        return;
    const auto &record = next->application_record;
    if (!record || record->admission.ready ())
        return;
    auto &owned =
      std::get<serial_application_admission_t::durable_t> (record->admission.reservation);
    if (std::holds_alternative<application_job_queue_t::waiter_t> (owned.supply))
        return;
    owned.supply.emplace<application_job_queue_t::waiter_t> ();
    std::weak_ptr<serial_application_record_t> weak_record = record;
    auto self = shared_from_this ();
    auto registration = record->admission.jobs->register_supply (
      [self, weak_record] (std::optional<application_job_queue_t::permit_t> permit) {
          auto grant =
            std::make_shared<std::optional<application_job_queue_t::permit_t>> (std::move (permit));
          // Returning another record's initial permit can grant this waiter
          // inside an owner turn. Late delivery therefore schedules the owner.
          (void) self->_executor.try_submit_internal ([self, weak_record, grant] {
              self->notify_control ("application-permit", [self, weak_record, grant] {
                  std::unique_lock lock (self->_mutex, std::defer_lock);
                  if (!self->owns_shared_gate ())
                      lock.lock ();
                  const auto record = weak_record.lock ();
                  if (!record || !record->admission.durable ())
                      return;
                  const auto next = self->next_application_locked ();
                  auto owned = next;
                  if (owned == self->_application.queue.end ()
                      || owned->application_record != record)
                      owned = std::find_if (self->_application.queue.begin (),
                                            self->_application.queue.end (),
                                            [&record] (const work_item_t &item) {
                                                return item.application_record == record;
                                            });
                  if (owned == self->_application.queue.end ())
                      return;
                  auto &state = std::get<serial_application_admission_t::durable_t> (
                    record->admission.reservation);
                  if (!*grant) {
                      if (lock.owns_lock ())
                          lock.unlock ();
                      self->cancel_pending_owned ();
                      return;
                  }
                  if (owned != next) {
                      state.supply.emplace<serial_application_admission_t::initial_t> ();
                      grant->reset ();
                      (void) self->schedule_drain_locked ();
                      return;
                  }
                  state.supply.emplace<application_job_queue_t::permit_t> (std::move (**grant));
                  std::get<application_job_queue_t::permit_t> (state.supply).mark_queued ();
                  grant->reset ();
                  (void) self->schedule_drain_locked ();
              });
          });
      },
      application_job_queue_t::origin_t::local);
    owned.supply = std::move (registration.waiter);
    if (registration.immediate && *registration.immediate) {
        owned.supply.emplace<application_job_queue_t::permit_t> (
          (**registration.immediate).take ());
        std::get<application_job_queue_t::permit_t> (owned.supply).mark_queued ();
    } else {
        record->admission.jobs->deliver_immediate (
          registration, std::get<application_job_queue_t::waiter_t> (owned.supply));
    }
}

task_t<bool> serial_execution_queue_t::accept_application_ready (
  std::function<std::optional<serial_prefix_work_t> ()> take)
{
    return control_async<bool> ([this, take = std::move (take)] {
        if (owns_shared_gate ())
            import_publications ();
        std::unique_lock lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        while (!_closed) {
            auto item = take ();
            if (!item)
                return true;
            if (owns_shared_gate ())
                lock.lock ();
            const bool accepted =
              !_closed
              && enqueue_locked (std::move (item->name), std::move (item->work),
                                 std::move (item->options), 0, adapt_cancellation (item->cancel));
            if (owns_shared_gate ())
                lock.unlock ();
            if (!accepted) {
                if (lock.owns_lock ())
                    lock.unlock ();
                if (item->cancel)
                    item->cancel ();
                return false;
            }
            // Complete ordinary FIFO admission and native claim return in
            // this owner turn, before another claim or lane selection.
            if (owns_shared_gate ())
                import_publications ();
        }
        return false;
    });
}

task_t<bool> serial_execution_queue_t::adopt_application_prefix (
  std::function<task_t<std::vector<serial_prefix_work_t>> ()> take)
{
    const auto outcome =
      co_await retained_control_task<bool> ([this, take = std::move (take)] () -> task_t<bool> {
          auto prefix = co_await take ();
          co_return co_await control_async<bool> ([this, prefix = std::move (prefix)] () mutable {
              if (owns_shared_gate ())
                  import_publications ();
              std::unique_lock lock (_mutex, std::defer_lock);
              if (!owns_shared_gate ())
                  lock.lock ();
              if (_closed) {
                  if (lock.owns_lock ())
                      lock.unlock ();
                  for (const auto &item : prefix)
                      cancel_pending_work (item.options.application_record,
                                           adapt_cancellation (item.cancel));
                  return false;
              }
              if (prefix.empty ())
                  return true;
              // Prefix admission also takes over already accepted, unstarted Actor
              // records. Later admissions use the same decision at their FIFO tail.
              for (auto &item : _application.queue)
                  if (item.application_record)
                      item.application_record->take_over ();
              // These payload records were accepted before the target's ordinary
              // suffix. Merge the two owned FIFO ranges without a control insertion.
              auto suffix = std::exchange (_application.queue, {});
              for (auto it = prefix.begin (); it != prefix.end (); ++it) {
                  work_item_t item;
                  item.name = std::move (it->name);
                  item.work = std::move (it->work);
                  item.cancel = adapt_cancellation (std::move (it->cancel));
                  item.lane = it->options.lane;
                  item.byte_cost = normalized_byte_cost (it->options);
                  item.retained_message = std::move (it->options.retained_message);
                  item.application_record = std::move (it->options.application_record);
                  item.turn = create_turn (item.name, item.lane, {},
                                           it->options.holds_application_while_waiting);
                  auto &lane = lane_locked (item.lane);
                  ++lane.messages;
                  lane.bytes += item.byte_cost;
                  lane.queue.push_back (std::move (item));
                  if (lane.queue.back ().application_record)
                      lane.queue.back ().application_record->take_over ();
                  if (it->options.transfer_owner_reservation)
                      it->options.transfer_owner_reservation ();
              }
              for (auto &item : suffix)
                  _application.queue.push_back (std::move (item));
              refresh_application_tail_locked ();
              if (shared_spot_gate ())
                  _spot_gate->disconnect_actor_head (shared_from_this ());
              const auto scheduled = schedule_drain_locked ();
              if (!scheduled) {
                  if (lock.owns_lock ())
                      lock.unlock ();
                  cancel_pending_owned ();
              }
              return scheduled;
          });
      });
    if (!outcome) {
        if (const auto *error = outcome.error ();
            error && error->kind () == framework_error_kind_t::shutting_down)
            co_return false;
        co_return detail::propagate_failure<bool> (outcome, "Actor prefix admission failed");
    }
    co_return outcome.value ();
}

task_t<std::vector<std::shared_ptr<serial_application_record_t>>>
serial_execution_queue_t::take_application_suffix (std::function<task_t<void> ()> seal)
{
    auto outcome =
      co_await retained_control_task<std::vector<std::shared_ptr<serial_application_record_t>>> (
        [this, seal = std::move (
                 seal)] () -> task_t<std::vector<std::shared_ptr<serial_application_record_t>>> {
            if (seal)
                co_await seal ();
            co_return co_await control_async<
              std::vector<std::shared_ptr<serial_application_record_t>>> ([this] {
                if (owns_shared_gate ())
                    import_publications ();
                std::unique_lock lock (_mutex, std::defer_lock);
                if (!owns_shared_gate ())
                    lock.lock ();
                std::vector<std::shared_ptr<serial_application_record_t>> suffix;
                suffix.reserve (_application.queue.size ());
                for (auto it = _application.queue.begin (); it != _application.queue.end ();) {
                    if (!it->application_record) {
                        ++it;
                        continue;
                    }
                    it->application_record->take_over ();
                    auto &owned = std::get<serial_application_admission_t::durable_t> (
                      it->application_record->admission.reservation);
                    owned.supply.emplace<serial_application_admission_t::initial_t> ();
                    if (auto retire = std::exchange (it->application_record->retire_invocation, {}))
                        retire (result_t<void>::success ());
                    suffix.push_back (std::move (it->application_record));
                    --_application.messages;
                    _application.bytes -= it->byte_cost;
                    it = _application.queue.erase (it);
                }
                _application_tail_record.reset ();
                if (shared_spot_gate ())
                    _spot_gate->disconnect_actor_head (shared_from_this ());
                return suffix;
            });
        });
    if (!outcome)
        co_return detail::propagate_failure<
          std::vector<std::shared_ptr<serial_application_record_t>>> (
          outcome, "Actor suffix mailbox release failed");
    co_return std::move (outcome.value ());
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
    return try_post_cancellable_async (std::move (name), std::move (work),
                                       adapt_cancellation (std::move (cancel)),
                                       std::move (options));
}

result_t<serial_submission_id_t> serial_execution_queue_t::try_post_cancellable_async (
  std::string name, async_work_t work, cancellation_t cancel, serial_work_options_t options)
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
              framework_error_kind_t::shutting_down,
              detail::registered_execution_context_shutting_down_message);
        }
    }
    if (transfer_owner_reservation) {
        [&] () noexcept { transfer_owner_reservation (); }();
    }
    return result_t<serial_submission_id_t>::success (submission_id);
}

task_t<serial_cancel_submission_outcome_t>
serial_execution_queue_t::cancel_submission (serial_submission_id_t submission_id)
{
    auto pending = std::make_shared<task_t<serial_cancel_submission_outcome_t>> (
      control_async<serial_cancel_submission_outcome_t> ([this, submission_id] {
          if (owns_shared_gate ())
              import_publications ();
          return cancel_submission_owned (submission_id);
      }));
    auto completion =
      std::make_shared<task_completion_source_t<serial_cancel_submission_outcome_t>> ();
    auto result = completion->task ();
    detail::observe_task_terminal (
      *pending, [this, retained = weak_from_this ().lock (), pending,
                 completion] (const result_t<serial_cancel_submission_outcome_t> &outcome) {
          if (outcome) {
              completion->complete (outcome);
          } else {
              report_deferred_error ("cancel-submission", outcome.exception ());
              completion->complete (result_t<serial_cancel_submission_outcome_t>::success (
                serial_cancel_submission_outcome_t::already_terminal));
          }
      });
    return result;
}

serial_cancel_submission_outcome_t
serial_execution_queue_t::cancel_submission_owned (serial_submission_id_t submission_id) noexcept
{
    if (submission_id == 0)
        return serial_cancel_submission_outcome_t::already_terminal;
    std::optional<work_item_t> cancelled_item;
    cancellation_t active_cancel;
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
            if (cancelled_item->application_record)
                refresh_application_tail_locked ();
            if (cancelled_item->barrier)
                refresh_application_barriers_locked ();
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
            if (_active_turn && _active_turn->submission_id == submission_id
                && _active_turn->turn) {
                outcome = serial_cancel_submission_outcome_t::active_cancel_requested;
                if (!_active_turn->cancel_requested) {
                    _active_turn->cancel_requested = true;
                    active_cancel = std::move (_active_turn->cancel);
                }
            }
            if (outcome == serial_cancel_submission_outcome_t::already_terminal
                && _suspended_lifecycle && _suspended_lifecycle->submission_id == submission_id
                && !_suspended_lifecycle->turn->released ()) {
                outcome = serial_cancel_submission_outcome_t::active_cancel_requested;
                if (!_suspended_lifecycle->cancel_requested) {
                    _suspended_lifecycle->cancel_requested = true;
                    active_cancel = std::move (_suspended_lifecycle->cancel);
                }
            }
        }
    }

    if (cancelled_item && cancelled_item->application_record)
        cancelled_item->application_record->admission.discard ();
    if (cancelled_item && cancelled_item->cancel)
        cancelled_item->cancel (outcome);
    if (active_cancel)
        active_cancel (outcome);
    if (shared_spot_gate () && outcome == serial_cancel_submission_outcome_t::queued_cancelled)
        _spot_gate->disconnect_actor_head (shared_from_this ());
    if (shared_spot_gate ())
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
    std::function<task_t<void> ()> deferred;
    if (work) {
        deferred = [work = std::move (work)] () mutable {
            work ();
            return task_t<void> (result_t<void>::success ());
        };
    }
    return try_post_deferred_task (std::move (name), std::move (deferred));
}

bool serial_execution_queue_t::try_post_deferred_task (std::string name,
                                                       std::function<task_t<void> ()> work)
{
    if (!work)
        throw std::invalid_argument ("serial execution queue work is empty");
    // The current turn owns this accepted after-handler position through the
    // supplied task's terminal, using the same existing deferred barrier.
    auto *turn =
      dynamic_cast<serial_turn_handle_impl_t *> (detail::capture_current_serial_turn ().get ());
    if (turn == nullptr || !turn->belongs_to (this))
        return false;
    return static_cast<bool> (
      turn->register_deferred (std::move (name), std::move (work), {}, true));
}

result_t<std::shared_ptr<detail::deferred_barrier_t>>
serial_execution_queue_t::reserve_deferred_barrier_next (
  std::string name,
  std::function<void ()> cancel,
  std::function<void (detail::deferred_barrier_t::async_completion_t)> settle)
{
    auto barrier = std::make_shared<serial_deferred_barrier_t> (
      std::move (cancel), std::move (settle),
      [this, retained = weak_from_this ().lock ()] { schedule_after_settle (); });
    bool accepted;
    {
        std::lock_guard lock (_mutex);
        accepted = !_closed
                   && enqueue_locked (
                     std::move (name),
                     [barrier] (auto complete) { barrier->reached (std::move (complete)); },
                     serial_work_options_t{.lane = serial_work_lane_t::lifecycle,
                                           .holds_application_while_waiting = true},
                     0, [barrier] (auto) { barrier->cancel (); }, item_origin_t{{}, barrier});
    }
    if (!accepted) {
        barrier->cancel ();
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          framework_error_kind_t::shutting_down, "Deferred lifecycle queue is closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (std::move (barrier));
}

bool serial_execution_queue_t::try_post_continuation (
  std::string name,
  async_work_t work,
  const std::shared_ptr<detail::serial_turn_t> &accepted_turn,
  bool holds_application_while_waiting)
{
    const auto turn = std::dynamic_pointer_cast<serial_turn_handle_impl_t> (accepted_turn);
    assert (turn);
    // The accepted operation retains its source FIFO while its callback uses
    // the execution owner selected by the current Spot membership.
    const auto chain = turn->chain ();
    std::lock_guard<std::mutex> lock (_mutex);
    return enqueue_locked (
      std::move (name), std::move (work),
      serial_work_options_t{.lane = turn->lane (),
                            .holds_application_while_waiting = holds_application_while_waiting},
      0, {}, item_origin_t{chain, {}});
}

void serial_execution_queue_t::hold_lifecycle (std::shared_ptr<serial_turn_chain_t> chain)
{
    notify_control ("hold-lifecycle", [this, chain = std::move (chain)] () mutable {
        hold_lifecycle_owned (std::move (chain));
    });
}

void serial_execution_queue_t::hold_lifecycle_owned (std::shared_ptr<serial_turn_chain_t> chain)
{
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    _lifecycle_hold = std::move (chain);
    if (shared_spot_gate ())
        _spot_gate->connect_actor_head (shared_from_this ());
}

void serial_execution_queue_t::release_lifecycle_hold (std::shared_ptr<serial_turn_chain_t> chain)
{
    notify_control ("release-lifecycle", [this, chain = std::move (chain)] {
        release_lifecycle_hold_owned (chain.get ());
    });
}

void serial_execution_queue_t::release_lifecycle_hold_owned (const serial_turn_chain_t *chain)
{
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
    notify_control ("settle", [this] { schedule_after_settle_owned (); });
}

void serial_execution_queue_t::schedule_after_settle_owned ()
{
    std::unique_lock lock (_mutex, std::defer_lock);
    if (!owns_shared_gate ())
        lock.lock ();
    refresh_application_barriers_locked ();
    (void) schedule_drain_locked ();
}

result_t<std::shared_ptr<detail::deferred_barrier_t>>
serial_execution_queue_t::reserve_barrier_next (
  std::string name,
  bool holds_application_while_waiting,
  std::function<task_t<std::vector<std::shared_ptr<serial_execution_queue_t>>> ()> participants)
{
    auto barrier = std::make_shared<serial_deferred_barrier_t> ();
    serial_work_options_t options{serial_work_lane_t::lifecycle};
    options.holds_application_while_waiting = holds_application_while_waiting && !participants;
    const auto submission = try_post_cancellable_async (
      std::move (name),
      [barrier, holds_application_while_waiting,
       participants = std::move (participants)] (auto complete) mutable {
          if (!participants) {
              barrier->reached (std::move (complete));
              return;
          }
          if (!barrier->prepare_participants (std::move (complete)))
              return;
          const auto turn = detail::capture_current_serial_turn ();
          barrier->acquire (
            std::move (participants),
            [barrier, turn, holds_application_while_waiting] (const result_t<void> &result) {
                const auto current = detail::capture_current_serial_turn ();
                if (current != turn || !turn || turn->released ()) {
                    barrier->cancel ();
                    return;
                }
                // The acquisition coroutine resumes this original lifecycle
                // item. Its one hold value follows the turn through suspension.
                std::static_pointer_cast<serial_turn_handle_impl_t> (turn)
                  ->set_holds_application_while_waiting (result && holds_application_while_waiting);
                barrier->reached ({});
            });
      },
      [barrier] { barrier->cancel (); }, options);
    if (!submission) {
        barrier->cancel ();
        return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
          submission.error_kind (), "Relocation readiness queue is closed");
    }
    return result_t<std::shared_ptr<detail::deferred_barrier_t>>::success (std::move (barrier));
}

result_t<void> serial_execution_queue_t::retain_mailbox_until (task_t<void> terminal,
                                                               std::function<void ()> cancel)
{
    return retain_mailbox_until (std::move (terminal), std::move (cancel), {});
}

result_t<void> serial_execution_queue_t::retain_mailbox_until (
  task_t<void> terminal,
  std::function<void ()> cancel,
  std::function<void (const result_t<void> &)> released)
{
    if (closed ())
        return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                        "Actor mailbox is closed");
    auto pending = std::make_shared<task_t<void>> (std::move (terminal));
    if (!submit_control ([this, retained = weak_from_this ().lock (), pending,
                          cancel = std::move (cancel), released = std::move (released)] () mutable {
            std::unique_lock lock (_mutex, std::defer_lock);
            if (!owns_shared_gate ())
                lock.lock ();
            if (_closed) {
                if (lock.owns_lock ())
                    lock.unlock ();
                if (cancel)
                    cancel ();
                if (released)
                    released (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                       "Actor mailbox is closed"));
                return;
            }
            if (_active_turn && _active_turn->turn) {
                _active_turn->turn->retain_until (std::move (*pending));
                if (released)
                    released (result_t<void>::success ());
                if (cancel) {
                    auto previous = std::move (_active_turn->cancel);
                    _active_turn->cancel = [previous = std::move (previous),
                                            cancel = std::move (cancel)] (auto disposition) {
                        if (previous)
                            previous (disposition);
                        cancel ();
                    };
                }
                return;
            }
            auto turn =
              create_turn ("actor-mailbox-terminal", serial_work_lane_t::application, {}, true);
            ++_active;
            _active_turn.emplace ();
            _active_turn->turn = turn;
            _active_turn->cancel = adapt_cancellation (std::move (cancel));
            turn->retain_until (std::move (*pending), std::move (released));
            if (shared_spot_gate ())
                _spot_gate->disconnect_actor_head (shared_from_this ());
            if (lock.owns_lock ())
                lock.unlock ();
            turn->idle_handler_terminal ();
            (void) turn->complete ([] {});
        }))
        return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                        "Actor mailbox executor is stopping");
    return result_t<void>::success ();
}

result_t<std::shared_ptr<serial_handler_participant_t>>
serial_execution_queue_t::observe_handler_terminal ()
{
    if (closed ())
        return result_t<std::shared_ptr<serial_handler_participant_t>>::failure (
          framework_error_kind_t::shutting_down, "Actor mailbox is closed");
    auto handler = std::make_shared<task_completion_source_t<void>> ();
    auto observer = std::make_shared<serial_handler_observer_t> (handler);
    auto retained =
      retain_mailbox_until (observer->terminal (), [observer] { observer->cancel (); });
    if (!retained) {
        observer->cancel ();
        return result_t<std::shared_ptr<serial_handler_participant_t>>::failure (
          retained.error_kind (), "Actor mailbox executor is stopping");
    }
    // Retention is published first. H may finish between the publications,
    // while its existing M still owns the observer's release continuation.
    const auto accepted =
      submit_control ([this, retained = weak_from_this ().lock (), handler, observer] {
          std::shared_ptr<task_t<void>> pending;
          {
              std::unique_lock lock (_mutex, std::defer_lock);
              if (!owns_shared_gate ())
                  lock.lock ();
              if (!_closed && _active_turn && _active_turn->turn)
                  pending = std::make_shared<task_t<void>> (_active_turn->turn->observe_handler ());
          }
          if (_closed) {
              observer->cancel ();
          } else if (pending) {
              detail::observe_task_terminal (
                *pending,
                [handler, pending] (const result_t<void> &result) { handler->complete (result); });
          } else {
              handler->complete (result_t<void>::success ());
          }
      });
    if (!accepted) {
        observer->cancel ();
        return result_t<std::shared_ptr<serial_handler_participant_t>>::failure (
          framework_error_kind_t::shutting_down, "Actor mailbox executor is stopping");
    }
    return result_t<std::shared_ptr<serial_handler_participant_t>>::success (std::move (observer));
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
            else if (!shared_spot_gate ()) {
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

task_t<std::shared_ptr<const void>> serial_execution_queue_t::first_pending_message_or_close ()
{
    return control_async<std::shared_ptr<const void>> (
      [this] { return first_pending_message_or_close_owned (); });
}

std::shared_ptr<const void> serial_execution_queue_t::first_pending_message_or_close_owned ()
{
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

task_t<std::vector<std::shared_ptr<const void>>> serial_execution_queue_t::pending_messages () const
{
    return const_cast<serial_execution_queue_t *> (this)
      ->control_async<std::vector<std::shared_ptr<const void>>> (
        [this] { return pending_messages_owned (); });
}

std::vector<std::shared_ptr<const void>> serial_execution_queue_t::pending_messages_owned () const
{
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
    notify_control ("cancel-waits", [this] { cancel_waits_owned (); });
}

void serial_execution_queue_t::cancel_waits_owned () noexcept
{
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
    close ();
    notify_control ("cancel-pending", [this] {
        if (owns_shared_gate ())
            import_publications ();
        cancel_pending_owned ();
    });
}

void serial_execution_queue_t::cancel_pending_work (
  const std::shared_ptr<serial_application_record_t> &record, const cancellation_t &cancel) noexcept
{
    if (record) {
        record->admission.discard ();
        if (auto retire = std::exchange (record->retire_invocation, {}))
            retire (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                             "Actor application queue is stopping"));
    }
    if (cancel)
        cancel (serial_cancel_submission_outcome_t::queued_cancelled);
}

void serial_execution_queue_t::cancel_pending_owned () noexcept
{
    std::vector<work_item_t> cancelled_items;
    std::vector<cancellation_t> active_cancellations;
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
                  if (item.chain || (_lane_policy.allows_turn_yield () && item.actor_mailbox))
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
        refresh_application_tail_locked ();
        refresh_application_barriers_locked ();
        if (_active_turn && _active_turn->turn && _active_turn->cancel
            && !_active_turn->cancel_requested) {
            _active_turn->cancel_requested = true;
            active_cancellations.push_back (std::move (_active_turn->cancel));
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
        cancel_pending_work (item.application_record, item.cancel);
    }
    for (auto &cancel : active_cancellations)
        cancel (serial_cancel_submission_outcome_t::active_cancel_requested);
    if (shared_spot_gate ())
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
    if (shared_spot_gate ()) {
        if (current_shared_gate == _spot_gate.get ())
            _spot_gate->connect_actor_head (shared_from_this ());
        return true;
    }
    if (!owns_shared_gate () && _active == 0)
        prepare_application_head_locked ();
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
    const bool scheduled = shared_spot_gate ()
                             ? _spot_gate->submit_control ([actor = shared_from_this ()] {
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
            auto &lane = lane_locked (next->item->lane);
            lane.queue.push_back (std::move (*next->item));
            next->item.reset ();
            accept_application_owned (lane.queue.back ());
            if (lane.queue.back ().barrier)
                refresh_application_barriers_locked ();
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
    if (!owns_shared_gate ()) {
        control ();
        return true;
    }
    auto *gate = shared_spot_gate () ? _spot_gate.get () : this;
    if (current_shared_gate == gate) {
        control ();
        return true;
    }
    if (shared_spot_gate ())
        return _spot_gate->submit_control (std::move (control));
    auto node = std::make_unique<publication_node_t> ();
    node->control = std::move (control);
    std::lock_guard lock (_mutex);
    return publish_locked (std::move (node));
}

void serial_execution_queue_t::notify_control (std::string name, std::function<void ()> control)
{
    if (!submit_control (
          [this, retained = weak_from_this ().lock (), name, control = std::move (control)] {
              try {
                  control ();
              }
              catch (const std::exception &) {
                  report_deferred_error (name, std::current_exception ());
              }
          }))
        report_deferred_error (
          name, std::make_exception_ptr (framework_exception_t (
                  framework_error_kind_t::shutting_down, "shared Spot gate executor is stopping")));
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
        if (_active == 0)
            prepare_application_head_locked ();
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
                                               cancellation_t cancel,
                                               item_origin_t origin)
{
    const auto bytes = normalized_byte_cost (options);
    auto &lane = lane_locked (options.lane);
    auto turn = create_turn (name, options.lane, origin, options.holds_application_while_waiting);
    work_item_t item{std::move (name),
                     std::move (work),
                     options.lane,
                     bytes,
                     submission_id,
                     std::move (cancel),
                     std::move (turn),
                     false,
                     {},
                     std::move (origin.chain),
                     std::move (origin.barrier),
                     std::move (options.retained_message),
                     std::move (options.application_record)};
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
    accept_application_owned (lane.queue.back ());
    if (lane.queue.back ().barrier)
        refresh_application_barriers_locked ();
    if (schedule_drain_locked ())
        return true;
    const bool barrier = bool (lane.queue.back ().barrier);
    const bool application_record = bool (lane.queue.back ().application_record);
    lane.queue.pop_back ();
    if (application_record)
        refresh_application_tail_locked ();
    if (barrier)
        refresh_application_barriers_locked ();
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
    if (queue.empty () || (queue.front ().barrier && queue.front ().barrier->pending ()))
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

std::deque<serial_execution_queue_t::work_item_t>::iterator
serial_execution_queue_t::next_application_locked () noexcept
{
    if (!_application_start_policy)
        return _application.queue.begin ();
    return std::find_if (_application.queue.begin (), _application.queue.end (),
                         [this] (const work_item_t &item) {
                             return _application_start_policy (item.retained_message);
                         });
}

bool serial_execution_queue_t::application_ready_locked () noexcept
{
    const auto next = next_application_locked ();
    return next != _application.queue.end ()
           && (!next->application_record || next->application_record->admission.ready ())
           && (!_suspended_lifecycle
               || !_suspended_lifecycle->turn->holds_application_while_waiting ())
           && _application_barriers == 0;
}

std::shared_ptr<serial_turn_handle_impl_t>
serial_execution_queue_t::create_turn (const std::string &name,
                                       serial_work_lane_t lane,
                                       item_origin_t origin,
                                       bool holds_application_while_waiting)
{
    auto turn = std::make_shared<serial_turn_handle_impl_t> (
      *this, name, lane, static_cast<bool> (origin.barrier), std::move (origin.chain),
      holds_application_while_waiting);
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
        const auto next = selected_lane == serial_work_lane_t::lifecycle
                            ? lifecycle_next
                            : next_application_locked ();
        item = std::move (*next);
        queue.erase (next);
        if (item.application_record && _application_tail_record.lock () == item.application_record)
            refresh_application_tail_locked ();
        if (item.barrier)
            refresh_application_barriers_locked ();
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
    if (actor->_active == 0)
        actor->prepare_application_head_locked ();
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
    if (actor_item->lane == serial_work_lane_t::lifecycle) {
        spot_item.work = [actor, actor_item, actor_turn] (auto spot_complete) mutable {
            // This ticket owns the shared execution gate for one Actor turn.
            // The Actor lifecycle item retains its own FIFO until completion.
            detail::serial_turn_scope_t scope (actor_turn);
            actor_item->work ([actor_turn] (std::function<void ()> finish) {
                (void) actor_turn->complete (std::move (finish));
            });
            if (!actor_turn->released ())
                actor->suspend_lifecycle (std::move (*actor_item));
            spot_complete ([] {});
        };
        execute_item (std::move (spot_item));
        return;
    }
    actor_turn->link_handler (spot_turn);
    spot_turn->link_handler (actor_turn);
    spot_item.work = [actor, actor_item, spot_turn, actor_turn] (auto spot_complete) mutable {
        actor_item->work ([spot_complete = std::move (spot_complete),
                           handler = std::weak_ptr<serial_turn_handle_impl_t> (spot_turn),
                           actor_turn] (std::function<void ()> finish) mutable {
            const auto spot_turn = handler.lock ();
            if (!spot_turn) {
                (void) actor_turn->complete (std::move (finish));
                return;
            }
            auto terminal = [actor_turn, spot_turn, finish = std::move (finish)] () mutable {
                std::exception_ptr error;
                try {
                    if (finish)
                        finish ();
                }
                catch (...) {
                    error = std::current_exception ();
                }
                (void) actor_turn->complete ([] {});
                if (error)
                    std::rethrow_exception (error);
            };
            if (spot_turn && spot_turn->released ())
                (void) spot_turn->finish_handler (std::move (terminal), true);
            else
                spot_complete (std::move (terminal));
        });
        if (actor_item->lane == serial_work_lane_t::application
            && actor_item->turn->holds_application_while_waiting () && !actor_turn->released ()
            && !spot_turn->released ())
            (void) spot_turn->release ();
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
        if (_active != 0) {
            // complete_one owns the next inline claim or clears _draining and schedules it.
            _draining = true;
            return;
        }
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

task_t<bool> serial_execution_queue_t::try_resume_suspended (
  const std::shared_ptr<serial_turn_handle_impl_t> &turn, std::function<void ()> work)
{
    return control_async<bool> ([this, turn, work = std::move (work)] () mutable {
        return resume_suspended_owned (turn, std::move (work));
    });
}

bool serial_execution_queue_t::resume_suspended_owned (
  const std::shared_ptr<serial_turn_handle_impl_t> &turn, std::function<void ()> work)
{
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
    if (turn->lane () == serial_work_lane_t::lifecycle) {
        std::unique_lock lock (_mutex, std::defer_lock);
        if (!owns_shared_gate ())
            lock.lock ();
        if (_suspended_lifecycle && _suspended_lifecycle->turn == turn) {
            _suspended_lifecycle->suspended_completion = std::move (completion);
            (void) schedule_drain_locked ();
            return;
        }
    }
    try {
        if (completion)
            completion ();
    }
    catch (...) {
        report_deferred_error (name, std::current_exception ());
    }
    turn->release_mailbox_owned ([this, retained = weak_from_this ().lock (), turn,
                                  name = std::move (name),
                                  allow_inline_claim] (std::unique_lock<std::mutex> claim) mutable {
        return complete_one (std::move (name), [] {}, allow_inline_claim, std::move (claim));
    });
}

result_t<void> serial_execution_queue_t::complete_one (std::string name,
                                                       std::function<void ()> completion,
                                                       bool allow_inline_claim,
                                                       std::unique_lock<std::mutex> claim)
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
        assert (current_shared_gate == (shared_spot_gate () ? _spot_gate.get () : this));
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
        if (shared_spot_gate ())
            _spot_gate->connect_actor_head (shared_from_this ());
        return result_t<void>::success ();
    }
    work_item_t next_item;
    bool execute_next = false;
    {
        if (!claim.mutex ())
            claim = std::unique_lock<std::mutex> (_mutex, std::defer_lock);
        if (!claim.owns_lock ())
            claim.lock ();
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
        prepare_application_head_locked ();
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
                if (!schedule_drain_locked ()) {
                    claim.unlock ();
                    cancel_pending_owned ();
                    _empty.notify_all ();
                    return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                    "Actor mailbox executor is stopping");
                }
            }
        } else {
            _draining = false;
            _claim_started_at.reset ();
            if (_active == 0)
                _empty.notify_all ();
        }
    }
    if (claim.owns_lock ())
        claim.unlock ();
    if (execute_next)
        execute_item (std::move (next_item));
    return result_t<void>::success ();
}

} // namespace zlink::framework::runtime
