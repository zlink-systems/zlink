/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <condition_variable>
#include <future>

#include <functional>
#include <mutex>
#include <queue>

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
        _ready.push (std::move (work));
        _wake->ready_changed.notify_one ();
        return true;
    }

    std::function<void ()> take_next ()
    {
        std::function<void ()> work;
        {
            std::lock_guard lock (_wake->mutex);
            if (_ready.empty ())
                return {};
            work = std::move (_ready.front ());
            _ready.pop ();
        }
        return [this, work = std::move (work)] () mutable {
            work ();
            if (_publication_fence)
                _publication_fence ();
        };
    }

    bool run_one ()
    {
        auto work = take_next ();
        if (!work)
            return false;
        work ();
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
    void run_until (const std::function<bool ()> &completed)
    {
        for (;;) {
            {
                std::unique_lock lock (_wake->mutex);
                _wake->ready_changed.wait (lock, [&] { return completed () || !_ready.empty (); });
                if (completed ())
                    return;
            }
            run_one ();
        }
    }

    // External completion executors can publish more work after run_all returns.
    template <typename T> void run_until (task_t<T> &terminal)
    {
        auto completed = std::make_shared<bool> (false);
        detail::observe_task_terminal (terminal, [wake = _wake, completed] (const result_t<T> &) {
            std::lock_guard lock (wake->mutex);
            *completed = true;
            wake->ready_changed.notify_one ();
        });
        run_until ([completed] { return *completed; });
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
    std::queue<std::function<void ()>> _ready;
    bool _accepting = true;
};
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
