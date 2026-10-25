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
