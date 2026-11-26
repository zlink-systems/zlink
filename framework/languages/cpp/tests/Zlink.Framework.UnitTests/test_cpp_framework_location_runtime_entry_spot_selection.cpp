/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/location_runtime_mesh_host_test_access.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace zlink::framework::runtime
{
namespace
{
using namespace std::chrono_literals;
using lease_access = location_runtime_test_access_t;
using monotonic_time = std::chrono::steady_clock::time_point;

class EntrySpotTargetSelection : public ::testing::TestWithParam<int>
{
  protected:
    void SetUp () override
    {
        state =
          test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("entry-selection");
        state->routing_id = zlink::routing_id_t::from ("local");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->channels.emplace ("selection",
                                 detail::mesh_channel_registration_t{100, {}, true, true});
        state->core_context = std::make_shared<zlink::context_t> ();
        store = std::make_shared<in_memory_location_repository_t> ();
        const auto claimed =
          store->claim_owner_lease ("entry-owner", std::chrono::hours (1)).result ().value ();
        const auto lease = std::get<owner_lease_claimed_t> (claimed).token;
        service_collection_t services;
        services.add_singleton<store_location_resolvers_t> (
          std::make_unique<store_location_resolvers_t> (*store));
        provider = services.build_provider ();
        host = std::make_unique<mesh_node_host_service_t> (
          std::make_shared<runtime_failure_collector_t> (),
          std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>>{state}, serializers);
        mesh_node_host_service_test_access_t::selection_services (*host, provider);
        host->nodes ().front ()->start ().result ().value ();
        local.mesh_name = "entry-selection";
        local.rid = *state->routing_id;
        local.lifecycle_generation = 1;
        local.descriptor_revision = 1;
        local.owner_id = lease.owner_id;
        local.lease_generation = lease.lease_generation;
        local.endpoint = "inproc://entry-selection";
        local.security_identity = "default";
        local.state = framework_runtime_state_t::serving;
        local.object_role = object_role_t::server;
        local.placement_weight = 1;
        local.entry_spot_id = "entry-local";
        local.object_capabilities.push_back ({.object_kind = placement_object_kind_t::actor,
                                              .stable_type = "player",
                                              .entry_accepted = true});
    }

    void TearDown () override { host->stop (); }

    void publish (mesh_node_descriptor_t descriptor)
    {
        EXPECT_EQ (
          location_write_status_t::stored,
          store->update_mesh_node (std::move (descriptor), location_write_intent_t::new_claim)
            .result ()
            .value ()
            .status);
    }

    void occupy_capacity (std::uint64_t active, std::uint64_t reserved)
    {
        for (std::uint64_t index = 0; index < active + reserved; ++index) {
            const auto id = "capacity-" + std::to_string (index);
            object_reserve_request_t request;
            request.key = {placement_object_kind_t::actor, id};
            request.intent.stable_type = "player";
            request.target = {local.mesh_name,
                              node_rid_t::from_string (local.rid.to_string ()),
                              local.lifecycle_generation,
                              {local.owner_id, local.lease_generation}};
            request.creating_payload = {std::byte{0}};
            request.capacity_bundle.actor_slots = 1;
            const auto result = store->reserve (request).result ().value ();
            const auto *allocation = std::get_if<object_reserved_t> (&result);
            ASSERT_NE (nullptr, allocation);
            if (index < active) {
                const auto payload = encode_actor_authority_payload (actor_authority_payload_t{
                  .state = actor_authority_state_t::ready,
                  .stable_type = "player",
                  .actor_id = id,
                  .current_spot_id = *local.entry_spot_id,
                  .current_spot_generation = local.lifecycle_generation,
                  .current_spot_kind = actor_authority_spot_kind_t::entry,
                  .owner_id = local.owner_id,
                  .owner_lease_generation = static_cast<std::uint64_t> (local.lease_generation),
                  .mesh_name = local.mesh_name,
                  .node_rid = request.target.node_rid,
                  .node_generation = local.lifecycle_generation});
                const auto committed =
                  store->commit ({request.key, allocation->fence, payload}).result ().value ();
                ASSERT_TRUE (std::holds_alternative<object_committed_t> (committed));
            }
        }
        const auto descriptors = store->list_mesh_nodes (local.mesh_name).result ().value ();
        ASSERT_EQ (1u, descriptors.items.size ());
        EXPECT_EQ (active, descriptors.items.front ().capacity.actors.active);
        EXPECT_EQ (reserved, descriptors.items.front ().capacity.actors.reserved);
    }

    result_t<mesh_node_descriptor_t> select (std::string id = "actor")
    {
        return host
          ->select_actor_entry_spot (detail::actor_ref_access_t::make (
            node_rid_t::from_string ("local"), "player", std::move (id), 1))
          .result ();
    }

    std::shared_ptr<detail::mesh_node_builder_state_t> state;
    std::shared_ptr<in_memory_location_repository_t> store;
    serializer_registry_t serializers;
    service_provider_t provider;
    std::unique_ptr<mesh_node_host_service_t> host;
    mesh_node_descriptor_t local;
};

TEST_F (EntrySpotTargetSelection, CurrentNodeIsEligible)
{
    publish (local);
    const auto selected = select ();
    ASSERT_TRUE (selected);
    EXPECT_EQ (local.rid, selected.value ().rid);
}

TEST_F (EntrySpotTargetSelection, NoCandidatesIsUnavailable)
{
    const auto selected = select ();
    ASSERT_FALSE (selected);
    EXPECT_EQ (framework_error_kind_t::unavailable, selected.error_kind ());
}

TEST_P (EntrySpotTargetSelection, RejectsIneligibleDescriptors)
{
    switch (GetParam ()) {
        case 0:
            local.state = framework_runtime_state_t::draining;
            break;
        case 1:
            local.object_role = object_role_t::client;
            local.object_capabilities.clear ();
            local.entry_spot_id.reset ();
            break;
        case 2:
            local.object_capabilities.front ().stable_type = "other";
            break;
        case 3:
            local.object_capabilities.front ().entry_accepted = false;
            break;
        case 4:
            local.placement_weight = 0;
            break;
        case 5:
            local.capacity.actors.limit = 1;
            break;
        case 6:
            local.capacity.actors.limit = 1;
            break;
    }
    publish (local);
    if (GetParam () == 5)
        occupy_capacity (1, 0);
    if (GetParam () == 6)
        occupy_capacity (0, 1);
    const auto selected = select ();
    ASSERT_FALSE (selected);
    EXPECT_EQ (framework_error_kind_t::unavailable, selected.error_kind ());
}

INSTANTIATE_TEST_SUITE_P (Eligibility, EntrySpotTargetSelection, ::testing::Range (0, 7));

TEST_F (EntrySpotTargetSelection, LostOwnerLeaseIsUnavailable)
{
    publish (local);
    store->release_owner_lease ({local.owner_id, local.lease_generation}).result ().value ();
    const auto selected = select ();
    ASSERT_FALSE (selected);
    EXPECT_EQ (framework_error_kind_t::unavailable, selected.error_kind ());
}

TEST_F (EntrySpotTargetSelection, RemainingCapacityIsEligible)
{
    local.capacity.actors.limit = 3;
    publish (local);
    occupy_capacity (1, 1);
    EXPECT_TRUE (select ());
}

TEST_F (EntrySpotTargetSelection, WeightDoesNotPreferCurrentNode)
{
    publish (local);
    auto remote = local;
    remote.rid = zlink::routing_id_t::from ("remote");
    remote.entry_spot_id = "entry-remote";
    remote.placement_weight = 9;
    publish (remote);
    auto &topology = host->nodes ().front ()->native_node ().transport ().topology ();
    auto peer = topology.local_descriptor ();
    peer.node_routing_id = remote.rid.to_bytes ();
    peer.lifecycle_generation = remote.lifecycle_generation;
    peer.advertised_endpoint = "inproc://entry-selection-remote";
    peer.state = mesh::service_node_state_t::serving;
    topology.admit (peer, peer.node_routing_id);
    ASSERT_TRUE (
      host->nodes ().front ()->has_admitted_peer (remote.rid, remote.lifecycle_generation));
    // Exercise identities that select each of the weighted targets.
    bool saw_local = false;
    bool saw_remote = false;
    for (int index = 0; index < 100 && !(saw_local && saw_remote); ++index) {
        const auto id = "weighted-" + std::to_string (index);
        const auto selected = select (id);
        ASSERT_TRUE (selected);
        EXPECT_EQ (std::hash<std::string>{}(id) % 10 == 0 ? local.rid : remote.rid,
                   selected.value ().rid);
        saw_local |= selected.value ().rid == local.rid;
        saw_remote |= selected.value ().rid == remote.rid;
    }
    EXPECT_TRUE (saw_local);
    EXPECT_TRUE (saw_remote);
}
} // namespace
} // namespace zlink::framework::runtime
