/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"
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
using zlink::framework::tests::owner_lease_time_store_t;
using namespace zlink::framework::test::opaque_store;

class delayed_renew_write_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (!delay_write)
            return inner.write (std::move (request));
        pending_request = std::move (request);
        write_called.set_value ();
        return pending.task ();
    }
    void finish_write ()
    {
        auto result = inner.write (std::move (*pending_request)).result ();
        pending.complete (result);
    }

    in_memory_location_store_t inner;
    bool delay_write = false;
    std::promise<void> write_called;
    task_completion_source_t<store_write_result_t> pending;
    std::optional<store_write_request_t> pending_request;
};

TEST (ZLinkFrameworkOpaqueStoreProviders, RenewReturnsBeforeDelayedProviderWriteCompletes)
{
    delayed_renew_write_store_t store;
    provider_location_repository_t repository (store);
    const auto claimed = repository.claim_owner_lease ("delayed-owner", 30s).result ().value ();
    const auto owner = std::get<owner_lease_claimed_t> (claimed).token;
    auto write_called = store.write_called.get_future ();
    store.delay_write = true;
    auto renewal = repository.renew_owner_lease (owner, 30s);
    write_called.wait ();
    EXPECT_FALSE (renewal.await_ready ());
    store.finish_write ();
    std::atomic_int completions = 0;
    std::promise<void> completed;
    auto completion = completed.get_future ();
    detail::observe_task_completion (renewal, [&] (const auto &) {
        ++completions;
        completed.set_value ();
    });
    completion.get ();
    EXPECT_TRUE (std::holds_alternative<owner_lease_renewed_t> (renewal.result ().value ()));
    EXPECT_EQ (1, completions.load ());
}

class renew_before_conditional_commit_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        const auto descriptor_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<store_put_t> (&mutation);
              return put && put->key.value.starts_with (std::string ("mesh-node") + '\0');
          });
        const auto reclaim_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *erase = std::get_if<store_delete_t> (&mutation);
              return erase && erase->key.value.starts_with (std::string ("authority") + '\0');
          });
        if (reclaim_write)
            ++reclaim_writes;
        if ((renew_on_descriptor_write && descriptor_write)
            || (renew_on_reclaim_write && reclaim_write)) {
            renew_on_descriptor_write = false;
            renew_on_reclaim_write = false;
            const store_key_t lease_key{std::string ("owner-lease") + '\0' + owner_id};
            const auto lease = std::get<store_found_t> (co_await inner.read (lease_key));
            store_write_request_t renew_request{
              {store_version_condition_t{lease_key, lease.value.version}},
              {store_put_t{lease_key, lease.value.bytes, 30s}}};
            const auto renewed = co_await inner.write (std::move (renew_request));
            if (!std::holds_alternative<store_write_applied_t> (renewed))
                throw std::runtime_error ("test lease renewal failed");
        }
        co_return co_await inner.write (std::move (request));
    }

    in_memory_location_store_t inner;
    std::string owner_id = "renewing-owner";
    bool renew_on_descriptor_write = false;
    bool renew_on_reclaim_write = false;
    unsigned reclaim_writes = 0;
};

TEST (ZLinkFrameworkOpaqueStoreProviders, DescriptorCommitAcceptsConcurrentLeaseRenewal)
{
    renew_before_conditional_commit_store_t store;
    provider_location_repository_t repository (store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claimed = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claimed);
    ASSERT_NE (owner, nullptr);
    const store_key_t lease_key{std::string ("owner-lease") + '\0' + store.owner_id};
    const auto before = std::get<store_found_t> (store.read (lease_key).result ().value ());
    ASSERT_TRUE (std::holds_alternative<owner_lease_renewed_t> (
      repository.renew_owner_lease (owner->token, 30s).result ().value ()));
    const auto after = std::get<store_found_t> (store.read (lease_key).result ().value ());
    EXPECT_EQ (before.value.bytes, after.value.bytes);
    EXPECT_NE (before.value.version.value, after.value.version.value);
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "renewal-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string ("renewal-node"));
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    store.renew_on_descriptor_write = true;
    const auto result = repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                          .result ()
                          .value ();
    EXPECT_EQ (result.status, location_write_status_t::stored);
    EXPECT_FALSE (store.renew_on_descriptor_write);
}

TEST (ZLinkFrameworkOpaqueStoreProviders, StaleReservationReclaimRechecksNewLeaseRenewal)
{
    renew_before_conditional_commit_store_t store;
    store.owner_id = "reclaim-source";
    provider_location_repository_t repository (store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_claim = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto target_claim =
      repository.claim_owner_lease ("reclaim-target", 30s).result ().value ();
    const auto *source = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source, nullptr);
    ASSERT_NE (target, nullptr);
    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "reclaim-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("reclaim-source-node", source->token);
    publish ("reclaim-target-node", target->token);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "reclaim-actor"};
    request.intent.stable_type = "player";
    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-source-node"), 1,
                      source->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto original = repository.reserve (request).result ().value ();
    ASSERT_NE (std::get_if<object_reserved_t> (&original), nullptr);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository.release_owner_lease (source->token).result ().value ()));
    const auto successor = repository.claim_owner_lease (store.owner_id, 30s).result ().value ();
    const auto *new_owner = std::get_if<owner_lease_claimed_t> (&successor);
    ASSERT_NE (new_owner, nullptr);
    ASSERT_NE (source->token.lease_generation, new_owner->token.lease_generation);

    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-target-node"), 1,
                      target->token};
    store.renew_on_reclaim_write = true;
    const auto reserved = repository.reserve (request).result ().value ();
    // Location runtime §6.1 fences the first batch with the observed lease version.
    // Renewing the successor lease keeps the original generation ended, so the
    // unchanged authority remains eligible for the second conditional batch.
    const auto *replacement = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (replacement, nullptr);
    EXPECT_GT (replacement->creating.object_generation,
               std::get<object_reserved_t> (original).creating.object_generation);
    EXPECT_EQ (
      std::get<authority_snapshot_t> (
        repository.read_authority (actor_authority_key (request.key.global_id)).result ().value ())
        .store_version,
      replacement->creating.store_version);
    const auto lease = repository.read_owner_lease (store.owner_id).result ().value ();
    const auto *live_successor = std::get_if<owner_lease_found_t> (&lease);
    ASSERT_NE (live_successor, nullptr);
    EXPECT_EQ (live_successor->token.lease_generation, new_owner->token.lease_generation);
    EXPECT_EQ (store.reclaim_writes, 2u);
    EXPECT_FALSE (store.renew_on_reclaim_write);
}

TEST (CppFrameworkOpaqueLocationStore, MeshNewClaimUsesStoredOwnerLeaseExactRead)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto expired_claim =
      repository.claim_owner_lease ("expired-descriptor-owner", 30s).result ().value ();
    const auto *expired_owner = std::get_if<owner_lease_claimed_t> (&expired_claim);
    ASSERT_NE (expired_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "descriptor-fence";
        value.rid = zlink::routing_id_t::from (std::move (rid));
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        return value;
    };
    ASSERT_EQ (repository
                 .update_mesh_node (descriptor ("expired", expired_owner->token),
                                    location_write_intent_t::fixed_rid_new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    repository.configure_reclaim_margin (5s);
    ASSERT_NE (std::get_if<owner_lease_released_t> (
                 &repository.release_owner_lease (expired_owner->token).result ().value ()),
               nullptr);
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      repository.read_owner_lease (expired_owner->token.owner_id).result ().value ()));

    const auto successor_claim =
      repository.claim_owner_lease ("successor-descriptor-owner", 30s).result ().value ();
    const auto *successor = std::get_if<owner_lease_claimed_t> (&successor_claim);
    ASSERT_NE (successor, nullptr);
    ASSERT_GT (successor->token.lease_generation, expired_owner->token.lease_generation);
    const auto waiting = repository
                           .update_mesh_node (descriptor ("expired", successor->token),
                                              location_write_intent_t::fixed_rid_new_claim)
                           .result ();
    ASSERT_TRUE (waiting);
    EXPECT_EQ (waiting.value ().status, location_write_status_t::waiting);
    clock.advance (15s - 1ms);
    const auto before_boundary = repository
                                   .update_mesh_node (descriptor ("expired", successor->token),
                                                      location_write_intent_t::fixed_rid_new_claim)
                                   .result ();
    ASSERT_TRUE (before_boundary);
    EXPECT_EQ (before_boundary.value ().status, location_write_status_t::waiting);
    clock.advance (1ms);
    EXPECT_EQ (repository
                 .update_mesh_node (descriptor ("expired", successor->token),
                                    location_write_intent_t::fixed_rid_new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    repository.configure_reclaim_margin (0ms);
    const auto live_claim =
      repository.claim_owner_lease ("live-descriptor-owner", 30s).result ().value ();
    const auto *live_owner = std::get_if<owner_lease_claimed_t> (&live_claim);
    ASSERT_NE (live_owner, nullptr);
    ASSERT_EQ (repository
                 .update_mesh_node (descriptor ("live", live_owner->token),
                                    location_write_intent_t::fixed_rid_new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto contender_claim =
      repository.claim_owner_lease ("contending-descriptor-owner", 30s).result ().value ();
    const auto *contender = std::get_if<owner_lease_claimed_t> (&contender_claim);
    ASSERT_NE (contender, nullptr);
    EXPECT_EQ (repository
                 .update_mesh_node (descriptor ("live", contender->token),
                                    location_write_intent_t::fixed_rid_new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::rejected_conflict);
}

TEST (CppFrameworkOpaqueLocationStore, ExpiredOwnerLeaseReclaimsReservedAuthority)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    source.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_claim = source.claim_owner_lease ("expired-source", 30s).result ().value ();
    const auto target_claim = source.claim_owner_lease ("expired-target", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "expired-mesh";
        value.rid = zlink::routing_id_t::from (rid);
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        value.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false,
           0}};
        value.capacity.actors.limit = 1;
        value.capacity.spots.limit = 1;
        value.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
        return value;
    };
    const auto source_descriptor = descriptor ("expired-source-node", source_owner->token);
    const auto target_descriptor = descriptor ("expired-target-node", target_owner->token);
    ASSERT_EQ (source.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_EQ (source.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    owner_lease_time_store_t expired (provider, source_owner->token.owner_id,
                                      owner_lease_time_store_t::lease_view_t::expired);
    provider_location_repository_t replacement (expired);
    replacement.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    for (const auto kind : {placement_object_kind_t::actor, placement_object_kind_t::user_spot}) {
        const auto stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
        object_reserve_request_t request;
        request.key = {kind,
                       kind == placement_object_kind_t::actor ? "expired-actor" : "expired-spot"};
        request.intent.stable_type = stable_type;
        request.target = {"expired-mesh", node_rid_t::from_string ("expired-source-node"), 1,
                          source_owner->token};
        request.creating_payload = bytes ("creating");
        request.capacity_bundle =
          kind == placement_object_kind_t::actor
            ? placement_capacity_bundle_t{.actor_slots = 1}
            : placement_capacity_bundle_t{
                .spot_slots = 1, .spot_type = spot_type_capacity_delta_t{kind, stable_type, 1}};
        const auto original = source.reserve (request).result ().value ();
        ASSERT_NE (std::get_if<object_reserved_t> (&original), nullptr);

        request.target = {"expired-mesh", node_rid_t::from_string ("expired-target-node"), 1,
                          target_owner->token};
        const auto reserved = replacement.reserve (request).result ().value ();
        const auto *reclaimed = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reclaimed, nullptr);
        EXPECT_EQ (reclaimed->creating.owner.owner_id, target_owner->token.owner_id);
        EXPECT_GT (reclaimed->creating.object_generation, 1u);
    }
}

TEST (CppFrameworkOpaqueLocationStore, LiveOwnerLeaseProtectsReservedAuthority)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    source.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_claim = source.claim_owner_lease ("live-source", 30s).result ().value ();
    const auto target_claim = source.claim_owner_lease ("live-target", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "live-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (source.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("live-source-node", source_owner->token);
    publish ("live-target-node", target_owner->token);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "live-actor"};
    request.intent.stable_type = "player";
    request.target = {"live-mesh", node_rid_t::from_string ("live-source-node"), 1,
                      source_owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto first = source.reserve (request).result ().value ();
    const auto *original = std::get_if<object_reserved_t> (&first);
    ASSERT_NE (original, nullptr);

    owner_lease_time_store_t live (provider, source_owner->token.owner_id,
                                   owner_lease_time_store_t::lease_view_t::live);
    provider_location_repository_t replacement (live);
    replacement.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    request.target = {"live-mesh", node_rid_t::from_string ("live-target-node"), 1,
                      target_owner->token};
    EXPECT_TRUE (std::holds_alternative<object_reserve_conflict_t> (
      replacement.reserve (request).result ().value ()));
    const auto current = std::get<authority_snapshot_t> (
      replacement.read_authority (actor_authority_key ("live-actor")).result ().value ());
    EXPECT_EQ (current.store_version, original->creating.store_version);
    EXPECT_EQ (current.owner.owner_id, source_owner->token.owner_id);
}

TEST (CppFrameworkOpaqueLocationStore, MissingOwnerLeaseExpiryFailsClosedDuringReclaim)
{
    in_memory_location_store_t provider;
    provider_location_repository_t source (provider);
    source.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim = source.claim_owner_lease ("corrupt-owner", 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (owner, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "corrupt-mesh";
    descriptor.rid = zlink::routing_id_t::from ("corrupt-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (source.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "corrupt-actor"};
    request.intent.stable_type = "player";
    request.target = {"corrupt-mesh", node_rid_t::from_string ("corrupt-node"), 1, owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = source.reserve (request).result ().value ();
    ASSERT_NE (std::get_if<object_reserved_t> (&reserved), nullptr);

    owner_lease_time_store_t corrupt (provider, owner->token.owner_id,
                                      owner_lease_time_store_t::lease_view_t::missing_expiry);
    provider_location_repository_t reopened (corrupt);
    reopened.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto failure = reopened.reserve (request).result ();
    ASSERT_FALSE (failure);
    EXPECT_EQ (failure.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (failure.error (), nullptr);
    EXPECT_STREQ (failure.error ()->what (), "Location Store owner lease record is invalid");
}
}
