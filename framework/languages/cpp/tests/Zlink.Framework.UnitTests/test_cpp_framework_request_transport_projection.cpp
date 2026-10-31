/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime_failure_fixture.hpp"
#include <cstdio>
#include <array>
#include <atomic>
#include <optional>
namespace
{
namespace fw = zlink::framework;
using reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;

TEST (RequestCallAllocations, ReadyNodeReplyRegistersWithoutAConversionResultState)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("ready-projection");
    fw::detail::mesh_node_runtime_t runtime (state);
    std::array<fw::tests::allocation_event_t, 64> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    std::size_t conversion_state_size = 0;
    {
        fw::tests::allocation_event_scope_t scope (log);
        const auto calibration = fw::tests::count_allocations (
          [] { (void) std::make_shared<fw::detail::application_task_shared_state_t<reply_t>> (); });
        ASSERT_EQ (calibration, 1u);
        ASSERT_EQ (log.count, 1u);
        conversion_state_size = events[0].size;
    }
    fw::runtime::host::pending_operation_t operation;
    auto submitted = fw::task_t<zlink::submit_result_t> (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    std::optional<fw::detail::request_receiver_t<reply_t>> receiver;
    auto pending = fw::detail::request_task_access_t::receive_internal<reply_t> (
      [&] (auto prepared) { receiver.emplace (std::move (prepared)); });
    log.count = 0;
    std::size_t measured = 0;
    {
        fw::tests::allocation_event_scope_t scope (log);
        measured = fw::tests::count_allocations ([&] {
            runtime.compose_node_request_reply (operation, std::move (submitted),
                                                std::move (*receiver));
        });
    }
    ASSERT_FALSE (log.overflow);
    std::size_t conversion_states = 0;
    for (std::size_t index = 0; index < log.count; ++index)
        conversion_states += events[index].size == conversion_state_size;
    std::printf ("ready node reply: conversion_states=%zu state_bytes=%zu allocations=%zu\n",
                 conversion_states, conversion_state_size, measured);
    EXPECT_EQ (conversion_states, 0u) << fw::tests::describe_allocations (log);
    EXPECT_FALSE (pending.await_ready ());
    ASSERT_TRUE (pending.admitted ().await_ready ());
    EXPECT_TRUE (pending.admitted ().result ());
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    reply.parts.push_back (zlink::message_t::from (std::string ("reply")));
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
    const auto &result = pending.result ();
    ASSERT_TRUE (result);
    ASSERT_TRUE (result.value ());
    EXPECT_EQ (result.value ().value ().items ().size (), 1u);
}

TEST (RequestCallAllocations, ReadyAdmissionKeepsALateReplyFailureSeparate)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "late-projection-failure");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t operation;
    auto pending =
      fw::detail::request_task_access_t::receive_internal<reply_t> ([&] (auto receiver) {
          runtime.compose_node_request_reply (
            operation,
            fw::task_t<zlink::submit_result_t> (
              fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)),
            std::move (receiver));
      });
    ASSERT_TRUE (pending.admitted ().result ());
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::failure (
        fw::framework_error_kind_t::deadline_exceeded, "late reply timeout"));
    const auto &result = pending.result ();
    ASSERT_TRUE (result);
    EXPECT_EQ (result.value ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (pending.admitted ().result ());
}
fw::task_t<int> read_projected_reply (fw::task_t<int> reply)
{
    co_return co_await reply;
}

TEST (RequestCallAllocations, AdoptedReplyPreservesBothResumeSchedulers)
{
    fw::task_completion_source_t<int> completion;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::atomic<unsigned> scheduled{0};
    auto reply = fw::detail::with_task_resume_scheduler (
      completion.task (), [&] (std::function<void ()> &&continuation) {
          ++scheduled;
          continuation ();
      });
    auto pending = fw::detail::request_task_access_t::from_admitted_reply (std::move (reply),
                                                                           std::move (admitted));
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    auto physical_wait = read_projected_reply (std::move (physical));
    auto public_wait = read_projected_reply (std::move (static_cast<fw::task_t<int> &> (pending)));
    completion.complete (fw::result_t<int>::success (42));
    ASSERT_TRUE (physical_wait.result ());
    ASSERT_TRUE (public_wait.result ());
    EXPECT_EQ (physical_wait.result ().value (), 42);
    EXPECT_EQ (public_wait.result ().value (), 42);
    EXPECT_EQ (scheduled.load (), 2u);
}
} // namespace
