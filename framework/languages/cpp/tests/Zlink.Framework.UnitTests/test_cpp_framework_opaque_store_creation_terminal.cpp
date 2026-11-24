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

#include "../support/opaque_store_creation_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using zlink::framework::tests::owner_lease_time_store_t;
using namespace zlink::framework::test::opaque_store;

enum class completion_kind_t
{
    created,
    rejected,
    failed
};

TEST (ZLinkFrameworkOpaqueStoreProviders,
      CreationReleaseRequalifiesUnchangedAuthorityAfterCapacityConflict)
{
    creation_terminal_failure_store_t store;
    provider_location_repository_t repository (store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("release-conflict-owner", 60s).result ().value ())
        .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "release-conflict";
    descriptor.rid = zlink::routing_id_t::from ("release-conflict-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.security_identity = "release-conflict-node";
    descriptor.entry_spot_id = "release-conflict-entry";
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::disabled, false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::user_spot, "release-conflict-room"};
    request.intent.stable_type = "room";
    request.target = {descriptor.mesh_name, node_rid_t::from_string (descriptor.rid.to_string ()),
                      1, owner};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
    request.relocation_policy = detail::factory_relocation_kind_t::disabled;
    const auto reserved =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    const auto ready = encode_user_spot_authority_payload (user_spot_authority_payload_t{
      .state = user_spot_authority_state_t::ready,
      .stable_type = "room",
      .spot_id = request.key.global_id,
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = descriptor.mesh_name,
      .node_rid = request.target.node_rid,
      .node_generation = 1});
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      repository.commit ({request.key, reserved.fence, ready}).result ().value ()));
    repository.configure_reclaim_margin (5s);
    const auto key = spot_authority_key (request.key.global_id);
    const auto before =
      std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ());
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository.release_owner_lease (owner).result ().value ()));
    const auto successor = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease (owner.owner_id, 60s).result ().value ())
                             .token;
    ASSERT_NE (successor.lease_generation, owner.lease_generation);
    ASSERT_EQ (capacity_record (store, descriptor).at ("active").at ("spots"), 1);
    const auto writes_before_wait = store.creation_writes.size ();
    const auto waiting =
      repository.release_ended_reservation (key, before.store_version, {}, request).result ();
    ASSERT_FALSE (waiting);
    EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
    EXPECT_EQ (store.creation_writes.size (), writes_before_wait);
    clock.advance (15s);
    store.conflicting_capacity_key = capacity_key (descriptor, "spots", "active");
    store.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    const auto writes_before = store.creation_writes.size ();
    request.operation_deadline = std::chrono::system_clock::now () + 5s;
    const auto released =
      repository.release_ended_reservation (key, before.store_version, {}, request).result ();
    ASSERT_TRUE (released);
    EXPECT_EQ (store.fault, creation_terminal_failure_store_t::fault_t::none);
    EXPECT_TRUE (released.value ());
    EXPECT_EQ (store.creation_writes.size (), writes_before + 2);
    const auto current = repository.read_authority (key).result ();
    ASSERT_TRUE (current);
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (current.value ()));
    EXPECT_EQ (capacity_record (store, descriptor).at ("active").at ("spots"), 0);
}

class CreationTerminalTest : public ::testing::TestWithParam<completion_kind_t>
{
  protected:
    void SetUp () override
    {
        repository.configure_reclaim_margin (0ms);
        const auto lease = repository.claim_owner_lease ("terminal-owner", 30s).result ().value ();
        const auto *owner = std::get_if<owner_lease_claimed_t> (&lease);
        ASSERT_NE (owner, nullptr);
        descriptor.mesh_name = "terminal-mesh";
        descriptor.rid = zlink::routing_id_t::from (std::string{"terminal-target"});
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner->token.owner_id;
        descriptor.lease_generation = owner->token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 1;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "player",
                                                   maintenance_policy_kind_t::disabled, false, 1});
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "player", {0, 0, 1}});

        provider.conflicting_capacity_key = capacity_key (descriptor, "actors", "pending");
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        reserve_request.key = {placement_object_kind_t::actor, "terminal-actor"};
        reserve_request.intent.stable_type = "player";
        reserve_request.target = {"terminal-mesh", node_rid_t::from_string ("terminal-target"), 1,
                                  owner->token};
        reserve_request.creating_payload = bytes ("creating");
        reserve_request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (reserve_request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reservation, nullptr);
        fence = reservation->fence;
        publication.operation = {
          node_rid_t::from_string (std::string{"\0\xff\x10", 3}), 7, {1, 0xabcdef}};
        publication.terminal_envelope =
          bytes (GetParam () == completion_kind_t::created
                   ? "created-terminal"
                   : (GetParam () == completion_kind_t::rejected ? "rejected-terminal"
                                                                 : "failed-terminal"));
        publication.operation_deadline = std::chrono::time_point_cast<std::chrono::milliseconds> (
                                           std::chrono::system_clock::now ())
                                         + 2s;
    }

    object_complete_creation_request_t request () const
    {
        object_creation_completion_t completion;
        switch (GetParam ()) {
            case completion_kind_t::created:
                completion = object_creation_completed_t{bytes ("ready"), publication};
                break;
            case completion_kind_t::rejected:
                completion = object_creation_rejected_t{publication};
                break;
            case completion_kind_t::failed:
                completion = object_creation_failed_t{publication};
                break;
        }
        return {reserve_request.key, fence, std::move (completion)};
    }

    void expect_published_terminal_and_replay ()
    {
        ASSERT_EQ (provider.writes, 1u);
        ASSERT_TRUE (provider.attempted);
        const std::string terminal_key = std::string ("creation-terminal") + '\0' + "00ff10" + '\0'
                                         + "7" + '\0' + "00000000000000010000000000abcdef";
        bool authority = false, capacity = false, terminal_condition = false;
        const store_put_t *terminal = nullptr;
        unsigned terminal_mutations = 0;
        for (const auto &mutation : provider.attempted->mutations) {
            const auto &key =
              std::visit ([] (const auto &value) -> const store_key_t & { return value.key; },
                          mutation)
                .value;
            authority |= key.starts_with (std::string ("authority") + '\0');
            capacity |= key.starts_with (capacity_domain);
            if (key.starts_with (std::string ("creation-terminal") + '\0')) {
                ++terminal_mutations;
                if (const auto *put = std::get_if<store_put_t> (&mutation);
                    put && key == terminal_key)
                    terminal = put;
            }
        }
        for (const auto &condition : provider.attempted->conditions)
            if (const auto *missing = std::get_if<store_missing_condition_t> (&condition))
                terminal_condition |= missing->key.value == terminal_key;
        ASSERT_NE (terminal, nullptr);
        EXPECT_EQ (terminal_mutations, 1u);
        EXPECT_EQ (terminal->bytes, publication.terminal_envelope);
        ASSERT_TRUE (provider.terminal_read_store_now);
        ASSERT_TRUE (terminal->retention);
        const auto target_expiry = std::chrono::time_point_cast<std::chrono::milliseconds> (
          publication.operation_deadline + 5min);
        EXPECT_EQ (*terminal->retention, std::chrono::ceil<std::chrono::milliseconds> (
                                           target_expiry - *provider.terminal_read_store_now));
        EXPECT_TRUE (authority && capacity && terminal_condition);
        const auto raw = provider.inner.read ({terminal_key}).result ().value ();
        const auto *raw_terminal = std::get_if<store_found_t> (&raw);
        ASSERT_NE (raw_terminal, nullptr);
        EXPECT_EQ (raw_terminal->value.bytes, publication.terminal_envelope);
        ASSERT_TRUE (raw_terminal->value.expires_at);
        provider_location_repository_t reopened (provider);
        reopened.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        const auto stored =
          reopened.read_creation_terminal (publication.operation).result ().value ();
        ASSERT_TRUE (stored);
        EXPECT_EQ (stored->terminal_envelope, publication.terminal_envelope);
        ASSERT_TRUE (provider.terminal_write_store_now);
        // Location runtime §7 retains through the target instant. Location Store §3
        // rounds retention up to milliseconds; the provider's later StoreNow
        // contributes only the read-to-write interval and less than 1 ms.
        EXPECT_GE (stored->expires_at, target_expiry);
        EXPECT_LT (stored->expires_at - target_expiry,
                   *provider.terminal_write_store_now - *provider.terminal_read_store_now + 1ms);
        const auto authority_result =
          reopened.read_authority (actor_authority_key (reserve_request.key.global_id))
            .result ()
            .value ();
        const auto counts = capacity_record (provider, descriptor);
        EXPECT_EQ (counts.at ("pending").at ("actors"), 0);
        if (GetParam () == completion_kind_t::created) {
            const auto *ready = std::get_if<authority_snapshot_t> (&authority_result);
            ASSERT_NE (ready, nullptr);
            EXPECT_EQ (ready->allocation.state, placement_allocation_state_t::active);
            EXPECT_EQ (ready->payload, bytes ("ready"));
            EXPECT_FALSE (ready->pending_creation);
            EXPECT_EQ (counts.at ("active").at ("actors"), 1);
        } else {
            EXPECT_TRUE (std::holds_alternative<authority_missing_t> (authority_result));
            EXPECT_EQ (counts.at ("active").at ("actors"), 0);
        }
        // Replays cannot refresh retention or borrow another source's terminal.
        publication.operation_deadline += 1min;
        const auto replay = reopened.complete_creation (request ()).result ().value ();
        const auto *retained = std::get_if<object_creation_already_completed_result_t> (&replay);
        ASSERT_NE (retained, nullptr);
        EXPECT_EQ (retained->terminal.terminal_envelope, stored->terminal_envelope);
        EXPECT_EQ (retained->terminal.expires_at, stored->expires_at);
        EXPECT_EQ (provider.writes, 1u);
        for (const int changed : {0, 1, 2}) {
            auto other = publication.operation;
            if (changed == 0)
                other.source_node_rid = node_rid_t::from_string ("another-source");
            if (changed == 1)
                ++other.source_node_generation;
            if (changed == 2)
                ++other.operation_id.low;
            EXPECT_FALSE (reopened.read_creation_terminal (other).result ().value ());
        }
    }

    void prepare_user_spot ();
    void verify_creation_conditions (bool user_spot);
    void verify_creation_descriptor_conflict (bool user_spot);

    creation_terminal_failure_store_t provider;
    provider_location_repository_t repository{provider};
    mesh_node_descriptor_t descriptor;
    object_reserve_request_t reserve_request;
    object_reservation_fence_t fence;
    creation_terminal_publication_t publication;
};

void CreationTerminalTest::prepare_user_spot ()
{
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({reserve_request.key, fence}).result ().value ()));
    const auto kind = placement_object_kind_t::user_spot;
    reserve_request.key.kind = kind;
    reserve_request.capacity_bundle = {0, 1, spot_type_capacity_delta_t{kind, "player", 1}};
    const auto reserved = repository.reserve (reserve_request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
    fence = std::get<object_reserved_t> (reserved).fence;
}

void CreationTerminalTest::verify_creation_conditions (bool user_spot)
{
    if (user_spot)
        prepare_user_spot ();

    const store_key_t descriptor_key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                                     + descriptor.rid.to_hex ()};
    const auto descriptor_read =
      std::get<store_found_t> (provider.inner.read (descriptor_key).result ().value ());
    for (int transition = 0; transition < 3; ++transition) {
        if (transition == 0) {
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
            provider.creation_writes.clear ();
            const auto reserved = repository.reserve (reserve_request).result ().value ();
            ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
            fence = std::get<object_reserved_t> (reserved).fence;
        } else if (transition == 1) {
            provider.creation_writes.clear ();
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        } else {
            reserve_request.key.global_id = "abort-actor";
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
            provider.creation_writes.clear ();
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository
                .commit ({reserve_request.key, fence, bytes ("ready")}, {},
                         publication.operation_deadline)
                .result ()
                .value ()));
        }
        ASSERT_EQ (provider.creation_writes.size (), 1u);
        const auto &conditions = provider.creation_writes.front ().conditions;
        const auto limited_dimensions = user_spot ? 2u : 1u;
        const auto sums = static_cast<unsigned> (
          std::count_if (conditions.begin (), conditions.end (), [] (const auto &condition) {
              return std::holds_alternative<store_sum_at_most_condition_t> (condition);
          }));
        EXPECT_EQ (conditions.size (), transition == 0 ? 5u + limited_dimensions : 3u);
        EXPECT_EQ (sums, transition == 0 ? limited_dimensions : 0u);
        EXPECT_TRUE (std::any_of (conditions.begin (), conditions.end (), [&] (const auto &c) {
            const auto *v = std::get_if<store_value_condition_t> (&c);
            return v && v->key.value == std::string ("owner-lease") + '\0' + descriptor.owner_id;
        }));
        EXPECT_TRUE (std::any_of (conditions.begin (), conditions.end (), [&] (const auto &c) {
            const auto *v = std::get_if<store_version_condition_t> (&c);
            return v && v->key.value == descriptor_key.value
                   && v->expected.value == descriptor_read.value.version.value;
        }));
    }
}

void CreationTerminalTest::verify_creation_descriptor_conflict (bool user_spot)
{
    if (user_spot)
        prepare_user_spot ();

    reserve_request.operation_deadline = publication.operation_deadline;
    const store_key_t descriptor_key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                                     + descriptor.rid.to_hex ()};
    for (int transition = 0; transition < 3; ++transition) {
        if (transition == 0)
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        if (transition == 2) {
            reserve_request.key.global_id = "abort-actor";
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
        }
        provider.creation_writes.clear ();
        provider.before_next_write = [&] () -> task_t<void> {
            const auto read = co_await provider.inner.read (descriptor_key);
            store_write_request_t republish;
            republish.mutations.emplace_back (store_put_t{
              descriptor_key, std::get<store_found_t> (read).value.bytes, std::nullopt});
            co_await provider.inner.write (std::move (republish));
        };
        if (transition == 0)
            fence =
              std::get<object_reserved_t> (repository.reserve (reserve_request).result ().value ())
                .fence;
        else if (transition == 1)
            ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({reserve_request.key, fence}, {}, publication.operation_deadline)
                .result ()
                .value ()));
        else
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository
                .commit ({reserve_request.key, fence, bytes ("ready")}, {},
                         publication.operation_deadline)
                .result ()
                .value ()));
        EXPECT_EQ (provider.creation_writes.size (), 2u);
    }
}

TEST_P (CreationTerminalTest, CapacityKeyUsesSharedUtf8AndRawRoutingIdHexAcrossLifecycles)
{
    auto escaped = descriptor;
    escaped.mesh_name = "mesh /한";
    const std::string raw_rid{"\0\0\0\x01", 4};
    escaped.rid = zlink::routing_id_t::from (raw_rid);
    ASSERT_EQ (repository.update_mesh_node (escaped, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    auto request = reserve_request;
    request.key.global_id = "escaped-capacity-actor";
    request.target.mesh_name = escaped.mesh_name;
    request.target.node_rid = node_rid_t::from_string (raw_rid);
    const auto reserved = repository.reserve (request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
    const store_key_t expected{std::string ("zlink:v12:capacity") + '\0' + "mesh /한" + '\0'
                               + "00000001" + '\0' + "actors" + '\0' + "pending"};
    const auto before = std::get<store_found_t> (provider.read (expected).result ().value ());
    const auto counts =
      nlohmann::json::parse (before.value.bytes.begin (), before.value.bytes.end ());
    EXPECT_EQ (counts, 1);
    repository.remove_mesh_node ({escaped.mesh_name, escaped.rid}, fence.target.owner)
      .result ()
      .value ();
    escaped.lifecycle_generation = 2;
    ASSERT_EQ (repository.update_mesh_node (escaped, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto unchanged = std::get<store_found_t> (provider.read (expected).result ().value ());
    EXPECT_EQ (before.value.version.value, unchanged.value.version.value);
    request.target.node_lifecycle_generation = 2;
    ASSERT_TRUE (
      std::holds_alternative<object_reserved_t> (repository.reserve (request).result ().value ()));
    const auto after = std::get<store_found_t> (provider.read (expected).result ().value ());
    EXPECT_EQ (after.value.bytes, bytes ("1"));
}

TEST_P (CreationTerminalTest, LiveTargetKeepsReservation)
{
    EXPECT_FALSE (repository
                    .release_ended_reservation (actor_authority_key (reserve_request.key.global_id),
                                                fence.expected_store_version)
                    .result ()
                    .value ());
}

TEST_P (CreationTerminalTest, ExpiredTargetReleasesPendingCapacity)
{
    owner_lease_time_store_t expired (provider.inner, fence.target.owner.owner_id,
                                      owner_lease_time_store_t::lease_view_t::expired);
    provider_location_repository_t recovery (expired);
    EXPECT_TRUE (recovery
                   .release_ended_reservation (actor_authority_key (reserve_request.key.global_id),
                                               fence.expected_store_version)
                   .result ()
                   .value ());
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ()));
    const auto capacity = std::get<store_found_t> (
      provider.read (capacity_key (descriptor, "actors", "pending")).result ().value ());
    const auto json =
      nlohmann::json::parse (capacity.value.bytes.begin (), capacity.value.bytes.end ());
    EXPECT_EQ (json, 0);
    EXPECT_TRUE (std::holds_alternative<object_commit_conflict_t> (
      repository.commit ({reserve_request.key, fence, bytes ("late")}).result ().value ()));
}

TEST_P (CreationTerminalTest, MissingPendingCounterReportsDataLostWithoutReleasingAuthority)
{
    provider.write ({{}, {store_delete_t{capacity_key (descriptor, "actors", "pending")}}})
      .result ()
      .value ();
    repository.release_owner_lease (fence.target.owner).result ().value ();
    const auto released =
      repository
        .release_ended_reservation (actor_authority_key (reserve_request.key.global_id),
                                    fence.expected_store_version)
        .result ();
    ASSERT_FALSE (released);
    EXPECT_EQ (released.error_kind (), framework_error_kind_t::data_lost);
    EXPECT_EQ (std::get<authority_snapshot_t> (
                 repository.read_authority (actor_authority_key (reserve_request.key.global_id))
                   .result ()
                   .value ())
                 .store_version,
               fence.expected_store_version);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (
      provider.read (capacity_key (descriptor, "actors", "pending")).result ().value ()));
}

TEST_P (CreationTerminalTest, UnderflowLeavesAuthorityAndCapacityUnchanged)
{
    provider
      .write (
        {{},
         {store_put_t{capacity_key (descriptor, "actors", "pending"), bytes ("0"), std::nullopt}}})
      .result ()
      .value ();
    repository.release_owner_lease (fence.target.owner).result ().value ();
    const auto before = std::get<store_found_t> (
      provider.read (capacity_key (descriptor, "actors", "pending")).result ().value ());
    const auto result =
      repository
        .release_ended_reservation (actor_authority_key (reserve_request.key.global_id),
                                    fence.expected_store_version)
        .result ();
    ASSERT_FALSE (result);
    try {
        (void) result.value ();
        FAIL () << "Capacity underflow must fail";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (error.kind (), framework_error_kind_t::data_lost);
    }
    const auto after = std::get<store_found_t> (
      provider.read (capacity_key (descriptor, "actors", "pending")).result ().value ());
    EXPECT_EQ (before.value.version.value, after.value.version.value);
    EXPECT_EQ (before.value.bytes, after.value.bytes);
    EXPECT_EQ (std::get<authority_snapshot_t> (
                 repository.read_authority (actor_authority_key (reserve_request.key.global_id))
                   .result ()
                   .value ())
                 .store_version,
               fence.expected_store_version);
}

TEST_P (CreationTerminalTest, LateCommitWinsAgainstReleaseStoreBatch)
{
    repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, fence.target.owner)
      .result ()
      .value ();
    provider.before_next_write = [&] () -> task_t<void> {
        co_await repository.update_mesh_node (descriptor, location_write_intent_t::new_claim);
        const auto committed =
          co_await repository.commit ({reserve_request.key, fence, bytes ("ready")});
        EXPECT_TRUE (std::holds_alternative<object_committed_t> (committed));
    };
    EXPECT_FALSE (repository
                    .release_ended_reservation (actor_authority_key (reserve_request.key.global_id),
                                                fence.expected_store_version)
                    .result ()
                    .value ());
}

TEST_P (CreationTerminalTest, DescriptorLifecycleReplacementReleasesReservedCapacity)
{
    ASSERT_EQ (
      repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, fence.target.owner)
        .result ()
        .value (),
      location_write_status_t::stored);
    descriptor.lifecycle_generation = 2;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    reserve_request.target.node_lifecycle_generation = 2;
    const auto replacement = repository.reserve (reserve_request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (replacement));
}

TEST_P (CreationTerminalTest, OwnerSweepReleasesEndedLifecycleReservation)
{
    ASSERT_EQ (
      repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, fence.target.owner)
        .result ()
        .value (),
      location_write_status_t::stored);
    descriptor.lifecycle_generation = 2;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    EXPECT_EQ (repository.list_mesh_nodes (descriptor.mesh_name)
                 .result ()
                 .value ()
                 .items.front ()
                 .capacity.actors.reserved,
               1u);
    EXPECT_EQ (repository.remove_all_by_owner (fence.target.owner).result ().value (), 1);
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ()));
    const auto after = repository.list_mesh_nodes (descriptor.mesh_name).result ().value ();
    ASSERT_EQ (after.items.size (), 1u);
    EXPECT_EQ (after.items.front ().lifecycle_generation, 2u);
    EXPECT_EQ (after.items.front ().capacity.actors.reserved, 0u);
    EXPECT_EQ (repository.remove_all_by_owner (fence.target.owner).result ().value (), 0);
}

TEST_P (CreationTerminalTest, OwnerSweepRetainsReservationWhenLeaseChangesDuringEndedRelease)
{
    ASSERT_EQ (
      repository.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, fence.target.owner)
        .result ()
        .value (),
      location_write_status_t::stored);
    descriptor.lifecycle_generation = 2;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    provider.before_next_write = [&] () -> task_t<void> {
        co_await repository.release_owner_lease (fence.target.owner);
        const auto replacement = co_await repository.claim_owner_lease (descriptor.owner_id, 30s);
        EXPECT_TRUE (std::holds_alternative<owner_lease_claimed_t> (replacement));
    };
    EXPECT_EQ (repository.remove_all_by_owner (fence.target.owner).result ().value (), 0);
    EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ()));
    const auto owner = repository.read_owner_lease (descriptor.owner_id).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (owner));
    EXPECT_NE (std::get<owner_lease_found_t> (owner).token.lease_generation,
               fence.target.owner.lease_generation);
    EXPECT_EQ (capacity_record (provider.inner, descriptor)["pending"]["actors"], 1);
}

TEST_P (CreationTerminalTest, CreationTransitionsFenceOwnerLeaseAndDescriptorVersion)
{
    verify_creation_conditions (false);
}

TEST_P (CreationTerminalTest, UserSpotCreationTransitionsFenceOwnerLeaseAndDescriptorVersion)
{
    verify_creation_conditions (true);
}

TEST_P (CreationTerminalTest, SameLifecycleDescriptorRepublishRebuildsQualifiedConflict)
{
    verify_creation_descriptor_conflict (false);
}

TEST_P (CreationTerminalTest, UserSpotSameLifecycleDescriptorRepublishRebuildsQualifiedConflict)
{
    verify_creation_descriptor_conflict (true);
}

TEST_P (CreationTerminalTest, PreviousLifecycleCommitIsRejected)
{
    provider.creation_writes.clear ();
    provider.before_next_write = [&] () -> task_t<void> {
        const store_key_t key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                              + descriptor.rid.to_hex ()};
        const auto read = co_await provider.inner.read (key);
        const auto &data = std::get<store_found_t> (read).value.bytes;
        auto record = nlohmann::json::parse (
          std::string (reinterpret_cast<const char *> (data.data ()), data.size ()));
        record["descriptor"]["lifecycleGeneration"] = "2";
        store_write_request_t republish;
        republish.mutations.emplace_back (store_put_t{key, bytes (record.dump ()), std::nullopt});
        co_await provider.inner.write (std::move (republish));
    };
    const auto result =
      repository
        .commit ({reserve_request.key, fence, bytes ("ready")}, {}, publication.operation_deadline)
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<object_commit_conflict_t> (result));
    EXPECT_EQ (provider.creation_writes.size (), 1u);
}

TEST_P (CreationTerminalTest, AcceptedCreationCompletesWhileTargetDraining)
{
    descriptor.state = framework_runtime_state_t::draining;
    ++descriptor.descriptor_revision;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto result = repository.complete_creation (request ()).result ().value ();
    EXPECT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
}

TEST_P (CreationTerminalTest, FailureBetweenFormerWritesCannotSplitPublication)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::between_writes;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, LostAtomicWriteReplyReplaysStoredTerminal)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::after_commit;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, ConditionalAuthorityConflictLeavesReservationAndCapacityUnchanged)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::before_commit;
    const auto result = repository.complete_creation (request ()).result ().value ();
    EXPECT_TRUE (std::holds_alternative<object_creation_completion_conflict_t> (result));
    EXPECT_EQ (provider.writes, 1u);
    EXPECT_FALSE (repository.read_creation_terminal (publication.operation).result ().value ());
    const auto authority =
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ();
    const auto *creating = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (creating, nullptr);
    EXPECT_EQ (creating->allocation.state, placement_allocation_state_t::reserved);
    EXPECT_EQ (creating->payload, bytes ("creating"));
    EXPECT_NE (creating->store_version, fence.expected_store_version);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("pending").at ("actors"), 1);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("active").at ("actors"), 0);
}

TEST_P (CreationTerminalTest, SharedCapacityConflictReconstructsWithoutChangingReservation)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    const auto result = repository.complete_creation (request ()).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_creation_completed_result_t> (result));
    expect_published_terminal_and_replay ();
}

TEST_P (CreationTerminalTest, LoweredCapacityLimitKeepsExistingReservations)
{
    const store_key_t descriptor_key{std::string ("mesh-node") + '\0' + descriptor.mesh_name + '\0'
                                     + descriptor.rid.to_hex ()};
    const auto publish_limit = [&] (int limit) {
        const auto current =
          std::get<store_found_t> (provider.inner.read (descriptor_key).result ().value ());
        auto row = nlohmann::json::parse (current.value.bytes);
        row["descriptor"]["capacity"]["actors"]["limit"] = limit;
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider.inner
            .write ({{store_version_condition_t{descriptor_key, current.value.version}},
                     {store_put_t{descriptor_key, bytes (row.dump ()), std::nullopt}}})
            .result ()
            .value ()));
    };

    publish_limit (2);
    auto second = reserve_request;
    second.key.global_id = "second-reservation-before-limit-drop";
    const auto reserved = repository.reserve (second).result ().value ();
    const auto *pending = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (pending, nullptr);
    publish_limit (1);
    ASSERT_EQ (capacity_count (provider, capacity_key (descriptor, "actors", "pending")), 2u);

    EXPECT_TRUE (std::holds_alternative<object_creation_completed_result_t> (
      repository.complete_creation (request ()).result ().value ()));
    EXPECT_EQ (capacity_count (provider, capacity_key (descriptor, "actors", "pending")), 1u);
    EXPECT_EQ (capacity_count (provider, capacity_key (descriptor)),
               GetParam () == completion_kind_t::created ? 1u : 0u);
    EXPECT_TRUE (std::holds_alternative<object_committed_t> (
      repository.commit ({second.key, pending->fence, bytes ("ready")}).result ().value ()));
    EXPECT_EQ (capacity_count (provider, capacity_key (descriptor, "actors", "pending")), 0u);
    EXPECT_EQ (capacity_count (provider, capacity_key (descriptor)),
               GetParam () == completion_kind_t::created ? 2u : 1u);
}

TEST_P (CreationTerminalTest, ReserveCounterConflictKeepsOperationReservationIdentity)
{
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({reserve_request.key, fence}).result ().value ()));
    provider.reservation_attempts.clear ();
    provider.fault = creation_terminal_failure_store_t::fault_t::reserve_counter_conflict;
    reserve_request.operation_deadline = publication.operation_deadline;
    const auto result = repository.reserve (reserve_request).result ().value ();
    const auto *reserved = std::get_if<object_reserved_t> (&result);
    ASSERT_NE (reserved, nullptr);
    ASSERT_EQ (provider.reservation_attempts.size (), 2u);
    EXPECT_EQ (provider.reservation_attempts.front (), provider.reservation_attempts.back ());
    EXPECT_EQ (reserved->fence.reservation_id, provider.reservation_attempts.front ());
}
TEST_P (CreationTerminalTest, ConcurrentTerminalEndsConflictReconstruction)
{
    provider.fault = creation_terminal_failure_store_t::fault_t::concurrent_terminal;
    const auto result = repository.complete_creation (request ()).result ().value ();
    const auto *completed = std::get_if<object_creation_already_completed_result_t> (&result);
    ASSERT_NE (completed, nullptr);
    EXPECT_EQ (completed->terminal.terminal_envelope, bytes ("winning-terminal"));
    EXPECT_EQ (provider.writes, 1u);
    const auto authority =
      repository.read_authority (actor_authority_key (reserve_request.key.global_id))
        .result ()
        .value ();
    const auto *reserved = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (reserved, nullptr);
    EXPECT_EQ (reserved->store_version, fence.expected_store_version);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("pending").at ("actors"), 1);
    EXPECT_EQ (capacity_record (provider, descriptor).at ("active").at ("actors"), 0);
}

INSTANTIATE_TEST_SUITE_P (CreationTerminalStates,
                          CreationTerminalTest,
                          ::testing::Values (completion_kind_t::created,
                                             completion_kind_t::rejected,
                                             completion_kind_t::failed));

std::vector<std::byte> from_hex (std::string_view value)
{
    const auto digit = [] (char value) -> unsigned char {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned char> (value - '0');
        return static_cast<unsigned char> (value - 'a' + 10);
    };
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (
          static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

TEST (CreationTerminalStoreRecord, ReadsNodeTerminalEnvelopeAtCanonicalKey)
{
    const creation_operation_identity_t operation{
      node_rid_t::from_string (std::string{"\0\xff\x10", 3}), 7, {1, 0xabcdef}};
    const std::string key = std::string ("creation-terminal") + '\0' + "00ff10" + '\0' + "7" + '\0'
                            + "00000000000000010000000000abcdef";
    // Generated by the Node production schema codec for a created terminal.
    const auto node_terminal =
      from_hex ("01000000250000000000000000010200180f6163746f722d63616e6f6e6963616c"
                "000000000000000100");
    in_memory_location_store_t provider;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({.conditions = {store_missing_condition_t{{key}}},
                 .mutations = {store_put_t{{key}, node_terminal, 1min}}})
        .result ()
        .value ()));

    provider_location_repository_t repository{provider};
    const auto stored = repository.read_creation_terminal (operation).result ().value ();
    ASSERT_TRUE (stored);
    EXPECT_EQ (stored->terminal_envelope, node_terminal);
}
}
