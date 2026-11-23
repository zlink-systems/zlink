/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/stateful_object_runtime.hpp"

#include <future>
#include <mutex>
#include <stdexcept>

namespace zlink::framework::runtime
{
struct state_lane_test_access_t
{
    static bool pending_work (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return !lane._mailbox.empty ();
    }
};
} // namespace zlink::framework::runtime

namespace zlink::framework::runtime::stateful
{
struct stateful_object_runtime_test_access_t
{
    static bool pending_work (stateful_object_runtime_t &objects)
    {
        return runtime::state_lane_test_access_t::pending_work (objects._lane);
    }

    static std::promise<void> hold_lane (stateful_object_runtime_t &objects)
    {
        return hold_lane (objects._lane);
    }

    static std::promise<void> hold_lane (runtime::state_lane_t &lane)
    {
        std::promise<void> release, entered;
        auto held = release.get_future ().share ();
        auto running = entered.get_future ();
        if (!lane.try_post ([held, &entered] {
                entered.set_value ();
                held.wait ();
            }))
            throw std::runtime_error ("state lane is closed");
        running.wait ();
        return release;
    }
};
} // namespace zlink::framework::runtime::stateful
