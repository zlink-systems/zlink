/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/allocation_counter.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <stdexcept>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;

TEST (CppFrameworkRequestSnapshots, MeshDefaultsAllocateOnlyTheReturnedName)
{
    auto state =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (std::string (128, 'm'));
    state->default_request_timeout = 123ms;
    detail::mesh_node_runtime_t node (state);
    ASSERT_EQ (123ms, node.default_request_timeout ());
    ASSERT_EQ (state->mesh_name, node.mesh_name ());
    auto timeout = 0ms;
    std::string baseline, name;
    const auto copy = tests::count_allocations ([&] { baseline = state->mesh_name; });
    const auto timeouts =
      tests::count_allocations ([&] { timeout = node.default_request_timeout (); });
    const auto names = tests::count_allocations ([&] { name = node.mesh_name (); });
    EXPECT_EQ (123ms, timeout);
    EXPECT_EQ (baseline, name);
    EXPECT_EQ (0u, timeouts);
    EXPECT_EQ (copy, names);
    std::fprintf (stderr, "request snapshots timeout=%zu name=%zu copy=%zu\n", timeouts, names,
                  copy);
}

TEST (CppFrameworkRequestSnapshots, RouteTimeoutDoesNotAllocateACompletionState)
{
    detail::route_channel_runtime_t route ("request-snapshot");
    route.default_request_timeout (321ms);
    auto timeout = 0ms;
    const auto allocations =
      tests::count_allocations ([&] { timeout = route.default_request_timeout (); });
    EXPECT_EQ (321ms, timeout);
    EXPECT_EQ (0u, allocations);
    std::fprintf (stderr, "request route timeout allocations=%zu\n", allocations);
}

TEST (CppFrameworkRequestSnapshots, MeshDefaultsRetainOwnerEntryErrors)
{
    auto state =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("request-errors");
    detail::mesh_node_runtime_t node (state);
    state->lane.run_sync ([&] {
        EXPECT_THROW (node.default_request_timeout (), std::logic_error);
        EXPECT_THROW (node.mesh_name (), std::logic_error);
    });
    state->lane.close ();
    EXPECT_THROW (node.default_request_timeout (), std::runtime_error);
    EXPECT_THROW (node.mesh_name (), std::runtime_error);
}
}
