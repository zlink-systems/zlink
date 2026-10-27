/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/route_channel_runtime.hpp"

#include <gtest/gtest.h>
#include <future>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using parts_t = runtime::messaging::message_parts_t;

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


}
