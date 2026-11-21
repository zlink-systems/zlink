/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/opaque_store_descriptor_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_record_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

void expect_cleanup3462_capacity_returned (bool active)
{
    in_memory_location_store_t opaque;
    provider_location_repository_t repository (opaque);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("cleanup-owner", 60s).result ().value ())
                         .token;
    const auto other = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("other-owner", 60s).result ().value ())
                         .token;
    for (const auto &token : {owner, other}) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "cleanup";
        descriptor.rid = zlink::routing_id_t::from (token.owner_id);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = token.owner_id;
        descriptor.lease_generation = token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "cleanup";
        descriptor.entry_spot_id = token.owner_id + "-entry";
        descriptor.object_capabilities = {{placement_object_kind_t::actor, "player",
                                           maintenance_policy_kind_t::disabled, false, 0, true}};
        descriptor.placement_weight = 100;
        descriptor.capacity.actors = {0, 0, 8};
        descriptor.activation_concurrency = {0, 128};
        ASSERT_EQ (location_write_status_t::stored,
                   repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        object_reserve_request_t request{
          .key = {placement_object_kind_t::actor, token.owner_id},
          .intent = {.stable_type = "player",
                     .request_content_reference = "root",
                     .request_encoded_size = 4},
          .target = {.mesh_name = "cleanup",
                     .node_rid = node_rid_t::from_string (token.owner_id),
                     .node_lifecycle_generation = 1,
                     .owner = token},
          .creating_payload = {std::byte{1}},
          .capacity_bundle = {.actor_slots = 1}};
        const auto result = repository.reserve (request).result ().value ();
        ASSERT_TRUE (std::holds_alternative<object_reserved_t> (result));
        if (active) {
            const auto &reservation = std::get<object_reserved_t> (result);
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository.commit ({request.key, reservation.fence, {std::byte{2}}})
                .result ()
                .value ()));
        }
    }
    EXPECT_EQ (0, repository.remove_all_by_owner ({owner.owner_id, owner.lease_generation + 1})
                    .result ()
                    .value ());
    const auto before = repository.list_mesh_nodes ("cleanup").result ().value ();
    for (const auto &descriptor : before.items) {
        EXPECT_EQ (active ? 1u : 0u, descriptor.capacity.actors.active);
        EXPECT_EQ (active ? 0u : 1u, descriptor.capacity.actors.reserved);
    }
    EXPECT_EQ (1, repository.remove_all_by_owner (owner).result ().value ());
    const auto after = repository.list_mesh_nodes ("cleanup").result ().value ();
    for (const auto &descriptor : after.items) {
        const bool kept = descriptor.owner_id == other.owner_id;
        EXPECT_EQ (active && kept ? 1u : 0u, descriptor.capacity.actors.active);
        EXPECT_EQ (!active && kept ? 1u : 0u, descriptor.capacity.actors.reserved);
    }
    EXPECT_EQ (0, repository.remove_all_by_owner (owner).result ().value ());
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository.read_authority (actor_authority_key (owner.owner_id)).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (
      repository.read_authority (actor_authority_key (other.owner_id)).result ().value ()));
    EXPECT_EQ (2u, repository.list_mesh_nodes ("cleanup").result ().value ().items.size ());
    EXPECT_TRUE (std::holds_alternative<owner_lease_found_t> (
      repository.read_owner_lease (owner.owner_id).result ().value ()));
}

TEST (ZLinkFrameworkOpaqueStoreProviders, ForceStop2646SweepDeletesAuthorityAndPreservesDescriptors)
{
    expect_cleanup3462_capacity_returned (false);
}
TEST (ZLinkFrameworkOpaqueStoreProviders, Cleanup3462ReservedSweepReturnsCapacity)
{
    expect_cleanup3462_capacity_returned (false);
}
TEST (ZLinkFrameworkOpaqueStoreProviders, Cleanup3462ActiveSweepReturnsCapacity)
{
    expect_cleanup3462_capacity_returned (true);
}

void expect_cleanup3462_descriptor_deadline (bool expire)
{
    unanswered_descriptor_store_t opaque (unanswered_descriptor_store_t::phase_t::initial_read);
    provider_location_repository_t repository (opaque);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("deadline-owner", 60s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "deadline";
    descriptor.rid = zlink::routing_id_t::from ("deadline-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.security_identity = "deadline";
    ASSERT_EQ (location_write_status_t::stored,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    const auto deadline = std::chrono::steady_clock::now () + (expire ? 20ms : 30s);
    opaque.armed = true;
    auto removing =
      repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner, deadline);
    ASSERT_TRUE (opaque.held);
    EXPECT_FALSE (removing.await_ready ());
    if (expire) {
        EXPECT_FALSE (removing.result_for (std::chrono::ceil<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ())));
        EXPECT_GE (std::chrono::steady_clock::now (), deadline);
    }
    opaque.release.complete (result_t<void>::success ());
    const auto result = removing.result ();
    EXPECT_EQ (expire ? 0 : 1, opaque.delete_writes);
    if (expire) {
        ASSERT_FALSE (result);
        ASSERT_NE (nullptr, result.error ());
        EXPECT_EQ (framework_error_kind_t::deadline_exceeded, result.error ()->kind ());
    } else {
        EXPECT_EQ (location_write_status_t::stored, result.value ());
    }
    opaque.armed = false;
    EXPECT_EQ (expire ? 1u : 0u,
               repository.list_mesh_nodes ("deadline").result ().value ().items.size ());
}
TEST (ZLinkFrameworkOpaqueStoreProviders, Cleanup3462DescriptorReadAfterDeadlineStartsNoWrite)
{
    expect_cleanup3462_descriptor_deadline (true);
}
TEST (ZLinkFrameworkOpaqueStoreProviders, Cleanup3462DescriptorReadBeforeDeadlineRemovesDescriptor)
{
    expect_cleanup3462_descriptor_deadline (false);
}
}
