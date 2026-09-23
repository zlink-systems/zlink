/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/execution/serial_execution_queue.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/channels/call.hpp>

#include <gtest/gtest.h>

#include <chrono>

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

} // namespace
