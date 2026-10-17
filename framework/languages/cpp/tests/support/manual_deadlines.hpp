/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/detail/deadline_scheduler.hpp>

namespace zlink::framework::detail
{
struct deadline_scheduler_test_access_t
{
    static void advance_to (std::chrono::steady_clock::time_point now)
    {
        auto &scheduler = deadline_scheduler_t::instance ();
        std::unique_lock lock (scheduler._mutex);
        scheduler.dispatch_due (lock, now);
    }
};
} // namespace zlink::framework::detail
