/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/route_channel_runtime.hpp"

#include <gtest/gtest.h>
#include <zlink/framework/detail/deadline_scheduler.hpp>
#include <future>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using parts_t = runtime::messaging::message_parts_t;

class readiness_turn_t final : public detail::serial_turn_t
{
  public:
    explicit readiness_turn_t (std::shared_ptr<std::promise<void>> destroyed) :
        _destroyed (std::move (destroyed))
    {
    }
    ~readiness_turn_t () override { _destroyed->set_value (); }
    bool release () override
    {
        _released = true;
        return true;
    }
    bool released () const override { return _released; }
    std::stop_token wait_cancellation () const override { return _cancellation.get_token (); }
    void cancel_waits () noexcept override { _cancellation.request_stop (); }
    detail::task_scheduler_t resume_scheduler () override
    {
        return [] (auto work) { work (); };
    }
    bool belongs_to (const void *owner) const noexcept override { return owner == this; }
    bool is_after_active_phase () const noexcept override { return false; }
    bool allows_yield () const noexcept override { return true; }
    bool claim_yield () override { return false; }
    result_t<void> defer (std::function<task_t<void> ()>,
                          std::function<void ()>,
                          std::shared_ptr<detail::deferred_barrier_t>) override
    {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "unexpected defer");
    }
    void cancel_deferred () noexcept override {}
    bool finish_handler (std::function<void ()> completion, bool) override
    {
        completion ();
        return release ();
    }

  private:
    bool _released = false;
    std::stop_source _cancellation;
    std::shared_ptr<std::promise<void>> _destroyed;
};

TEST (CppFrameworkRouteChannelRequest, SubmissionReturnsBeforeBindingTerminal)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> submitted;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        submitted.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission =
      std::async (std::launch::async, [&] { return route.request_reply_parts (target, {}, 1s); });
    submitted.get_future ().wait ();
    const auto status = submission.wait_for (100ms);
    // Always release the old blocking implementation before asserting.
    admitted->complete (result_t<void>::success ());
    reply.complete (result_t<parts_t>::success ({}));
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSubmissionReturnsBeforeBindingTerminal)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> submitted;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        submitted.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto entered_status = submitted.get_future ().wait_for (1s);
    const auto status = submission.wait_for (100ms);
    admitted->complete (result_t<void>::success ());
    reply.complete (result_t<parts_t>::success ({}));
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, entered_status);
    EXPECT_EQ (std::future_status::ready, status);
}

TEST (CppFrameworkRouteChannelRequest, PendingFakeReturnsBeforeCallerRelease)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    std::promise<void> entered;
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        entered.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission =
      std::async (std::launch::async, [&] { return route.request_reply_parts (target, {}, 1s); });
    const auto entered_status = entered.get_future ().wait_for (1s);
    const auto status = submission.wait_for (100ms);
    reply.complete (result_t<parts_t>::success ({}));
    auto pending = submission.get ();
    EXPECT_EQ (std::future_status::ready, entered_status);
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_TRUE (pending.result ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationReturnsPendingBeforePeerReady)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto status = submission.wait_for (100ms);
    route.mark_peer_ready (target);
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, status);
}

TEST (CppFrameworkRouteChannelRequest, ThrowingSubmissionRemovesPendingEntry)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    route.set_request_backend (
      [] (const auto &, const auto &, const auto &, auto) -> request_task_t<parts_t> {
          throw std::runtime_error ("submission failed");
      });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    EXPECT_THROW ((void) route.request_reply_parts (target, {}, 1s), std::runtime_error);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, PhysicalTerminalRetainsBackendAfterAdmissionFailureAndStop)
{
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto backend_lifetime = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = backend_lifetime;
    std::optional<request_task_t<result_t<parts_t>>> pending;
    const zlink::routing_id_t *borrowed_target = nullptr;
    const parts_t *borrowed_parts = nullptr;
    {
        detail::route_channel_runtime_t route ("route");
        route.start ();
        route.set_request_backend (
          [held = backend_lifetime, &physical_reply, admitted, &borrowed_target,
           &borrowed_parts] (const auto &target, const auto &, const auto &parts, auto) {
              (void) held;
              borrowed_target = &target;
              borrowed_parts = &parts;
              return request_task_t<parts_t> (physical_reply.task (), admitted);
          });
        pending.emplace (
          route.request_reply_parts (zlink::routing_id_t::from (std::string ("peer")), {}, 1s));
        route.stop ();
    }
    backend_lifetime.reset ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    EXPECT_FALSE (pending->admitted ().result ());
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_FALSE (lifetime.expired ());
    EXPECT_EQ ("peer", borrowed_target->to_string ());
    EXPECT_EQ (0u, borrowed_parts->size ());
    physical_reply.complete (result_t<parts_t>::success ({}));
    ASSERT_TRUE (pending->result ());
    EXPECT_TRUE (pending->result ().value ());
    EXPECT_TRUE (lifetime.expired ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationConsumesBindingBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    std::chrono::milliseconds binding_budget{};
    std::chrono::steady_clock::duration elapsed{};
    const auto started = std::chrono::steady_clock::now ();
    std::thread ready ([&] {
        std::this_thread::sleep_for (40ms);
        route.connect (target, "tcp://peer:7500");
        route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto timeout) {
            binding_budget = timeout;
            elapsed = std::chrono::steady_clock::now () - started;
            return request_task_t<parts_t> (result_t<parts_t>::success ({}));
        });
        std::this_thread::sleep_for (40ms);
        route.mark_peer_ready (target);
    });
    auto pending = route.request_reply_spot_parts (target, "spot", {}, started + 500ms);
    auto reply = pending.result ();
    ready.join ();
    ASSERT_TRUE (reply);
    ASSERT_GE (elapsed, 80ms);
    EXPECT_LE (binding_budget,
               500ms - std::chrono::duration_cast<std::chrono::milliseconds> (elapsed) + 1ms);
    EXPECT_GT (binding_budget, 0ms);
}

TEST (CppFrameworkRouteChannelRequest, SpotDeadlineBeforeBindingIsTimedOut)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto reply =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 20ms)
        .result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (0, submitted);
    EXPECT_EQ (0u, route.pending_request_count ());
}
TEST (CppFrameworkRouteChannelRequest, ExpiredCallerDeadlineDoesNotStartBinding)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto reply =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () - 1ms)
        .result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (0, submitted);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, ThrowingSpotSubmissionLeavesNoPendingEntry)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.set_request_backend (
      [] (const auto &, const auto &, const auto &, auto) -> request_task_t<parts_t> {
          throw std::runtime_error ("submission failed");
      });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto pending =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 1s);
    auto reply = pending.result ();
    EXPECT_FALSE (reply);
    try {
        (void) reply.value ();
        ADD_FAILURE () << "backend exception was not preserved";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ ("submission failed", error.what ());
    }
    EXPECT_EQ (0u, route.pending_request_count ());
}


TEST (CppFrameworkRouteChannelRequest, SpotAdmissionFailureEndsWaitBeforePhysicalTerminal)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto status = submission.wait_for (100ms);
    physical_reply.complete (result_t<parts_t>::success ({}));
    auto reply = submission.get ().result ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationStopAndDestructionCompletePending)
{
    std::optional<request_task_t<parts_t>> pending;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        route.bind_endpoint ("tcp://self:7500");
        const auto target = zlink::routing_id_t::from (std::string ("peer"));
        route.connect (target, "tcp://peer:7500");
        route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
            return request_task_t<parts_t> (result_t<parts_t>::success ({}));
        });
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
        EXPECT_FALSE (pending->await_ready ());
        route.stop ();
    }
    auto reply = pending->result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationDeadlineKeepsOriginalBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto pending =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 20ms);
    EXPECT_FALSE (pending.await_ready ());
    auto terminal = std::async (std::launch::async, [&] { return pending.result (); });
    const auto status = terminal.wait_for (1s);
    // Release on failure as well, so a broken expiry cannot hang test teardown.
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPhysicalTerminalRetainsArgumentsAfterStopAndDestruction)
{
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto held = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = held;
    std::optional<request_task_t<parts_t>> pending;
    const zlink::routing_id_t *borrowed_target = nullptr;
    const parts_t *borrowed_parts = nullptr;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        route.set_request_backend (
          [held, &physical_reply, admitted, &borrowed_target,
           &borrowed_parts] (const auto &target, const auto &, const auto &parts, auto) {
              borrowed_target = &target;
              borrowed_parts = &parts;
              return request_task_t<parts_t> (physical_reply.task (), admitted);
          });
        pending.emplace (
          route.request_reply_spot_parts (zlink::routing_id_t::from (std::string ("peer")), "spot",
                                          {}, std::chrono::steady_clock::now () + 1s));
        route.stop ();
    }
    held.reset ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    EXPECT_FALSE (pending->result ());
    EXPECT_FALSE (lifetime.expired ());
    EXPECT_EQ ("peer", borrowed_target->to_string ());
    EXPECT_EQ (0u, borrowed_parts->size ());
    physical_reply.complete (result_t<parts_t>::success ({}));
    EXPECT_TRUE (lifetime.expired ());
}

TEST (CppFrameworkRouteChannelRequest, ZeroTimeoutChecksUnavailableWithoutRegisteringDeadline)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    const auto before = detail::deadline_scheduler_t::instance ().scheduled_count ();
    auto result = route.wait_until_connected (0ms);
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
    EXPECT_EQ (before, detail::deadline_scheduler_t::instance ().scheduled_count ());
}

TEST (CppFrameworkRouteChannelRequest, ExpiredSpotDeadlineRemainsTimedOutWhenStopped)
{
    detail::route_channel_runtime_t route ("spot-route");
    auto pending =
      route.request_reply_spot_parts (zlink::routing_id_t::from (std::string ("peer")), "spot", {},
                                      std::chrono::steady_clock::now () - 1ms);
    auto reply = pending.result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest,
      SpotPreparationTimeoutReleasesApplicationTurnWithoutStateChange)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    auto destroyed = std::make_shared<std::promise<void>> ();
    auto destruction = destroyed->get_future ();
    std::optional<request_task_t<parts_t>> pending;
    {
        auto turn = std::make_shared<readiness_turn_t> (destroyed);
        detail::serial_turn_scope_t scope (turn);
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 20ms));
    }
    auto reply = pending->result ();
    pending.reset ();
    const auto status = destruction.wait_for (100ms);
    // Stop only after observing cleanup; the old observer is released by this event.
    route.stop ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (std::future_status::ready, status);
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationPreservesParentTurnCancellation)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    auto turn = std::make_shared<readiness_turn_t> (std::make_shared<std::promise<void>> ());
    std::optional<request_task_t<parts_t>> pending;
    {
        detail::serial_turn_scope_t scope (turn);
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
    }
    turn->cancel_waits ();
    auto terminal = std::async (std::launch::async, [&] { return pending->result (); });
    const auto status = terminal.wait_for (100ms);
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

}
