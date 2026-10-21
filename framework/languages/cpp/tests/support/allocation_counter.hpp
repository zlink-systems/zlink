/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <cstddef>
#include <utility>
namespace zlink::framework::tests
{
extern thread_local bool tracking_allocations;
extern thread_local std::size_t tracked_allocation_count;
extern thread_local std::size_t tracked_allocation_size;
template <typename TAction>
std::size_t count_allocations (TAction &&action, std::size_t allocation_size = 0)
{
    const bool previous_tracking = tracking_allocations;
    const auto previous_size = tracked_allocation_size;
    tracked_allocation_size = allocation_size;
    const std::size_t previous_count = tracked_allocation_count;
    tracked_allocation_count = 0;
    tracking_allocations = true;
    try {
        std::forward<TAction> (action) ();
    }
    catch (...) {
        tracking_allocations = previous_tracking;
        tracked_allocation_size = previous_size;
        tracked_allocation_count = previous_count;
        throw;
    }
    tracking_allocations = previous_tracking;
    tracked_allocation_size = previous_size;
    const std::size_t measured = tracked_allocation_count;
    tracked_allocation_count = previous_count;
    return measured;
}
} // namespace zlink::framework::tests
