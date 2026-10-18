/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/remote_actor_commit_turn_state.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/channels/call.hpp>

#include <gtest/gtest.h>

#include "runtime_context_hooks_environment.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace
{

using namespace zlink::framework;

TEST (ZLinkFrameworkSharedSerialTurn, ActorJoinRegistersAcrossLibraryBoundary)
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    int activated = 0;
    queue.run ("deferred-actor-join-handler", [&] {
        EXPECT_NO_THROW (
          actor_join_call_t ([&] (std::chrono::milliseconds) { ++activated; }).defer ());
        EXPECT_EQ (0, activated);
    });
    EXPECT_EQ (1, activated);
}

TEST (ZLinkFrameworkSharedSerialTurn, ApplicationJobContextReachesRuntimeLibrary)
{
    int job = 0;
    const auto previous = detail::application_job_context_t::exchange (&job);
    EXPECT_THROW (detail::ensure_blocking_submit_allowed (), framework_exception_t);
    EXPECT_EQ (&job, detail::application_job_context_t::current ());
    detail::application_job_context_t::exchange (previous);
}

TEST (ZLinkFrameworkSharedSerialTurn, SerialResumeFailureUsesRuntimeLibrary)
{
    detail::set_serial_resume_failure (framework_error_kind_t::shutting_down, "stopping");
    const auto failure = detail::take_serial_resume_failure ();
    ASSERT_TRUE (failure.has_value ());
    EXPECT_EQ (framework_error_kind_t::shutting_down, failure->kind);
    EXPECT_EQ ("stopping", failure->message);
    EXPECT_FALSE (detail::take_serial_resume_failure ().has_value ());
}

TEST (ZLinkFrameworkSharedSerialTurn, AmbientFlowHooksReachExecutable)
{
    EXPECT_NE (nullptr, detail::current_ambient_context_hooks ());
}

TEST (ZLinkFrameworkSharedSerialTurn, ActiveCommitCancellationBeforeBeginKeepsMailboxUntilCleanup)
{
    using state_t = detail::remote_actor_commit_turn_state_t;
    using queue_t = runtime::serial_execution_queue_t;
    using disposition_t = runtime::serial_cancel_submission_outcome_t;
    runtime::offload_executor_t executor (1);
    queue_t queue (executor);
    state_t state;
    std::mutex mutex;
    std::condition_variable changed;
    bool selected = false;
    bool begin = false;
    bool began = false;
    bool terminal_selected = false;
    queue_t::async_completion_t finish_cleanup;
    std::vector<std::string> order;
    state.on_terminal ([&] (state_t::outcome_t outcome, queue_t::async_completion_t complete) {
        std::lock_guard lock (mutex);
        EXPECT_FALSE (outcome.success);
        terminal_selected = true;
        finish_cleanup = std::move (complete);
        changed.notify_all ();
    });
    auto submitted = queue.try_post_cancellable_async (
      "commit-before-begin",
      [&] (auto complete) {
          {
              std::unique_lock lock (mutex);
              selected = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return begin; });
          }
          began = state.begin_active ();
          if (began) {
              EXPECT_FALSE (state.try_begin_commit ());
              state.settle (state.cancel_outcome (), std::move (complete));
          } else {
              // Always release the real queue even for the old erroneous owner.
              complete ([] {});
          }
          changed.notify_all ();
      },
      [&] (disposition_t disposition) { state.request_stop (disposition); });
    ASSERT_TRUE (submitted);
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return selected; });
    }
    EXPECT_TRUE (queue.try_post ("following-actor", [&] {
        std::lock_guard lock (mutex);
        order.emplace_back ("following Actor");
    }));
    EXPECT_EQ (disposition_t::active_cancel_requested,
               queue.cancel_submission (submitted.value ()).result ().value ());
    EXPECT_FALSE (state.terminal ());
    {
        std::lock_guard lock (mutex);
        begin = true;
    }
    changed.notify_all ();
    queue_t::async_completion_t release;
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return terminal_selected; });
        EXPECT_TRUE (order.empty ());
        release = std::move (finish_cleanup);
        order.emplace_back ("cleanup terminal");
    }
    EXPECT_TRUE (static_cast<bool> (release));
    if (release)
        release ([&] {
            std::lock_guard lock (mutex);
            order.emplace_back ("Join completion");
        });
    queue.drain ();
    EXPECT_TRUE (began);
    EXPECT_EQ ((std::vector<std::string>{"cleanup terminal", "Join completion", "following Actor"}),
               order);
}

TEST (ZLinkFrameworkSharedSerialTurn, QueuedCommitCancellationSelectsOneTerminalWithoutActorRight)
{
    using state_t = detail::remote_actor_commit_turn_state_t;
    using disposition_t = runtime::serial_cancel_submission_outcome_t;
    state_t state;
    int terminals = 0;
    state.on_terminal ([&] (state_t::outcome_t outcome, auto complete) {
        ++terminals;
        EXPECT_FALSE (outcome.success);
        EXPECT_FALSE (static_cast<bool> (complete));
    });
    state.request_stop (disposition_t::queued_cancelled);
    state.request_stop (disposition_t::queued_cancelled);
    state.settle ({true, framework_error_kind_t::internal_failure, {}});
    EXPECT_FALSE (state.begin_active ());
    EXPECT_EQ (1, terminals);
}

TEST (ZLinkFrameworkSharedSerialTurn, CancellationAfterCommitSelectionPreservesCommittedOutcome)
{
    using state_t = detail::remote_actor_commit_turn_state_t;
    state_t state;
    int terminals = 0;
    state.on_terminal ([&] (state_t::outcome_t outcome, auto complete) {
        ++terminals;
        EXPECT_TRUE (outcome.success);
        complete ([] {});
    });
    EXPECT_TRUE (state.begin_active ());
    EXPECT_TRUE (state.try_begin_commit ());
    state.request_stop (runtime::serial_cancel_submission_outcome_t::active_cancel_requested);
    EXPECT_FALSE (state.terminal ());
    state.mark_committed ();
    state.settle ({true, framework_error_kind_t::internal_failure, {}},
                  [] (auto finish) { finish (); });
    EXPECT_EQ (1, terminals);
}

} // namespace
