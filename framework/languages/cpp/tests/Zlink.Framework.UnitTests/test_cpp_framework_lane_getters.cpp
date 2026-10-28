/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/allocation_counter.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/channels/route_channel_runtime.hpp"

#include <gtest/gtest.h>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;

TEST (CppFrameworkLaneGetters, SynchronousMeshSnapshotsOnlyAllocateTheirReturnedValue)
{
    auto state =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (std::string (128, 'm'));
    state->routing_id = zlink::routing_id_t::from (std::string ("getter-node"));
    state->default_request_timeout = 123ms;
    detail::mesh_node_runtime_t node (state);
    // Warm the lane and counter before measuring the synchronous owner turn.
    ASSERT_EQ (state->routing_id, node.routing_id ());
    ASSERT_EQ (state->default_request_timeout, node.default_request_timeout ());
    ASSERT_EQ (state->mesh_name, node.mesh_name ());

    std::optional<zlink::routing_id_t> rid;
    auto timeout = 0ms;
    std::string name;
    std::string baseline;
    const auto copy_allocations = tests::count_allocations ([&] { baseline = state->mesh_name; });
    const auto rid_allocations = tests::count_allocations ([&] { rid = node.routing_id (); });
    const auto timeout_allocations =
      tests::count_allocations ([&] { timeout = node.default_request_timeout (); });
    const auto name_allocations = tests::count_allocations ([&] { name = node.mesh_name (); });
    EXPECT_EQ (state->routing_id, rid);
    EXPECT_EQ (123ms, timeout);
    EXPECT_EQ (baseline, name);
    EXPECT_EQ (0u, rid_allocations);
    EXPECT_EQ (0u, timeout_allocations);
    EXPECT_EQ (copy_allocations, name_allocations);
    RecordProperty ("routing_id_allocations", rid_allocations);
    RecordProperty ("timeout_allocations", timeout_allocations);
    RecordProperty ("mesh_name_allocations", name_allocations);
    RecordProperty ("mesh_name_copy_allocations", copy_allocations);
}

TEST (CppFrameworkLaneGetters, RouteChannelSnapshotsDoNotAllocateCompletionState)
{
    detail::route_channel_runtime_t route ("getter-route");
    const auto expected = zlink::routing_id_t::from (std::string ("route-node"));
    route.routing_id (expected);
    route.default_request_timeout (321ms);
    route.start ();
    ASSERT_EQ (expected, route.routing_id ());
    ASSERT_EQ (321ms, route.default_request_timeout ());
    ASSERT_TRUE (route.running ());
    std::optional<zlink::routing_id_t> rid;
    auto timeout = 0ms;
    bool running = false;
    const auto rid_allocations = tests::count_allocations ([&] { rid = route.routing_id (); });
    const auto timeout_allocations =
      tests::count_allocations ([&] { timeout = route.default_request_timeout (); });
    const auto running_allocations = tests::count_allocations ([&] { running = route.running (); });
    EXPECT_EQ (expected, rid);
    EXPECT_EQ (321ms, timeout);
    EXPECT_TRUE (running);
    EXPECT_EQ (0u, rid_allocations);
    EXPECT_EQ (0u, timeout_allocations);
    EXPECT_EQ (0u, running_allocations);
    RecordProperty ("route_routing_id_allocations", rid_allocations);
    RecordProperty ("route_timeout_allocations", timeout_allocations);
    RecordProperty ("route_running_allocations", running_allocations);
    route.stop ();
}

TEST (CppFrameworkLaneGetters, MeshSnapshotsRetainReentrancyAndClosedLaneErrors)
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("getter-errors");
    detail::mesh_node_runtime_t node (state);
    state->lane.run_sync ([&] {
        EXPECT_THROW (node.routing_id (), std::logic_error);
        EXPECT_THROW (node.default_request_timeout (), std::logic_error);
        EXPECT_THROW (node.mesh_name (), std::logic_error);
    });
    state->lane.close ();
    EXPECT_THROW (node.routing_id (), std::runtime_error);
    EXPECT_THROW (node.default_request_timeout (), std::runtime_error);
    EXPECT_THROW (node.mesh_name (), std::runtime_error);
}
} // namespace
