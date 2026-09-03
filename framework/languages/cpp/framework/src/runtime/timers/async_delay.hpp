/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/timers/core_timer_drain_loop.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace zlink::framework::detail
{

/*
 * A coroutine-native replacement for std::this_thread::sleep_for inside a
 * task_t<T> body. Callers that need a bounded, non-blocking retry delay
 * (e.g. actor_client_t::request_erased) `co_await` this instead of parking
 * the executing thread, so a serial lane or worker thread stays free for
 * other work while the delay elapses.
 *
 * There is no spot_context_t available at every call site that needs this
 * (actor_client is not Spot-scoped), so this helper cannot reuse
 * timer_runtime_t::dispatch_fire_count_async. It is built directly on the
 * core zlink::timer_t primitive that the same "fire on a background thread"
 * pattern already uses elsewhere in this codebase (see
 * remote_actor_commit_deadline_t in spot_runtime.cpp).
 */
class async_delay_timer_t
{
  public:
    static task_t<void> start (std::chrono::milliseconds duration)
    {
        /* capture_native_continuation_scheduler() must run on the awaiting
         * coroutine's own thread, before it suspends -- it snapshots the
         * ambient dispatch context and (if the coroutine currently holds a
         * serial turn) a scheduler that re-enters that turn around the
         * resume. Without this the coroutine would resume with empty
         * thread-local turn/ambient state, since it resumes on a thread this
         * helper spins up rather than the caller's thread. */
        auto source = std::make_shared<detail::task_completion_source_t<void>> (
          capture_native_continuation_scheduler ());
        auto pending = source->task ();
        auto timer = std::make_shared<async_delay_timer_t> ();
        void *const key = timer.get ();
        {
            std::lock_guard lock (registry_mutex ());
            registry ()[key] = timer;
        }
        timer->_timer.start (
          duration > std::chrono::milliseconds::zero ()
            ? duration
            : std::chrono::milliseconds (1),
          1, [key, source] (std::uint64_t) mutable {
            /* Resuming the coroutine synchronously on the drain loop
             * would run the rest of request_erased's retry loop (another
             * network submit, possibly another delay) on that thread,
             * and destroying this owner from the drain thread would
             * self-join. A fresh, throwaway thread avoids both hazards. */
            std::thread ([key, source] () mutable {
                source->complete (result_t<void>::success ());
                std::shared_ptr<async_delay_timer_t> owner;
                {
                    std::lock_guard lock (registry_mutex ());
                    auto it = registry ().find (key);
                    if (it != registry ().end ()) {
                        owner = std::move (it->second);
                        registry ().erase (it);
                    }
                }
                // owner (and its zlink::timer_t) destructs here.
            }).detach ();
          });
        return pending;
    }

  private:
    static std::mutex &registry_mutex ()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::unordered_map<void *, std::shared_ptr<async_delay_timer_t>> &
    registry ()
    {
        static std::unordered_map<void *, std::shared_ptr<async_delay_timer_t>>
          registry;
        return registry;
    }

    core_timer_drain_loop_t _timer;
};

/* co_await zlink::framework::detail::delay(50ms); suspends the calling
 * coroutine and resumes it once the duration elapses, without blocking the
 * thread it was running on. */
inline task_t<void> delay (std::chrono::milliseconds duration)
{
    return async_delay_timer_t::start (duration);
}

} // namespace zlink::framework::detail
