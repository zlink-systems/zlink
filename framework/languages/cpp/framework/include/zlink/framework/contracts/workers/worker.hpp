/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace zlink::framework
{

class spot_context_t;

class worker_options_t
{
  public:
    std::size_t min_threads () const noexcept { return _min_threads; }

    worker_options_t &min_threads (std::size_t value)
    {
        if (value > _max_threads) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "worker minimum thread count must not exceed the maximum");
        }
        ensure_mutable ();
        _min_threads = value;
        return *this;
    }

    std::size_t max_threads () const noexcept { return _max_threads; }

    worker_options_t &max_threads (std::size_t value)
    {
        if (value == 0 || value < _min_threads) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "worker maximum thread count must be positive and cover the minimum");
        }
        ensure_mutable ();
        _max_threads = value;
        return *this;
    }

    std::chrono::milliseconds idle_timeout () const noexcept
    {
        return _idle_timeout;
    }

    worker_options_t &idle_timeout (std::chrono::milliseconds value)
    {
        if (value < std::chrono::milliseconds::zero ()) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "worker idle timeout must not be negative");
        }
        ensure_mutable ();
        _idle_timeout = value;
        return *this;
    }

    std::size_t max_queue_length () const noexcept { return _max_queue_length; }

    worker_options_t &max_queue_length (std::size_t value)
    {
        ensure_mutable ();
        _max_queue_length = value;
        return *this;
    }

  private:
    friend class zlink_framework_options_t;

    static std::size_t default_max_threads () noexcept
    {
        const auto hardware = static_cast<std::size_t> (std::thread::hardware_concurrency ());
        if (hardware == 0)
            return 1;
        return hardware > (std::numeric_limits<std::size_t>::max () / 2)
                 ? std::numeric_limits<std::size_t>::max ()
                 : hardware * 2;
    }

    void ensure_mutable () const
    {
        if (_sealed) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "worker options cannot change after host configuration");
        }
    }

    void seal () noexcept { _sealed = true; }

    std::size_t _min_threads = 0;
    std::size_t _max_threads = default_max_threads ();
    std::chrono::milliseconds _idle_timeout{std::chrono::seconds (30)};
    std::size_t _max_queue_length = 1024;
    bool _sealed = false;
};

namespace detail
{

template <typename TTask> struct task_result;

template <typename TResult> struct task_result<task_t<TResult>>
{
    using type = TResult;
};

template <typename TTask>
using task_result_t = typename task_result<std::remove_cvref_t<TTask>>::type;

template <typename TWork, bool = std::is_invocable_v<TWork &, std::stop_token>>
struct worker_sync_result;

template <typename TWork> struct worker_sync_result<TWork, false>
{
    using type = std::invoke_result_t<TWork &>;
};

template <typename TWork> struct worker_sync_result<TWork, true>
{
    using type = std::invoke_result_t<TWork &, std::stop_token>;
};

template <typename TWork>
using worker_sync_result_t = typename worker_sync_result<TWork>::type;

template <typename TWork, bool = std::is_invocable_v<TWork &, std::stop_token>>
struct worker_async_result;

template <typename TWork> struct worker_async_result<TWork, false>
{
    using task_type = std::invoke_result_t<TWork &>;
};

template <typename TWork> struct worker_async_result<TWork, true>
{
    using task_type = std::invoke_result_t<TWork &, std::stop_token>;
};

template <typename TWork>
using worker_async_task_t = typename worker_async_result<TWork>::task_type;

template <typename TWork>
decltype(auto) invoke_worker_async (TWork &work, std::stop_token cancellation)
{
    if constexpr (std::is_invocable_v<TWork &, std::stop_token>) {
        return work (cancellation);
    } else {
        return work ();
    }
}

class worker_scheduler_t
{
  public:
    virtual ~worker_scheduler_t () = default;

    virtual bool try_schedule (std::function<void (std::stop_token)> work) = 0;
    virtual void post_owner (std::function<void ()> work) = 0;
    virtual std::stop_token stop_token () const noexcept { return {}; }
};

template <typename TResult, typename TWork>
result_t<TResult> run_worker_body (TWork &work, std::stop_token cancellation)
{
    try {
        auto invoke = [&] {
            if constexpr (std::is_invocable_v<TWork &, std::stop_token>) {
                return work (cancellation);
            } else {
                return work ();
            }
        };
        if constexpr (std::is_void_v<TResult>) {
            invoke ();
            return result_t<void>::success ();
        } else {
            return result_t<TResult>::success (invoke ());
        }
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<TResult> (error);
    }
    catch (const std::exception &error) {
        return result_t<TResult>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        return result_t<TResult>::failure (framework_error_kind_t::internal_failure,
                                           "worker task threw an exception");
    }
}

struct worker_control_t
{
    explicit worker_control_t (std::stop_token host_cancellation) : host (host_cancellation) {}

    ~worker_control_t () { cancel_deadline (); }

    std::stop_source cancellation;
    std::optional<std::stop_callback<std::function<void ()>>> host_callback;
    std::stop_token host;

    void arm_deadline (std::chrono::milliseconds timeout,
                       std::function<void ()> callback)
    {
        _deadline = worker_deadline_scheduler_t::instance ().schedule (
          timeout, std::move (callback));
    }

    void cancel_deadline () noexcept
    {
        auto state = _deadline;
        if (!state)
            return;
        _deadline.reset ();
        state->cancelled.store (true, std::memory_order_release);
        worker_deadline_scheduler_t::instance ().wake ();
    }

    void arm_host (std::function<void ()> callback)
    {
        if (host.stop_possible ()) {
            host_callback.emplace (host, std::move (callback));
        }
    }

  private:
    struct deadline_state_t
    {
        std::atomic_bool cancelled{false};
    };

    class worker_deadline_scheduler_t
    {
      public:
        static worker_deadline_scheduler_t &instance ()
        {
            static worker_deadline_scheduler_t scheduler;
            return scheduler;
        }

        std::shared_ptr<deadline_state_t> schedule (
          std::chrono::milliseconds timeout,
          std::function<void ()> callback)
        {
            auto state = std::make_shared<deadline_state_t> ();
            {
                std::lock_guard lock (_mutex);
                _deadlines.push (deadline_t{
                  std::chrono::steady_clock::now () + timeout,
                  _next_sequence++, state, std::move (callback)});
            }
            _changed.notify_one ();
            return state;
        }

        void wake () noexcept { _changed.notify_one (); }

      private:
        struct deadline_t
        {
            std::chrono::steady_clock::time_point at;
            std::uint64_t sequence;
            std::shared_ptr<deadline_state_t> state;
            std::function<void ()> callback;
        };

        struct later_deadline_t
        {
            bool operator() (const deadline_t &left,
                             const deadline_t &right) const noexcept
            {
                return left.at == right.at
                         ? left.sequence > right.sequence
                         : left.at > right.at;
            }
        };

        worker_deadline_scheduler_t () :
            _worker ([this] (std::stop_token stop) { run (stop); })
        {
        }

        ~worker_deadline_scheduler_t ()
        {
            _worker.request_stop ();
            _changed.notify_all ();
        }

        void run (std::stop_token stop)
        {
            std::unique_lock lock (_mutex);
            while (!stop.stop_requested ()) {
                while (!_deadlines.empty ()
                       && _deadlines.top ().state->cancelled.load (
                         std::memory_order_acquire)) {
                    _deadlines.pop ();
                }
                if (_deadlines.empty ()) {
                    _changed.wait (lock, [&] {
                        return stop.stop_requested () || !_deadlines.empty ();
                    });
                    continue;
                }
                const auto at = _deadlines.top ().at;
                if (_changed.wait_until (lock, at) == std::cv_status::no_timeout) {
                    continue;
                }
                auto deadline = std::move (
                  const_cast<deadline_t &> (_deadlines.top ()));
                _deadlines.pop ();
                if (deadline.state->cancelled.exchange (
                      true, std::memory_order_acq_rel)) {
                    continue;
                }
                lock.unlock ();
                deadline.callback ();
                lock.lock ();
            }
        }

        std::mutex _mutex;
        std::condition_variable _changed;
        std::priority_queue<deadline_t,
                            std::vector<deadline_t>,
                            later_deadline_t> _deadlines;
        std::uint64_t _next_sequence = 1;
        std::jthread _worker;
    };

    std::shared_ptr<deadline_state_t> _deadline;
};

template <typename TResult>
task_t<TResult> apply_worker_deadline (
  task_t<TResult> task,
  const std::shared_ptr<worker_control_t> &control,
  std::optional<std::chrono::milliseconds> timeout)
{
    auto completion = std::make_shared<task_completion_source_t<TResult>> ();
    auto output = completion->task ();
    auto completed = std::make_shared<std::atomic_bool> (false);
    const std::weak_ptr<worker_control_t> weak_control = control;
    auto finish = [completion, completed, weak_control] (result_t<TResult> result) mutable {
        if (completed->exchange (true, std::memory_order_acq_rel)) {
            return;
        }
        if (const auto control = weak_control.lock ()) {
            control->cancel_deadline ();
        }
        completion->complete (std::move (result));
    };

    control->arm_host ([weak_control, finish] () mutable {
        if (const auto control = weak_control.lock ()) {
            control->cancellation.request_stop ();
        }
        finish (result_t<TResult>::failure (
          framework_error_kind_t::shutting_down, "worker host is shutting down"));
    });

    detail::observe_task_completion (
      task, [control, finish] (const result_t<TResult> &result) mutable {
          /* Keep the cancellation source registered until the inner task has
           * reached a terminal state. The callback uses a weak control
           * reference, so this ownership does not form a cycle. */
          static_cast<void> (control);
          finish (result);
      });
    if (timeout && *timeout > std::chrono::milliseconds::zero ()) {
        control->arm_deadline (*timeout, [weak_control, finish] () mutable {
            if (const auto control = weak_control.lock ()) {
                control->cancellation.request_stop ();
            }
            finish (result_t<TResult>::failure (
              framework_error_kind_t::deadline_exceeded, "worker task timed out"));
        });
    }
    return output;
}

} // namespace detail

template <typename TResult> class worker_call_t
{
  public:
    using executor_t = std::function<task_t<TResult> (std::stop_token)>;

    worker_call_t () = default;
    explicit worker_call_t (executor_t executor) :
        worker_call_t (std::move (executor), {})
    {
    }

    worker_call_t &timeout (std::chrono::milliseconds value)
    {
        _timeout = value;
        return *this;
    }

    task_t<TResult> submit () { return start (false); }

    task_t<TResult> yield () { return start (true); }

  private:
    task_t<TResult> start (bool release_turn)
    {
        if (release_turn && !detail::current_serial_turn_allows_yield ()) {
            return detail::unsupported_yield_task<TResult> ();
        }
        if (!try_start ()) {
            return task_t<TResult> (
              result_t<TResult>::failure (framework_error_kind_t::protocol_error,
                                          "worker call already has a terminator"));
        }
        if (!_executor) {
            return task_t<TResult> (result_t<TResult>::failure (
              framework_error_kind_t::internal_failure, "worker runtime is not configured"));
        }
        auto control = std::make_shared<detail::worker_control_t> (_host_cancellation);
        auto turn_plan = detail::prepare_serial_turn_await (release_turn);
        std::optional<task_t<TResult>> task;
        try {
            task.emplace (_executor (control->cancellation.get_token ()));
        }
        catch (const framework_exception_t &error) {
            return task_t<TResult> (detail::result_access_t::failure<TResult> (error));
        }
        catch (const std::exception &error) {
            return task_t<TResult> (result_t<TResult>::failure (
              framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            return task_t<TResult> (result_t<TResult>::failure (
              framework_error_kind_t::internal_failure, "worker executor failed"));
        }
        auto guarded = detail::apply_worker_deadline (
          std::move (*task), control, _timeout);
        if (!turn_plan) {
            return guarded;
        }
        return detail::reschedule_task (std::move (guarded),
                                        std::move (turn_plan->scheduler));
    }
    bool try_start ()
    {
        if (_started) {
            return false;
        }
        _started = true;
        return true;
    }

    executor_t _executor;
    std::optional<std::chrono::milliseconds> _timeout;
    std::stop_token _host_cancellation;
    bool _started = false;

    friend class spot_context_t;
    explicit worker_call_t (executor_t executor,
                            std::stop_token host_cancellation) :
        _executor (std::move (executor)), _host_cancellation (host_cancellation)
    {
    }
};

} // namespace zlink::framework
