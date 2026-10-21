/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <cstddef>
#include <utility>
namespace zlink::framework::tests
{
extern thread_local bool tracking_allocations;
extern thread_local std::size_t tracked_allocation_count;
extern thread_local std::size_t tracked_allocation_size;
extern thread_local std::size_t tracked_allocation_bytes;
extern thread_local std::size_t largest_allocation;
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

struct allocation_measurement_t
{
    std::size_t count;
    std::size_t bytes;
    std::size_t largest;
};

template <typename TAction> allocation_measurement_t measure_allocations (TAction &&action)
{
    const auto previous_bytes = tracked_allocation_bytes;
    const auto previous_largest = largest_allocation;
    tracked_allocation_bytes = 0;
    largest_allocation = 0;
    try {
        const auto count = count_allocations (std::forward<TAction> (action));
        const allocation_measurement_t result{count, tracked_allocation_bytes, largest_allocation};
        tracked_allocation_bytes = previous_bytes;
        largest_allocation = previous_largest;
        return result;
    }
    catch (...) {
        tracked_allocation_bytes = previous_bytes;
        largest_allocation = previous_largest;
        throw;
    }
}
} // namespace zlink::framework::tests
