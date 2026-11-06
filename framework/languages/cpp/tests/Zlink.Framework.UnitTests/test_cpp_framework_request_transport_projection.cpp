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
TEST (RequestCallAllocations, ReceiverCopiesRetainPhysicalProjectionWithoutAllocating)
{
    std::optional<fw::detail::request_receiver_t<long>> selected;
    auto lifetime = std::make_shared<int> (42);
    std::weak_ptr<int> weak = lifetime;
    int projections = 0;
    auto pending = fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
        selected.emplace (receiver.template project<long> (
          [lifetime, &projections] (const fw::result_t<long> &result) {
              ++projections;
              return fw::result_t<int>::success (static_cast<int> (result.value ()) + *lifetime);
          }));
    });
    lifetime.reset ();
    std::optional<fw::detail::request_receiver_t<long>> copy;
    const auto allocations = fw::tests::count_allocations ([&] { copy.emplace (*selected); });
    EXPECT_EQ (allocations, 0u);
    selected.reset ();
    ASSERT_FALSE (weak.expired ());
    const auto failure = fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                                      "admission failed before physical reply");
    copy->admitted->complete (failure);
    ASSERT_TRUE (pending.await_ready ());
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_FALSE (physical.await_ready ());
    copy->complete (fw::result_t<long>::success (1));
    EXPECT_EQ (physical.result ().value (), 43);
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    EXPECT_EQ (projections, 1);
    copy.reset ();
    EXPECT_TRUE (weak.expired ());
}

TEST (RequestCallAllocations, TerminalObserversKeepRegistrationOrderDuringReentry)
{
    fw::task_completion_source_t<int> source;
    auto task = source.task ();
    std::vector<int> order;
    fw::detail::observe_task_terminal (task, [&] (const auto &) {
        order.push_back (1);
        fw::detail::observe_task_terminal (task, [&] (const auto &) { order.push_back (9); });
    });
    fw::detail::observe_task_terminal (task, [&] (const auto &) { order.push_back (2); });
    source.complete (fw::result_t<int>::success (42));
    EXPECT_EQ (order, (std::vector<int>{1, 9, 2}));
}

TEST (RequestCallAllocations, SharedAdmissionSourceUsesOneAllocationAndReleasesItsState)
{
    std::shared_ptr<fw::task_completion_source_t<int>> source;
    const auto allocations = fw::tests::count_allocations (
      [&] { source = fw::detail::make_task_completion_source<int> (); });
    EXPECT_EQ (allocations, 1u);
    std::optional<fw::task_t<int>> pending (source->task ());
    std::weak_ptr<fw::task_completion_source_t<int>> state = source;
    source->complete (fw::result_t<int>::success (42));
    source.reset ();
    ASSERT_FALSE (state.expired ());
    EXPECT_EQ (pending->result ().value (), 42);
    pending.reset ();
    EXPECT_TRUE (state.expired ());
}

TEST (RequestCallAllocations, CompletionObserverReleasesCapturesBeforeDeliveryStorage)
{
    fw::task_completion_source_t<int> source;
    std::function<void ()> delivery;
    auto task = fw::detail::with_task_resume_scheduler (
      source.task (), [&] (std::function<void ()> work) { delivery = std::move (work); });
    auto captured = std::make_shared<int> (42);
    std::weak_ptr<int> weak = captured;
    int calls = 0;
    fw::detail::observe_task_completion (task, [captured, &calls] (const auto &result) {
        EXPECT_EQ (result.value (), *captured);
        ++calls;
    });
    captured.reset ();
    source.complete (fw::result_t<int>::success (42));
    ASSERT_TRUE (delivery);
    ASSERT_FALSE (weak.expired ());
    delivery ();
    EXPECT_EQ (calls, 1);
    EXPECT_TRUE (weak.expired ());
    EXPECT_TRUE (delivery);
    delivery ();
    EXPECT_EQ (calls, 1);
}

TEST (RequestCallAllocations, SharedAdmissionSourceCannotMoveAfterRegistration)
{
    auto source = fw::detail::make_task_completion_source<int> ();
    auto task = source->task ();
    fw::detail::observe_task_completion (task, [] (const auto &) {});
    EXPECT_DEATH ({ fw::task_completion_source_t<int> moved (std::move (*source)); }, "");
    EXPECT_DEATH (
      {
          fw::task_completion_source_t<int> destination;
          destination = std::move (*source);
      },
      "");
    EXPECT_DEATH (
      {
          fw::task_completion_source_t<int> replacement;
          *source = std::move (replacement);
      },
      "");
    EXPECT_TRUE (source->complete (fw::result_t<int>::success (42)));
    EXPECT_EQ (task.result ().value (), 42);
}

TEST (RequestCallAllocations, InlineRawCallbackCopiesAndMovesWithoutAllocating)
{
    using callback_t = fw::detail::backend::raw_request_callback_t;
    using raw_result_t = fw::result_t<fw::detail::backend::raw_request_completion_t>;
    auto captured = std::make_shared<int> (42);
    std::weak_ptr<int> weak = captured;
    int calls = 0;
    callback_t original = [captured, &calls,
                           padding = std::array<char, 64>{}] (raw_result_t result) {
        EXPECT_TRUE (result);
        EXPECT_EQ (*captured, 42);
        EXPECT_EQ (padding.front (), 0);
        ++calls;
    };
    captured.reset ();
    callback_t copy;
    const auto allocations = fw::tests::count_allocations ([&] { copy = original; });
    EXPECT_EQ (allocations, 0u);
    callback_t moved (std::move (copy));
    EXPECT_FALSE (copy);
    original = nullptr;
    EXPECT_FALSE (weak.expired ());
    moved (raw_result_t::success ({}));
    EXPECT_EQ (calls, 1);
    moved = nullptr;
    EXPECT_TRUE (weak.expired ());
}

TEST (RequestCallAllocations, InlineRawCallbackCopyFailureKeepsTheDestination)
{
    using callback_t = fw::detail::backend::raw_request_callback_t;
    using raw_result_t = fw::result_t<fw::detail::backend::raw_request_completion_t>;
    struct throwing_copy_t
    {
        bool *throw_copy;
        int *calls;
        throwing_copy_t (bool *flag, int *count) : throw_copy (flag), calls (count) {}
        throwing_copy_t (throwing_copy_t &&) noexcept = default;
        throwing_copy_t (const throwing_copy_t &other) :
            throw_copy (other.throw_copy), calls (other.calls)
        {
            if (*throw_copy)
                throw std::runtime_error ("callback copy failed");
        }
        void operator() (raw_result_t) const { ++*calls; }
    };
    bool throw_copy = false;
    int source_calls = 0;
    int destination_calls = 0;
    callback_t source = throwing_copy_t (&throw_copy, &source_calls);
    callback_t destination = [&] (raw_result_t) { ++destination_calls; };
    throw_copy = true;
    EXPECT_THROW (destination = source, std::runtime_error);
    destination (raw_result_t::success ({}));
    EXPECT_EQ (destination_calls, 1);
    EXPECT_EQ (source_calls, 0);
    throw_copy = false;
    destination = source;
    destination (raw_result_t::success ({}));
    EXPECT_EQ (source_calls, 1);
    callback_t empty = std::function<void (raw_result_t)>{};
    EXPECT_FALSE (empty);
    EXPECT_THROW (empty (raw_result_t::success ({})), std::bad_function_call);
}

} // namespace
