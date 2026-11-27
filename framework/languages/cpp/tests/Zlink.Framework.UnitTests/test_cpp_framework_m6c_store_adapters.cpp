/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_public_authority_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/stateful/public_store_adapters.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

namespace
{

class public_memory_relocation_repository_t final : public zlink::framework::relocation_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::relocation_stored_t>
    put_relocation (std::vector<std::byte> payload,
                    std::chrono::hours retention,
                    std::chrono::steady_clock::time_point,
                    std::stop_token) override
    {
        if (retention != std::chrono::hours (24))
            throw std::runtime_error ("unexpected retention");
        std::vector<std::uint8_t> checksum_input;
        checksum_input.reserve (payload.size ());
        for (const auto value : payload)
            checksum_input.push_back (std::to_integer<std::uint8_t> (value));
        const auto reference = "public-root";
        roots[reference] = std::move (payload);
        return completed (zlink::framework::relocation_stored_t{
          reference, maintenance_runtime_t::crc32c (checksum_input),
          std::chrono::system_clock::now () + retention, std::chrono::system_clock::now ()});
    }

    zlink::framework::task_t<zlink::framework::relocation_read_result_t>
    get_relocation (std::string reference, std::stop_token) override
    {
        const auto found = roots.find (reference);
        if (found == roots.end ())
            return completed (
              zlink::framework::relocation_read_result_t{zlink::framework::relocation_missing_t{}});
        return completed (zlink::framework::relocation_read_result_t{
          zlink::framework::relocation_found_t{found->second}});
    }

    zlink::framework::task_t<zlink::framework::relocation_renew_result_t>
    renew_relocation (std::string reference, std::chrono::hours retention, std::stop_token) override
    {
        if (!roots.contains (reference))
            return completed (zlink::framework::relocation_renew_result_t{
              zlink::framework::relocation_renew_missing_t{}});
        const auto now = std::chrono::system_clock::now ();
        return completed (zlink::framework::relocation_renew_result_t{
          zlink::framework::relocation_renewed_t{now + retention, now}});
    }

    zlink::framework::task_t<zlink::framework::relocation_delete_result_t>
    delete_relocation (std::string reference, std::stop_token) override
    {
        return completed (roots.erase (reference) > 0
                            ? zlink::framework::relocation_delete_result_t::deleted
                            : zlink::framework::relocation_delete_result_t::missing);
    }

  private:
    template <typename T> static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }

    std::map<std::string, std::vector<std::byte>> roots;
};

} // namespace

void test_public_relocation_store_adapter (test_context_t &test)
{
    auto public_store = std::make_shared<public_memory_relocation_repository_t> ();
    public_relocation_store_adapter_t adapter (public_store);
    const std::vector<std::uint8_t> payload{0, 1, 127, 255};
    const auto stored = adapter.put (payload, std::chrono::hours (24),
                                     std::chrono::steady_clock::now () + std::chrono::minutes (1));
    test.require (stored.reference == "public-root"
                    && stored.checksum_crc32c == maintenance_runtime_t::crc32c (payload),
                  "public relocation adapter must preserve reference and CRC32C");
    test.require (adapter.get (stored.reference)
                    == std::optional<std::vector<std::uint8_t>>{payload},
                  "public relocation adapter must preserve immutable payload bytes");
    adapter.remove (stored.reference);
    test.require (!adapter.get (stored.reference),
                  "public relocation adapter must map delete and missing results");
}

void test_public_authority_store_adapter (test_context_t &test)
{
    {
        namespace runtime = zlink::framework::runtime;
        const auto now = std::chrono::system_clock::now ();
        public_memory_authority_store_t spots;
        const zlink::framework::location_owner_token_t source_owner{"source-owner", 1};
        const zlink::framework::location_owner_token_t target_owner{"target-owner", 2};
        const auto payload = runtime::encode_user_spot_authority_payload (
          {.state = runtime::user_spot_authority_state_t::ready,
           .stable_type = "matchmaker",
           .spot_id = "matchmaking",
           .owner_id = source_owner.owner_id,
           .owner_lease_generation = 1,
           .mesh_name = "mesh",
           .node_rid = zlink::framework::node_rid_t::from_string ("source"),
           .node_generation = 1});
        spots.snapshot =
          zlink::framework::authority_snapshot_t{"1", payload, 1, 1, source_owner, now};
        spots.snapshot->allocation.state = zlink::framework::placement_allocation_state_t::active;
        spots.snapshot->allocation.object_kind =
          zlink::framework::placement_object_kind_t::user_spot;
        spots.snapshot->allocation.stable_type = "matchmaker";
        spots.snapshot->allocation.target = {
          "mesh", zlink::framework::node_rid_t::from_string ("source"), 1, source_owner};
        spots.owner_lease_read =
          zlink::framework::owner_lease_found_t{source_owner, now + std::chrono::minutes (1), now};
        runtime::store_location_resolvers_t before (spots);
        test.require (
          before.resolve_spot_address ("mesh", "matchmaking").result ().value ().has_value (),
          "Ready Spot must resolve before relocation");
        public_authority_store_adapter_t adapter (spots);
        const object_ref_t source{object_kind_t::user_spot, "matchmaking", 1, 1, "mesh", "source"};
        auto target = source;
        target.node_id = "target";
        const zlink::framework::object_creation_target_t placement{
          "mesh", zlink::framework::node_rid_t::from_string ("target"), 2, target_owner};
        const auto published =
          adapter.publish (source, target, target_owner, placement, "spot-root", 42,
                           digest_with (9), {}, "1", {}, source_owner);
        test.require (published.status == authority_publish_status_t::published,
                      "single Spot authority must commit to target");
        // Complete the test repository's placement projection as the real Store does.
        spots.snapshot->allocation.target = placement;
        spots.owner_lease_read =
          zlink::framework::owner_lease_found_t{target_owner, now + std::chrono::minutes (1), now};
        runtime::store_location_resolvers_t after (spots);
        const auto located = after.resolve_spot_address ("mesh", "matchmaking").result ().value ();
        test.require (
          located && located->node_rid.to_string () == "target" && located->object_generation == 1
            && located->authority_owner_generation == spots.snapshot->authority_owner_generation,
          "fresh resolver must locate a committed single Spot before completion cleanup");
        runtime::store_location_resolvers_t other (spots);
        test.require (!other.resolve_spot_address ("mesh", "other-spot").result ().value (),
                      "relocation metadata must not authorize a different Spot ID");
        spots.snapshot->payload.pop_back ();
        runtime::store_location_resolvers_t malformed (spots);
        test.require (!malformed.resolve_spot_address ("mesh", "matchmaking").result ().value (),
                      "truncated relocation authority must not produce a Spot address");
    }

    public_memory_authority_store_t store;
    const auto source_application_payload =
      zlink::framework::runtime::encode_actor_authority_payload (
        {.state = zlink::framework::runtime::actor_authority_state_t::ready,
         .stable_type = "player",
         .actor_id = "actor-public",
         .current_spot_id = "source-spot",
         .current_spot_generation = 11,
         .current_spot_kind = zlink::framework::runtime::actor_authority_spot_kind_t::user,
         .owner_id = "owner-a",
         .owner_lease_generation = 3,
         .mesh_name = "mesh",
         .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
         .node_generation = 16});
    store.snapshot = zlink::framework::authority_snapshot_t{
      "1", source_application_payload, 7, 11, {"owner-a", 3}, std::chrono::system_clock::now ()};
    public_authority_store_adapter_t adapter (store);
    const object_ref_t source{object_kind_t::actor, "actor-public", 7, 11, "mesh", "node-a"};
    const zlink::framework::location_owner_token_t target_owner{"owner-b", 5};
    const zlink::framework::object_creation_target_t target_placement{
      "mesh-b", zlink::framework::node_rid_t::from_string ("node-b"), 17, target_owner};
    auto target = source;
    target.mesh_name = "mesh-b";
    target.node_id = "node-b";
    const auto relocated_application_payload =
      zlink::framework::runtime::encode_actor_authority_payload (
        {.state = zlink::framework::runtime::actor_authority_state_t::ready,
         .stable_type = "player",
         .actor_id = "actor-public",
         .current_spot_id = "target-spot",
         .current_spot_generation = 12,
         .current_spot_kind = zlink::framework::runtime::actor_authority_spot_kind_t::user,
         .owner_id = "owner-b",
         .owner_lease_generation = 5,
         .mesh_name = "mesh-b",
         .node_rid = zlink::framework::node_rid_t::from_string ("node-b"),
         .node_generation = 17});
    public_memory_authority_store_t conflicting_store;
    conflicting_store.snapshot = store.snapshot;
    conflicting_store.remaining_authority_conflicts = 1;
    public_authority_store_adapter_t conflicting_adapter (conflicting_store);
    const auto conflicted = conflicting_adapter.publish (
      source, target, target_owner, target_placement, "root-public", 42, digest_with (9),
      relocated_application_payload, store.snapshot->store_version, {}, store.snapshot->owner);
    test.require (
      conflicted.status == authority_publish_status_t::conflict && conflicted.current
        && conflicting_store.remaining_authority_conflicts == 0
        && conflicting_store.snapshot->store_version != store.snapshot->store_version
        && conflicting_store.snapshot->authority_owner_generation
             == source.authority_owner_generation
        && conflicting_store.snapshot->payload == source_application_payload,
      "public authority adapter must preserve a changed initial authority version as conflict");
    namespace actor_wire = zlink::framework::runtime::actor_authority_detail;
    const auto source_projection =
      zlink::framework::runtime::decode_direct_actor_authority_payload (source_application_payload);
    constexpr std::uint64_t relocation_id_high = 1;
    constexpr std::uint64_t relocation_id_low = 2;
    constexpr std::uint8_t absent_wire_field = 0;
    constexpr std::uint8_t present_wire_slot = 1;
    constexpr std::size_t optional_authority_slot_count = 2;
    std::vector<std::byte> captured_slot;
    actor_wire::append_u64be (captured_slot, relocation_id_high);
    actor_wire::append_u64be (captured_slot, relocation_id_low);
    actor_wire::append_u64be (captured_slot, source.object_generation);
    actor_wire::append_u64be (captured_slot,
                              absent_wire_field); // Source capture has no target attempt.
    actor_wire::append_text16be (captured_slot, "root-public");
    actor_wire::append_u32be (captured_slot, 42);
    actor_wire::append_text8 (captured_slot, source.node_id);
    actor_wire::append_u64be (captured_slot, source_projection->node_generation);
    actor_wire::append_text8 (captured_slot, store.snapshot->owner.owner_id);
    actor_wire::append_u64be (captured_slot, store.snapshot->owner.lease_generation);
    actor_wire::append_u8 (captured_slot, absent_wire_field); // Target node is absent.
    actor_wire::append_u64be (captured_slot, absent_wire_field);
    actor_wire::append_u8 (captured_slot, absent_wire_field); // Target owner is absent.
    actor_wire::append_u64be (captured_slot, absent_wire_field);
    actor_wire::append_text8 (captured_slot, store.snapshot->owner.owner_id);
    actor_wire::append_u64be (captured_slot, store.snapshot->owner.lease_generation);
    actor_wire::append_text8 (captured_slot, source.node_id);
    actor_wire::append_u64be (captured_slot, source_projection->node_generation);
    actor_wire::append_text8 (captured_slot, store.snapshot->store_version);
    constexpr std::uint8_t captured_phase = 2;
    actor_wire::append_u8 (captured_slot, captured_phase);
    actor_wire::append_u64be (captured_slot, absent_wire_field);
    actor_wire::append_u8 (captured_slot, absent_wire_field);
    auto captured_payload =
      zlink::framework::runtime::decode_canonical_authority_payload (source_application_payload);
    constexpr auto absent_slot_size = sizeof (std::uint8_t) + sizeof (std::uint32_t);
    captured_payload->body.resize (captured_payload->body.size ()
                                   - optional_authority_slot_count * absent_slot_size);
    actor_wire::append_u8 (captured_payload->body, present_wire_slot);
    actor_wire::append_u32be (captured_payload->body,
                              static_cast<std::uint32_t> (captured_slot.size ()));
    actor_wire::append_bytes (captured_payload->body, captured_slot);
    actor_wire::append_u8 (captured_payload->body, absent_wire_field); // Activation slot is absent.
    actor_wire::append_u32be (captured_payload->body, absent_wire_field);
    public_memory_authority_store_t captured_store;
    captured_store.snapshot = store.snapshot;
    captured_store.snapshot->store_version = "2";
    captured_store.snapshot->allocation.target = {source.mesh_name, source_projection->node_rid,
                                                  source_projection->node_generation,
                                                  captured_store.snapshot->owner};
    captured_store.snapshot->payload =
      zlink::framework::runtime::encode_canonical_authority_payload (*captured_payload);
    const auto captured_projection =
      zlink::framework::runtime::decode_direct_actor_authority_payload (
        captured_store.snapshot->payload);
    test.require (captured_projection && captured_projection->relocation_phase == captured_phase
                    && captured_projection->relocation_expected_store_version
                         == store.snapshot->store_version,
                  "captured authority fixture must retain the original coordinator version");
    public_authority_store_adapter_t captured_adapter (captured_store);
    const auto captured_conflict = captured_adapter.publish (
      source, target, target_owner, target_placement, "root-public", 42, digest_with (9),
      relocated_application_payload, store.snapshot->store_version);
    test.require (captured_conflict.status == authority_publish_status_t::conflict
                    && captured_store.snapshot->authority_owner_generation
                         == source.authority_owner_generation
                    && !captured_store.observed_target_owner,
                  "captured source metadata must not replace the initial authority version fence");
    const relocation_authority_fence_t captured_fence{
      source.kind,           source.key,   store.snapshot->store_version,
      store.snapshot->owner, target_owner, {relocation_id_high, relocation_id_low}};
    test.require (captured_adapter.observe_relocation (captured_fence)
                    == relocation_authority_t::unsettled,
                  "matching Captured source must keep target staging eligible");
    auto other_relocation = captured_fence;
    ++other_relocation.relocation.low;
    test.require (captured_adapter.observe_relocation (other_relocation)
                    == relocation_authority_t::source_preserved,
                  "another relocation must not inherit the captured source fence");
    auto other_owner = captured_fence;
    other_owner.source_owner.lease_generation++;
    test.require (captured_adapter.observe_relocation (other_owner)
                    == relocation_authority_t::source_preserved,
                  "another source lease must not inherit the captured source fence");
    for (const auto changed : {"owner", "lease", "baseline", "empty-baseline"}) {
        auto changed_fence = captured_fence;
        if (std::string_view (changed) == "owner")
            changed_fence.source_owner.owner_id += "-other";
        if (std::string_view (changed) == "lease")
            ++changed_fence.source_owner.lease_generation;
        if (std::string_view (changed) == "baseline")
            changed_fence.expected_store_version = captured_store.snapshot->store_version;
        if (std::string_view (changed) == "empty-baseline")
            changed_fence.expected_store_version.clear ();
        test.require (captured_adapter.observe_relocation (changed_fence)
                        == relocation_authority_t::source_preserved,
                      "Captured authority must reject a changed coordinator fence");
        const auto rejected = captured_adapter.publish (
          source, target, target_owner, target_placement, "root-public", 42, digest_with (9),
          relocated_application_payload, changed_fence.expected_store_version,
          changed_fence.relocation, changed_fence.source_owner);
        test.require (rejected.status == authority_publish_status_t::conflict
                        && captured_store.snapshot->store_version == "2"
                        && !captured_store.observed_target_owner,
                      "Captured authority must reject NewOwner before changing the source");
    }
    const auto captured_published = captured_adapter.publish (
      source, target, target_owner, target_placement, "root-public", 42, digest_with (9),
      relocated_application_payload, store.snapshot->store_version,
      {relocation_id_high, relocation_id_low}, store.snapshot->owner);
    test.require (captured_published.status == authority_publish_status_t::published
                    && captured_store.observed_target_owner,
                  "matching Captured source must use its current provider version for target CAS");
    store.retarget_authority_generation_advance = 9;
    const auto published = adapter.publish (
      source, target, target_owner, target_placement, "root-public", 42, digest_with (9),
      relocated_application_payload, store.snapshot->store_version, {}, store.snapshot->owner);
    test.require (
      published.status == authority_publish_status_t::published && published.current
        && published.current->source.mesh_name == "mesh"
        && published.current->target.mesh_name == "mesh-b"
        && published.current->target.node_id == "node-b"
        && published.current->target.authority_owner_generation == 20
        && published.current->application_payload == relocated_application_payload
        && store.remaining_authority_conflicts == 0,
      "public authority adapter must publish with an unchanged initial authority fence");
    const auto stored_projection = store.snapshot
                                     ? zlink::framework::runtime::decode_actor_authority_payload (
                                         store.snapshot->payload, store.snapshot->object_generation)
                                     : std::nullopt;
    const auto begins_with_zlra3 = [] (const std::vector<std::byte> &bytes) {
        constexpr std::string_view magic{"ZLRA3"};
        return bytes.size () >= magic.size ()
               && std::equal (magic.begin (), magic.end (), bytes.begin (),
                              [] (char expected, std::byte actual) {
                                  return static_cast<unsigned char> (expected)
                                         == std::to_integer<unsigned char> (actual);
                              });
    };
    test.require (store.snapshot && !begins_with_zlra3 (store.snapshot->payload)
                    && stored_projection && stored_projection->actor.object_generation () == 7
                    && stored_projection->actor.actor_id ().value () == "actor-public"
                    && stored_projection->actor.node_rid ().value () == "node-b",
                  "remote actor commit must store a canonical authority row resolvable by "
                  "actor_directory_t::find");
    test.require (
      store.observed_target_owner && store.observed_target_owner->owner_id == "owner-b"
        && store.observed_target_owner->lease_generation == 5 && store.observed_target_placement
        && store.observed_target_placement->mesh_name == "mesh-b"
        && store.observed_target_placement->node_rid.value () == "node-b"
        && store.observed_target_placement->node_lifecycle_generation == 17
        && store.snapshot->owner.owner_id == "owner-b" && !store.observed_keys.empty ()
        && std::all_of (store.observed_keys.begin (), store.observed_keys.end (),
                        [] (const auto &key) { return key == "zla1:a:12:actor-public"; }),
      "public authority adapter must pass the exact target placement");
    const auto read = adapter.read (object_kind_t::actor, "actor-public");
    test.require (read && read->target.key == "actor-public" && read->target.object_generation == 7
                    && read->target.node_id == "node-b" && read->target_owner.lease_generation == 5,
                  "public authority adapter must decode its canonical actor authority row");

    store.snapshot.reset ();
    test.require (adapter.observe_relocation (captured_fence)
                    == relocation_authority_t::source_lease_expired,
                  "target commit then authority removal must end the source as expired owner");
    test.require (adapter.settle_source_relocation (captured_fence, {read->source}, true).outcome
                    == relocation_authority_t::source_lease_expired,
                  "Missing must never attempt source Preserve");

    const std::vector<std::byte> application_payload{std::byte{0x31}, std::byte{0x32}};
    store.snapshot = zlink::framework::authority_snapshot_t{
      "10", application_payload, 7, 12, {"owner-b", 5}, std::chrono::system_clock::now ()};
    const auto completion_published = adapter.publish_completion (
      object_kind_t::actor, "actor-public", "mesh", 7, "completion-prepared", 51);
    const auto completion_replaced =
      adapter.replace_completion (object_kind_t::actor, "actor-public", 7, "completion-prepared",
                                  51, "completion-delivered", 52);
    const auto completion_read = adapter.read (object_kind_t::actor, "actor-public");
    test.require (
      completion_published.status == authority_publish_status_t::published
        && completion_replaced.status == authority_publish_status_t::published && completion_read
        && completion_read->relocation_reference == "completion-delivered"
        && completion_read->checksum_crc32c == 52 && completion_read->source.object_generation == 7
        && completion_read->source.authority_owner_generation == 12,
      "completion cursor roots must use exact preserve-generation authority CAS");
    const auto completion_released = adapter.release_completion (
      object_kind_t::actor, "actor-public", 7, "completion-delivered", 52);
    test.require (completion_released && store.snapshot
                    && store.snapshot->payload == application_payload
                    && store.snapshot->authority_owner_generation == 12
                    && !adapter.read (object_kind_t::actor, "actor-public"),
                  "Delivered release must restore authority payload before root cleanup");
}

} // namespace m6c_test
