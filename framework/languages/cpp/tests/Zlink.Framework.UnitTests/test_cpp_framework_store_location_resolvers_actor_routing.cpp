/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../support/store_location_resolver_repository_fixture.hpp"

namespace
{
using namespace zlink::framework::test::store_location_resolvers;

using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_object_entry_t;
using zlink::framework::location_object_filter_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_page_t;
using zlink::framework::location_role_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_resolvers_t;
using zlink::framework::runtime::store_location_runtime_query_t;

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesSpotAddressByGlobalIdAcrossMeshes)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "1",
      .payload = user_spot_authority_payload ("spot-b", 41),
      .object_generation = 41,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                     .object_kind = zlink::framework::placement_object_kind_t::user_spot,
                     .stable_type = "play",
                     .target = {.mesh_name = "mesh-b",
                                .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                .node_lifecycle_generation = 1,
                                .owner = owner}}};
    store.set_authority ("zla1:s:6:spot-b", authority);
    location_options_t options;
    options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t resolvers (store, options);

    /* No mesh name in the lookup: the rid is searched across every mesh. */
    const auto initial = resolvers.resolve_spot_address ({}, "spot-b").result ().value ();
    ASSERT_TRUE (initial.has_value ());
    EXPECT_EQ ("spot-b", initial->spot_id);
    EXPECT_EQ (41u, initial->spot_generation);

    authority.store_version = "2";
    authority.payload = user_spot_authority_payload ("spot-b", 42);
    authority.object_generation = 42;
    store.set_authority ("zla1:s:6:spot-b", authority);
    const auto renewed = resolvers.resolve_spot_address ({}, "spot-b").result ().value ();
    ASSERT_TRUE (renewed.has_value ());
    EXPECT_EQ (42u, renewed->spot_generation);

    const auto missing = resolvers.resolve_spot_address ({}, "missing").result ().value ();
    EXPECT_FALSE (missing.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesActorAddress)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    store.set_authority (
      "zla1:a:7:actor-b",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = zlink::framework::runtime::encode_actor_authority_payload (
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("node-a"), "player", "actor-b", 1),
          "spot-b", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::actor,
                       .stable_type = "player",
                       .target = {.mesh_name = "mesh-b",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});
    store_location_resolvers_t resolvers (
      store, {}, std::make_shared<zlink::framework::runtime::actor_location_observer_t> (),
      "mesh-b");

    const auto address = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (address.has_value ());
    EXPECT_EQ ("spot-b", address->spot_id);

    const auto cached = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (cached.has_value ());
    EXPECT_EQ (1u, store.resolve_actor_count.load ());
    EXPECT_FALSE (resolvers.invalidate_actor_address_if_matches (
      "actor-b",
      zlink::framework::runtime::spot_address_t{
        cached->mesh_name, cached->node_rid, cached->spot_id, cached->spot_generation,
        cached->store_version, cached->object_generation, cached->authority_owner_generation,
        cached->owner, cached->node_generation + 1}));
    auto wire_fence = *cached;
    wire_fence.owner.owner_id.clear ();
    EXPECT_TRUE (resolvers.invalidate_actor_address_if_matches ("actor-b", wire_fence));
    const auto after_wire_invalidation =
      resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (after_wire_invalidation.has_value ());
    EXPECT_EQ (2u, store.resolve_actor_count.load ());
    EXPECT_TRUE (
      resolvers.invalidate_actor_address_if_matches ("actor-b", *after_wire_invalidation));
    const auto reloaded = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (reloaded.has_value ());
    EXPECT_EQ (cached->object_generation, reloaded->object_generation);
    EXPECT_EQ (3u, store.resolve_actor_count.load ());

    /* Destroy removes the authority, so a cached route must be discarded even
     * when the replacement incarnation can reuse the same generation. */
    resolvers.invalidate_actor_address ("actor-b");
    const auto after_destroy = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (after_destroy.has_value ());
    EXPECT_EQ (4u, store.resolve_actor_count.load ());

    const auto missing = resolvers.resolve_actor_address ("nobody").result ().value ();
    EXPECT_FALSE (missing.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, SessionBindRefreshesFenceWhileNormalRoutingKeepsCache)
{
    using namespace zlink::framework;
    test_location_repository_t store;
    const auto old_owner = claim_test_owner (store, "old-bind-owner");
    const auto old_actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("old-bind-node"), "player", "bind-actor", 7, "bind-mesh");
    authority_snapshot_t authority{
      .store_version = "1",
      .payload = runtime::encode_actor_authority_payload (old_actor, "bind-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 11,
      .owner = old_owner,
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "bind-mesh",
                                .node_rid = old_actor.node_rid (),
                                .node_lifecycle_generation = 13,
                                .owner = old_owner}}};
    store.set_authority ("zla1:a:10:bind-actor", authority);
    store_location_resolvers_t resolvers (store);
    const auto cached = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (cached);
    const auto authority_reads = store.authority_read_count.load ();
    const auto owner_reads = store.owner_read_count.load ();
    const auto normal_cached = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (normal_cached);
    EXPECT_EQ (cached->authority_owner_generation, normal_cached->authority_owner_generation);
    EXPECT_EQ (authority_reads, store.authority_read_count.load ());
    EXPECT_EQ (owner_reads, store.owner_read_count.load ());
    const auto cached_binding =
      resolvers.resolve_actor_session_binding_address (old_actor).result ().value ();
    ASSERT_TRUE (cached_binding);
    EXPECT_EQ (cached->node_rid, cached_binding->node_rid);
    EXPECT_EQ (cached->object_generation, cached_binding->object_generation);
    EXPECT_EQ (cached->mesh_name, cached_binding->mesh_name);
    EXPECT_EQ (cached->node_generation, cached_binding->node_generation);
    EXPECT_EQ (cached->authority_owner_generation, cached_binding->authority_owner_generation);
    EXPECT_EQ (cached->owner.owner_id, cached_binding->owner.owner_id);
    EXPECT_EQ (cached->owner.lease_generation, cached_binding->owner.lease_generation);
    EXPECT_EQ (authority_reads + 1, store.authority_read_count.load ());
    EXPECT_GT (store.owner_read_count.load (), owner_reads);
    const auto refreshed_authority_reads = store.authority_read_count.load ();
    const auto refreshed_owner_reads = store.owner_read_count.load ();
    const auto normal_after_bind =
      resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (normal_after_bind);
    EXPECT_EQ (cached_binding->authority_owner_generation,
               normal_after_bind->authority_owner_generation);
    EXPECT_EQ (refreshed_authority_reads, store.authority_read_count.load ());
    EXPECT_EQ (refreshed_owner_reads, store.owner_read_count.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, SessionBindUsesCallerRefAfterRelocation)
{
    using namespace zlink::framework;
    test_location_repository_t store;
    const auto old_owner = claim_test_owner (store, "old-bind-owner");
    const auto new_owner = claim_test_owner (store, "new-bind-owner");
    const auto old_actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("old-bind-node"), "player", "bind-actor", 7);
    const auto new_actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("new-bind-node"), "player", "bind-actor", 7);
    authority_snapshot_t authority{
      .store_version = "1",
      .payload = runtime::encode_actor_authority_payload (old_actor, "bind-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 11,
      .owner = old_owner,
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "bind-mesh",
                                .node_rid = old_actor.node_rid (),
                                .node_lifecycle_generation = 13,
                                .owner = old_owner}}};
    store.set_authority ("zla1:a:10:bind-actor", authority);
    store_location_resolvers_t resolvers (store);
    const auto cached = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (cached);
    authority.store_version = "2";
    authority.payload = runtime::encode_actor_authority_payload (new_actor, "bind-spot", 2);
    authority.authority_owner_generation = 17;
    authority.owner = new_owner;
    authority.allocation.target.node_rid = new_actor.node_rid ();
    authority.allocation.target.node_lifecycle_generation = 19;
    authority.allocation.target.owner = new_owner;
    store.set_authority ("zla1:a:10:bind-actor", authority);
    const auto binding =
      resolvers.resolve_actor_session_binding_address (new_actor).result ().value ();
    ASSERT_TRUE (binding);
    EXPECT_EQ ("new-bind-node", binding->node_rid.to_string ());
    EXPECT_EQ (19u, binding->node_generation);
    EXPECT_EQ (17u, binding->authority_owner_generation);
    EXPECT_EQ (new_owner.owner_id, binding->owner.owner_id);
    EXPECT_EQ (new_owner.lease_generation, binding->owner.lease_generation);
    const auto normal = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (normal);
    EXPECT_EQ (binding->node_rid, normal->node_rid);
    EXPECT_EQ (binding->node_generation, normal->node_generation);
    EXPECT_EQ (binding->object_generation, normal->object_generation);
    EXPECT_EQ (binding->authority_owner_generation, normal->authority_owner_generation);
    EXPECT_EQ (binding->owner.owner_id, normal->owner.owner_id);
    EXPECT_EQ (binding->owner.lease_generation, normal->owner.lease_generation);
    EXPECT_FALSE (resolvers.resolve_actor_session_binding_address (old_actor).result ().value ());
    const auto other_incarnation =
      detail::actor_ref_access_t::make (new_actor.node_rid (), "player", "bind-actor", 8);
    try {
        (void) resolvers.resolve_actor_session_binding_address (other_incarnation)
          .result ()
          .value ();
        FAIL () << "a different ObjectGeneration must end InvalidOperation";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (framework_error_kind_t::invalid_operation, error.kind ());
    }
    const auto other_mesh = detail::actor_ref_access_t::make (new_actor.node_rid (), "player",
                                                              "bind-actor", 7, "other-mesh");
    EXPECT_FALSE (resolvers.resolve_actor_session_binding_address (other_mesh).result ().value ());
    const auto exact_mesh = detail::actor_ref_access_t::make (new_actor.node_rid (), "player",
                                                              "bind-actor", 7, "bind-mesh");
    EXPECT_TRUE (resolvers.resolve_actor_session_binding_address (exact_mesh).result ().value ());
    const auto missing_actor =
      detail::actor_ref_access_t::make (new_actor.node_rid (), "player", "missing-actor", 7);
    const auto missing = resolvers.resolve_actor_session_binding_address (missing_actor).result ();
    ASSERT_FALSE (missing.has_value ());
    EXPECT_EQ (framework_error_kind_t::not_found, missing.error_kind ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, SessionBindRefreshesAuthorityAfterReturningToSameNode)
{
    using namespace zlink::framework;
    test_location_repository_t store;
    const auto old_owner = claim_test_owner (store, "old-bind-owner");
    const auto new_owner = old_owner;
    const auto old_actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("old-bind-node"), "player", "bind-actor", 7);
    const auto new_actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("old-bind-node"), "player", "bind-actor", 7);
    authority_snapshot_t authority{
      .store_version = "1",
      .payload = runtime::encode_actor_authority_payload (old_actor, "bind-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 11,
      .owner = old_owner,
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "bind-mesh",
                                .node_rid = old_actor.node_rid (),
                                .node_lifecycle_generation = 13,
                                .owner = old_owner}}};
    store.set_authority ("zla1:a:10:bind-actor", authority);
    store_location_resolvers_t resolvers (store);
    const auto cached = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (cached);
    authority.store_version = "2";
    authority.payload = runtime::encode_actor_authority_payload (new_actor, "bind-spot", 2);
    authority.authority_owner_generation = 17;
    authority.owner = new_owner;
    authority.allocation.target.node_rid = new_actor.node_rid ();
    authority.allocation.target.node_lifecycle_generation = 13;
    authority.allocation.target.owner = new_owner;
    store.set_authority ("zla1:a:10:bind-actor", authority);
    const auto binding =
      resolvers.resolve_actor_session_binding_address (new_actor).result ().value ();
    ASSERT_TRUE (binding);
    EXPECT_EQ ("old-bind-node", binding->node_rid.to_string ());
    EXPECT_EQ (13u, binding->node_generation);
    EXPECT_EQ (17u, binding->authority_owner_generation);
    EXPECT_EQ (new_owner.owner_id, binding->owner.owner_id);
    EXPECT_EQ (new_owner.lease_generation, binding->owner.lease_generation);
    const auto normal = resolvers.resolve_actor_address ("bind-actor").result ().value ();
    ASSERT_TRUE (normal);
    EXPECT_EQ (binding->node_rid, normal->node_rid);
    EXPECT_EQ (binding->node_generation, normal->node_generation);
    EXPECT_EQ (binding->object_generation, normal->object_generation);
    EXPECT_EQ (binding->authority_owner_generation, normal->authority_owner_generation);
    EXPECT_EQ (binding->owner.owner_id, normal->owner.owner_id);
    EXPECT_EQ (binding->owner.lease_generation, normal->owner.lease_generation);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ReadyAuthorityWithMismatchedOwnerGenerationIsUnavailable)
{
    test_location_repository_t store;
    const auto current_owner = claim_test_owner (store, "owner-generation-mismatch");
    const location_owner_token_t stale_owner{current_owner.owner_id,
                                             current_owner.lease_generation + 1};
    store.set_authority (
      "zla1:s:15:mismatched-spot",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("mismatched-spot", 1, stale_owner.owner_id,
                                                stale_owner.lease_generation),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = stale_owner,
        .allocation = {
          .state = zlink::framework::placement_allocation_state_t::active,
          .object_kind = zlink::framework::placement_object_kind_t::user_spot,
          .stable_type = "play",
          .target = {.mesh_name = "mesh-mismatch",
                     .node_rid = zlink::framework::node_rid_t::from_string ("node-mismatch"),
                     .node_lifecycle_generation = 1,
                     .owner = stale_owner}}});

    store_location_resolvers_t resolvers (store);
    try {
        (void) resolvers.resolve_spot_address ({}, "mismatched-spot").result ().value ();
        FAIL () << "an authority owner generation mismatch must be unavailable";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, error.kind ());
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, ReadyAuthorityWithMissingOwnerIsUnavailable)
{
    test_location_repository_t store;
    const location_owner_token_t missing_owner{"owner-missing", 1};
    store.set_authority (
      "zla1:s:12:missing-spot",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("missing-spot", 1, missing_owner.owner_id,
                                                missing_owner.lease_generation),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = missing_owner,
        .allocation = {
          .state = zlink::framework::placement_allocation_state_t::active,
          .object_kind = zlink::framework::placement_object_kind_t::user_spot,
          .stable_type = "play",
          .target = {.mesh_name = "mesh-missing",
                     .node_rid = zlink::framework::node_rid_t::from_string ("node-missing"),
                     .node_lifecycle_generation = 1,
                     .owner = missing_owner}}});

    store_location_resolvers_t resolvers (store);
    try {
        (void) resolvers.resolve_spot_address ({}, "missing-spot").result ().value ();
        FAIL () << "an authority with a missing owner lease must be unavailable";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, error.kind ());
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, ReadyAuthorityWithExpiredOwnerIsUnavailable)
{
    test_location_repository_t store;
    const auto owner =
      claim_test_owner (store, "owner-expired-authority", std::chrono::milliseconds (20));
    store.set_authority (
      "zla1:s:12:expired-spot",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("expired-spot", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {
          .state = zlink::framework::placement_allocation_state_t::active,
          .object_kind = zlink::framework::placement_object_kind_t::instance_spot,
          .stable_type = "play",
          .target = {.mesh_name = "mesh-expired",
                     .node_rid = zlink::framework::node_rid_t::from_string ("node-expired"),
                     .node_lifecycle_generation = 1,
                     .owner = owner}}});
    std::this_thread::sleep_for (std::chrono::milliseconds (30));

    store_location_resolvers_t resolvers (store);
    try {
        (void) resolvers.resolve_spot_address ({}, "expired-spot").result ().value ();
        FAIL () << "an expired authority owner must be unavailable";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, error.kind ());
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, RejectsMalformedActorAuthorityPayload)
{
    test_location_repository_t store;
    location_options_t options;
    options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t resolvers (store, options);
    (void) claim_test_owner (store, "owner");
    const auto owner = live_owner_token (store, "owner");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "1",
      .payload = {std::byte{1}},
      .object_generation = 1,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                     .object_kind = zlink::framework::placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "mesh",
                                .node_rid = zlink::framework::node_rid_t::from_string ("node"),
                                .node_lifecycle_generation = 1,
                                .owner = owner}}};
    store.set_authority ("zla1:a:14:actor-observed", authority);
    EXPECT_FALSE (resolvers.resolve_actor_address ("actor-observed").result ().value ());
    authority.payload = zlink::framework::runtime::encode_actor_authority_payload (
      zlink::framework::detail::actor_ref_access_t::make (
        zlink::framework::node_rid_t::from_string ("node"), "player", "actor-observed", 3),
      "spot-observed", 2);
    store.set_authority ("zla1:a:14:actor-observed", authority);
    const auto committed = resolvers.resolve_actor_address ("actor-observed").result ().value ();
    ASSERT_TRUE (committed);
    EXPECT_EQ ("node", committed->node_rid.to_string ());
    EXPECT_EQ ("spot-observed", committed->spot_id);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolverFiltersExpiredMeshNodeDescriptors)
{
    in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    seed_mesh_node (store, "owner-live", "mesh-a", "node-live", "tcp://127.0.0.1:7001");
    const auto expired = claim_test_owner (store, "owner-expired", std::chrono::milliseconds (20));
    ASSERT_EQ (location_write_status_t::stored,
               store
                 .update_mesh_node (
                   zlink::framework::mesh_node_descriptor_t{
                     .mesh_name = "mesh-a",
                     .rid = zlink::routing_id_t::from ("node-expired"),
                     .lifecycle_generation = 1,
                     .descriptor_revision = 1,
                     .endpoint = "tcp://127.0.0.1:7002",
                     .application_version = 1,
                     .object_role = zlink::framework::object_role_t::server,
                     .state = zlink::framework::framework_runtime_state_t::serving,
                     .security_identity = "test",
                     .owner_id = expired.owner_id,
                     .lease_generation = expired.lease_generation},
                   location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    std::this_thread::sleep_for (std::chrono::milliseconds (25));

    const auto raw_rows = store.list_mesh_nodes ("mesh-a").result ().value ().items;
    ASSERT_EQ (2u, raw_rows.size ());

    location_options_t options;
    options.polling_interval = std::chrono::milliseconds::zero ();
    zlink::framework::runtime::live_location_reader_t live_reader (store, options);
    const auto live_rows = live_reader.list_mesh_nodes ("mesh-a").result ().value ().items;
    ASSERT_EQ (1u, live_rows.size ());
    EXPECT_EQ ("node-live", live_rows.front ().rid.to_string ());

    store_location_resolvers_t resolvers (live_reader);
    const auto resolver_rows = resolvers.list_live_mesh_nodes ("mesh-a").result ().value ();

    ASSERT_EQ (1u, resolver_rows.size ());
    EXPECT_EQ ("node-live", resolver_rows.front ().rid.to_string ());

    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (live_reader, runtime, options);
    const auto query_rows =
      query.list_topology (location_topology_filter_t{.mesh_name = "mesh-a"}).result ().value ();

    ASSERT_EQ (1u, query_rows.items.size ());
}
} // namespace
