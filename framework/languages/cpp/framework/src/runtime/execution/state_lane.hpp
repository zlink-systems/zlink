/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

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
template<typename T> class boxed_future_t
{
  public:
    using stored_t = std::unique_ptr<T>;

    explicit boxed_future_t (std::future<stored_t> future)
        : _future (std::move (future))
    {
    }

    boxed_future_t (boxed_future_t &&) noexcept = default;
    boxed_future_t &operator= (boxed_future_t &&) noexcept = default;
    boxed_future_t (const boxed_future_t &) = delete;
    boxed_future_t &operator= (const boxed_future_t &) = delete;

    bool valid () const noexcept { return _future.valid (); }
    void wait () const { _future.wait (); }

    template<typename Rep, typename Period>
    std::future_status wait_for (const std::chrono::duration<Rep, Period> &timeout) const
    {
        return _future.wait_for (timeout);
    }

    template<typename Clock, typename Duration>
    std::future_status
    wait_until (const std::chrono::time_point<Clock, Duration> &deadline) const
    {
        return _future.wait_until (deadline);
    }

    T get () { return std::move (*_future.get ()); }

  private:
    std::future<stored_t> _future;
};

#if defined(_MSC_VER)
// MSVC's promise implementation assigns the result into shared state. Box
// only value types that cannot satisfy that implementation detail; all usual
// results retain std::future and the allocation-free path.
template<typename T>
inline constexpr bool requires_boxed_result_v =
  !std::is_void_v<T> && !std::is_reference_v<T>
  && !std::is_move_assignable_v<T>;
#else
template<typename T> inline constexpr bool requires_boxed_result_v = false;
#endif

template<typename T, bool Boxed = requires_boxed_result_v<T>> struct result_bridge_t
{
    using stored_t = T;
    using future_t = std::future<T>;

    static future_t make_future (std::future<stored_t> future)
    {
        return future;
    }

    template<typename U> static decltype(auto) store (U &&value)
    {
        return std::forward<U> (value);
    }
};

template<typename T> struct result_bridge_t<T, true>
{
    using stored_t = std::unique_ptr<T>;
    using future_t = boxed_future_t<T>;

    static future_t make_future (std::future<stored_t> future)
    {
        return future_t (std::move (future));
    }

    template<typename U> static stored_t store (U &&value)
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

    template<typename Work>
    auto run (Work &&work)
      -> typename state_lane_internal::result_bridge_t<
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
                  }
                  else {
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
        return result;
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
    bool closed () const noexcept
    {
        return _closed.load (std::memory_order_acquire);
    }

  private:
    struct mailbox_item_t
    {
        std::function<void ()> work;
        std::function<void (std::exception_ptr)> abandon;
    };

    bool enqueue (std::function<void ()> work,
                  std::function<void (std::exception_ptr)> abandon);
    void schedule_drain ();
    void drain_loop ();
    void abandon_pending (std::exception_ptr error) noexcept;

    static constexpr std::size_t drain_batch_limit = 100;
    static thread_local state_lane_t *_current_lane;

    offload_executor_t &_executor;
    mutable std::mutex _mailbox_mutex;
    std::condition_variable _drained;
    std::deque<mailbox_item_t> _mailbox;
    std::atomic_bool _scheduled{false};
    std::atomic_bool _closed{false};
};

} // namespace zlink::framework::runtime
