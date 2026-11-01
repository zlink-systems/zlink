/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "allocation_counter.hpp"

#include <memory>
#include <stdexcept>

namespace zlink::framework::tests
{
#if defined(ZLINK_FRAMEWORK_TEST_TASK_ALLOCATION_STACKS)
inline constexpr bool shared_state_stack_metrics_available = true;
#else
inline constexpr bool shared_state_stack_metrics_available = false;
#endif

struct shared_state_allocation_site_t
{
    std::size_t size = 0;
    void *allocator_return_address = nullptr;

    bool matches (const allocation_event_t &event) const noexcept
    {
        if (!allocator_return_address || event.size != size)
            return false;
        for (std::size_t index = 0; index != event.stack_depth; ++index) {
            if (event.stack[index] == allocator_return_address)
                return true;
        }
        return false;
    }
};

template <typename TAction> allocation_event_t capture_single_allocation (TAction &&action)
{
    std::array<allocation_event_t, 1> events;
    allocation_event_log_t log{events.data (), events.size ()};
    const allocation_event_scope_t recording (log);
    const auto count = count_allocations (std::forward<TAction> (action));
    if (count != 1 || log.count != 1 || log.overflow)
        throw std::runtime_error (
          "shared state allocation calibration did not record one allocation");
    return events[0];
}

template <typename TState> shared_state_allocation_site_t shared_state_allocation_site ()
{
    if constexpr (!shared_state_stack_metrics_available) {
        return {};
    } else {
        const auto state = capture_single_allocation ([] { (void) std::make_shared<TState> (); });
        const auto ordinary = capture_single_allocation ([&] {
            void *memory = ::operator new (state.size);
            ::operator delete (memory);
        });
        std::size_t frame = 0;
        while (frame != state.stack_depth && frame != ordinary.stack_depth
               && state.stack[frame] == ordinary.stack[frame])
            ++frame;
        if (frame == state.stack_depth || frame == ordinary.stack_depth)
            throw std::runtime_error ("shared state allocator call site was not recorded");
        // The first frame after the common allocation wrapper identifies this
        // make_shared control-block allocator, rather than its allocation size.
        const shared_state_allocation_site_t site{state.size, state.stack[frame]};
        const auto another_state =
          capture_single_allocation ([] { (void) std::make_shared<TState> (); });
        if (!site.matches (state) || !site.matches (another_state) || site.matches (ordinary))
            throw std::runtime_error ("shared state allocator call-site calibration failed");
        return site;
    }
}
} // namespace zlink::framework::tests
