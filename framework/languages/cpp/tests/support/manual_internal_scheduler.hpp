/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
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
        std::lock_guard lock (_mutex);
        if (!_accepting)
            return false;
        _ready.push (std::move (work));
        return true;
    }

    bool run_one ()
    {
        std::function<void ()> work;
        {
            std::lock_guard lock (_mutex);
            if (_ready.empty ())
                return false;
            work = std::move (_ready.front ());
            _ready.pop ();
        }
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

    void retire ()
    {
        run_all ();
        {
            std::lock_guard lock (_mutex);
            _accepting = false;
        }
        drain ();
    }

  private:
    std::function<void ()> _publication_fence;
    std::mutex _mutex;
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
