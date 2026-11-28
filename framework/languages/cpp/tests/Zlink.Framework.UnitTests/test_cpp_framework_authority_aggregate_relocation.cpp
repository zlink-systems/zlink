/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "../support/opaque_store_record_fixture.hpp"
#include "../support/aggregate_page_read_fixture.hpp"
#include "../support/actor_authority_fixture.hpp"
#include "../support/stateful_object_sync.hpp"

#include <bit>
#include <filesystem>
#include <fstream>

namespace
{
using namespace zlink::framework::test::opaque_store;
using namespace zlink::framework::runtime::stateful;
using stateful_relocation_reason_t = zlink::framework::runtime::stateful::relocation_reason_t;

nlohmann::ordered_json golden_records ()
{
    std::ifstream fixture (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH);
    return nlohmann::ordered_json::parse (fixture);
}

TEST (AuthorityAggregateRelocation, ProviderSealMatchesAggregateStorageGolden)
{
    const auto fixture_path =
      std::filesystem::path (ZLINK_AUTHORITY_AGGREGATE_RELOCATION_GOLDEN_PATH).parent_path ()
      / "aggregate-storage-v1.json";
    std::ifstream fixture (fixture_path);
    const auto golden = nlohmann::json::parse (fixture);
    for (const auto &item : golden.at ("cases")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        in_memory_location_store_t provider;
        provider_location_repository_t repository (provider);
        const auto &record = item.at ("aggregate").at ("decoded");
        const auto &root = item.at ("root");
        const auto digest = root.at ("decoded").at ("digestSha256").get<std::string> ();
        const auto &hash_inputs = item.at ("participantHashInputs");
        const auto payload = zlink::framework::tests::from_hex (
          hash_inputs.at ("authorityPayloadHex").get<std::string> ());
        const auto membership = zlink::framework::tests::from_hex (
          hash_inputs.at ("membershipMutationHex").get<std::string> ());
        aggregate_fence_t fence;
        const auto id = zlink::framework::tests::from_hex ("0123456789abcdef0123456789abcdef");
        std::ranges::copy (id, fence.aggregate_id.value.begin ());
        fence.aggregate_generation = 9007199254740993;
        std::vector<aggregate_participant_t> participants;
        for (const auto &page : item.at ("pages")) {
            for (const auto &entry : page.at ("decoded").at ("entries")) {
                participants.push_back ({{entry.at ("authorityKey").get<std::string> ()},
                                         entry.at ("expectedStoreVersion").get<std::string> (),
                                         entry.at ("ownerTransition") == "preserve"
                                           ? authority_generation_transition_t::preserve
                                           : authority_generation_transition_t::new_owner,
                                         payload,
                                         membership});
            }
        }
        ASSERT_EQ (participants.size (), record.at ("participantCount").get<std::size_t> ());
        ASSERT_TRUE (
          repository.seal_relocation_inventory (fence, digest, participants).result ().value ());
        const auto assert_record = [&] (const nlohmann::json &expected) {
            const auto key = expected.at ("key").get<std::string> ();
            const auto row = provider.read ({"zlink:v11:" + key.substr (7)}).result ().value ();
            const auto *found = std::get_if<store_found_t> (&row);
            ASSERT_NE (found, nullptr) << key;
            EXPECT_EQ (found->value.bytes, zlink::framework::tests::from_hex (
                                             expected.at ("jcsHex").get<std::string> ()))
              << key;
        };
        assert_record (root);
        for (const auto &page : item.at ("pages"))
            assert_record (page);
        const auto read = repository.read_relocation_inventory (fence, digest).result ().value ();
        ASSERT_TRUE (read);
        EXPECT_EQ (read->size (), participants.size ());
        EXPECT_FALSE (
          repository.read_relocation_inventory (fence, std::string (64, '0')).result ().value ());
    }
}

TEST (AuthorityAggregateRelocation, SharedRecordsRoundTripAndMalformedRecordsFailClosed)
{
    const auto golden = golden_records ();
    for (const auto &item : golden.at ("cases")) {
        SCOPED_TRACE (item.at ("name").get<std::string> ());
        const auto &aggregate = item.at ("aggregate");
        if (item.at ("expect") == "reject") {
            EXPECT_THROW ((void) authority_aggregate_codec::read (aggregate),
                          std::invalid_argument);
        } else {
            const auto decoded = authority_aggregate_codec::read (aggregate);
            EXPECT_EQ (decoded->encoded.dump (), aggregate.dump ());
            if (decoded->relocation) {
                const auto rewritten =
                  authority_aggregate_codec::replace (decoded, aggregate.at ("relocation"));
                EXPECT_EQ (rewritten->encoded.dump (), aggregate.dump ());
            }
        }
    }
}

TEST (AuthorityAggregateRelocation, ForeignPrecommitHoldsItsOriginalSourceFence)
{
    const nlohmann::json golden = golden_records ();
    const auto &cases = golden.at ("cases");
    for (const auto phase : {"preparing", "captured", "prepared"}) {
        SCOPED_TRACE (phase);
        const auto match = std::find_if (cases.begin (), cases.end (), [phase] (const auto &item) {
            return item.at ("name") == std::string ("relocation-only-") + phase;
        });
        ASSERT_NE (match, cases.end ());
        auto aggregate = match->at ("aggregate");
        const bool prepared = std::string_view (phase) == "prepared";
        if (prepared)
            aggregate["relocation"]["target"]["ownerLeaseGeneration"] = "8";

        in_memory_location_store_t provider;
        provider_location_repository_t repository (provider);
        public_authority_store_adapter_t adapter (repository);
        const store_key_t key{std::string{"authority\0actor\0golden-actor", 28}};
        nlohmann::json row = {
          {"recordVersion", 1},
          {"payload", ""},
          {"objectGeneration", "1"},
          {"authorityOwnerGeneration", "1"},
          {"ownerId", "owner-src"},
          {"ownerLeaseGeneration", "7"},
          {"allocation",
           {{"state", "active"},
            {"objectKind", "actor"},
            {"stableType", "Game.Actor"},
            {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "0a0b"}}},
            {"descriptorLifecycleGeneration", "3"},
            {"capacity", {{"actors", 1}, {"spots", 0}, {"spotType", nullptr}}}}},
          {"pendingCreation", nullptr},
          {"aggregate", aggregate}};
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider.write ({{}, {store_put_t{key, bytes (row.dump ()), std::nullopt}}})
            .result ()
            .value ()));
        relocation_authority_fence_t fence{object_kind_t::actor,
                                           "golden-actor",
                                           "opaque-v1",
                                           {"owner-src", 7},
                                           {"owner-tgt", 8},
                                           {0x0f1e2d3c4b5a4968, 0x8778a1b2c3d4e5f6},
                                           {0x0a, 0x0b},
                                           3,
                                           2};
        EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::unsettled);
        const auto authority_key = actor_authority_key ("golden-actor");
        const auto read = repository.read_authority (authority_key).result ().value ();
        const auto *authority = std::get_if<authority_snapshot_t> (&read);
        ASSERT_NE (authority, nullptr);
        const auto rewritten =
          repository
            .compare_exchange_authority (authority_key, authority->store_version,
                                         authority_restore_t{{}, {"owner-src", 7}})
            .result ()
            .value ();
        EXPECT_TRUE (std::holds_alternative<authority_stored_t> (rewritten));
        const auto persisted = std::get<store_found_t> (provider.read (key).result ().value ());
        const auto &persisted_bytes = persisted.value.bytes;
        const auto *persisted_data = reinterpret_cast<const char *> (persisted_bytes.data ());
        const auto persisted_row =
          nlohmann::json::parse (persisted_data, persisted_data + persisted_bytes.size ());
        EXPECT_EQ (persisted_row.at ("aggregate"), aggregate);
        fence.source_node_generation = 4;
        EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
        fence.source_node_generation = 3;
        EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::unsettled);
        if (prepared) {
            ++fence.target_attempt_generation;
            EXPECT_EQ (adapter.observe_relocation (fence),
                       relocation_authority_t::source_preserved);
            --fence.target_attempt_generation;
        }
        fence.expected_store_version = "wrong-version";
        EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
    }
}
TEST (AuthorityAggregateRelocation, ForeignCommittedRecordFencesItsTargetAttempt)
{
    const nlohmann::json golden = golden_records ();
    const auto &cases = golden.at ("cases");
    const auto committed = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "marker-and-committed";
    });
    ASSERT_NE (committed, cases.end ());

    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    public_authority_store_adapter_t adapter (repository);
    const store_key_t key{std::string{"authority\0actor\0golden-actor", 28}};
    auto aggregate = committed->at ("aggregate");
    // The storage codec accepts a full-range record token. The repository's
    // live owner lease counter is bounded, so use a live counter value here.
    aggregate["relocation"]["target"]["ownerLeaseGeneration"] = "8";
    aggregate["authorityPayloadSha256"] = zlink::framework::detail::sha256_hex ("");
    const auto id = aggregate.at ("aggregateId").get<std::string> ();
    const auto generation = aggregate.at ("aggregateGeneration").get<std::string> ();
    const auto suffix = id + ":" + generation;
    const nlohmann::json stored_aggregate{
      {"aggregateId", id},
      {"aggregateGeneration", generation},
      {"state", "committed"},
      {"targetDescriptor", {{"meshName", "mesh"}, {"routingIdHex", "0c0d"}}},
      {"targetDescriptorLifecycleGeneration", "5"},
      {"targetOwner", {{"ownerId", "owner-tgt"}, {"ownerLeaseGeneration", "8"}}}};
    const nlohmann::json row = {
      {"recordVersion", 1},
      {"payload", ""},
      {"objectGeneration", "1"},
      {"authorityOwnerGeneration", "12"},
      {"ownerId", "owner-tgt"},
      {"ownerLeaseGeneration", "8"},
      {"allocation",
       {{"state", "active"},
        {"objectKind", "actor"},
        {"stableType", "Game.Actor"},
        {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "0c0d"}}},
        {"descriptorLifecycleGeneration", "5"},
        {"capacity", {{"actors", 1}, {"spots", 0}, {"spotType", nullptr}}}}},
      {"pendingCreation", nullptr},
      {"visibleStoreVersion", aggregate.at ("expectedStoreVersion")},
      {"aggregate", aggregate}};
    const auto index = aggregate.at ("index").get<std::size_t> ();
    store_write_request_t seed;
    seed.mutations = {
      store_put_t{key, bytes (row.dump ()), {}},
      store_put_t{{"zlink:v11:aggregate:" + suffix}, bytes (stored_aggregate.dump ()), {}},
      store_put_t{{"zlink:v11:aggregate-inventory:" + suffix
                   + ":participant:" + std::to_string (index) + ":authorityPayload"},
                  {},
                  {}}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write (std::move (seed)).result ().value ()));
    relocation_authority_fence_t fence{
      object_kind_t::actor, "golden-actor",   "opaque-v1",
      {"owner-src", 7},     {"owner-tgt", 8}, {0x0f1e2d3c4b5a4968, 0x8778a1b2c3d4e5f6}};
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::target_committed);
    ++fence.relocation.low;
    EXPECT_EQ (adapter.observe_relocation (fence), relocation_authority_t::source_preserved);
}

class ActorAuthorityRelocation : public ::testing::Test
{
  protected:
    const std::string actor_id = "relocating-actor";
    const authority_key_t key = actor_authority_key (actor_id);
    in_memory_location_store_t provider;
    provider_location_repository_t repository{provider};
    in_memory_relocation_store_t blob_provider;
    provider_relocation_repository_t blobs{blob_provider};
    std::shared_ptr<public_authority_store_adapter_t> authority =
      std::make_shared<public_authority_store_adapter_t> (repository);
    stateful_object_runtime_t objects;
    maintenance_runtime_t runtime{objects, authority,
                                  std::make_shared<public_relocation_store_adapter_t> (blobs)};
    object_creation_target_t source;
    object_creation_target_t target;
    authority_snapshot_t original;
    object_ref_t actor;
    unsigned captures = 0;
    unsigned prepares = 0;

    object_creation_target_t host (const std::string &rid, std::uint64_t generation)
    {
        const auto lease = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease (rid, 30s).result ().value ())
                             .token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = generation;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = lease.owner_id;
        descriptor.lease_generation = lease.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.capacity.actors.limit = 2;
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types = {
          {placement_object_kind_t::user_spot, "Game.Spot", {0, 0, 1}}};
        descriptor.object_capabilities = {{placement_object_kind_t::actor, "Game.Actor",
                                           maintenance_policy_kind_t::recreate, false, 0},
                                          {placement_object_kind_t::user_spot, "Game.Spot",
                                           maintenance_policy_kind_t::snapshot, true, 1}};
        EXPECT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        return {descriptor.mesh_name, node_rid_t::from_string (rid), generation, lease};
    }

    void SetUp () override
    {
        source = host ("s", 3);
        target = host ("t", 4);
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, actor_id};
        request.intent.stable_type = "Game.Actor";
        request.target = source;
        request.capacity_bundle.actor_slots = 1;
        request.creating_payload = bytes ("creating");
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reservation, nullptr);
        const auto committed =
          repository.commit ({request.key, reservation->fence, payload_for (source)})
            .result ()
            .value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        ASSERT_NE (ready, nullptr);
        original = ready->ready;
        actor = {object_kind_t::actor,
                 actor_id,
                 original.object_generation,
                 original.authority_owner_generation,
                 "mesh",
                 "s"};
        const auto created =
          zlink::framework::test::begin_reserved_object (objects, actor, "Game.Actor", {});
        ASSERT_EQ (created.error, stateful_error_t::none);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful_error_t::none);
    }

    std::vector<std::byte> payload_for (const object_creation_target_t &placement) const
    {
        return encode_actor_authority_payload (
          {.stable_type = "Game.Actor",
           .actor_id = actor_id,
           .current_spot_id = "entry",
           .current_spot_generation = placement.node_lifecycle_generation,
           .current_spot_kind = actor_authority_spot_kind_t::entry,
           .owner_id = placement.owner.owner_id,
           .owner_lease_generation = static_cast<std::uint64_t> (placement.owner.lease_generation),
           .mesh_name = placement.mesh_name,
           .node_rid = placement.node_rid,
           .node_generation = placement.node_lifecycle_generation});
    }

    authority_snapshot_t read ()
    {
        return std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ());
    }

    eligible_relocation_unit_t::canonical_wire_context_t wire ()
    {
        return {.relocation = {1, 2},
                .target_attempt_generation = 1,
                .coordinator = {source.owner.owner_id,
                                static_cast<std::uint64_t> (source.owner.lease_generation),
                                {'s'},
                                3,
                                original.store_version},
                .target_node_routing_id = {'t'},
                .target_node_generation = 4,
                .application_version = 1,
                .prepare_target = [this] (const auto &,
                                          const auto &) -> task_t<stateful_relocation_reason_t> {
                    ++prepares;
                    co_return stateful_relocation_reason_t::none;
                },
                .send_state_chunk = [] (const auto &) -> task_t<bool> { co_return true; },
                .send_relocation_data = [] (const auto &, const auto &) -> task_t<bool> {
                    co_return true;
                },
                .send_cutover = [] (const auto &) -> task_t<bool> { co_return true; },
                .abort_target_before_cutover = [] { return true; }};
    }

    zlink::framework::runtime::protocol::relocation_prepare_t
    target_prepare (const eligible_relocation_unit_t::canonical_wire_context_t &context) const
    {
        return {.relocation = context.relocation,
                .target_attempt_generation = context.target_attempt_generation,
                .coordinator = context.coordinator,
                .target = {{'t'},
                           4,
                           target.owner.owner_id,
                           static_cast<std::uint64_t> (target.owner.lease_generation)},
                .source_node_routing_id = {'s'},
                .source_node_generation = 3,
                .application_version = context.application_version};
    }

    authority_publish_result_t
    publish (const zlink::framework::runtime::protocol::relocation_prepare_t &attempt)
    {
        auto placed = actor;
        placed.node_id = "t";
        return authority->publish (actor, placed, target.owner, target, "direct-test", 0, {},
                                   payload_for (target), original.store_version, attempt.relocation,
                                   source.owner, &attempt);
    }

    void capture_until (bool fail)
    {
        objects.configure_relocation_state (
          [this, fail] (const auto &, const auto &,
                        std::stop_token) -> task_t<frozen_object_state_t> {
              ++captures;
              const auto snapshot = read ();
              EXPECT_NE (snapshot.store_version, original.store_version);
              EXPECT_EQ (snapshot.authority_owner_generation, original.authority_owner_generation);
              EXPECT_EQ (snapshot.owner.owner_id, source.owner.owner_id);
              if (snapshot.aggregate && snapshot.aggregate->relocation) {
                  const auto &record = snapshot.aggregate->encoded.at ("relocation");
                  EXPECT_EQ (record.at ("phase"), "preparing");
                  EXPECT_TRUE (record.at ("target").is_null ());
                  EXPECT_EQ (record.at ("coordinator").at ("expectedStoreVersion"),
                             original.store_version);
              } else {
                  ADD_FAILURE () << "Capture began without a durable Preparing record";
              }
              if (fail)
                  throw std::runtime_error ("scripted application capture failure");
              co_return frozen_object_state_t{};
          },
          [] (const auto &, const auto &, std::stop_token) -> task_t<bool> { co_return true; });
    }

    void expect_source_preserved ()
    {
        const auto snapshot = read ();
        EXPECT_EQ (snapshot.payload, original.payload);
        EXPECT_EQ (snapshot.authority_owner_generation, original.authority_owner_generation);
        EXPECT_EQ (snapshot.owner.owner_id, source.owner.owner_id);
        EXPECT_FALSE (snapshot.aggregate);
        EXPECT_EQ (objects.enqueue (actor, turn_domain_t::application, {2, {0x42}}),
                   stateful_error_t::none);
    }
};

class SpotWideAuthorityRelocation : public ActorAuthorityRelocation
{
  protected:
    const authority_key_t spot_key = spot_authority_key ("golden-spot");
    authority_snapshot_t spot_original;
    object_ref_t spot;
    zlink::framework::runtime::protocol::relocation_prepare_t prepare;

    void SetUp () override
    {
        ActorAuthorityRelocation::SetUp ();
        if (HasFatalFailure ())
            return;
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::user_spot, "golden-spot"};
        request.intent.stable_type = "Game.Spot";
        request.target = source;
        request.capacity_bundle = {
          0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "Game.Spot", 1}};
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (reservation, nullptr);
        const auto payload = encode_user_spot_authority_payload (
          {.state = user_spot_authority_state_t::ready,
           .stable_type = "Game.Spot",
           .spot_id = "golden-spot",
           .owner_id = source.owner.owner_id,
           .owner_lease_generation = static_cast<std::uint64_t> (source.owner.lease_generation),
           .mesh_name = source.mesh_name,
           .node_rid = source.node_rid,
           .node_generation = source.node_lifecycle_generation});
        const auto committed =
          repository.commit ({request.key, reservation->fence, payload}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        ASSERT_NE (ready, nullptr);
        spot_original = ready->ready;
        spot = {object_kind_t::user_spot,
                "golden-spot",
                spot_original.object_generation,
                spot_original.authority_owner_generation,
                "mesh",
                "s"};
        prepare = target_prepare (wire ());
        prepare.coordinator.expected_authority_store_version = spot_original.store_version;
        prepare.object = {zlink::framework::runtime::protocol::relocation_object_kind_t::user_spot,
                          "Game.Spot", spot.key, spot.object_generation,
                          spot.authority_owner_generation};

        const auto golden = golden_records ();
        const auto &cases = golden.at ("cases");
        const auto captured = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
            return item.at ("name") == "relocation-only-captured";
        });
        ASSERT_NE (captured, cases.end ());
        auto aggregate = captured->at ("aggregate");
        auto &record = aggregate.at ("relocation");
        record["relocationId"] = authority_aggregate_codec::wire_uuid (1, 2);
        record["source"] = {
          {"nodeRid", "73"},
          {"nodeGeneration", "3"},
          {"ownerId", source.owner.owner_id},
          {"ownerLeaseGeneration", std::to_string (source.owner.lease_generation)}};
        record["coordinator"] = record.at ("source");
        record["coordinator"]["expectedStoreVersion"] = spot_original.store_version;
        const auto target_spot_payload = encode_user_spot_authority_payload (
          {.state = user_spot_authority_state_t::ready,
           .stable_type = "Game.Spot",
           .spot_id = "golden-spot",
           .owner_id = target.owner.owner_id,
           .owner_lease_generation = static_cast<std::uint64_t> (target.owner.lease_generation),
           .mesh_name = target.mesh_name,
           .node_rid = target.node_rid,
           .node_generation = target.node_lifecycle_generation});
        const std::vector<aggregate_participant_t> participants{
          {key,
           original.store_version,
           authority_generation_transition_t::new_owner,
           payload_for (target),
           {}},
          {spot_key,
           spot_original.store_version,
           authority_generation_transition_t::new_owner,
           target_spot_payload,
           {}}};
        using json_t = nlohmann::json;
        json_t entries = json_t::array ();
        for (const auto &participant : participants)
            entries.push_back (
              {{"index", entries.size ()},
               {"authorityKey", participant.key.value},
               {"expectedStoreVersion", participant.expected_store_version},
               {"ownerTransition", "newOwner"},
               {"authorityPayloadSha256",
                zlink::framework::detail::encode_hex (sha256 (participant.authority_payload))},
               {"membershipMutationSha256", zlink::framework::detail::sha256_hex ("[]")}});
        const json_t page{{"level", 0},      {"index", 0},         {"startIndex", 0},
                          {"entryCount", 2}, {"entries", entries}, {"children", json_t::array ()}};
        const json_t reference{{"level", 0},
                               {"index", 0},
                               {"startIndex", 0},
                               {"entryCount", 2},
                               {"sha256", zlink::framework::detail::sha256_hex (page.dump ())}};
        json_t root{{"totalCount", 2}, {"topLevel", 0}, {"topPages", json_t::array ({reference})}};
        const auto digest = zlink::framework::detail::sha256_hex (root.dump ());
        root["digestSha256"] = digest;
        record["sourceInventorySha256"] = digest;
        const auto relocation_id =
          authority_aggregate_codec::wire_uuid (prepare.relocation.high, prepare.relocation.low);
        const auto prefix = "zlink:v11:aggregate-inventory:"
                            + aggregate_inventory::key_suffix (
                              relocation_id, aggregate_inventory::sealed_inventory_generation);
        store_write_request_t seal;
        seal.mutations.push_back (store_put_t{{prefix + ":root"}, bytes (root.dump ()), {}});
        seal.mutations.push_back (store_put_t{{prefix + ":page:0:0"}, bytes (page.dump ()), {}});
        for (std::size_t index = 0; index < participants.size (); ++index) {
            const auto key_prefix = prefix + ":participant:" + std::to_string (index);
            seal.mutations.push_back (store_put_t{
              {key_prefix + ":authorityPayload"}, participants[index].authority_payload, {}});
            seal.mutations.push_back (
              store_put_t{{key_prefix + ":membershipMutation"}, bytes ("[]"), {}});
        }
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider.write (std::move (seal)).result ().value ()));
        authority_put_t put{spot_original.payload};
        put.replace_aggregate = authority_aggregate_codec::read (std::move (aggregate));
        ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
          repository
            .compare_exchange_authority (spot_key, spot_original.store_version, std::move (put))
            .result ()
            .value ()));
        ASSERT_FALSE (read ().aggregate);
    }

    aggregate_publish_result_t prepare_aggregate ()
    {
        public_aggregate_authority_adapter_t adapter (repository);
        return adapter.prepare ({actor, spot}, "t", target.owner, "direct:1:2:1", 0, {},
                                relocation_authority_fence_t::from_prepare (spot, prepare));
    }
};

TEST_F (SpotWideAuthorityRelocation, AcceptsForeignRepresentativeCaptureWithSteadyMember)
{
    ASSERT_TRUE (authority->record_target_prepared (spot, prepare).result ().value ());
    const auto result = prepare_aggregate ();
    ASSERT_EQ (result.status, aggregate_publish_status_t::prepared);
    public_aggregate_authority_adapter_t adapter (repository);
    EXPECT_EQ (adapter.commit (result.fence).status, aggregate_publish_status_t::committed);
    EXPECT_EQ (read ().owner.owner_id, target.owner.owner_id);
    EXPECT_NE (read ().store_version, original.store_version);
}

TEST_F (SpotWideAuthorityRelocation, RejectsMemberWithOnlyStoreVersionChanged)
{
    ASSERT_TRUE (authority->record_target_prepared (spot, prepare).result ().value ());
    const auto changed =
      repository
        .compare_exchange_authority (key, original.store_version, authority_put_t{original.payload})
        .result ()
        .value ();
    const auto *updated = std::get_if<authority_stored_t> (&changed);
    ASSERT_NE (updated, nullptr);
    ASSERT_NE (updated->snapshot.store_version, original.store_version);
    ASSERT_EQ (updated->snapshot.payload, original.payload);
    ASSERT_EQ (updated->snapshot.authority_owner_generation, original.authority_owner_generation);
    EXPECT_EQ (prepare_aggregate ().status, aggregate_publish_status_t::conflict);
}

TEST_F (SpotWideAuthorityRelocation, ProviderWritesCanonicalInventoryAndAuthorityMarkers)
{
    ASSERT_TRUE (authority->record_target_prepared (spot, prepare).result ().value ());
    const auto result = prepare_aggregate ();
    ASSERT_EQ (result.status, aggregate_publish_status_t::prepared);
    const auto lock_rows = provider.scan ({"zlink:v11:aggregate-lock:", {}, 10}).result ().value ();
    EXPECT_TRUE (std::get<store_scan_page_t> (lock_rows).items.empty ());
    const auto row =
      provider.read ({std::string{"authority\0actor\0", 16} + actor_id}).result ().value ();
    const auto *found = std::get_if<store_found_t> (&row);
    ASSERT_NE (found, nullptr);
    const auto encoded = nlohmann::json::parse (found->value.bytes);
    EXPECT_TRUE (encoded.contains ("aggregate"));
    EXPECT_EQ (encoded.value ("visibleStoreVersion", ""), original.store_version);
    const auto id = zlink::framework::detail::encode_uuid (
      std::bit_cast<std::array<std::uint8_t, 16>> (result.fence.durable_fence.aggregate_id.value));
    const auto generation = std::to_string (result.fence.durable_fence.aggregate_generation);
    const auto suffix = id + ":" + generation;
    const auto root =
      provider.read ({"zlink:v11:aggregate-inventory:" + suffix + ":root"}).result ().value ();
    const auto *root_row = std::get_if<store_found_t> (&root);
    ASSERT_NE (root_row, nullptr);
    const auto digest = nlohmann::json::parse (root_row->value.bytes).at ("digestSha256");
    const auto aggregate = provider.read ({"zlink:v11:aggregate:" + suffix}).result ().value ();
    const auto *aggregate_row = std::get_if<store_found_t> (&aggregate);
    ASSERT_NE (aggregate_row, nullptr);
    const nlohmann::json expected{
      {"aggregateId", id},
      {"aggregateGeneration", generation},
      {"state", "prepared"},
      {"participantCount", 2},
      {"inventoryDigestSha256", digest},
      {"targetDescriptor", {{"meshName", target.mesh_name}, {"routingIdHex", "74"}}},
      {"targetDescriptorLifecycleGeneration", std::to_string (target.node_lifecycle_generation)},
      {"targetOwner",
       {{"ownerId", target.owner.owner_id},
        {"ownerLeaseGeneration", std::to_string (target.owner.lease_generation)}}},
      {"capacity",
       {{"actors", 1},
        {"spots", 1},
        {"spotType", {{"objectKind", "userSpot"}, {"stableType", "Game.Spot"}, {"count", 1}}}}}};
    EXPECT_EQ (aggregate_row->value.bytes, bytes (expected.dump ()));
    const auto &marker = encoded.at ("aggregate");
    EXPECT_EQ (marker.at ("aggregateId"), id);
    EXPECT_EQ (marker.at ("aggregateGeneration"), generation);
    EXPECT_EQ (marker.at ("expectedStoreVersion"), original.store_version);
}

TEST_F (ActorAuthorityRelocation, CaptureFailurePreservesPreparingOwner)
{
    ASSERT_EQ (objects.enqueue (actor, turn_domain_t::application, {1, {0x41}}),
               stateful_error_t::none);
    capture_until (true);
    const auto result =
      runtime.relocate (actor, "t", target.owner, 1024, {}, wire ()).result ().value ();
    EXPECT_EQ (result.terminal, relocation_terminal_t::blocked);
    EXPECT_EQ (captures, 1u);
    EXPECT_EQ (prepares, 0u);
    expect_source_preserved ();
    const auto [claimed, pending] = objects.try_claim (actor, turn_domain_t::application);
    ASSERT_EQ (claimed, stateful_error_t::none);
    ASSERT_TRUE (pending);
    EXPECT_EQ (pending->sequence, 1u);
    EXPECT_EQ (objects.complete_claim (actor, turn_domain_t::application), stateful_error_t::none);
}

TEST_F (ActorAuthorityRelocation, PreparingConflictStopsCapture)
{
    capture_until (true);
    auto stale = wire ();
    stale.coordinator.expected_authority_store_version = "stale-version";
    const auto result =
      runtime.relocate (actor, "t", target.owner, 1024, {}, stale).result ().value ();
    EXPECT_EQ (result.terminal, relocation_terminal_t::store_failed);
    EXPECT_EQ (captures, 0u);
    EXPECT_EQ (prepares, 0u);
    EXPECT_EQ (read ().store_version, original.store_version);
    expect_source_preserved ();
}

TEST_F (ActorAuthorityRelocation, CapturedUsesVersionReturnedByPreparing)
{
    objects.configure_relocation_state (
      [this] (const auto &, const auto &, std::stop_token) -> task_t<frozen_object_state_t> {
          ++captures;
          const auto preparing = read ();
          EXPECT_TRUE (preparing.aggregate && preparing.aggregate->relocation);
          const auto intervening = co_await repository.compare_exchange_authority (
            key, preparing.store_version, authority_put_t{preparing.payload});
          EXPECT_TRUE (std::holds_alternative<authority_stored_t> (intervening));
          co_return frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> task_t<bool> { co_return true; });
    const auto result =
      runtime.relocate (actor, "t", target.owner, 1024, {}, wire ()).result ().value ();
    EXPECT_EQ (result.terminal, relocation_terminal_t::store_failed);
    EXPECT_EQ (captures, 1u);
    EXPECT_EQ (prepares, 0u);
    expect_source_preserved ();
}

TEST_F (ActorAuthorityRelocation, PreparedTargetFenceControlsCommit)
{
    capture_until (false);
    auto context = wire ();
    const auto prepare = target_prepare (context);
    context.prepare_target = [&] (const auto &,
                                  const auto &) -> task_t<stateful_relocation_reason_t> {
        ++prepares;
        const auto captured = read ();
        if (!captured.aggregate || !captured.aggregate->relocation) {
            ADD_FAILURE () << "PREPARE began without a durable Captured record";
            co_return stateful_relocation_reason_t::restore_failed;
        }
        nlohmann::json expected = captured.aggregate->encoded.at ("relocation");
        EXPECT_EQ (expected.at ("phase"), "captured");
        EXPECT_TRUE (expected.at ("target").is_null ());
        if (!co_await authority->record_target_prepared (actor, prepare)) {
            ADD_FAILURE () << "Target could not write Prepared";
            co_return stateful_relocation_reason_t::restore_failed;
        }
        const auto prepared = read ();
        if (!prepared.aggregate || !prepared.aggregate->relocation) {
            ADD_FAILURE () << "Target readiness returned without a durable Prepared record";
            co_return stateful_relocation_reason_t::restore_failed;
        }
        expected["phase"] = "prepared";
        expected["target"] = {
          {"nodeRid", "74"},
          {"nodeGeneration", "4"},
          {"ownerId", target.owner.owner_id},
          {"ownerLeaseGeneration", std::to_string (target.owner.lease_generation)},
          {"attemptGeneration", "1"}};
        const nlohmann::json actual = prepared.aggregate->encoded.at ("relocation");
        EXPECT_EQ (actual, expected);
        EXPECT_EQ (prepared.owner.owner_id, source.owner.owner_id);
        EXPECT_EQ (prepared.authority_owner_generation, original.authority_owner_generation);
        EXPECT_EQ (prepared.payload, original.payload);
        EXPECT_NE (prepared.store_version, captured.store_version);
        const auto repeated = co_await authority->record_target_prepared (actor, prepare);
        EXPECT_TRUE (repeated);
        EXPECT_EQ (read ().store_version, prepared.store_version);
        co_return stateful_relocation_reason_t::none;
    };
    context.send_cutover = [&] (const auto &) -> task_t<bool> {
        auto stale_attempt = prepare;
        ++stale_attempt.target_attempt_generation;
        EXPECT_EQ (publish (stale_attempt).status, authority_publish_status_t::conflict);
        stale_attempt = prepare;
        ++stale_attempt.target.target_node_generation;
        EXPECT_EQ (publish (stale_attempt).status, authority_publish_status_t::conflict);
        EXPECT_EQ (publish (prepare).status, authority_publish_status_t::published);
        co_return true;
    };
    context.restore_deadline = std::chrono::steady_clock::time_point{};
    const auto result =
      runtime.relocate (actor, "t", target.owner, 1024, {}, context).result ().value ();
    EXPECT_EQ (result.terminal, relocation_terminal_t::completed);
    EXPECT_EQ (captures, 1u);
    EXPECT_EQ (prepares, 1u);
    const auto committed = read ();
    ASSERT_TRUE (committed.aggregate && committed.aggregate->relocation);
    EXPECT_EQ (committed.aggregate->relocation->phase, "committed");
    EXPECT_EQ (committed.owner.owner_id, target.owner.owner_id);
    EXPECT_EQ (committed.object_generation, original.object_generation);
    EXPECT_GT (committed.authority_owner_generation, original.authority_owner_generation);
}

TEST_F (ActorAuthorityRelocation, SteadySourceCannotRecordPrepared)
{
    const auto before = read ();
    ASSERT_FALSE (before.aggregate);
    const auto written =
      authority->record_target_prepared (actor, target_prepare (wire ())).result ().value ();
    EXPECT_FALSE (written);
    const auto after = read ();
    EXPECT_EQ (after.store_version, before.store_version);
    EXPECT_EQ (after.payload, before.payload);
    EXPECT_FALSE (after.aggregate);
}

TEST_F (ActorAuthorityRelocation, FailedReadyPreservesPreparedSource)
{
    capture_until (false);
    auto context = wire ();
    const auto prepare = target_prepare (context);
    context.prepare_target = [&] (const auto &,
                                  const auto &) -> task_t<stateful_relocation_reason_t> {
        ++prepares;
        const auto written = co_await authority->record_target_prepared (actor, prepare);
        EXPECT_TRUE (written);
        co_return stateful_relocation_reason_t::restore_failed;
    };
    const auto result =
      runtime.relocate (actor, "t", target.owner, 1024, {}, context).result ().value ();
    EXPECT_EQ (result.terminal, relocation_terminal_t::blocked);
    EXPECT_EQ (captures, 1u);
    EXPECT_EQ (prepares, 1u);
    expect_source_preserved ();
}

TEST_F (ActorAuthorityRelocation, ForeignPreparedTargetCanCommit)
{
    const auto golden = golden_records ();
    const auto &cases = golden.at ("cases");
    const auto match = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "relocation-only-prepared";
    });
    ASSERT_NE (match, cases.end ());
    auto aggregate = match->at ("aggregate");
    auto &record = aggregate.at ("relocation");
    record["relocationId"] = authority_aggregate_codec::wire_uuid (1, 2);
    record["source"] = {{"nodeRid", "73"},
                        {"nodeGeneration", "3"},
                        {"ownerId", source.owner.owner_id},
                        {"ownerLeaseGeneration", std::to_string (source.owner.lease_generation)}};
    record["coordinator"] = record.at ("source");
    record["coordinator"]["expectedStoreVersion"] = original.store_version;
    record["target"] = {{"nodeRid", "74"},
                        {"nodeGeneration", "4"},
                        {"ownerId", target.owner.owner_id},
                        {"ownerLeaseGeneration", std::to_string (target.owner.lease_generation)},
                        {"attemptGeneration", "1"}};
    authority_put_t put{original.payload};
    put.replace_aggregate = authority_aggregate_codec::read (std::move (aggregate));
    const auto exchanged =
      repository.compare_exchange_authority (key, original.store_version, std::move (put))
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (exchanged));
    auto context = wire ();
    context.application_version = 42;
    EXPECT_EQ (publish (target_prepare (context)).status, authority_publish_status_t::published);
}

TEST (AuthorityAggregateRelocation, SpotWideCommitConsumesForeignCapture)
{
    const auto golden = golden_records ();
    const auto &cases = golden.at ("cases");
    const auto captured = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "relocation-only-captured";
    });
    ASSERT_NE (captured, cases.end ());
    in_memory_location_store_t provider;
    zlink::framework::test::aggregate_read_keeps_source_version_until_commit (
      provider, [] (std::string_view value) { return bytes (value); }, captured->at ("aggregate"));
}

TEST (AuthorityAggregateRelocation, SpotWideCommitConsumesForeignPrepared)
{
    const auto golden = golden_records ();
    const auto &cases = golden.at ("cases");
    const auto prepared = std::find_if (cases.begin (), cases.end (), [] (const auto &item) {
        return item.at ("name") == "relocation-only-prepared";
    });
    ASSERT_NE (prepared, cases.end ());
    in_memory_location_store_t provider;
    zlink::framework::test::aggregate_read_keeps_source_version_until_commit (
      provider, [] (std::string_view value) { return bytes (value); }, prepared->at ("aggregate"));
}

}
