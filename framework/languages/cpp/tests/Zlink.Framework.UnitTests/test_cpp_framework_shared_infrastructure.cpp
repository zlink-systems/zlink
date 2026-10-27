/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/service_liveness_registry.hpp"
#include <gtest/gtest.h>
#include "../support/scope_exit.hpp"
#include <chrono>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

#ifdef __linux__
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
