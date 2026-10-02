/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/detail/runtime/dispatch/application_job_context.hpp>
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
#include <zlink/framework/detail/infrastructure_wait_context.hpp>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework
{

template <typename T> class task_t;
template <typename T> class task_completion_source_t;

namespace detail
{

using task_scheduler_t = std::function<void (std::function<void ()>)>;

inline constexpr char registered_execution_context_shutting_down_message[] =
  "registered execution context is shutting down";

// Returns a late-bound handoff to the host-owned coroutine executor. Pending
// binding operations own no executor lifetime; invocation and post admission
// are serialized with host shutdown.
task_scheduler_t capture_runtime_native_continuation_scheduler ();
void ensure_blocking_submit_allowed ();
std::stop_token current_wait_owner ();
std::stop_token exchange_wait_owner (std::stop_token token);

struct runtime_execution_hooks_t
{
    task_scheduler_t (*capture_scheduler) ();
    void (*ensure_blocking_allowed) ();
};

void set_runtime_execution_hooks (const runtime_execution_hooks_t *hooks) noexcept;


struct serial_resume_failure_t
{
    framework_error_kind_t kind;
    std::string message;
};

void set_serial_resume_failure (framework_error_kind_t kind, std::string message);
std::optional<serial_resume_failure_t> take_serial_resume_failure ();

class deferred_barrier_t
{
  public:
    using async_completion_t = std::function<void (result_t<void>)>;
    using async_work_t = std::function<void (async_completion_t)>;

    virtual ~deferred_barrier_t () = default;
    virtual result_t<void> activate (std::function<void ()> work) = 0;
    // The barrier owns the serial turn until `complete` is invoked.  This is
    // used by deferred Actor operations whose transport tail is asynchronous.
    virtual result_t<void> activate_async (async_work_t work) = 0;
    virtual void cancel () noexcept = 0;
};

using deferred_barrier_reserver_t = std::function<result_t<std::shared_ptr<deferred_barrier_t>> ()>;

class serial_turn_t
{
  public:
    virtual ~serial_turn_t () = default;
    virtual bool release () = 0;
    virtual bool released () const = 0;
    virtual std::stop_token wait_cancellation () const { return {}; }
    virtual void cancel_waits () noexcept {}
    virtual task_scheduler_t resume_scheduler () = 0;
    virtual bool belongs_to (const void *owner) const noexcept = 0;
    // True for a turn released from the previous handler's after-active
    // phase. It still owns the serial queue, but it is no longer the handler
    // turn for lifecycle fences such as relocation readiness.
    virtual bool is_after_active_phase () const noexcept = 0;
    virtual bool allows_yield () const noexcept = 0;
    virtual result_t<void> defer (std::function<void ()> work,
                                  std::function<void ()> cancel = {}) = 0;
    virtual void cancel_deferred () noexcept = 0;
};

std::shared_ptr<serial_turn_t> capture_current_serial_turn ();
std::shared_ptr<serial_turn_t> exchange_current_serial_turn (std::shared_ptr<serial_turn_t> turn);

class serial_turn_scope_t
{
  public:
    explicit serial_turn_scope_t (std::shared_ptr<serial_turn_t> turn) :
        _previous (exchange_current_serial_turn (std::move (turn)))
    {
    }

    ~serial_turn_scope_t () { (void) exchange_current_serial_turn (std::move (_previous)); }

    serial_turn_scope_t (const serial_turn_scope_t &) = delete;
    serial_turn_scope_t &operator= (const serial_turn_scope_t &) = delete;

  private:
    std::shared_ptr<serial_turn_t> _previous;
};

inline task_scheduler_t held_serial_turn_scheduler (std::shared_ptr<serial_turn_t> turn)
{
    return [turn = std::move (turn)] (std::function<void ()> work) mutable {
        serial_turn_scope_t scope (turn);
        if (work) {
            work ();
        }
    };
}

struct serial_turn_await_plan_t
{
    std::shared_ptr<serial_turn_t> turn;
    task_scheduler_t scheduler;
    bool holds_turn = false;
};

inline std::optional<serial_turn_await_plan_t> prepare_serial_turn_await (bool release_turn)
{
    auto turn = capture_current_serial_turn ();
    if (!turn || turn->released ()) {
        return std::nullopt;
    }
    if (release_turn) {
        if (!turn->release ()) {
            return std::nullopt;
        }
        return serial_turn_await_plan_t{turn, turn->resume_scheduler (), false};
    }
    return serial_turn_await_plan_t{turn, held_serial_turn_scheduler (turn), true};
}

inline bool current_serial_turn_allows_yield ()
{
    const auto turn = capture_current_serial_turn ();
    return turn && !turn->released () && turn->allows_yield ();
}

inline result_t<void> defer_current_serial_turn (std::function<void ()> work,
                                                 std::function<void ()> cancel = {})
{
    auto turn = capture_current_serial_turn ();
    if (!turn || turn->released ()) {
        return result_t<void>::failure (framework_error_kind_t::not_configured,
                                        "Actor join defer requires an open Framework handler turn");
    }
    return turn->defer (std::move (work), std::move (cancel));
}

template <typename T> task_t<T> unsupported_yield_task ();

/* 코루틴 일시 중단 중 실행 문맥 전달(flow-correlation MFLOW-EXT-014).
 * Framework 라이브러리가 hook 쌍을 원자적으로 등록한다. 재개 범위가 끝나면
 * 이전 문맥을 복원하며, hook이 없으면 application job 문맥만 전달한다. */
struct ambient_context_hooks_t
{
    std::shared_ptr<void> (*capture) ();
    std::shared_ptr<void> (*enter) (const std::shared_ptr<void> &);
};

const ambient_context_hooks_t *current_ambient_context_hooks () noexcept;
void set_ambient_context_hooks (const ambient_context_hooks_t *hooks) noexcept;

struct ambient_context_snapshot_t
{
    std::shared_ptr<void> state;
    const void *application_job = nullptr;
    std::stop_token wait_owner;
};

// Extend the existing resume guard with the application job context. The
// pointer is independent of optional tracing and needs no allocation.
class ambient_context_scope_t
{
  public:
    ambient_context_scope_t (std::shared_ptr<void> state,
                             const void *application_job,
                             std::stop_token wait_owner = {},
                             bool inherit_invocation = true) :
        _state (std::move (state)),
        _previous_application_job (application_job_context_t::current ()),
        _previous_wait_owner (exchange_wait_owner (
          inherit_invocation && !wait_owner.stop_possible () ? current_wait_owner () : wait_owner))

    {
        // A callback captured outside a job may run inline inside a handler.
        // An absent captured context must not erase that physical invocation.
        application_job_context_t::exchange (inherit_invocation && application_job == nullptr
                                               ? _previous_application_job
                                               : application_job);
    }

    ~ambient_context_scope_t ()
    {
        application_job_context_t::exchange (_previous_application_job);
        exchange_wait_owner (std::move (_previous_wait_owner));
    }

    ambient_context_scope_t (const ambient_context_scope_t &) = delete;
    ambient_context_scope_t &operator= (const ambient_context_scope_t &) = delete;

  private:
    std::shared_ptr<void> _state;
    const void *_previous_application_job;
    std::stop_token _previous_wait_owner;
};

inline ambient_context_snapshot_t capture_ambient_context ()
{
    const auto *hooks = current_ambient_context_hooks ();
    return {hooks != nullptr ? hooks->capture () : nullptr, application_job_context_t::current (),
            current_wait_owner ()};
}

inline ambient_context_scope_t enter_ambient_context (const ambient_context_snapshot_t &snapshot,
                                                      bool inherit_invocation = true)
{
    const auto *hooks = current_ambient_context_hooks ();
    return {hooks != nullptr && snapshot.state ? hooks->enter (snapshot.state) : nullptr,
            snapshot.application_job, snapshot.wait_owner, inherit_invocation};
}

// Native binding awaitables can ask the active Framework promise for this
// handoff. It carries only the currently-held serial turn and ambient context;
// admission retry/readiness remains owned by the binding.
inline task_scheduler_t capture_native_continuation_scheduler ()
{
    auto turn_plan = prepare_serial_turn_await (false);
    task_scheduler_t turn_scheduler =
      turn_plan ? std::move (turn_plan->scheduler) : task_scheduler_t{};
    task_scheduler_t runtime_scheduler = capture_runtime_native_continuation_scheduler ();
    auto ambient = capture_ambient_context ();
    if (!turn_scheduler && !runtime_scheduler && !ambient.state && !ambient.application_job
        && !ambient.wait_owner.stop_possible ())
        return [] (std::function<void ()> work) {
            if (work)
                work ();
        };
    return [turn_scheduler = std::move (turn_scheduler),
            runtime_scheduler = std::move (runtime_scheduler),
            ambient = std::move (ambient)] (std::function<void ()> work) mutable {
        auto resume = [ambient, work = std::move (work)] () mutable {
            const auto ambient_guard = enter_ambient_context (ambient);
            if (work)
                work ();
        };
        auto dispatch = [turn_scheduler, resume = std::move (resume)] () mutable {
            if (turn_scheduler) {
                turn_scheduler (std::move (resume));
            } else {
                resume ();
            }
        };
        if (runtime_scheduler) {
            runtime_scheduler (std::move (dispatch));
        } else {
            dispatch ();
        }
    };
}

// A registration settles only when resumption starts or its execution owner
// cancels it. A queued result delivery therefore cannot revive a stopped turn.
class task_wait_registration_t : public std::enable_shared_from_this<task_wait_registration_t>
{
  public:
    task_wait_registration_t (std::coroutine_handle<> handle, task_scheduler_t scheduler) :
        task_wait_registration_t (
          [handle] (bool cancelled) {
              if (cancelled)
                  set_serial_resume_failure (framework_error_kind_t::shutting_down,
                                             registered_execution_context_shutting_down_message);
              handle.resume ();
              if (cancelled)
                  (void) take_serial_resume_failure ();
          },
          std::move (scheduler))
    {
    }

    task_wait_registration_t (std::function<void (bool)> work,
                              task_scheduler_t explicit_scheduler) :
        _work (std::move (work)),
        _turn (capture_current_serial_turn ()),
        _ambient (capture_ambient_context ())
    {
        if (_turn && _turn->released ())
            _turn.reset ();
        if (_turn)
            _ambient.wait_owner = _turn->wait_cancellation ();
        if (_turn || _ambient.application_job)
            _scheduler = capture_runtime_native_continuation_scheduler ();
        if (explicit_scheduler) {
            _scheduler = [explicit_scheduler = std::move (explicit_scheduler),
                          runtime = std::move (_scheduler)] (std::function<void ()> work) {
                explicit_scheduler ([runtime, work = std::move (work)] () mutable {
                    if (runtime)
                        runtime (std::move (work));
                    else if (work)
                        work ();
                });
            };
        }
    }

    bool has_wait_owner () const noexcept { return _ambient.wait_owner.stop_possible (); }

    void bind_owner (std::weak_ptr<void> state,
                     void (*cleanup) (const std::shared_ptr<void> &,
                                      const task_wait_registration_t *))
    {
        if (_ambient.wait_owner.stop_possible ())
            _cancellation.emplace (_ambient.wait_owner,
                                   cancellation_t{weak_from_this (), std::move (state), cleanup});
    }

    bool pending () const noexcept { return !_started.load (std::memory_order_acquire); }

    bool deliver (std::shared_ptr<void> lifetime) noexcept
    {
        if (_started.load (std::memory_order_acquire))
            return true;
        auto self = shared_from_this ();
        try {
            if (_scheduler)
                _scheduler ([self, lifetime = std::move (lifetime)] {
                    (void) lifetime; // Retain the terminal result until delivery finishes.
                    self->resume (false);
                });
            else
                self->resume (false);
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::shutting_down)
                std::terminate ();
            if (_ambient.wait_owner.stop_requested ())
                return true;
            if (_started.load (std::memory_order_acquire))
                std::terminate ();
            if (_ambient.wait_owner.stop_possible ())
                return false;
            std::terminate ();
        }
        return true;
    }

    bool cancel () noexcept { return resume (true); }

  private:
    bool resume (bool cancelled) noexcept
    {
        bool pending = false;
        if (!_started.compare_exchange_strong (pending, true, std::memory_order_acq_rel))
            return true;
        const auto ambient_guard = enter_ambient_context (_ambient, false);
        serial_turn_scope_t turn_guard (_turn);
        auto work = std::move (_work);
        work (cancelled);
        return true;
    }

    std::atomic_bool _started{false};
    std::function<void (bool)> _work;
    task_scheduler_t _scheduler;
    std::shared_ptr<serial_turn_t> _turn;
    ambient_context_snapshot_t _ambient;
    struct cancellation_t
    {
        std::weak_ptr<task_wait_registration_t> waiter;
        std::weak_ptr<void> state;
        void (*cleanup) (const std::shared_ptr<void> &, const task_wait_registration_t *);
        void operator() () const noexcept
        {
            if (auto self = waiter.lock ()) {
                self->cancel ();
                if (auto owner = state.lock ())
                    cleanup (owner, self.get ());
            }
        }
    };
    std::optional<std::stop_callback<cancellation_t>> _cancellation;
};

template <typename T>
class task_shared_state_t : public std::enable_shared_from_this<task_shared_state_t<T>>
{
    struct callback_t
    {
        std::function<void (const result_t<T> &)> invoke;
        ambient_context_snapshot_t ambient;
        std::unique_ptr<task_scheduler_t> scheduler;
        std::shared_ptr<serial_turn_t> owner;
    };

  public:
    virtual ~task_shared_state_t () = default;

    bool complete (result_t<T> result)
    {
        std::vector<std::shared_ptr<task_wait_registration_t>> continuations;
        std::vector<callback_t> callbacks;
        std::vector<callback_t> terminal_callbacks;
        auto self = this->shared_from_this ();
        {
            std::lock_guard lock (_mutex);
            if (_result) {
                return false;
            }
            _result.emplace (std::move (result));
            continuations = std::move (_continuations);
            callbacks = std::move (_callbacks);
            terminal_callbacks = std::move (_terminal_callbacks);
        }
        _ready.notify_all ();
        // Internal delivery defects terminate; the first result is already fixed.
        [&] () noexcept {
            for (auto &callback : terminal_callbacks) {
                const auto ambient_guard = enter_ambient_context (callback.ambient);
                callback.invoke (*_result);
            }
            for (auto &callback : callbacks) {
                auto scheduler = std::move (callback.scheduler);
                if (!scheduler) {
                    const auto ambient_guard = enter_ambient_context (callback.ambient);
                    callback.invoke (*_result);
                    continue;
                }
                auto invoke = [self, invoke = std::move (callback.invoke),
                               ambient = callback.ambient, owner = callback.owner] () noexcept {
                    (void) owner;
                    const auto ambient_guard = enter_ambient_context (ambient);
                    invoke (*self->_result);
                };
                (*scheduler) (std::move (invoke));
            }
            for (auto &continuation : continuations)
                deliver_registration (continuation, self);
        }();
        return true;
    }

    bool is_ready () const
    {
        std::lock_guard lock (_mutex);
        return _result.has_value ();
    }

    void set_continuation (std::coroutine_handle<> continuation,
                           task_scheduler_t explicit_scheduler = {})
    {
        auto self = this->shared_from_this ();
        std::shared_ptr<task_shared_state_t<T>> lifetime;
        auto registration =
          std::make_shared<task_wait_registration_t> (continuation, std::move (explicit_scheduler));
        const auto owner_state = registration->has_wait_owner ()
                                   ? this->weak_from_this ()
                                   : std::weak_ptr<task_shared_state_t<T>>{};
        {
            std::lock_guard lock (_mutex);
            if (_result) {
                lifetime = std::move (self);
            } else {
                _continuations.push_back (registration);
            }
        }
        bind_wait_owner (registration, owner_state);
        if (lifetime)
            lifetime->deliver_registration (registration, lifetime);
    }

    const result_t<T> &result ()
    {
        std::unique_lock lock (_mutex);
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        check_infrastructure_wait (_result.has_value (), "task/result");
#endif
        _ready.wait (lock, [&] { return _result.has_value (); });
        return *_result;
    }

    std::optional<result_t<T>> result_for (std::chrono::milliseconds timeout,
                                           std::stop_token cancellation = {})
    {
        std::stop_callback wake_waiter (cancellation, [this] { _ready.notify_all (); });
        std::unique_lock lock (_mutex);
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        if (timeout > std::chrono::milliseconds::zero ())
            check_infrastructure_wait (_result.has_value () || cancellation.stop_requested (),
                                       "task/result_for");
#endif
        if (!_ready.wait_for (
              lock, timeout, [&] { return _result.has_value () || cancellation.stop_requested (); })
            || !_result)
            return std::nullopt;
        return *_result;
    }

    void on_completed (std::function<void (const result_t<T> &)> callback,
                       task_scheduler_t scheduler = {})
    {
        const auto self = this->shared_from_this ();
        std::shared_ptr<task_wait_registration_t> registration;
        std::weak_ptr<task_shared_state_t<T>> owner_state;
        auto owner = capture_current_serial_turn ();
        if (owner && owner->released ())
            owner.reset ();
        {
            std::lock_guard lock (_mutex);
            if (!_result) {
                const auto token = owner ? owner->wait_cancellation () : current_wait_owner ();
                if (!token.stop_possible ()) {
                    _callbacks.push_back (callback_t{
                      std::move (callback), capture_ambient_context (),
                      scheduler ? std::make_unique<task_scheduler_t> (std::move (scheduler))
                                : nullptr,
                      std::move (owner)});
                    return;
                }
                owner_state = this->weak_from_this ();
                registration = std::make_shared<task_wait_registration_t> (
                  [weak = owner_state, callback = std::move (callback)] (bool cancelled) {
                      if (cancelled) {
                          const auto failure = result_t<T>::failure (
                            framework_error_kind_t::shutting_down,
                            registered_execution_context_shutting_down_message);
                          callback (failure);
                      } else if (auto self = weak.lock ())
                          callback (*self->_result);
                  },
                  std::move (scheduler));
                _continuations.push_back (registration);
            }
        }
        if (registration) {
            bind_wait_owner (registration, owner_state);
            return;
        }
        auto invoke = [self, callback = std::move (callback),
                       ambient = capture_ambient_context ()] () noexcept {
            const auto ambient_guard = enter_ambient_context (ambient);
            callback (*self->_result);
        };
        [&] () noexcept {
            if (scheduler)
                scheduler (std::move (invoke));
            else
                invoke ();
        }();
    }

    void on_terminal (std::function<void (const result_t<T> &)> callback)
    {
        auto snapshot = capture_ambient_context ();
        {
            std::lock_guard lock (_mutex);
            if (!_result) {
                _terminal_callbacks.push_back (callback_t{
                  std::move (callback), std::move (snapshot), {}, capture_current_serial_turn ()});
                return;
            }
        }
        [&] () noexcept {
            const auto ambient_guard = enter_ambient_context (snapshot);
            callback (*_result);
        }();
    }

  private:
    void deliver_registration (const std::shared_ptr<task_wait_registration_t> &registration,
                               const std::shared_ptr<task_shared_state_t<T>> &lifetime) noexcept
    {
        if (registration->deliver (lifetime))
            return;
        // A rejected delivery remains owned until its registered owner stops.
        // Keeping it in the original list also covers ready-registration races.
        std::lock_guard lock (_mutex);
        if (registration->pending ())
            _continuations.push_back (registration);
    }

  protected:
    virtual void bind_wait_owner (const std::shared_ptr<task_wait_registration_t> &,
                                  std::weak_ptr<void>)
    {
    }

    static void erase_registration (const std::shared_ptr<void> &state,
                                    const task_wait_registration_t *registration)
    {
        auto owner = std::static_pointer_cast<task_shared_state_t<T>> (state);
        std::lock_guard lock (owner->_mutex);
        std::erase_if (owner->_continuations,
                       [registration] (const auto &entry) { return entry.get () == registration; });
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::optional<result_t<T>> _result;
    std::vector<std::shared_ptr<task_wait_registration_t>> _continuations;
    std::vector<callback_t> _callbacks;
    std::vector<callback_t> _terminal_callbacks;
};

template <typename T> class application_task_shared_state_t final : public task_shared_state_t<T>
{
  private:
    void bind_wait_owner (const std::shared_ptr<task_wait_registration_t> &registration,
                          std::weak_ptr<void> state) override
    {
        registration->bind_owner (std::move (state), &task_shared_state_t<T>::erase_registration);
    }
};

} // namespace detail

template <typename T> class task_completion_source_t
{
    static_assert (std::is_void_v<T> || std::is_copy_constructible_v<T>);

  public:
    task_completion_source_t () :
        _state (std::make_shared<detail::application_task_shared_state_t<T>> ())
    {
    }
    task_completion_source_t (task_completion_source_t &&) noexcept = default;
    task_completion_source_t &operator= (task_completion_source_t &&) noexcept = default;
    task_completion_source_t (const task_completion_source_t &) = delete;
    task_completion_source_t &operator= (const task_completion_source_t &) = delete;

    task_t<T> task () const;
    bool complete (result_t<T> result) { return _state->complete (std::move (result)); }

  private:
    std::shared_ptr<detail::task_shared_state_t<T>> _state;
};

namespace detail
{

template <typename T, typename TCallback>
void observe_task_completion (task_t<T> &task, TCallback &&callback);

// Explicit terminal owners may need to release a held serial turn at the
// physical task terminal. Unlike ordinary continuations, this observer does
// not schedule back through that same turn.
template <typename T, typename TCallback>
void observe_task_terminal (task_t<T> &task, TCallback &&callback);

template <typename T>
std::optional<result_t<T>> observe_task_result_for (const task_t<T> &task,
                                                    std::chrono::milliseconds timeout,
                                                    std::stop_token cancellation);

template <typename T> task_t<T> reschedule_task (task_t<T> task, task_scheduler_t scheduler);
template <typename T>
task_t<T> with_task_resume_scheduler (task_t<T> task, task_scheduler_t scheduler);

// The one conversion of the exception being handled into a task failure.
// Call only inside a catch handler.
template <typename T> result_t<T> current_exception_result ()
{
    return result_access_t::failure<T> (std::current_exception ());
}

template <typename T> class coroutine_promise_t
{
  public:
    task_scheduler_t zlink_continuation_scheduler ()
    {
        return capture_native_continuation_scheduler ();
    }

    std::suspend_never initial_suspend () noexcept { return {}; }
    std::suspend_never final_suspend () noexcept
    {
        completion->complete (std::move (*_return_result));
        return {};
    }
    void unhandled_exception () { store_return (current_exception_result<T> ()); }

  protected:
    void store_return (result_t<T> result) { _return_result.emplace (std::move (result)); }

    std::shared_ptr<task_shared_state_t<T>> completion =
      std::make_shared<task_shared_state_t<T>> ();

  private:
    std::optional<result_t<T>> _return_result;
};

} // namespace detail

template <typename T> class task_t
{
  public:
    struct promise_type : detail::coroutine_promise_t<T>
    {
        task_t get_return_object () { return task_t<T> (this->completion); }

        void return_value (result_t<T> result) { this->store_return (std::move (result)); }

        template <typename U>
            requires (!std::is_same_v<std::remove_cvref_t<U>, result_t<T>>)
        void return_value (U &&value)
        {
            if constexpr (!std::is_constructible_v<T, U &&> && std::is_constructible_v<T, U &>)
                this->store_return (result_t<T>::success (T (value)));
            else
                this->store_return (result_t<T>::success (T (std::forward<U> (value))));
        }
    };

    explicit task_t (result_t<T> result) :
        _state (std::make_shared<detail::task_shared_state_t<T>> ())
    {
        _state->complete (std::move (result));
    }

    task_t (task_t &&) noexcept = default;
    task_t &operator= (task_t &&) noexcept = default;
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () = default;

    bool await_ready () const noexcept { return _state->is_ready (); }
    void await_suspend (std::coroutine_handle<> continuation)
    {
        _state->set_continuation (continuation, _resume_scheduler ? *_resume_scheduler
                                                                  : detail::task_scheduler_t{});
    }
    T await_resume ()
    {
        if (const auto failure = detail::take_serial_resume_failure ())
            throw framework_exception_t (failure->kind, failure->message);
        return result ().value ();
    }

    const result_t<T> &result () const { return _state->result (); }

    // Submit/completion §1: rejected in a runtime execution context.
    std::optional<result_t<T>> result_for (std::chrono::milliseconds timeout) const
    {
        detail::ensure_blocking_submit_allowed ();
        return _state->result_for (timeout);
    }

  private:
    explicit task_t (std::shared_ptr<detail::task_shared_state_t<T>> state) :
        _state (std::move (state))
    {
    }

    std::shared_ptr<detail::task_shared_state_t<T>> _state;

    std::unique_ptr<detail::task_scheduler_t> _resume_scheduler;

    template <typename U>
    friend task_t<U> detail::with_task_resume_scheduler (task_t<U>, detail::task_scheduler_t);
    friend class task_completion_source_t<T>;
    template <typename U>
    friend std::optional<result_t<U>>
    detail::observe_task_result_for (const task_t<U> &, std::chrono::milliseconds, std::stop_token);
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_completion (task_t<TObserved> &, TCallback &&);
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_terminal (task_t<TObserved> &, TCallback &&);
};

template <> class task_t<void>
{
  public:
    struct promise_type : detail::coroutine_promise_t<void>
    {
        task_t get_return_object () { return task_t<void> (this->completion); }
        void return_void () noexcept { this->store_return (result_t<void>::success ()); }
    };

    explicit task_t (result_t<void> result) :
        _state (std::make_shared<detail::task_shared_state_t<void>> ())
    {
        _state->complete (std::move (result));
    }

    task_t (task_t &&) noexcept = default;
    task_t &operator= (task_t &&) noexcept = default;
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () = default;

    bool await_ready () const noexcept { return _state->is_ready (); }
    void await_suspend (std::coroutine_handle<> continuation)
    {
        _state->set_continuation (continuation, _resume_scheduler ? *_resume_scheduler
                                                                  : detail::task_scheduler_t{});
    }
    void await_resume ()
    {
        if (const auto failure = detail::take_serial_resume_failure ())
            throw framework_exception_t (failure->kind, failure->message);
        result ().value ();
    }

    const result_t<void> &result () const { return _state->result (); }

    // Submit/completion §1: rejected in a runtime execution context.
    std::optional<result_t<void>> result_for (std::chrono::milliseconds timeout) const
    {
        detail::ensure_blocking_submit_allowed ();
        return _state->result_for (timeout);
    }

  private:
    explicit task_t (std::shared_ptr<detail::task_shared_state_t<void>> state) :
        _state (std::move (state))
    {
    }

    std::shared_ptr<detail::task_shared_state_t<void>> _state;

    std::unique_ptr<detail::task_scheduler_t> _resume_scheduler;

    template <typename U>
    friend task_t<U> detail::with_task_resume_scheduler (task_t<U>, detail::task_scheduler_t);
    friend class task_completion_source_t<void>;
    template <typename U>
    friend std::optional<result_t<U>>
    detail::observe_task_result_for (const task_t<U> &, std::chrono::milliseconds, std::stop_token);
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_completion (task_t<TObserved> &, TCallback &&);
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_terminal (task_t<TObserved> &, TCallback &&);
};

template <typename T> task_t<T> task_completion_source_t<T>::task () const
{
    return task_t<T> (_state);
}

namespace detail
{

template <typename T, typename TCallback>
void observe_task_completion (task_t<T> &task, TCallback &&callback)
{
    task._state->on_completed (
      std::function<void (const result_t<T> &)> (std::forward<TCallback> (callback)),
      task._resume_scheduler ? *task._resume_scheduler : task_scheduler_t{});
}

template <typename T, typename TCallback>
void observe_task_terminal (task_t<T> &task, TCallback &&callback)
{
    task._state->on_terminal (
      std::function<void (const result_t<T> &)> (std::forward<TCallback> (callback)));
}

template <typename T>
std::optional<result_t<T>> observe_task_result_for (const task_t<T> &task,
                                                    std::chrono::milliseconds timeout,
                                                    std::stop_token cancellation)
{
    return task._state->result_for (timeout, cancellation);
}

template <typename T>
task_t<T> with_task_resume_scheduler (task_t<T> task, task_scheduler_t scheduler)
{
    task._resume_scheduler =
      scheduler ? std::make_unique<task_scheduler_t> (std::move (scheduler)) : nullptr;
    return task;
}

template <typename T> task_t<T> reschedule_task (task_t<T> task, task_scheduler_t scheduler)
{
    auto source = std::make_shared<task_completion_source_t<T>> ();
    auto output = source->task ();
    auto observed = std::make_shared<task_t<T>> (std::move (task));
    auto target_scheduler = std::make_shared<task_scheduler_t> (std::move (scheduler));
    detail::observe_task_completion (
      *observed, [source, observed, target_scheduler] (const result_t<T> &result) mutable {
          auto complete = [source, result] () mutable { source->complete (std::move (result)); };
          if (*target_scheduler) {
              (*target_scheduler) (std::move (complete));
          } else {
              complete ();
          }
      });
    return output;
}

template <typename T> task_t<T> unsupported_yield_task ()
{
    return task_t<T> (result_t<T>::failure (
      framework_error_kind_t::not_configured,
      "yield is available only in a SpotWide User Spot or Instance Spot callback"));
}


} // namespace detail

} // namespace zlink::framework
