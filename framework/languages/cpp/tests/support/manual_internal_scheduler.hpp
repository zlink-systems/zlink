/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
#include "scope_exit.hpp"
#include <chrono>
#include <latch>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <condition_variable>
#include <future>

#include <functional>
#include <mutex>
#include <optional>
#include <deque>
#include <variant>
#include <algorithm>

namespace zlink::framework::test
{
// A fixture advances internal publications without relying on elapsed time.
class manual_internal_scheduler_t final : public runtime::offload_executor_t
{
  public:
    manual_internal_scheduler_t () : offload_executor_t (1, "manual-internal") {}

    bool try_submit_internal (std::function<void ()> work) override
    {
        std::lock_guard lock (_wake->mutex);
        if (!_accepting)
            return false;
        _ready.emplace_back (std::move (work));
        _wake->ready_changed.notify_all ();
        return true;
    }

    bool try_submit_drain (std::shared_ptr<drain_source_t> source) override
    {
        std::lock_guard lock (_wake->mutex);
        if (!_accepting)
            return false;
        const auto found = std::find_if (_ready.begin (), _ready.end (), [&] (const auto &entry) {
            const auto existing = std::get_if<std::shared_ptr<drain_source_t>> (&entry);
            return existing && (*existing)->group == source->group
                   && (*existing)->owner == source->owner;
        });
        if (found == _ready.end ())
            _ready.emplace_back (std::move (source));
        _wake->ready_changed.notify_all ();
        return true;
    }

    bool
    try_submit_drain (const void *, const std::string &, const drain_factory_t &create) override
    {
        return try_submit_drain (create ());
    }

    drain_source_t::record_t try_take_drain (const std::shared_ptr<drain_source_t> &source) override
    {
        std::shared_ptr<drain_source_t> selected;
        drain_source_t::record_t record;
        {
            std::lock_guard lock (_wake->mutex);
            const auto found =
              std::find_if (_ready.begin (), _ready.end (), [&] (const auto &entry) {
                  const auto existing = std::get_if<std::shared_ptr<drain_source_t>> (&entry);
                  return existing && (*existing)->group == source->group
                         && (*existing)->owner == source->owner;
              });
            if (found != _ready.end ()) {
                selected = std::get<std::shared_ptr<drain_source_t>> (*found);
                record = selected->take ();
                if (record.more)
                    std::rotate (found, found + 1, _ready.end ());
                else
                    _ready.erase (found);
            }
        }
        return record;
    }

    std::size_t pending_work () const
    {
        std::lock_guard lock (_wake->mutex);
        return _ready.size ();
    }

    bool wait_for_pending_work (std::size_t count,
                                std::chrono::steady_clock::duration timeout) const
    {
        std::unique_lock lock (_wake->mutex);
        return _wake->ready_changed.wait_for (lock, timeout,
                                              [&] { return _ready.size () >= count; });
    }

    bool run_one ()
    {
        std::function<void ()> work;
        std::shared_ptr<drain_source_t> source;
        drain_source_t::record_t record;
        {
            std::lock_guard lock (_wake->mutex);
            if (_ready.empty ())
                return false;
            if (const auto drain =
                  std::get_if<std::shared_ptr<drain_source_t>> (&_ready.front ())) {
                source = *drain;
                const auto turn_started = std::chrono::steady_clock::now ();
                record = source->take ();
                record.turn_started = turn_started;
                if (record.more)
                    std::rotate (_ready.begin (), _ready.begin () + 1, _ready.end ());
                else
                    _ready.pop_front ();
            } else {
                work = std::move (std::get<std::function<void ()>> (_ready.front ()));
                _ready.pop_front ();
            }
        }
        if (source) {
            if (record.value)
                source->run (std::move (record));
        } else
            work ();
        if (_publication_fence)
            _publication_fence ();
        return true;
    }

    void publication_fence (std::function<void ()> fence)
    {
        _publication_fence = std::move (fence);
    }

    void run_all ()
    {
        if (_publication_fence)
            _publication_fence ();
        while (run_one ()) {
        }
    }

    // The predicate observes a transition made by work submitted to this scheduler.
    bool run_until (const std::function<bool ()> &completed,
                    std::optional<std::chrono::milliseconds> timeout = {})
    {
        const auto deadline = timeout ? std::chrono::steady_clock::now () + *timeout
                                      : std::chrono::steady_clock::time_point::max ();
        for (;;) {
            {
                std::unique_lock lock (_wake->mutex);
                if (!completed () && std::chrono::steady_clock::now () >= deadline)
                    return false;
                if (!_wake->ready_changed.wait_until (
                      lock, deadline, [&] { return completed () || !_ready.empty (); }))
                    return false;
                if (completed ())
                    return true;
            }
            run_one ();
        }
    }

    // External completion executors can publish more work after run_all returns.
    template <typename T>
    bool run_until (task_t<T> &terminal, std::optional<std::chrono::milliseconds> timeout = {})
    {
        auto completed = std::make_shared<bool> (false);
        detail::observe_task_terminal (terminal, [wake = _wake, completed] (const result_t<T> &) {
            std::lock_guard lock (wake->mutex);
            *completed = true;
            wake->ready_changed.notify_all ();
        });
        return run_until ([completed] { return *completed; }, timeout);
    }

    void retire ()
    {
        run_all ();
        {
            std::lock_guard lock (_wake->mutex);
            _accepting = false;
        }
        drain ();
    }

  private:
    std::function<void ()> _publication_fence;
    struct wake_state_t
    {
        std::mutex mutex;
        std::condition_variable ready_changed;
    };
    std::shared_ptr<wake_state_t> _wake = std::make_shared<wake_state_t> ();
    std::deque<std::variant<std::function<void ()>, std::shared_ptr<drain_source_t>>> _ready;
    bool _accepting = true;
};
// Preserve the original single available worker precondition on the shared pool.
inline scope_exit_t reserve_single_infrastructure_worker (runtime::offload_executor_t &executor)
{
    struct blockers_t
    {
        explicit blockers_t (std::ptrdiff_t count) : entered (count), returned (count) {}
        std::promise<void> release;
        std::shared_future<void> released{release.get_future ().share ()};
        std::latch entered;
        std::latch returned;
    };
    const auto count = executor.live_worker_count () - 1;
    auto blockers = std::make_shared<blockers_t> (static_cast<std::ptrdiff_t> (count));
    for (std::size_t index = 0; index != count; ++index)
        executor.submit ([blockers] {
            blockers->entered.count_down ();
            blockers->released.wait ();
            blockers->returned.count_down ();
        });
    blockers->entered.wait ();
    return scope_exit_t ([blockers] {
        blockers->release.set_value ();
        blockers->returned.wait ();
    });
}

// A lane result is delivered after its lane scope ends. The executor FIFO
// sentinel also fences that delivery, before the fixture observes runnable work.
inline auto state_lane_publication_fence (runtime::state_lane_t &lane,
                                          runtime::offload_executor_t &executor)
{
    return [&lane, &executor] {
        lane.run ([] {}).get ();
        std::promise<void> delivered;
        auto ready = delivered.get_future ();
        executor.submit ([&delivered] { delivered.set_value (); });
        ready.get ();
    };
}
} // namespace zlink::framework::test
