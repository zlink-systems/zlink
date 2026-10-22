/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"

#include <cstdio>
#include <memory>
#include <optional>

namespace
{
namespace fw = zlink::framework;
namespace detail = fw::detail;
namespace mesh = fw::runtime::mesh;

std::shared_ptr<detail::mesh_node_builder_state_t> mesh_state ()
{
    auto state =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("send-allocation");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->routing_id = zlink::routing_id_t::from ("source");
    return state;
}

TEST (SendAllocations, KnownTargetForwardsTheNativeTaskWithoutAnotherResult)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    const auto target = zlink::routing_id_t::from ("target");
    auto peer = node.native_node ().transport ().topology ().local_descriptor ();
    peer.node_routing_id = target.to_bytes ();
    peer.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (node.native_node ().transport ().topology ().admit (peer, {1}),
               mesh::peer_admission_result_t::admitted);
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    const auto native = [&] {
        (void) node.native_node ().transport ().classify_node_target (target.to_bytes ());
        return node.native_node ().send_to_node (target, parts).result ();
    };
    const auto forward = [&] {
        return node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    };
    ASSERT_TRUE (native ());
    ASSERT_TRUE (forward ());
    std::optional<fw::result_t<zlink::submit_result_t>> expected;
    std::optional<fw::result_t<zlink::submit_result_t>> actual;
    const auto baseline = fw::tests::measure_allocations ([&] { expected.emplace (native ()); });
    const auto measured = fw::tests::measure_allocations ([&] { actual.emplace (forward ()); });
    ASSERT_TRUE (*expected);
    ASSERT_TRUE (*actual);
    EXPECT_EQ (expected->value (), zlink::submit_result_t::not_connected);
    EXPECT_EQ (actual->value (), expected->value ());
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    std::printf ("known send allocations: native=%zu/%zu forwarded=%zu/%zu\n", baseline.count,
                 baseline.bytes, measured.count, measured.bytes);
    node.stop ();
}

class pending_listing_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    using page_t = fw::location_page_t<fw::mesh_node_descriptor_t>;
    fw::task_completion_source_t<page_t> completion;
    unsigned calls = 0;
    fw::task_t<page_t> list_mesh_nodes (std::string, fw::location_page_request_t = {}) override
    {
        ++calls;
        return completion.task ();
    }
};

TEST (SendAllocations, UnknownTargetPreservesLocationLookupAndFailureClassification)
{
    for (const bool move_parts : {false, true}) {
        for (const bool failed_listing : {false, true}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            const auto target = zlink::routing_id_t::from ("unknown");
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            EXPECT_TRUE (store->completion.complete (
              failed_listing ? fw::result_t<pending_listing_t::page_t>::failure (
                                 fw::framework_error_kind_t::unavailable, "listing unavailable")
                             : fw::result_t<pending_listing_t::page_t>::success ({})));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, UnknownTargetKeepsValuesAndFailuresAcrossLocationSuspension)
{
    enum class scenario_t
    {
        replaced_target,
        cleared_parts,
        failed_listing
    };
    for (const bool move_parts : {false, true}) {
        for (const auto scenario :
             {scenario_t::replaced_target, scenario_t::cleared_parts, scenario_t::failed_listing}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            auto target = zlink::routing_id_t::from ("unknown");
            const auto submitted_target = target;
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            if (scenario == scenario_t::replaced_target)
                target = zlink::routing_id_t::from ("changed");
            else
                parts.clear ();
            pending_listing_t::page_t page;
            if (scenario == scenario_t::replaced_target) {
                fw::mesh_node_descriptor_t client;
                client.mesh_name = "send-allocation";
                client.rid = submitted_target;
                client.object_role = fw::object_role_t::client;
                page.items.push_back (std::move (client));
            }
            EXPECT_TRUE (store->completion.complete (
              scenario == scenario_t::failed_listing
                ? fw::result_t<pending_listing_t::page_t>::failure (
                    fw::framework_error_kind_t::unavailable, "listing unavailable")
                : fw::result_t<pending_listing_t::page_t>::success (std::move (page))));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), scenario == scenario_t::replaced_target
                                          ? zlink::submit_result_t::not_found
                                          : zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, StoppedNodeKeepsItsTaskFailureBoundary)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    const auto target = zlink::routing_id_t::from ("target");
    const std::vector<zlink::message_t> parts;
    const auto result = node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    EXPECT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::protocol_error);
}
} // namespace
