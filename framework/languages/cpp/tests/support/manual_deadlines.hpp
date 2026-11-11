/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/detail/deadline_scheduler.hpp>

namespace zlink::framework::detail
{
struct deadline_scheduler_test_access_t
{
    static std::vector<std::function<void ()>> take_due (std::chrono::steady_clock::time_point now)
    {
        auto &scheduler = deadline_scheduler_t::instance ();
        std::lock_guard lock (scheduler._mutex);
        std::vector<std::function<void ()>> callbacks;
        while (!scheduler._deadlines.empty () && scheduler._deadlines.top ().at <= now) {
            auto deadline = std::move (
              const_cast<deadline_scheduler_t::deadline_t &> (scheduler._deadlines.top ()));
            scheduler._deadlines.pop ();
            if (!deadline.state->cancelled.exchange (true, std::memory_order_acq_rel))
                callbacks.push_back (std::move (deadline.callback));
        }
        return callbacks;
    }

    static void advance_to (std::chrono::steady_clock::time_point now)
    {
        auto &scheduler = deadline_scheduler_t::instance ();
        std::unique_lock lock (scheduler._mutex);
        scheduler.dispatch_due (lock, now);
    }
};
} // namespace zlink::framework::detail
