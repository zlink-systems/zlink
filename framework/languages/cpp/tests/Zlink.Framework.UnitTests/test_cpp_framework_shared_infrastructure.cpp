/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/service_liveness_registry.hpp"
#include <gtest/gtest.h>
#include "../support/scope_exit.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

#ifdef __linux__
TEST (ZLinkStateLane, LastRuntimeOwnerReleasesInfrastructureThreads)
{
    const auto thread_count = [] {
        return std::distance (std::filesystem::directory_iterator ("/proc/self/task"),
                              std::filesystem::directory_iterator ());
    };
    const auto before = thread_count ();
    {
        zlink::framework::runtime::mesh::service_liveness_registry_t owner;
        std::promise<void> completed;
        zlink::framework::runtime::state_lane_t lane;
        ASSERT_TRUE (lane.try_post ([&] { completed.set_value (); }));
        completed.get_future ().get ();
        EXPECT_EQ (0u, owner.size ());
    }
    // All accepted work and its execution resources belong to the runtime scope.
    const auto after = thread_count ();
    RecordProperty ("threads_before", static_cast<int> (before));
    RecordProperty ("threads_after", static_cast<int> (after));
    EXPECT_EQ (before, after);
}

TEST (ZLinkStateLane, ComponentCountDoesNotIncreaseInfrastructureThreads)
{
    using zlink::framework::runtime::mesh::service_liveness_registry_t;
    const auto thread_count = [] {
        return std::distance (std::filesystem::directory_iterator ("/proc/self/task"),
                              std::filesystem::directory_iterator ());
    };
    service_liveness_registry_t first;
    const auto before = thread_count ();
    std::vector<std::unique_ptr<service_liveness_registry_t>> components;
    for (int index = 0; index < 16; ++index) {
        auto component = std::make_unique<service_liveness_registry_t> ();
        EXPECT_EQ (0u, component->size ());
        components.push_back (std::move (component));
    }
    const auto after = thread_count ();
    RecordProperty ("threads_before", static_cast<int> (before));
    RecordProperty ("threads_after", static_cast<int> (after));
    EXPECT_EQ (before, after);
    components.clear ();
    // Releasing one owner must not stop resources used by another owner.
    EXPECT_EQ (0u, first.size ());
}
#endif

TEST (ZLinkStateLane, ClosingOwnerPreservesOtherOwnersAsyncDrain)
{
    using namespace zlink::framework::runtime;
    state_lane_t remaining;
    {
        state_lane_t departing;
        std::promise<void> completed;
        ASSERT_TRUE (departing.try_post ([&] { completed.set_value (); }));
        completed.get_future ().get ();
    }
    std::promise<void> completed;
    ASSERT_TRUE (remaining.try_post ([&] { completed.set_value (); }));
    completed.get_future ().get ();
}

TEST (ZLinkStateLane, CloseFromOnlyWorkerYieldsUntilPostedCapturesAreReleased)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    ASSERT_EXIT (
      ([] {
          struct capture_t
          {
              std::atomic_bool &released;
              ~capture_t () { released.store (true); }
          };
          offload_executor_t executor (1);
          state_lane_t lane (executor);
          std::atomic_bool released{false};
          std::promise<bool> completed;
          auto result = completed.get_future ();
          executor.submit ([&] {
              auto capture = std::make_shared<capture_t> (released);
              if (!lane.try_post ([capture = std::move (capture)] {}))
                  std::_Exit (2);
              auto closing = lane.close_async ();
              if (closing.await_ready ())
                  std::_Exit (3);
              detail::observe_task_completion (closing, [&] (const result_t<void> &settled) {
                  completed.set_value (settled && released.load ());
              });
          });
          if (result.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
              std::_Exit (4);
          if (!result.get () || lane.try_post ([] {}))
              std::_Exit (5);
          // Repeated close and destruction retain the same completed drain.
          lane.close ();
          executor.drain ();
          std::_Exit (0);
      }()),
      ::testing::ExitedWithCode (0), "");
}

TEST (ZLinkStateLane, InfrastructureProgressesWhileApplicationWorkerWaits)
{
    using namespace zlink::framework::runtime;
    offload_executor_t application (1);
    std::promise<void> release;
    const auto released = release.get_future ().share ();
    std::promise<void> entered;
    state_lane_t lane;
    std::promise<void> completed;
    auto completion = completed.get_future ();
    zlink::framework::test::scope_exit_t unblock ([&] {
        release.set_value ();
        application.drain ();
        lane.close ();
    });
    application.submit ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().get ();
    const bool accepted = lane.try_post ([&] { completed.set_value (); });
    const auto status = completion.wait_for (std::chrono::seconds (2));
    unblock.run ();
    EXPECT_TRUE (accepted);
    EXPECT_EQ (std::future_status::ready, status);
}
