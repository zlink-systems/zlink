/* SPDX-License-Identifier: Elastic-2.0 */

#include "manual_internal_scheduler.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime_failure_fixture.hpp"

#include <gtest/gtest.h>

namespace
{

namespace fw = zlink::framework;
using namespace std::chrono_literals;

struct timer_spot_t
{
    int old_calls = 0;
    int new_calls = 0;
    std::optional<fw::timer_tick_t> new_tick;
    std::function<void ()> on_first_old_callback;
};

struct old_handler_t
{
    fw::task_t<void> handle (timer_spot_t &spot, const fw::timer_tick_t &)
    {
        if (++spot.old_calls == 1 && spot.on_first_old_callback)
            spot.on_first_old_callback ();
        co_return;
    }
};

struct new_handler_t
{
    fw::task_t<void> handle (timer_spot_t &spot, const fw::timer_tick_t &tick)
    {
        ++spot.new_calls;
        spot.new_tick = tick;
        co_return;
    }
};

class SpotTimerGeneration : public testing::Test
{
  protected:
    SpotTimerGeneration ()
    {
        state->channel_runtime =
          fw::test::runtime_failure_fixture<fw::detail::channel_runtime_state_t> ();
        state->channel_runtime->serializers = &serializers;
        state->node =
          fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> ("timer");
        state->spot_instance = spot;
        state->serial_queue = std::make_shared<fw::runtime::serial_execution_queue_t> (scheduler);
    }

    ~SpotTimerGeneration () override
    {
        fw::detail::timer_runtime_t::cancel_all (*state);
        scheduler.run_all ();
    }

    void post_tick (const std::shared_ptr<fw::detail::timer_state_t> &timer,
                    std::uint64_t count = 1)
    {
        fw::detail::timer_runtime_t::post_fire_count (state, timer, {}, count);
    }

    std::size_t registered_timer_count () const
    {
        return fw::detail::spot_context_access_t::state (context)->timers.snapshot ().size ();
    }

    fw::serializer_registry_t serializers;
    fw::test::manual_internal_scheduler_t scheduler;
    std::shared_ptr<timer_spot_t> spot = std::make_shared<timer_spot_t> ();
    std::shared_ptr<fw::detail::spot_context_state_t> state =
      std::make_shared<fw::detail::spot_context_state_t> ();
    fw::spot_context_t context = fw::detail::spot_context_access_t::create (state);
};

TEST_F (SpotTimerGeneration, SameNameUsesNewCallback)
{
    auto first = context.add_timer<old_handler_t> ("pulse", 24h);
    const auto first_state = state->timers.snapshot ().front ();
    auto second = context.add_timer<new_handler_t> ("pulse", 48h);

    EXPECT_TRUE (first.is_disposed ());
    EXPECT_FALSE (second.is_disposed ());
    post_tick (first_state);
    post_tick (state->timers.snapshot ().back ());
    scheduler.run_all ();
    EXPECT_EQ (spot->old_calls, 0);
    EXPECT_EQ (spot->new_calls, 1);
    ASSERT_TRUE (spot->new_tick.has_value ());
    EXPECT_EQ (spot->new_tick->delivery_index, 1u);
    EXPECT_EQ (spot->new_tick->period, 48h);
}

TEST_F (SpotTimerGeneration, RepeatedReplacementKeepsOnlyTheLiveGeneration)
{
    auto timer = context.add_timer<old_handler_t> ("pulse", 24h);
    for (int i = 0; i < 100; ++i) {
        auto replacement = context.add_timer<old_handler_t> ("pulse", 24h);
        ASSERT_TRUE (timer.is_disposed ());
        timer = std::move (replacement);
    }

    EXPECT_EQ (registered_timer_count (), 1u);
    EXPECT_FALSE (timer.is_disposed ());
}

TEST_F (SpotTimerGeneration, RepeatedCancelAndRegistrationKeepsOnlyTheLiveGeneration)
{
    auto timer = context.add_timer<old_handler_t> ("pulse", 24h);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE (timer.cancel ().result ());
        timer = context.add_timer<old_handler_t> ("pulse", 24h);
    }

    EXPECT_EQ (registered_timer_count (), 1u);
    ASSERT_TRUE (timer.cancel ().result ());
    EXPECT_EQ (registered_timer_count (), 0u);
}

TEST_F (SpotTimerGeneration, DropsTickQueuedBeforeReplacement)
{
    auto first = context.add_timer<old_handler_t> ("pulse", 24h);
    post_tick (state->timers.snapshot ().front ());
    ASSERT_EQ (state->serial_queue->pending_count (), 1u);

    auto second = context.add_timer<new_handler_t> ("pulse", 24h);
    post_tick (state->timers.snapshot ().back ());
    scheduler.run_all ();

    EXPECT_EQ (spot->old_calls, 0);
    EXPECT_EQ (spot->new_calls, 1);
    EXPECT_TRUE (first.cancel ().result ());
    EXPECT_TRUE (first.cancel ().result ());
    EXPECT_FALSE (second.is_disposed ());
}

TEST_F (SpotTimerGeneration, RetainsCancelledTimerUntilItsCallbackReturns)
{
    auto timer = context.add_timer<old_handler_t> ("pulse", 24h);
    std::optional<fw::task_t<void>> cancellation;
    spot->on_first_old_callback = [&] {
        cancellation.emplace (timer.cancel ());
        EXPECT_FALSE (cancellation->await_ready ());
        EXPECT_EQ (registered_timer_count (), 1u);
    };

    post_tick (state->timers.snapshot ().front ());
    scheduler.run_all ();

    ASSERT_TRUE (cancellation.has_value ());
    ASSERT_TRUE (cancellation->await_ready ());
    EXPECT_TRUE (cancellation->result ());
    EXPECT_EQ (registered_timer_count (), 0u);
}

TEST_F (SpotTimerGeneration, ReplacesItselfWithoutWaitingForItsOwnCallback)
{
    fw::timer_options_t options;
    options.overrun_policy = fw::timer_overrun_policy_t::catch_up_bounded;
    options.max_catch_up_ticks = 2;
    auto first = context.add_timer<old_handler_t> ("pulse", 24h, options);
    std::optional<fw::timer_t> second;
    std::optional<fw::task_t<void>> first_cancel;
    std::optional<fw::task_t<void>> second_cancel;
    spot->on_first_old_callback = [&] {
        ASSERT_NO_THROW (second.emplace (context.add_timer<new_handler_t> ("pulse", 24h)));
        first_cancel.emplace (first.cancel ());
        second_cancel.emplace (first.cancel ());
        EXPECT_FALSE (first_cancel->await_ready ());
        EXPECT_FALSE (second_cancel->await_ready ());
        EXPECT_EQ (registered_timer_count (), 2u);
        post_tick (state->timers.snapshot ().back ());
    };

    post_tick (state->timers.snapshot ().front (), 2);
    scheduler.run_all ();
    spot->on_first_old_callback = {};

    ASSERT_TRUE (second.has_value ());
    EXPECT_EQ (spot->old_calls, 1);
    EXPECT_EQ (spot->new_calls, 1);
    EXPECT_EQ (registered_timer_count (), 1u);
    ASSERT_TRUE (first_cancel->await_ready ());
    ASSERT_TRUE (second_cancel->await_ready ());
    EXPECT_TRUE (first_cancel->result ());
    EXPECT_EQ (&first_cancel->result (), &second_cancel->result ());
    const auto later_cancel = first.cancel ();
    EXPECT_EQ (&first_cancel->result (), &later_cancel.result ());
}

} // namespace
