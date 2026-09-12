/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <type_traits>

namespace
{

using zlink::framework::runtime::offload_executor_t;
using zlink::framework::runtime::state_lane_t;

static_assert (std::has_virtual_destructor_v<offload_executor_t>);

class counting_direct_executor_t final : public offload_executor_t
{
  public:
    counting_direct_executor_t () :
        offload_executor_t (0, 1, std::chrono::milliseconds{1},
                            "lane-roundtrip-counting")
    {
    }

    bool try_submit_internal (std::function<void ()> work) override
    {
        ++_hops;
        work ();
        return true;
    }

    std::size_t hops () const noexcept { return _hops.load (); }

  private:
    std::atomic_size_t _hops = 0;
};

void expect_one_caller_owned_turn ()
{
    counting_direct_executor_t executor;
    state_lane_t lane (executor);
    std::size_t lane_turns = 0;

    lane.run ([&] {
        EXPECT_EQ (&lane, state_lane_t::current ());
        ++lane_turns;
    }).get ();

    EXPECT_EQ (1u, lane_turns);
    EXPECT_EQ (0u, executor.hops ());
}

TEST (LaneRoundtrip, RequestToTargetUsesOneLaneTurnAndNoExecutorHop)
{
    expect_one_caller_owned_turn ();
}

TEST (LaneRoundtrip, RequestToChannelUsesOneLaneTurnAndNoExecutorHop)
{
    // request_to_channel delegates to request_to_target before the shared
    // request_with_header owner turn.
    expect_one_caller_owned_turn ();
}

TEST (LaneRoundtrip, ReceiveUsesOneLaneTurnAndNoExecutorHop)
{
    // The ordinary pump_one receive path enters its owner state once with
    // synchronous state_lane_t::run.
    expect_one_caller_owned_turn ();
}

} // namespace
