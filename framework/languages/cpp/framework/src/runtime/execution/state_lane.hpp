/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
#include <zlink/framework/detail/infrastructure_wait_context.hpp>
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace zlink::framework::runtime
{

namespace state_lane_internal
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
void check_pending_infrastructure_wait (std::future_status status, const char *site) noexcept;
template <typename Future> decltype (auto) get (Future &&future, const char *site);
template <typename Future> void wait (Future &&future, const char *site);
template <typename Future, typename Rep, typename Period>
std::future_status
wait_for (Future &&future, const std::chrono::duration<Rep, Period> &timeout, const char *site);
template <typename Future, typename Clock, typename Duration>
std::future_status wait_until (Future &&future,
                               const std::chrono::time_point<Clock, Duration> &deadline,
                               const char *site);

template <typename Future> class checked_future_t
{
  public:
    explicit checked_future_t (Future future) : _future (std::move (future)) {}
    checked_future_t (checked_future_t &&) noexcept = default;
    checked_future_t &operator= (checked_future_t &&) noexcept = default;
    checked_future_t (const checked_future_t &) = delete;
    checked_future_t &operator= (const checked_future_t &) = delete;

    bool valid () const noexcept { return _future.valid (); }
    decltype (auto) get () { return state_lane_internal::get (_future, "state-lane/get"); }
    void wait () const { state_lane_internal::wait (_future, "state-lane/wait"); }

    template <typename Rep, typename Period>
    std::future_status wait_for (const std::chrono::duration<Rep, Period> &timeout) const
    {
        return state_lane_internal::wait_for (_future, timeout, "state-lane/wait_for");
    }

    template <typename Clock, typename Duration>
    std::future_status wait_until (const std::chrono::time_point<Clock, Duration> &deadline) const
    {
        return state_lane_internal::wait_until (_future, deadline, "state-lane/wait_until");
    }

  private:
    Future _future;
};
#endif
template <typename T> class boxed_future_t
{
  public:
    using stored_t = std::unique_ptr<T>;

    explicit boxed_future_t (std::future<stored_t> future) : _future (std::move (future)) {}

    boxed_future_t (boxed_future_t &&) noexcept = default;
    boxed_future_t &operator= (boxed_future_t &&) noexcept = default;
    boxed_future_t (const boxed_future_t &) = delete;
    boxed_future_t &operator= (const boxed_future_t &) = delete;

    bool valid () const noexcept { return _future.valid (); }
    void wait () const
    {
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        state_lane_internal::wait (_future, "state-lane/boxed-wait");
#else
        _future.wait ();
#endif
    }

    template <typename Rep, typename Period>
    std::future_status wait_for (const std::chrono::duration<Rep, Period> &timeout) const
    {
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        return state_lane_internal::wait_for (_future, timeout, "state-lane/boxed-wait_for");
#else
        return _future.wait_for (timeout);
#endif
    }

    template <typename Clock, typename Duration>
    std::future_status wait_until (const std::chrono::time_point<Clock, Duration> &deadline) const
    {
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        return state_lane_internal::wait_until (_future, deadline, "state-lane/boxed-wait_until");
#else
        return _future.wait_until (deadline);
#endif
    }

    T get ()
    {
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        auto stored = state_lane_internal::get (_future, "state-lane/boxed-get");
        return std::move (*stored);
#else
        return std::move (*_future.get ());
#endif
    }

  private:
    std::future<stored_t> _future;
};

#if defined(_MSC_VER)
// MSVC's promise implementation assigns the result into shared state. Box
// only value types that cannot satisfy that implementation detail; all usual
// results retain std::future and the allocation-free path.
template <typename T>
inline constexpr bool requires_boxed_result_v =
  !std::is_void_v<T> && !std::is_reference_v<T> && !std::is_move_assignable_v<T>;
#else
template <typename T> inline constexpr bool requires_boxed_result_v = false;
#endif

template <typename T, bool Boxed = requires_boxed_result_v<T>> struct result_bridge_t
{
    using stored_t = T;
    using future_t = std::future<T>;

    static future_t make_future (std::future<stored_t> future) { return future; }

    template <typename U> static decltype (auto) store (U &&value)
    {
        return std::forward<U> (value);
    }
};

template <typename T> struct result_bridge_t<T, true>
{
    using stored_t = std::unique_ptr<T>;
    using future_t = boxed_future_t<T>;

    static future_t make_future (std::future<stored_t> future)
    {
        return future_t (std::move (future));
    }

    template <typename U> static stored_t store (U &&value)
    {
        return std::make_unique<T> (std::forward<U> (value));
    }
};
} // namespace state_lane_internal

// Single-owner execution lane for one component's mutable state.  This is
// deliberately separate from serial_execution_queue_t: a state owner needs
// FIFO single-turn execution, not handler admission or lifecycle policy.
class state_lane_t
{
  public:
    explicit state_lane_t (offload_executor_t &executor) noexcept;
    ~state_lane_t ();

    state_lane_t (const state_lane_t &) = delete;
    state_lane_t &operator= (const state_lane_t &) = delete;

    template <typename Work>
    auto run (Work &&work) -> typename state_lane_internal::result_bridge_t<
                             std::invoke_result_t<std::decay_t<Work> &>>::future_t
    {
        using result_t = std::invoke_result_t<std::decay_t<Work> &>;
        using bridge_t = state_lane_internal::result_bridge_t<result_t>;
        using stored_t = typename bridge_t::stored_t;

        throw_if_reentrant ();
        if (_closed.load (std::memory_order_acquire)) {
            throw std::runtime_error ("state lane is closed");
        }

        auto completion = std::make_shared<std::promise<stored_t>> ();
        auto result = bridge_t::make_future (completion->get_future ());
        if (!enqueue (
              [completion, work = std::forward<Work> (work)] () mutable {
                  try {
                      if constexpr (std::is_void_v<result_t>) {
                          std::invoke (work);
                          completion->set_value ();
                      } else {
                          completion->set_value (bridge_t::store (std::invoke (work)));
                      }
                  }
                  catch (...) {
                      completion->set_exception (std::current_exception ());
                  }
              },
              [completion] (std::exception_ptr error) {
                  completion->set_exception (std::move (error));
              })) {
            throw std::runtime_error ("state lane is closed");
        }
        schedule_drain (true);
        return result;
    }

    template <typename Work> auto run_checked (Work &&work)
    {
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
        auto result = run (std::forward<Work> (work));
        return state_lane_internal::checked_future_t<decltype (result)> (std::move (result));
#else
        return run (std::forward<Work> (work));
#endif
    }

    template <typename Work>
    auto run_task (Work &&work) -> task_t<std::invoke_result_t<std::decay_t<Work> &>>
    {
        using value_t = std::invoke_result_t<std::decay_t<Work> &>;
        throw_if_reentrant ();
        auto completion = std::make_shared<task_completion_source_t<value_t>> ();
        auto task = completion->task ();
        if (!enqueue (
              [this, completion, work = std::forward<Work> (work)] () mutable {
                  result_t<value_t> result = [&] {
                      try {
                          return result_t<value_t>::success (std::invoke (work));
                      }
                      catch (const framework_exception_t &error) {
                          return result_t<value_t>::failure (error.kind (), error.what ());
                      }
                      catch (const std::exception &error) {
                          return result_t<value_t>::failure (
                            framework_error_kind_t::internal_failure, error.what ());
                      }
                      catch (...) {
                          return result_t<value_t>::failure (
                            framework_error_kind_t::internal_failure,
                            "unhandled state lane exception");
                      }
                  }();
                  if (!_executor.try_submit_internal (
                        [completion, result = std::move (result)] () mutable {
                            completion->complete (std::move (result));
                        })) {
                      completion->complete (result_t<value_t>::failure (
                        framework_error_kind_t::shutting_down, "state lane is closed"));
                  }
              },
              [completion] (std::exception_ptr) {
                  completion->complete (result_t<value_t>::failure (
                    framework_error_kind_t::shutting_down, "state lane is closed"));
              }))
            throw std::runtime_error ("state lane is closed");
        schedule_drain (true);
        return task;
    }

    // Queues synchronous state work without waiting for it.  Its exception is
    // intentionally contained so one fire-and-forget callback cannot strand
    // the turns behind it.
    bool try_post (std::function<void ()> work);

    void throw_if_reentrant () const;
    bool is_on_lane () const noexcept;
    static state_lane_t *current () noexcept;

    // Stops admission and waits for the already accepted FIFO mailbox to
    // finish.  Repeated calls are safe.
    void close ();
    bool closed () const noexcept { return _closed.load (std::memory_order_acquire); }

  private:
    struct mailbox_item_t
    {
        std::function<void ()> work;
        std::function<void (std::exception_ptr)> abandon;
    };

    bool enqueue (std::function<void ()> work, std::function<void (std::exception_ptr)> abandon);
    void schedule_drain (bool inline_drain);
    void drain_loop ();
    void abandon_pending (std::exception_ptr error) noexcept;

    static thread_local state_lane_t *_current_lane;

    offload_executor_t &_executor;
    mutable std::mutex _mailbox_mutex;
    std::condition_variable _drained;
    std::deque<mailbox_item_t> _mailbox;
    std::atomic_bool _scheduled{false};
    std::atomic_bool _closed{false};
};

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
inline void state_lane_internal::check_pending_infrastructure_wait (std::future_status status,
                                                                    const char *site) noexcept
{
    ::zlink::framework::detail::check_infrastructure_wait (status != std::future_status::timeout,
                                                           site);
}
#endif

namespace state_lane_internal
{
template <typename Future> decltype (auto) get (Future &&future, const char *site)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    check_pending_infrastructure_wait (future.wait_for (std::chrono::seconds (0)), site);
#else
    (void) site;
#endif
    return std::forward<Future> (future).get ();
}

template <typename Future> void wait (Future &&future, const char *site)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    check_pending_infrastructure_wait (future.wait_for (std::chrono::seconds (0)), site);
#else
    (void) site;
#endif
    std::forward<Future> (future).wait ();
}

template <typename Future, typename Rep, typename Period>
std::future_status
wait_for (Future &&future, const std::chrono::duration<Rep, Period> &timeout, const char *site)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (timeout > timeout.zero ())
        check_pending_infrastructure_wait (future.wait_for (std::chrono::seconds (0)), site);
#else
    (void) site;
#endif
    return std::forward<Future> (future).wait_for (timeout);
}

template <typename Future, typename Clock, typename Duration>
std::future_status wait_until (Future &&future,
                               const std::chrono::time_point<Clock, Duration> &deadline,
                               const char *site)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (deadline > Clock::now ())
        check_pending_infrastructure_wait (future.wait_for (std::chrono::seconds (0)), site);
#else
    (void) site;
#endif
    return std::forward<Future> (future).wait_until (deadline);
}
} // namespace state_lane_internal

} // namespace zlink::framework::runtime
