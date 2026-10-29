/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/service_liveness_registry.hpp"
#include "runtime/mesh/service_topology_registry.hpp"
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
#include <unistd.h>
TEST (ZLinkStateLane, OneWorkerTopologyAdmissionSharesLivenessOwner)
{
    ASSERT_EXIT (([] {
                     alarm (5);
                     zlink::framework::completion_owner_t completion_owner;
                     namespace mesh = zlink::framework::runtime::mesh;
                     using namespace zlink::framework::runtime;
                     offload_executor_t executor (1);
                     state_lane_t lane (executor);
                     if (&lane.executor () != &executor)
                         std::_Exit (7);
                     mesh::service_liveness_registry_t::connection_t retained;
                     {
                         mesh::service_liveness_registry_t liveness (lane);
                         mesh::service_node_descriptor_t local{"one-worker",
                                                               {'a'},
                                                               1,
                                                               1,
                                                               "tcp://127.0.0.1:1",
                                                               {{"X", 100}},
                                                               mesh::service_node_state_t::serving};
                         mesh::service_topology_registry_t topology (lane, local, {}, &liveness);
                         auto remote = local;
                         remote.node_routing_id = {'b'};
                         const std::vector<std::uint8_t> route{1};
                         const auto now = mesh::service_liveness_registry_t::clock_t::now ();
                         if (topology.admit (remote, {1}, now)
                             != mesh::peer_admission_result_t::admitted)
                             std::_Exit (1);
                         auto peer = topology.peer (remote.node_routing_id);
                         retained = liveness.connection (remote.node_routing_id);
                         if (!peer || !peer->ready || !retained || peer->liveness != retained)
                             std::_Exit (2);
                         auto tick = liveness.tick (now);
                         if (tick.probes.size () != 1
                             || !liveness.acknowledge_probe (remote.node_routing_id, route,
                                                             tick.probes.front ().probe_id))
                             std::_Exit (3);
                         if (!liveness.acknowledge (remote.node_routing_id, route,
                                                    tick.probes.front ().probe_id, now))
                             std::_Exit (4);
                         if (liveness.disconnect (remote.node_routing_id, {2})
                             || !topology.disconnect (remote.node_routing_id, {1})
                             || !liveness.disconnect (remote.node_routing_id, {1})
                             || topology.peer (remote.node_routing_id)
                             || liveness.connection (remote.node_routing_id))
                             std::_Exit (5);
                     }
                     // Child registries borrow: destroying them must not close their caller's lane.
                     if (!lane.run ([] { return true; }).get ())
                         std::_Exit (6);
                     retained->record_received (mesh::service_liveness_registry_t::clock_t::now ());
                     lane.close ();
                     std::_Exit (0);
                 }()),
                 ::testing::ExitedWithCode (0), "");
}

TEST (ZLinkStateLane, LastRuntimeOwnerReleasesInfrastructureThreads)
{
    const auto thread_count = [] {
        return std::distance (std::filesystem::directory_iterator ("/proc/self/task"),
                              std::filesystem::directory_iterator ());
    };
    const auto before = thread_count ();
    {
        zlink::framework::runtime::state_lane_t owner_lane;
        zlink::framework::runtime::mesh::service_liveness_registry_t owner (owner_lane);
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
    zlink::framework::runtime::state_lane_t first_lane;
    service_liveness_registry_t first (first_lane);
    const auto before = thread_count ();
    std::vector<std::unique_ptr<zlink::framework::runtime::state_lane_t>> lanes;
    std::vector<std::unique_ptr<service_liveness_registry_t>> components;
    for (int index = 0; index < 16; ++index) {
        lanes.push_back (std::make_unique<zlink::framework::runtime::state_lane_t> ());
        auto component = std::make_unique<service_liveness_registry_t> (*lanes.back ());
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
        EXPECT_EQ (&remaining.executor (), &departing.executor ());
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
    ASSERT_EXIT (([] {
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
                         detail::observe_task_completion (
                           closing, [&] (const result_t<void> &settled) {
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
