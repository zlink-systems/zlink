/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// A single-threaded coroutine driver for the raw ZLink rows.
//
// The C++ binding's request terminal is `async()` (an awaitable) or `submit()`
// (a blocking wait). A blocking terminal can hold exactly one request open at a
// time, so `request-window` with a window of 100 has to be expressed as 100
// coroutines suspended on their own replies. That is the shape this file
// provides, and it is the same shape `bindings/cpp/perf` uses: one application
// thread, one ready queue, `poller_t::wait` driving the socket-local completion
// drain.
//
// G4: everything here is public binding API. There is no second poller, no
// reflection, and no retry inside a measured window.
#ifndef ZLINK_CPP_BENCH_ASYNC_HPP
#define ZLINK_CPP_BENCH_ASYNC_HPP

#include <zlink.hpp>

#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zlink_cpp_bench
{

// Continuations may be published from Core's completion drain; only the
// application thread runs them. `_ready` is touched by the application thread
// alone, `_remote` under its mutex by whichever thread completes an operation.
class ready_queue_t
{
  private:
    struct remote_state_t
    {
        std::mutex mutex;
        std::deque<std::function<void ()>> pending;
    };

  public:
    ready_queue_t () : _remote (std::make_shared<remote_state_t> ()) {}
    ready_queue_t (const ready_queue_t &) = delete;
    ready_queue_t &operator= (const ready_queue_t &) = delete;

    class schedule_awaiter_t
    {
      public:
        explicit schedule_awaiter_t (ready_queue_t &queue) noexcept : _queue (queue) {}
        bool await_ready () const noexcept { return false; }
        template <typename TPromise>
        void await_suspend (std::coroutine_handle<TPromise> continuation)
        {
            if constexpr (requires (TPromise &promise) {
                              promise.zlink_bind_continuation_scheduler (_queue);
                          }) {
                continuation.promise ().zlink_bind_continuation_scheduler (_queue);
            }
            _queue.enqueue (continuation);
        }
        void await_resume () const noexcept {}

      private:
        ready_queue_t &_queue;
    };

    schedule_awaiter_t schedule () noexcept { return schedule_awaiter_t (*this); }

    void enqueue (std::coroutine_handle<> handle) { _ready.push_back (handle); }

    std::function<void (std::function<void ()>)> continuation_scheduler ()
    {
        const std::shared_ptr<remote_state_t> remote = _remote;
        return [remote] (std::function<void ()> callback) noexcept {
            std::lock_guard<std::mutex> lock (remote->mutex);
            remote->pending.push_back (std::move (callback));
        };
    }

    // Runs the snapshot observed at entry. A coroutine that immediately becomes
    // ready again lands in the next round rather than monopolizing this one, so
    // one slot cannot starve the other 99.
    size_t run_ready_round ()
    {
        const size_t local_round = _ready.size ();
        _remote_batch.clear ();
        {
            std::lock_guard<std::mutex> lock (_remote->mutex);
            _remote_batch.assign (std::make_move_iterator (_remote->pending.begin ()),
                                  std::make_move_iterator (_remote->pending.end ()));
            _remote->pending.clear ();
        }
        size_t ran = 0;
        for (size_t i = 0; i < local_round; ++i) {
            std::coroutine_handle<> handle = _ready.front ();
            _ready.pop_front ();
            if (handle && !handle.done ())
                handle.resume ();
            ++ran;
        }
        for (auto &callback : _remote_batch) {
            if (callback)
                callback ();
            ++ran;
        }
        return ran;
    }

    bool idle ()
    {
        if (!_ready.empty ())
            return false;
        std::lock_guard<std::mutex> lock (_remote->mutex);
        return _remote->pending.empty ();
    }

  private:
    std::shared_ptr<remote_state_t> _remote;
    std::deque<std::coroutine_handle<>> _ready;
    std::vector<std::function<void ()>> _remote_batch;
};

// A fire-and-track coroutine. The bench never blocks on one of these; the
// driver loop asks `done()` and keeps pumping the poller until every slot has
// finished, so a slot that never completes shows up as a wedged cell rather
// than as a hung process.
class task_t
{
  public:
    struct promise_type
    {
        using handle_t = std::coroutine_handle<promise_type>;

        task_t get_return_object () { return task_t (handle_t::from_promise (*this)); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_always final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { failure = std::current_exception (); }

        void zlink_bind_continuation_scheduler (ready_queue_t &queue) noexcept
        {
            ready_queue = &queue;
        }

        std::function<void (std::function<void ()>)> zlink_continuation_scheduler ()
        {
            return ready_queue ? ready_queue->continuation_scheduler ()
                               : std::function<void (std::function<void ()>)> ();
        }

        std::exception_ptr failure;
        ready_queue_t *ready_queue = nullptr;
    };

    task_t () = default;
    explicit task_t (promise_type::handle_t handle) : _handle (handle) {}
    task_t (task_t &&other) noexcept : _handle (std::exchange (other._handle, {})) {}
    task_t &operator= (task_t &&other) noexcept
    {
        if (this != &other) {
            destroy ();
            _handle = std::exchange (other._handle, {});
        }
        return *this;
    }
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () { destroy (); }

    bool done () const noexcept { return !_handle || _handle.done (); }

    std::exception_ptr failure () const
    {
        return _handle ? _handle.promise ().failure : std::exception_ptr {};
    }

  private:
    void destroy () noexcept
    {
        if (_handle) {
            _handle.destroy ();
            _handle = {};
        }
    }

    promise_type::handle_t _handle {};
};

} // namespace zlink_cpp_bench

#endif
