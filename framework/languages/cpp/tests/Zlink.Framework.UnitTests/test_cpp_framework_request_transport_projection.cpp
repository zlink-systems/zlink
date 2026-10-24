/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime_failure_fixture.hpp"
#include <cstdio>
#include <atomic>
#include <optional>
namespace
{
namespace fw = zlink::framework;
using reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;

TEST (RequestCallAllocations, ReadyNodeReplyAddsOnlyItsProjectionAndAdmission)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("ready-projection");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t reference_operation;
    std::shared_ptr<fw::task_completion_source_t<void>> reference_admitted;
    std::optional<fw::task_t<reply_t>> reference_reply;
    const auto baseline = fw::tests::count_allocations ([&] {
        reference_admitted = std::make_shared<fw::task_completion_source_t<void>> ();
        reference_reply.emplace (
          runtime.await_request_reply (reference_operation, "MeshNode request"));
        fw::detail::observe_request_failure (*reference_reply, reference_admitted);
        reference_admitted->complete (fw::result_t<void>::success ());
    });
    fw::runtime::host::pending_operation_t operation;
    auto submitted = fw::task_t<zlink::submit_result_t> (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    std::optional<fw::request_task_t<reply_t>> pending;
    const auto measured = fw::tests::count_allocations ([&] {
        pending.emplace (runtime.compose_node_request_reply (operation, std::move (submitted)));
    });
    std::printf ("ready node reply: measured=%zu projection_and_admission=%zu\n", measured,
                 baseline);
    EXPECT_EQ (measured, baseline);
    EXPECT_FALSE (pending->await_ready ());
    ASSERT_TRUE (pending->admitted ().await_ready ());
    EXPECT_TRUE (pending->admitted ().result ());
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    reply.parts.push_back (zlink::message_t::from (std::string ("reply")));
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
    const auto &result = pending->result ();
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
    auto pending = runtime.compose_node_request_reply (
      operation, fw::task_t<zlink::submit_result_t> (
                   fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)));
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
// Compare against the same runtime's result slot plus a non-owning terminal
// registration. The test retains that slot itself; production must put the
// callback lifetime in the registration without another allocation.
TEST (RequestCallAllocations, TerminalProjectionUsesOnlyTheResultSlotAndRegistration)
{
    fw::task_completion_source_t<int> reference_source;
    auto reference_input = reference_source.task ();
    std::optional<fw::task_t<int>> reference;
    const auto baseline = fw::tests::count_allocations ([&] {
        reference.emplace (fw::result_t<int>::success (0));
        fw::detail::observe_task_terminal (reference_input,
                                           [slot = &reference] (const auto &) { (void) slot; });
    });
    for (const bool ready : {false, true}) {
        fw::task_completion_source_t<int> source;
        auto input = source.task ();
        if (ready)
            source.complete (fw::result_t<int>::success (41));
        std::optional<fw::task_t<int>> projected;
        unsigned calls = 0;
        const auto measured = fw::tests::count_allocations ([&] {
            projected.emplace (fw::detail::project_task_terminal<int> (
              input, [&calls] (const fw::result_t<int> &value) {
                  ++calls;
                  return fw::result_t<int>::success (value.value () + 1);
              }));
        });
        // A ready source has no registration vector allocation.
        const auto slot = fw::tests::count_allocations (
          [] { fw::task_t<int> result (fw::result_t<int>::success (0)); });
        std::printf ("terminal projection ready=%d allocations=%zu baseline=%zu\n", ready, measured,
                     ready ? slot : baseline);
        EXPECT_EQ (measured, ready ? slot : baseline);
        if (!ready)
            source.complete (fw::result_t<int>::success (41));
        EXPECT_EQ (projected->result ().value (), 42);
        EXPECT_EQ (calls, 1u);
    }
}
TEST (RequestCallAllocations, TerminalProjectionReleasesProducerCapturesAtPhysicalCompletion)
{
    for (const bool retain_reply : {false, true}) {
        fw::task_completion_source_t<int> source;
        auto input = source.task ();
        auto lifetime = std::make_shared<int> (41);
        const std::weak_ptr<int> weak = lifetime;
        auto projected = fw::detail::project_task_terminal<int> (
          input, [lifetime = std::move (lifetime)] (const auto &result) {
              return fw::result_t<int>::success (*lifetime + result.value ());
          });
        std::optional<fw::task_t<int>> retained;
        if (retain_reply)
            retained.emplace (std::move (projected));
        else {
            auto discarded = std::move (projected);
        }
        EXPECT_FALSE (weak.expired ());
        source.complete (fw::result_t<int>::success (1));
        EXPECT_TRUE (weak.expired ());
        if (retained)
            EXPECT_EQ (retained->result ().value (), 42);
    }
}
TEST (RequestCallAllocations, TerminalProjectionDestroysConstCapturesAtPhysicalCompletion)
{
    for (const bool complete : {false, true}) {
        fw::task_completion_source_t<int> source;
        auto input = source.task ();
        std::weak_ptr<int> weak;
        std::optional<fw::task_t<int>> output;
        {
            const auto capture = std::make_shared<int> (41);
            weak = capture;
            output.emplace (fw::detail::project_task_terminal<int> (
              input, [capture] (const fw::result_t<int> &result) {
                  return fw::result_t<int>::success (*capture + result.value ());
              }));
        }
        EXPECT_FALSE (weak.expired ());
        if (complete) {
            source.complete (fw::result_t<int>::success (1));
            EXPECT_EQ (output->result ().value (), 42);
            EXPECT_TRUE (weak.expired ());
        } else {
            // An abandoned pending projection still owns its capture through
            // the source registration, which releases it when the source dies.
            output.reset ();
            EXPECT_FALSE (weak.expired ());
        }
    }
}
} // namespace
