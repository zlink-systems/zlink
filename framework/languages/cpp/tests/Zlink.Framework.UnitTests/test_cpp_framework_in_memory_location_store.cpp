/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_location_store.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/live_location_reader.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{

using zlink::framework::fanout_publisher_descriptor_key_t;
using zlink::framework::fanout_publisher_descriptor_t;
using zlink::framework::framework_runtime_state_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::owner_lease_found_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::live_location_reader_t;

location_owner_token_t owner_token (std::string owner_id, std::int64_t generation)
{
    return location_owner_token_t{std::move (owner_id), generation};
}

location_owner_token_t claim_owner (
  in_memory_location_repository_t &store,
  std::string owner_id,
  std::chrono::milliseconds ttl = std::chrono::seconds (15))
{
    const auto claimed =
      store.claim_owner_lease (owner_id, ttl)
        .result ()
        .value ();
    const auto *value =
      std::get_if<zlink::framework::owner_lease_claimed_t> (
        &claimed);
    if (value == nullptr)
        throw std::runtime_error (
          "test owner lease claim failed");
    return value->token;
}

zlink::framework::mesh_node_descriptor_t make_mesh_node (
  std::string rid,
  location_owner_token_t owner,
  std::int32_t actor_limit = 10000)
{
    using namespace zlink::framework;
    return mesh_node_descriptor_t{
      .mesh_name = "play",
      .rid = zlink::routing_id_t::from (rid),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:5001",
      .application_version = 1,
      .object_capabilities =
        {{.object_kind = placement_object_kind_t::actor,
          .stable_type = "player",
          .policy = maintenance_policy_kind_t::recreate},
         {.object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .policy = maintenance_policy_kind_t::recreate}},
      .object_role = object_role_t::server,
      .capacity = {
        .actors = {.limit = actor_limit},
        .spots = {.limit = 128}},
      .activation_concurrency = {.limit = 128},
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = std::move (owner.owner_id),
      .lease_generation = owner.lease_generation};
}

void publish_mesh_node (
  in_memory_location_repository_t &store,
  std::string rid,
  location_owner_token_t owner,
  std::int32_t actor_limit = 10000)
{
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_mesh_node (
          make_mesh_node (
            std::move (rid), std::move (owner),
            actor_limit),
          location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);
}


TEST (ZLinkFrameworkInMemoryLocationStore,
      AuthorityAndReservationPreserveExactOwnerFence)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner_a = claim_owner (store, "owner-a");
    const auto owner_b = claim_owner (store, "owner-b");
    publish_mesh_node (store, "node-a", owner_a);
    publish_mesh_node (store, "node-b", owner_b);

    object_reserve_request_t reservation{
      .key = {placement_object_kind_t::actor,
              "actor-authority"},
      .intent =
        {.stable_type = "player",
         .request_content_reference = "request-root",
         .request_encoded_size = 4},
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("node-a"),
         .node_lifecycle_generation = 1,
         .owner = owner_a},
      .creating_payload = {
        std::byte{0x01}, std::byte{0x02}},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved =
      store.reserve (reservation).result ().value ();
    const auto *reserved_value =
      std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (nullptr, reserved_value);
    EXPECT_EQ (
      reservation.creating_payload,
      reserved_value->creating.payload);
    EXPECT_EQ (
      owner_a.lease_generation,
      reserved_value->creating.owner.lease_generation);

    const std::vector<std::byte> ready_payload{
      std::byte{0x03}, std::byte{0x04}};
    const auto committed =
      store
        .commit (
          {reservation.key, reserved_value->fence,
           ready_payload})
        .result ()
        .value ();
    const auto *committed_value =
      std::get_if<object_committed_t> (&committed);
    ASSERT_NE (nullptr, committed_value);
    EXPECT_EQ (ready_payload, committed_value->ready.payload);

    const authority_key_t authority_key{
      "1:actor-authority"};
    const auto preserved =
      store
        .compare_exchange_authority (
          authority_key,
          committed_value->ready.store_version,
          authority_put_t{
            {std::byte{0x05}},
            authority_generation_transition_t::preserve,
            std::nullopt})
        .result ()
        .value ();
    const auto *preserved_value =
      std::get_if<authority_stored_t> (&preserved);
    ASSERT_NE (nullptr, preserved_value);
    EXPECT_EQ (
      owner_a.lease_generation,
      preserved_value->snapshot.owner.lease_generation);
    EXPECT_EQ (
      committed_value->ready.authority_owner_generation,
      preserved_value->snapshot.authority_owner_generation);

    std::array<std::byte, 16> relocation_id{};
    relocation_id[15] = std::byte{0x01};
    const relocation_capacity_reserve_request_t capacity_request{
      .reservation_id = relocation_id,
      .key = authority_key,
      .expected_store_version =
        preserved_value->snapshot.store_version,
      .object_kind = placement_object_kind_t::actor,
      .stable_type = "player",
      .source = reservation.target,
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("node-b"),
         .node_lifecycle_generation = 1,
         .owner = owner_b},
      .capacity_bundle = {.actor_slots = 1}};
    const auto capacity =
      store.reserve_relocation_capacity (capacity_request)
        .result ()
        .value ();
    const auto *capacity_value =
      std::get_if<relocation_capacity_reserved_t> (
        &capacity);
    ASSERT_NE (nullptr, capacity_value);
    const auto capacity_again =
      store.reserve_relocation_capacity (capacity_request)
        .result ()
        .value ();
    EXPECT_NE (
      nullptr,
      std::get_if<
        relocation_capacity_already_reserved_t> (
        &capacity_again));

    store.release_owner_lease (owner_a).result ().value ();
    const auto moved =
      store
        .compare_exchange_authority (
          authority_key,
          preserved_value->snapshot.store_version,
          authority_put_t{
            {std::byte{0x06}},
            authority_generation_transition_t::new_owner,
            owner_b,
            capacity_value->fence})
        .result ()
        .value ();
    const auto *moved_value =
      std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (nullptr, moved_value);
    EXPECT_EQ (
      owner_b.lease_generation,
      moved_value->snapshot.owner.lease_generation);
    EXPECT_GT (
      moved_value->snapshot.authority_owner_generation,
      preserved_value->snapshot.authority_owner_generation);
    EXPECT_EQ (
      relocation_capacity_abort_result_t::already_committed,
      store
        .abort_relocation_capacity (
          capacity_value->fence)
        .result ()
        .value ());
    EXPECT_THROW (
      store.compare_exchange_authority (
        authority_key,
        moved_value->snapshot.store_version,
        authority_put_t{
          {},
          authority_generation_transition_t::preserve,
          owner_b}),
      std::invalid_argument);
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      DescriptorFencesCapabilityProfileAndPendingCapacity)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner =
      std::get<owner_lease_claimed_t> (
        store
          .claim_owner_lease (
            "owner-target", std::chrono::seconds (15))
          .result ()
          .value ())
        .token;

    object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "profiled-a"},
      .intent = {.stable_type = "player"},
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("node-target"),
         .node_lifecycle_generation = 1,
         .owner = owner},
      .capacity_bundle = {.actor_slots = 1}};
    EXPECT_NE (
      nullptr,
      std::get_if<object_reserve_conflict_t> (
        &store.reserve (request).result ().value ()));

    publish_mesh_node (
      store, "node-target", owner, 1);
    EXPECT_NE (
      nullptr,
      std::get_if<object_reserved_t> (
        &store.reserve (request).result ().value ()));
    auto over_limit = request;
    over_limit.key.global_id = "profiled-b";
    EXPECT_NE (
      nullptr,
      std::get_if<object_placement_capacity_exhausted_t> (
        &store.reserve (over_limit).result ().value ()));

    auto changed =
      make_mesh_node ("node-target", owner, 2);
    changed.descriptor_revision = 2;
    EXPECT_EQ (
      location_write_status_t::rejected_conflict,
      store
        .update_mesh_node (
          changed, location_write_intent_t::renew)
        .result ()
        .value ()
        .status);
    const auto listed =
      store.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (1u, listed.items.size ());
    EXPECT_EQ (
      1, listed.items.front ().capacity.actors.limit);
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      AuthorityRestorePreservesIdentityWithoutLiveOwnerLease)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner = claim_owner (store, "restore-owner");
    publish_mesh_node (store, "restore-node", owner);

    const object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "restore-actor"},
      .intent = {.stable_type = "player"},
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("restore-node"),
         .node_lifecycle_generation = 1,
         .owner = owner},
      .creating_payload = {std::byte{0x01}},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = store.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (nullptr, reservation);
    const auto committed = store
      .commit ({request.key, reservation->fence, {std::byte{0x02}}})
      .result ().value ();
    const auto *ready = std::get_if<object_committed_t> (&committed);
    ASSERT_NE (nullptr, ready);

    store.release_owner_lease (owner).result ().value ();
    const auto wrong_owner = store.compare_exchange_authority (
      {"1:restore-actor"}, ready->ready.store_version,
      authority_restore_t{{std::byte{0x03}}, {"other-owner", 1}})
      .result ().value ();
    EXPECT_NE (nullptr, std::get_if<authority_conflict_t> (&wrong_owner));

    const auto restored = store.compare_exchange_authority (
      {"1:restore-actor"}, ready->ready.store_version,
      authority_restore_t{{std::byte{0x04}}, owner})
      .result ().value ();
    const auto *stored = std::get_if<authority_stored_t> (&restored);
    ASSERT_NE (nullptr, stored);
    EXPECT_EQ ((std::vector<std::byte>{std::byte{0x04}}),
               stored->snapshot.payload);
    EXPECT_EQ (ready->ready.object_generation,
               stored->snapshot.object_generation);
    EXPECT_EQ (ready->ready.authority_owner_generation,
               stored->snapshot.authority_owner_generation);
    EXPECT_EQ (ready->ready.owner.owner_id,
               stored->snapshot.owner.owner_id);
    EXPECT_EQ (ready->ready.owner.lease_generation,
               stored->snapshot.owner.lease_generation);
    EXPECT_EQ (ready->ready.allocation.target.node_rid.value (),
               stored->snapshot.allocation.target.node_rid.value ());

    const auto stale = store.compare_exchange_authority (
      {"1:restore-actor"}, ready->ready.store_version,
      authority_restore_t{{std::byte{0x05}}, owner})
      .result ().value ();
    EXPECT_NE (nullptr, std::get_if<authority_conflict_t> (&stale));
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      StoreRevisionExhaustionDoesNotReuseVersionOrMutateAuthority)
{
    using namespace zlink::framework;
    const auto max_revision =
      static_cast<std::uint64_t> (
        std::numeric_limits<std::int64_t>::max ());
    in_memory_location_repository_t store{max_revision - 2};
    const auto owner =
      std::get<owner_lease_claimed_t> (
        store
          .claim_owner_lease (
            "owner-max", std::chrono::seconds (15))
          .result ()
          .value ())
        .token;
    publish_mesh_node (store, "node-max", owner);
    object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "max-revision"},
      .intent = {.stable_type = "player"},
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("node-max"),
         .node_lifecycle_generation = 1,
         .owner = owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved =
      std::get<object_reserved_t> (
        store.reserve (request).result ().value ());
    const auto committed =
      std::get<object_committed_t> (
        store
          .commit (
            {request.key, reserved.fence, {std::byte{0x01}}})
          .result ()
          .value ());
    ASSERT_EQ (std::to_string (max_revision),
               committed.ready.store_version);

    const auto result =
      store
        .compare_exchange_authority (
          { "1:max-revision" },
          committed.ready.store_version,
          authority_put_t{
            {std::byte{0x02}},
            authority_generation_transition_t::preserve,
            std::nullopt})
        .result ()
        .value ();
    EXPECT_NE (
      nullptr,
      std::get_if<authority_generation_exhausted_t> (&result));
    const auto current =
      std::get<authority_snapshot_t> (
        store
          .read_authority ({"1:max-revision"})
          .result ()
          .value ());
    EXPECT_EQ (committed.ready.store_version,
               current.store_version);
    EXPECT_EQ (committed.ready.payload, current.payload);
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      CreationTerminalDedupesOnlyTheSameOperation)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner = claim_owner (store, "creation-owner");
    publish_mesh_node (store, "creation-node", owner);

    const creation_operation_identity_t first_operation{
      node_rid_t::from_string ("source-node"), 7, {11, 13}};
    object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "actor-terminal"},
      .intent = {.stable_type = "player"},
      .target =
        {.mesh_name = "play",
         .node_rid = node_rid_t::from_string ("creation-node"),
         .node_lifecycle_generation = 1,
         .owner = owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved =
      std::get<object_reserved_t> (
        store.reserve (request).result ().value ());

    const std::vector<std::byte> envelope{
      std::byte{'r'}, std::byte{'e'}, std::byte{'j'}};
    creation_terminal_publication_t publication{
      first_operation,
      envelope,
      zlink::framework::runtime::sha256 (envelope),
      std::chrono::system_clock::now ()
        + std::chrono::seconds (30)};
    const auto completed =
      store.complete_creation (
        {request.key, reserved.fence,
         object_creation_rejected_t{publication}})
        .result ()
        .value ();
    ASSERT_NE (
      nullptr,
      std::get_if<object_creation_completed_result_t> (
        &completed));
    const auto terminal =
      store.read_creation_terminal (first_operation)
        .result ()
        .value ();
    ASSERT_TRUE (terminal);
    EXPECT_EQ (
      creation_terminal_state_t::rejected,
      terminal->state);

    auto second = request;
    EXPECT_NE (
      nullptr,
      std::get_if<object_reserved_t> (
        &store.reserve (second).result ().value ()));
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      AggregateRequiresExactCapacityBundle)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner_a = claim_owner (store, "owner-a");
    const auto owner_b = claim_owner (store, "owner-b");
    publish_mesh_node (store, "node-a", owner_a);
    publish_mesh_node (store, "node-b", owner_b);

    const auto create =
      [&] (placement_object_kind_t kind,
           std::string stable_type,
           std::string id,
           std::byte marker)
        -> authority_snapshot_t {
        object_reserve_request_t request{
          .key = {kind, id},
          .intent = {.stable_type = stable_type},
          .target =
            {.mesh_name = "play",
             .node_rid = node_rid_t::from_string ("node-a"),
             .node_lifecycle_generation = 1,
             .owner = owner_a},
          .creating_payload = {marker},
          .capacity_bundle =
            kind == placement_object_kind_t::actor
              ? placement_capacity_bundle_t{.actor_slots = 1}
              : placement_capacity_bundle_t{
                  .spot_slots = 1,
                  .spot_type =
                    spot_type_capacity_delta_t{
                      .object_kind = kind,
                      .stable_type = stable_type,
                      .slots = 1}}};
        const auto reserved =
          store.reserve (request).result ().value ();
        const auto &fence =
          std::get<object_reserved_t> (reserved).fence;
        return std::get<object_committed_t> (
                 store
                   .commit (
                     {request.key, fence, {marker}})
                   .result ()
                   .value ())
          .ready;
      };
    const auto first = create (
      placement_object_kind_t::user_spot,
      "room",
      "aggregate-room",
      std::byte{0x11});
    const auto second = create (
      placement_object_kind_t::actor,
      "player",
      "aggregate-actor",
      std::byte{0x12});

    aggregate_prepare_request_t request;
    request.aggregate_id.value[15] = std::byte{0x21};
    request.aggregate_generation = 1;
    request.participants = {
      {{ "2:aggregate-room" },
       first.store_version,
       authority_generation_transition_t::new_owner,
       {std::byte{0x31}},
       {}},
      {{ "1:aggregate-actor" },
       second.store_version,
       authority_generation_transition_t::new_owner,
       {std::byte{0x32}},
       {}}};
    std::sort (
      request.participants.begin (),
      request.participants.end (),
      [] (const auto &left, const auto &right) {
          return left.key.value < right.key.value;
      });
    request.target_descriptor = {
      .mesh_name = "play",
      .rid = zlink::routing_id_t::from ("node-b")};
    request.target_descriptor_lifecycle_generation = 1;
    request.target_owner = owner_b;

    request.capacity_bundle = {
      .actor_slots = 2,
      .spot_slots = 1,
      .spot_type =
        spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot,
          .stable_type = "room",
          .slots = 1}};
    EXPECT_NE (
      nullptr,
      std::get_if<aggregate_prepare_conflict_t> (
        &store.prepare_aggregate (request).result ().value ()));
    request.capacity_bundle.actor_slots = 1;
    auto unsupported_membership = request;
    unsupported_membership.aggregate_id.value[14] = std::byte{0x22};
    unsupported_membership.participants.front ().membership_mutation = {
      std::byte{0x01}};
    EXPECT_NE (
      nullptr,
      std::get_if<aggregate_prepare_conflict_t> (
        &store.prepare_aggregate (unsupported_membership).result ().value ()));
    EXPECT_EQ (
      first.store_version,
      std::get<authority_snapshot_t> (
        store.read_authority ({"2:aggregate-room"})
          .result ()
          .value ())
        .store_version);
    EXPECT_EQ (
      second.store_version,
      std::get<authority_snapshot_t> (
        store.read_authority ({"1:aggregate-actor"})
          .result ()
          .value ())
        .store_version);

    store.release_owner_lease (owner_a).result ().value ();
    const auto prepared =
      store.prepare_aggregate (request).result ().value ();
    const auto *prepared_value =
      std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (nullptr, prepared_value);
    EXPECT_EQ (
      aggregate_commit_result_t::committed,
      store.commit_aggregate (prepared_value->fence)
        .result ()
        .value ());
    EXPECT_EQ (
      owner_b.lease_generation,
      std::get<authority_snapshot_t> (
        store.read_authority ({"2:aggregate-room"})
          .result ()
          .value ())
        .owner.lease_generation);
    EXPECT_EQ (
      owner_b.lease_generation,
      std::get<authority_snapshot_t> (
        store.read_authority ({"1:aggregate-actor"})
          .result ()
          .value ())
        .owner.lease_generation);
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      AggregateConsumesPerObjectCapacityFencesOnce)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    const auto owner_a = claim_owner (store, "fence-source");
    const auto owner_b = claim_owner (store, "fence-target");
    publish_mesh_node (store, "fence-source-node", owner_a);
    publish_mesh_node (store, "fence-target-node", owner_b);

    const auto create =
      [&] (placement_object_kind_t kind,
           std::string stable_type,
           std::string id,
           std::byte marker) {
          object_reserve_request_t request;
          request.key = {kind, std::move (id)};
          request.intent.stable_type = stable_type;
          request.target = {
            .mesh_name = "play",
            .node_rid = node_rid_t::from_string ("fence-source-node"),
            .node_lifecycle_generation = 1,
            .owner = owner_a};
          request.creating_payload = {marker};
          request.capacity_bundle =
            kind == placement_object_kind_t::actor
              ? placement_capacity_bundle_t{.actor_slots = 1}
              : placement_capacity_bundle_t{
                  .spot_slots = 1,
                  .spot_type = spot_type_capacity_delta_t{
                    placement_object_kind_t::user_spot,
                    stable_type,
                    1}};
          const auto reserved = store.reserve (request).result ().value ();
          const auto *reservation =
            std::get_if<object_reserved_t> (&reserved);
          EXPECT_NE (reservation, nullptr);
          const auto committed =
            store.commit ({
              request.key,
              reservation->fence,
              {marker}}).result ().value ();
          return std::get<object_committed_t> (committed).ready;
      };

    const auto spot = create (
      placement_object_kind_t::user_spot,
      "room",
      "fenced-room",
      std::byte{0x11});
    const auto actor = create (
      placement_object_kind_t::actor,
      "player",
      "fenced-actor",
      std::byte{0x12});

    const auto reserve =
      [&] (const authority_snapshot_t &snapshot,
           placement_object_kind_t kind,
           std::string stable_type,
           std::array<std::byte, 16> reservation_id) {
          relocation_capacity_reserve_request_t request;
          request.reservation_id = reservation_id;
          request.key = {
            kind == placement_object_kind_t::actor
              ? "1:fenced-actor"
              : "2:fenced-room"};
          request.expected_store_version = snapshot.store_version;
          request.object_kind = kind;
          request.stable_type = std::move (stable_type);
          request.source = snapshot.allocation.target;
          request.target = {
            .mesh_name = "play",
            .node_rid = node_rid_t::from_string ("fence-target-node"),
            .node_lifecycle_generation = 1,
            .owner = owner_b};
          request.capacity_bundle =
            kind == placement_object_kind_t::actor
              ? placement_capacity_bundle_t{.actor_slots = 1}
              : placement_capacity_bundle_t{
                  .spot_slots = 1,
                  .spot_type = spot_type_capacity_delta_t{
                    placement_object_kind_t::user_spot,
                    request.stable_type,
                    1}};
          const auto result =
            store.reserve_relocation_capacity (request)
              .result ().value ();
          return std::get<relocation_capacity_reserved_t> (result).fence;
      };

    std::array<std::byte, 16> actor_reservation_id{};
    actor_reservation_id[15] = std::byte{1};
    std::array<std::byte, 16> spot_reservation_id{};
    spot_reservation_id[15] = std::byte{2};
    const auto actor_capacity = reserve (
      actor,
      placement_object_kind_t::actor,
      "player",
      actor_reservation_id);
    const auto spot_capacity = reserve (
      spot,
      placement_object_kind_t::user_spot,
      "room",
      spot_reservation_id);

    aggregate_prepare_request_t request;
    request.aggregate_id.value[15] = std::byte{3};
    request.aggregate_generation = 1;
    request.participants = {
      {{"1:fenced-actor"},
       actor.store_version,
       authority_generation_transition_t::new_owner,
       {std::byte{0x21}},
       {},
       actor_capacity},
      {{"2:fenced-room"},
       spot.store_version,
       authority_generation_transition_t::new_owner,
       {std::byte{0x22}},
       {},
       spot_capacity}};
    request.capacity_fences = {actor_capacity, spot_capacity};
    request.target_descriptor = {
      "play",
      zlink::routing_id_t::from ("fence-target-node")};
    request.target_descriptor_lifecycle_generation = 1;
    request.capacity_bundle = {
      .actor_slots = 1,
      .spot_slots = 1,
      .spot_type = spot_type_capacity_delta_t{
        placement_object_kind_t::user_spot,
        "room",
        1}};
    request.target_owner = owner_b;

    const auto prepared = store.prepare_aggregate (request).result ().value ();
    const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (fence, nullptr);
    const auto before_commit =
      store.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (before_commit.items.size (), 2u);
    const auto target_before = std::find_if (
      before_commit.items.begin (),
      before_commit.items.end (),
      [] (const auto &node) {
          return node.rid.to_string () == "fence-target-node";
      });
    ASSERT_NE (target_before, before_commit.items.end ());
    EXPECT_EQ (target_before->capacity.actors.reserved, 1u);
    EXPECT_EQ (target_before->capacity.spots.reserved, 1u);

    EXPECT_EQ (
      store.commit_aggregate (fence->fence).result ().value (),
      aggregate_commit_result_t::committed);
    const auto after_commit =
      store.list_mesh_nodes ("play").result ().value ();
    const auto target_after = std::find_if (
      after_commit.items.begin (),
      after_commit.items.end (),
      [] (const auto &node) {
          return node.rid.to_string () == "fence-target-node";
      });
    ASSERT_NE (target_after, after_commit.items.end ());
    EXPECT_EQ (target_after->capacity.actors.reserved, 0u);
    EXPECT_EQ (target_after->capacity.actors.active, 1u);
    EXPECT_EQ (target_after->capacity.spots.reserved, 0u);
    EXPECT_EQ (target_after->capacity.spots.active, 1u);
    EXPECT_EQ (
      store.abort_relocation_capacity (actor_capacity).result ().value (),
      relocation_capacity_abort_result_t::already_committed);
    EXPECT_EQ (
      store.abort_relocation_capacity (spot_capacity).result ().value (),
      relocation_capacity_abort_result_t::already_committed);
    EXPECT_EQ (
      store.commit_aggregate (fence->fence).result ().value (),
      aggregate_commit_result_t::already_committed);
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      AggregateInventoryUsesBoundedLeafPagesWithoutTotalCountCap)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime::aggregate_inventory;
    std::vector<aggregate_participant_t> participants;
    participants.reserve (2049);
    for (std::size_t index = 0; index < 2049; ++index) {
        aggregate_participant_t participant;
        participant.key.value =
          std::to_string (index) + ":inventory-actor";
        participant.expected_store_version = std::to_string (index + 1);
        participant.authority_payload = {std::byte{0x01}};
        participants.push_back (std::move (participant));
    }

    const auto tree = build_tree (participants);
    ASSERT_TRUE (tree);
    EXPECT_EQ (tree->participant_count, 2049u);
    EXPECT_EQ (tree->pages.size (), 3u);
    for (std::size_t index = 0; index < tree->pages.size (); ++index) {
        EXPECT_LE (tree->pages[index].participants.size (), page_item_limit);
        EXPECT_LE (tree->pages[index].encoded.size (), page_byte_limit);
        const auto decoded = decode_page (
          tree->pages[index].encoded, index);
        ASSERT_TRUE (decoded);
        EXPECT_EQ (decoded->size (), tree->pages[index].participants.size ());
        EXPECT_FALSE (
          decode_page (tree->pages[index].encoded, index + 1));
    }
    EXPECT_EQ (tree->root, tree_root (tree->pages, 2049));
}



TEST (ZLinkFrameworkInMemoryLocationStore, MaintainsOwnerLeasesAndUsesPollingWithoutStampHint)
{
    in_memory_location_repository_t store;
    const auto owner = claim_owner (store, "owner-a");
    const auto lease =
      store.read_owner_lease ("owner-a").result ().value ();
    const auto *found =
      std::get_if<owner_lease_found_t> (&lease);
    ASSERT_NE (nullptr, found);
    EXPECT_EQ (owner.lease_generation,
               found->token.lease_generation);
    EXPECT_GT (found->lease_expires_at, found->store_now);

    const auto stamp =
      store.get_mesh_node_change_stamp ("play").result ().value ();
    EXPECT_FALSE (stamp.has_value ());
    const auto released =
      store.release_owner_lease (owner).result ().value ();
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_released_t> (
        released));
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::owner_lease_missing_t> (
        store.read_owner_lease ("owner-a")
          .result ()
          .value ()));
}



TEST (ZLinkFrameworkInMemoryLocationStore,
      FanoutPublisherRowsFenceIdentityRevisionAndCleanup)
{
    in_memory_location_repository_t store;
    const auto owner_a = claim_owner (store, "fanout-owner-a");
    const auto owner_b = claim_owner (store, "fanout-owner-b");
    const auto owner_c = claim_owner (store, "fanout-owner-c");

    const fanout_publisher_descriptor_t original{
      .channel_name = "events",
      .publisher_rid = zlink::routing_id_t::from ("publisher-a"),
      .lifecycle_generation = 7,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:7500",
      .state = framework_runtime_state_t::serving,
      .security_identity = "cluster-a",
      .owner_id = owner_a.owner_id,
      .lease_generation = owner_a.lease_generation};
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_fanout_publisher (
          original, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);

    auto conflicting = original;
    conflicting.owner_id = owner_b.owner_id;
    conflicting.lease_generation = owner_b.lease_generation;
    EXPECT_EQ (
      location_write_status_t::rejected_conflict,
      store
        .update_fanout_publisher (
          conflicting, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);

    auto immutable_change = original;
    immutable_change.descriptor_revision = 2;
    immutable_change.endpoint = "tcp://127.0.0.1:7501";
    EXPECT_EQ (
      location_write_status_t::rejected_conflict,
      store
        .update_fanout_publisher (
          immutable_change, location_write_intent_t::renew)
        .result ()
        .value ()
        .status);

    auto draining = original;
    draining.descriptor_revision = 2;
    draining.state = framework_runtime_state_t::draining;
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_fanout_publisher (
          draining, location_write_intent_t::renew)
        .result ()
        .value ()
        .status);
    auto same_revision_conflict = draining;
    same_revision_conflict.state =
      framework_runtime_state_t::serving;
    EXPECT_EQ (
      location_write_status_t::rejected_conflict,
      store
        .update_fanout_publisher (
          same_revision_conflict,
          location_write_intent_t::renew)
        .result ()
        .value ()
        .status);
    EXPECT_EQ (
      location_write_status_t::ignored_stale,
      store
        .update_fanout_publisher (
          original, location_write_intent_t::renew)
        .result ()
        .value ()
        .status);

    EXPECT_NE (
      nullptr,
      std::get_if<zlink::framework::owner_lease_released_t> (
        &store.release_owner_lease (owner_a).result ().value ()));
    auto replacement = original;
    replacement.lifecycle_generation = 8;
    replacement.endpoint = "tcp://127.0.0.1:7501";
    replacement.owner_id = owner_b.owner_id;
    replacement.lease_generation = owner_b.lease_generation;
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_fanout_publisher (
          replacement, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);

    auto second = original;
    second.publisher_rid = zlink::routing_id_t::from ("publisher-b");
    second.lifecycle_generation = 1;
    second.owner_id = owner_c.owner_id;
    second.lease_generation = owner_c.lease_generation;
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_fanout_publisher (
          second, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);

    const auto first_page =
      store
        .list_fanout_publishers (
          "events", location_page_request_t{.page_size = 1})
        .result ()
        .value ();
    ASSERT_EQ (1u, first_page.items.size ());
    ASSERT_TRUE (first_page.continuation_token.has_value ());
    const auto second_page =
      store
        .list_fanout_publishers (
          "events",
          location_page_request_t{
            .page_size = 1,
            .continuation_token =
              first_page.continuation_token})
        .result ()
        .value ();
    ASSERT_EQ (1u, second_page.items.size ());
    EXPECT_FALSE (second_page.continuation_token.has_value ());

    EXPECT_EQ (
      location_write_status_t::ignored_stale,
      store
        .remove_fanout_publisher (
          fanout_publisher_descriptor_key_t{
            .channel_name = original.channel_name,
            .publisher_rid = original.publisher_rid},
          owner_a)
        .result ()
        .value ());
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .remove_fanout_publisher (
          fanout_publisher_descriptor_key_t{
            .channel_name = replacement.channel_name,
            .publisher_rid = replacement.publisher_rid},
          owner_b)
        .result ()
        .value ());
    EXPECT_EQ (
      1u,
      store.remove_all_by_owner (owner_c).result ().value ());
    EXPECT_TRUE (
      store
        .list_fanout_publishers ("events")
        .result ()
        .value ()
        .items.empty ());
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      FanoutPublisherPageStopsAtFourMiB)
{
    in_memory_location_repository_t store;
    const std::string maximal_escaped_text (
      255, '\x01');
    const auto owner =
      claim_owner (
        store, maximal_escaped_text,
        std::chrono::seconds (60));
    for (std::size_t index = 0; index < 1000;
         ++index) {
        const fanout_publisher_descriptor_t descriptor{
          .channel_name = maximal_escaped_text,
          .publisher_rid =
            zlink::routing_id_t::from (
              "page-publisher-"
              + std::to_string (index)),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = maximal_escaped_text,
          .state =
            framework_runtime_state_t::serving,
          .security_identity =
            maximal_escaped_text,
          .owner_id = owner.owner_id,
          .lease_generation =
            owner.lease_generation};
        ASSERT_EQ (
          location_write_status_t::stored,
          store
            .update_fanout_publisher (
              descriptor,
              location_write_intent_t::new_claim)
            .result ()
            .value ()
            .status);
    }

    const auto first =
      store
        .list_fanout_publishers (
          maximal_escaped_text,
          location_page_request_t{
            .page_size = 1000})
        .result ()
        .value ();
    ASSERT_FALSE (first.items.empty ());
    ASSERT_LT (first.items.size (), 1000u);
    ASSERT_TRUE (
      first.continuation_token.has_value ());
    std::size_t encoded_upper_bound = 0;
    for (const auto &descriptor : first.items) {
        const auto escaped_text_bytes =
          descriptor.channel_name.size ()
          + descriptor.endpoint.size ()
          + descriptor.security_identity.size ()
          + descriptor.owner_id.size ();
        encoded_upper_bound +=
          512u + escaped_text_bytes * 6u
          + descriptor.publisher_rid.size () * 2u;
    }
    EXPECT_LE (encoded_upper_bound,
               4u * 1024u * 1024u);

    const auto second =
      store
        .list_fanout_publishers (
          maximal_escaped_text,
          location_page_request_t{
            .page_size = 1000,
            .continuation_token =
              first.continuation_token})
        .result ()
        .value ();
    EXPECT_EQ (
      1000u,
      first.items.size () + second.items.size ());
    EXPECT_FALSE (
      second.continuation_token.has_value ());
    EXPECT_EQ (
      1000,
      store.remove_all_by_owner (owner)
        .result ()
        .value ());
}

TEST (ZLinkFrameworkInMemoryLocationStore,
      SignedWeightBoundariesAreValidatedWithoutNarrowing)
{
    in_memory_location_repository_t store;
    const auto owner = claim_owner (
      store, "weight-owner");

    auto upper = make_mesh_node (
      "weight-upper", owner);
    upper.channel_weights = {{"default", 10000}};
    upper.placement_weight = 10000;
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_mesh_node (
          upper, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);
    const auto listed =
      store
        .list_mesh_nodes (
          "play",
          location_page_request_t{.page_size = 1000})
        .result ()
        .value ();
    ASSERT_EQ (1u, listed.items.size ());
    EXPECT_EQ (
      10000, listed.items.front ().placement_weight);
    EXPECT_EQ (
      10000,
      listed.items.front ().channel_weights.at (
        "default"));

    auto negative = make_mesh_node (
      "weight-negative", owner);
    negative.channel_weights = {{"default", -1}};
    EXPECT_THROW (
      (void) store.update_mesh_node (
        negative, location_write_intent_t::new_claim),
      std::invalid_argument);

    auto over = make_mesh_node (
      "weight-over", owner);
    over.placement_weight = 10001;
    EXPECT_THROW (
      (void) store.update_mesh_node (
        over, location_write_intent_t::new_claim),
      std::invalid_argument);

    zlink::framework::client_server_server_descriptor_t server{
      .channel_name = "orders",
      .server_rid = zlink::routing_id_t::from (
        "weight-server"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:5002",
      .weight = 10000,
      .state = framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
    EXPECT_EQ (
      location_write_status_t::stored,
      store
        .update_client_server (
          server, location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status);
    server.server_rid =
      zlink::routing_id_t::from ("weight-server-over");
    server.weight = 10001;
    EXPECT_THROW (
      (void) store.update_client_server (
        server, location_write_intent_t::new_claim),
      std::invalid_argument);
}

} // namespace
