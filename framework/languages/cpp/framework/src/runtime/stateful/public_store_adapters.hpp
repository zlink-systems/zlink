/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/stateful/maintenance_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/execution/task_result.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace zlink::framework::runtime::stateful
{

class public_aggregate_authority_adapter_t;

class public_relocation_store_adapter_t final : public relocation_store_port_t
{
  public:
    explicit public_relocation_store_adapter_t (
      std::shared_ptr<zlink::framework::relocation_repository_t> store) :
        _owner (std::move (store)), _store (_owner.get ())
    {
        if (!_store)
            throw std::invalid_argument ("relocation store must not be null");
    }

    explicit public_relocation_store_adapter_t (
      zlink::framework::relocation_repository_t &store) noexcept :
        _store (&store)
    {
    }

    relocation_stored_t put (const std::vector<std::uint8_t> &payload,
                             std::chrono::hours retention,
                             std::chrono::steady_clock::time_point operation_deadline) override
    {
        std::vector<std::byte> public_payload;
        public_payload.reserve (payload.size ());
        for (const auto value : payload)
            public_payload.push_back (static_cast<std::byte> (value));
        const auto stored = infrastructure_result ([&] {
                                return _store->put_relocation (std::move (public_payload),
                                                               retention, operation_deadline);
                            }).value ();
        return {stored.reference, stored.checksum_crc32c};
    }

    std::optional<std::vector<std::uint8_t>> get (const std::string &reference) override
    {
        const auto read =
          infrastructure_result ([&] { return _store->get_relocation (reference); }).value ();
        const auto *found = std::get_if<zlink::framework::relocation_found_t> (&read);
        if (!found)
            return std::nullopt;
        std::vector<std::uint8_t> payload;
        payload.reserve (found->payload.size ());
        for (const auto value : found->payload)
            payload.push_back (std::to_integer<std::uint8_t> (value));
        return payload;
    }

    void remove (const std::string &reference) override
    {
        (void) infrastructure_result ([&] {
            return _store->delete_relocation (reference);
        }).value ();
    }

  private:
    std::shared_ptr<zlink::framework::relocation_repository_t> _owner;
    zlink::framework::relocation_repository_t *_store;
};

class public_authority_store_adapter_t final : public authority_relocation_port_t
{
  public:
    explicit public_authority_store_adapter_t (
      zlink::framework::location_repository_t &store) noexcept :
        _store (&store)
    {
    }

    authority_publish_result_t publish (const object_ref_t &source,
                                        const object_ref_t &target,
                                        location_owner_token_t target_owner,
                                        object_creation_target_t target_placement,
                                        std::string relocation_reference,
                                        std::uint32_t checksum_crc32c,
                                        inventory_digest_t inventory_digest,
                                        std::vector<std::byte> target_application_payload = {},
                                        std::string expected_store_version = {},
                                        protocol::relocation_id_t relocation = {},
                                        location_owner_token_t source_owner = {}) override
    {
        if (target.kind != source.kind || target.key != source.key
            || target.object_generation != source.object_generation
            || target.authority_owner_generation != source.authority_owner_generation
            || target.mesh_name.empty () || target.node_id.empty ())
            return {authority_publish_status_t::failed, std::nullopt};
        const auto key = authority_key (source);
        const auto read =
          infrastructure_result ([&] { return _store->read_authority (key); }).value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot || snapshot->object_generation != source.object_generation
            || snapshot->authority_owner_generation != source.authority_owner_generation) {
            if (std::getenv ("ZLINK_CPP_AUTO_CONNECT_TRACE") != nullptr) {
                std::cerr << "zlink authority-publish stage=snapshot-mismatch key=" << source.key
                          << " expected-object=" << source.object_generation
                          << " expected-owner=" << source.authority_owner_generation;
                if (snapshot != nullptr) {
                    std::cerr << " snapshot-object=" << snapshot->object_generation
                              << " snapshot-owner=" << snapshot->authority_owner_generation;
                } else {
                    std::cerr << " snapshot=absent";
                }
                std::cerr << '\n';
            }
            return {authority_publish_status_t::conflict, decode_current (read)};
        }

        if (target_placement.mesh_name != target.mesh_name
            || target_placement.node_rid.value () != target.node_id
            || !same_owner (target_placement.owner, target_owner))
            return {authority_publish_status_t::failed, std::nullopt};
        if (!expected_store_version.empty () && snapshot->store_version != expected_store_version
            && !holds_captured_fence (*snapshot, source.key, relocation, source_owner,
                                      expected_store_version))
            return {authority_publish_status_t::conflict, decode_current (read)};
        const std::string cas_version = snapshot->store_version;
        std::vector<std::byte> application_payload = target_application_payload;
        authority_relocation_reference_t reference{
          source,
          source,
          relocation_reference,
          checksum_crc32c,
          inventory_digest,
          target_owner,
          application_payload.empty () ? snapshot->payload : std::move (application_payload)};
        reference.target = target;
        if (source.kind == object_kind_t::actor) {
            const auto projection = runtime::decode_actor_authority_payload (
              reference.application_payload, snapshot->object_generation);
            if (!projection || projection->actor.actor_id ().value () != source.key
                || projection->actor.node_rid ().value () != target.node_id
                || projection->actor.mesh_name () != target.mesh_name
                || projection->actor.object_generation () != target.object_generation)
                return {authority_publish_status_t::failed, std::nullopt};
        }
        const auto exchanged =
          infrastructure_result ([&] {
              return _store->compare_exchange_authority (
                key, cas_version,
                authority_retarget_t{source.kind == object_kind_t::actor
                                       ? reference.application_payload
                                       : authority_relocation_payload_codec_t::encode (reference),
                                     target_placement});
          }).value ();
        if (const auto *stored = std::get_if<authority_stored_t> (&exchanged)) {
            if (source.kind == object_kind_t::actor) {
                reference.application_payload = stored->snapshot.payload;
                if (!same_owner (stored->snapshot.owner, target_owner))
                    return {authority_publish_status_t::failed, std::nullopt};
                // Location Store authority generations are globally
                // allocated, so another object's commit can make the
                // durable target generation larger than source + 1.
                // Return the Store-assigned fence, not the proposal.
                reference.target.authority_owner_generation =
                  stored->snapshot.authority_owner_generation;
                return {authority_publish_status_t::published, std::move (reference)};
            }
            auto current = authority_relocation_payload_codec_t::decode (stored->snapshot);
            if (!current || !same_owner (stored->snapshot.owner, target_owner)) {
                if (std::getenv ("ZLINK_CPP_AUTO_CONNECT_TRACE") != nullptr) {
                    std::cerr << "zlink authority-publish stage=stored-owner-mismatch key="
                              << source.key << " decoded=" << (current ? 1 : 0) << '\n';
                }
                return {authority_publish_status_t::failed, std::move (current)};
            }
            return {authority_publish_status_t::published, std::move (current)};
        }
        const auto *conflict = std::get_if<authority_conflict_t> (&exchanged);
        if (conflict == nullptr)
            return {authority_publish_status_t::failed, std::nullopt};
        return {authority_publish_status_t::conflict, decode_current (conflict->current)};
    }
    std::optional<std::vector<relocation_participant_identity_t>>
    list_participant_identities () override
    {
        try {
            std::vector<relocation_participant_identity_t> output;
            std::optional<authority_scan_cursor_t> cursor;
            for (;;) {
                const auto result = infrastructure_result ([&] {
                                        return _store->list_authorities ("", cursor, 1000);
                                    }).value ();
                const auto *page = std::get_if<authority_page_t> (&result);
                if (!page)
                    return std::nullopt;
                for (const auto &entry : page->items) {
                    const auto decoded_key =
                      runtime::authority_key_codec_detail::decode_authority_key (entry.key.value);
                    if (!decoded_key)
                        continue;
                    const auto &snapshot = entry.snapshot;
                    relocation_participant_identity_t identity;
                    identity.owner.key = decoded_key->object_id;
                    identity.owner.object_generation = snapshot.object_generation;
                    identity.owner.authority_owner_generation = snapshot.authority_owner_generation;
                    identity.owner.mesh_name = snapshot.allocation.target.mesh_name;
                    identity.owner.node_id = snapshot.allocation.target.node_rid.value ();
                    identity.stable_type = snapshot.allocation.stable_type;
                    if (decoded_key->kind == 'a') {
                        identity.owner.kind = object_kind_t::actor;
                        const auto projection = runtime::decode_actor_authority_payload (
                          snapshot.payload, snapshot.object_generation);
                        if (projection) {
                            identity.spot_membership = std::pair{std::string (projection->spot_id),
                                                                 projection->spot_generation};
                            if (identity.stable_type.empty ())
                                identity.stable_type =
                                  ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                    projection->actor);
                        }
                    } else {
                        identity.owner.kind =
                          snapshot.allocation.object_kind == placement_object_kind_t::instance_spot
                            ? object_kind_t::instance_spot
                            : object_kind_t::user_spot;
                    }
                    output.push_back (std::move (identity));
                }
                if (!page->next_cursor)
                    return output;
                cursor = page->next_cursor;
            }
        }
        catch (...) {
            return std::nullopt;
        }
    }

    relocation_authority_t observe_relocation (const relocation_authority_fence_t &fence) override
    {
        try {
            const auto read = infrastructure_result ([&] {
                                  return _store->read_authority (authority_key (
                                    object_ref_t{.kind = fence.kind, .key = fence.key}));
                              }).value ();
            return settlement_of (read, fence);
        }
        catch (...) {
            return relocation_authority_t::unsettled;
        }
    }

    source_relocation_settlement_t
    settle_source_relocation (const relocation_authority_fence_t &fence,
                              const std::vector<object_ref_t> &participants,
                              bool preserve) override
    {
        try {
            const auto &primary = relocation_primary (participants);
            const auto key = authority_key (primary);
            auto read =
              infrastructure_result ([&] { return _store->read_authority (key); }).value ();
            auto result = source_settlement_of (read, fence, primary);
            if (result.outcome == relocation_authority_t::unsettled && preserve) {
                const auto live = owner_lease_live (fence.source_owner);
                if (!live)
                    return {};
                if (!*live)
                    return {relocation_authority_t::source_lease_expired, {}};
                const auto &snapshot = std::get<authority_snapshot_t> (read);
                const auto exchanged =
                  infrastructure_result ([&] {
                      return _store->compare_exchange_authority (key, fence.expected_store_version,
                                                                 authority_put_t{snapshot.payload});
                  }).value ();
                if (std::holds_alternative<authority_stored_t> (exchanged))
                    return {relocation_authority_t::source_preserved, {}};
                if (const auto *conflict = std::get_if<authority_conflict_t> (&exchanged)) {
                    read = conflict->current;
                    result = source_settlement_of (read, fence, primary);
                } else
                    return {};
            }
            if (result.outcome == relocation_authority_t::source_preserved) {
                const auto live = owner_lease_live (fence.source_owner);
                if (!live)
                    return {};
                if (!*live)
                    return {relocation_authority_t::source_lease_expired, {}};
            }
            if (result.outcome == relocation_authority_t::target_committed) {
                for (const auto &participant : participants) {
                    const auto observed =
                      participant.kind == primary.kind && participant.key == primary.key
                        ? read
                        : infrastructure_result ([&] {
                              return _store->read_authority (authority_key (participant));
                          }).value ();
                    const auto *current = std::get_if<authority_snapshot_t> (&observed);
                    if (current && current->object_generation == participant.object_generation
                        && current->authority_owner_generation
                             > participant.authority_owner_generation
                        && current->authority_owner_generation
                             <= static_cast<std::uint64_t> (INT64_MAX)
                        && same_owner (current->owner, fence.target_owner))
                        result.follow_generations.emplace (
                          std::pair{participant.kind, participant.key},
                          current->authority_owner_generation);
                }
            }
            return result;
        }
        catch (...) {
            return {};
        }
    }

    std::optional<bool> owner_lease_live (const location_owner_token_t &owner) override
    {
        try {
            const auto read = infrastructure_result ([&] {
                                  return _store->read_owner_lease (owner.owner_id);
                              }).value ();
            const auto *found = std::get_if<owner_lease_found_t> (&read);
            return found != nullptr && same_owner (found->token, owner)
                   && found->lease_expires_at > found->store_now;
        }
        catch (...) {
            return std::nullopt;
        }
    }

    std::optional<authority_relocation_reference_t> read (object_kind_t kind,
                                                          const std::string &key) override
    {
        const auto result = infrastructure_result ([&] {
                                return _store->read_authority (
                                  authority_key (object_ref_t{.kind = kind, .key = key}));
                            }).value ();
        return decode_current (result);
    }

    authority_publish_result_t publish_completion (object_kind_t kind,
                                                   const std::string &key,
                                                   const std::string &mesh_name,
                                                   std::uint64_t object_generation,
                                                   std::string relocation_reference,
                                                   std::uint32_t checksum_crc32c) override
    {
        const auto authority = authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          infrastructure_result ([&] { return _store->read_authority (authority); }).value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot || snapshot->object_generation != object_generation)
            return {authority_publish_status_t::conflict, decode_current (read)};
        if (auto current = authority_relocation_payload_codec_t::decode (*snapshot)) {
            return current->relocation_reference == relocation_reference
                       && current->checksum_crc32c == checksum_crc32c
                     ? authority_publish_result_t{authority_publish_status_t::published,
                                                  std::move (current)}
                     : authority_publish_result_t{authority_publish_status_t::conflict,
                                                  std::move (current)};
        }

        object_ref_t actor{kind,
                           key,
                           snapshot->object_generation,
                           snapshot->authority_owner_generation,
                           mesh_name,
                           snapshot->owner.owner_id};
        authority_relocation_reference_t reference{actor,
                                                   actor,
                                                   std::move (relocation_reference),
                                                   checksum_crc32c,
                                                   {},
                                                   snapshot->owner,
                                                   snapshot->payload};
        return store_preserving_authority (authority, *snapshot, std::move (reference));
    }

    authority_publish_result_t replace_completion (object_kind_t kind,
                                                   const std::string &key,
                                                   std::uint64_t object_generation,
                                                   const std::string &expected_reference,
                                                   std::uint32_t expected_checksum_crc32c,
                                                   std::string next_reference,
                                                   std::uint32_t next_checksum_crc32c) override
    {
        const auto authority = authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          infrastructure_result ([&] { return _store->read_authority (authority); }).value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        auto current =
          snapshot ? authority_relocation_payload_codec_t::decode (*snapshot) : std::nullopt;
        if (!snapshot || !current || snapshot->object_generation != object_generation
            || current->relocation_reference != expected_reference
            || current->checksum_crc32c != expected_checksum_crc32c)
            return {authority_publish_status_t::conflict, std::move (current)};
        current->relocation_reference = std::move (next_reference);
        current->checksum_crc32c = next_checksum_crc32c;
        return store_preserving_authority (authority, *snapshot, std::move (*current));
    }

    bool release_completion (object_kind_t kind,
                             const std::string &key,
                             std::uint64_t object_generation,
                             const std::string &expected_reference,
                             std::uint32_t expected_checksum_crc32c) override
    {
        const auto authority = authority_key (object_ref_t{.kind = kind, .key = key});
        const auto read =
          infrastructure_result ([&] { return _store->read_authority (authority); }).value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        const auto current =
          snapshot ? authority_relocation_payload_codec_t::decode (*snapshot) : std::nullopt;
        if (!snapshot || !current || snapshot->object_generation != object_generation
            || current->relocation_reference != expected_reference
            || current->checksum_crc32c != expected_checksum_crc32c)
            return false;
        const auto exchanged =
          infrastructure_result ([&] {
              return _store->compare_exchange_authority (
                authority, snapshot->store_version, authority_put_t{current->application_payload});
          }).value ();
        return std::holds_alternative<authority_stored_t> (exchanged);
    }

  private:
    friend class public_aggregate_authority_adapter_t;
    authority_publish_result_t
    store_preserving_authority (const authority_key_t &key,
                                const authority_snapshot_t &snapshot,
                                authority_relocation_reference_t reference)
    {
        const auto expected_reference = reference.relocation_reference;
        const auto expected_checksum = reference.checksum_crc32c;
        const auto exchanged =
          infrastructure_result ([&] {
              return _store->compare_exchange_authority (
                key, snapshot.store_version,
                authority_put_t{authority_relocation_payload_codec_t::encode (reference)});
          }).value ();
        if (const auto *stored = std::get_if<authority_stored_t> (&exchanged)) {
            auto current = authority_relocation_payload_codec_t::decode (stored->snapshot);
            return current && current->relocation_reference == expected_reference
                       && current->checksum_crc32c == expected_checksum
                     ? authority_publish_result_t{authority_publish_status_t::published,
                                                  std::move (current)}
                     : authority_publish_result_t{authority_publish_status_t::failed,
                                                  std::move (current)};
        }
        if (const auto *conflict = std::get_if<authority_conflict_t> (&exchanged))
            return {authority_publish_status_t::conflict, decode_current (conflict->current)};
        return {authority_publish_status_t::failed, std::nullopt};
    }

    static bool same_owner (const location_owner_token_t &left,
                            const location_owner_token_t &right) noexcept
    {
        return left.owner_id == right.owner_id && left.lease_generation == right.lease_generation;
    }

    static bool holds_captured_fence (const authority_snapshot_t &snapshot,
                                      std::string_view actor_id,
                                      const protocol::relocation_id_t &relocation,
                                      const location_owner_token_t &source_owner,
                                      std::string_view original_version)
    {
        if ((relocation.high == 0 && relocation.low == 0) || source_owner.lease_generation <= 0)
            return false;
        runtime::actor_authority_detail::relocation_capture_identity_t identity;
        const auto actor =
          runtime::decode_direct_actor_authority_payload (snapshot.payload, &identity);
        return actor && actor->actor_id == actor_id && actor->has_relocation_state
               && actor->relocation_phase
                    == runtime::actor_authority_detail::captured_relocation_phase
               && actor->relocation_expected_store_version == original_version
               && identity.high == relocation.high && identity.low == relocation.low
               && same_owner (snapshot.owner, source_owner)
               && actor->owner_id == source_owner.owner_id
               && actor->owner_lease_generation
                    == static_cast<std::uint64_t> (source_owner.lease_generation)
               && identity.source_owner_id == source_owner.owner_id
               && identity.source_owner_lease_generation
                    == static_cast<std::uint64_t> (source_owner.lease_generation)
               && identity.coordinator_owner_id == source_owner.owner_id
               && identity.coordinator_lease_generation
                    == static_cast<std::uint64_t> (source_owner.lease_generation);
    }

    /* Target-side current-CAS observation (01 §6.1, §10): target-owned means
     * committed; a changed source fence excludes this target CAS. Source
     * settlement separately recognizes historical commits below. */
    static relocation_authority_t settlement_of (const authority_read_result_t &read,
                                                 const relocation_authority_fence_t &fence)
    {
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        // Location runtime §10: Missing has no source fence and ends the expired owner.
        if (snapshot == nullptr)
            return relocation_authority_t::source_lease_expired;
        if (same_owner (snapshot->owner, fence.target_owner))
            return relocation_authority_t::target_committed;
        return snapshot->store_version == fence.expected_store_version
                   || holds_captured_fence (*snapshot, fence.key, fence.relocation,
                                            fence.source_owner, fence.expected_store_version)
                 ? relocation_authority_t::unsettled
                 : relocation_authority_t::source_preserved;
    }

    static source_relocation_settlement_t
    source_settlement_of (const authority_read_result_t &read,
                          const relocation_authority_fence_t &fence,
                          const object_ref_t &source)
    {
        const auto *current = std::get_if<authority_snapshot_t> (&read);
        if (!current)
            return {relocation_authority_t::source_lease_expired, {}};
        if (current->object_generation == source.object_generation
            && current->authority_owner_generation > source.authority_owner_generation
            && current->authority_owner_generation <= static_cast<std::uint64_t> (INT64_MAX)
            && !same_owner (current->owner, fence.source_owner))
            return {relocation_authority_t::target_committed, {}};
        if (current->object_generation != source.object_generation
            || current->authority_owner_generation != source.authority_owner_generation
            || !same_owner (current->owner, fence.source_owner))
            return {relocation_authority_t::data_lost, {}};
        return {current->store_version == fence.expected_store_version
                    || holds_captured_fence (*current, fence.key, fence.relocation,
                                             fence.source_owner, fence.expected_store_version)
                  ? relocation_authority_t::unsettled
                  : relocation_authority_t::source_preserved,
                {}};
    }

    static authority_key_t authority_key (const object_ref_t &object)
    {
        return object.kind == object_kind_t::actor ? actor_authority_key (object.key)
                                                   : spot_authority_key (object.key);
    }

  private:
    static std::optional<authority_relocation_reference_t>
    decode_current (const authority_read_result_t &current)
    {
        const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
        return snapshot ? authority_relocation_payload_codec_t::decode (*snapshot) : std::nullopt;
    }

    zlink::framework::location_repository_t *_store;
};

class public_aggregate_authority_adapter_t final : public aggregate_authority_port_t
{
  public:
    explicit public_aggregate_authority_adapter_t (
      zlink::framework::location_repository_t &store) noexcept :
        _store (&store)
    {
    }

    aggregate_publish_result_t prepare (const std::vector<object_ref_t> &sources,
                                        std::string target_node_id,
                                        location_owner_token_t target_owner,
                                        std::string relocation_reference,
                                        std::uint32_t,
                                        inventory_digest_t inventory_digest,
                                        std::string expected_primary_store_version) override
    {
        if (sources.size () < 2 || target_node_id.empty () || target_owner.owner_id.empty ()
            || target_owner.lease_generation <= 0 || expected_primary_store_version.empty ())
            return {};
        const auto &primary = relocation_primary (sources);
        std::vector<authority_snapshot_t> snapshots;
        snapshots.reserve (sources.size ());
        placement_capacity_bundle_t capacity;
        std::optional<spot_type_capacity_delta_t> spot_type;
        for (const auto &source : sources) {
            const auto read = infrastructure_result ([&] {
                                  return _store->read_authority (
                                    public_authority_store_adapter_t::authority_key (source));
                              }).value ();
            const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
            if (!snapshot || snapshot->object_generation != source.object_generation
                || snapshot->authority_owner_generation != source.authority_owner_generation
                || (&source == &primary
                    && snapshot->store_version != expected_primary_store_version))
                return {aggregate_publish_status_t::conflict, {}, {}};
            snapshots.push_back (*snapshot);
            capacity.actor_slots += snapshot->allocation.capacity_bundle.actor_slots;
            capacity.spot_slots += snapshot->allocation.capacity_bundle.spot_slots;
            if (snapshot->allocation.capacity_bundle.spot_type)
                spot_type = snapshot->allocation.capacity_bundle.spot_type;
        }
        capacity.spot_type = spot_type;
        const auto nodes = infrastructure_result ([&] {
                               return _store->list_mesh_nodes (sources.front ().mesh_name);
                           }).value ();
        const auto target_node = std::find_if (
          nodes.items.begin (), nodes.items.end (), [&] (const mesh_node_descriptor_t &node) {
              return node.rid.to_string () == target_node_id
                     && node.owner_id == target_owner.owner_id
                     && node.lease_generation == target_owner.lease_generation;
          });
        if (target_node == nodes.items.end ())
            return {};

        std::vector<std::byte> seed;
        for (const auto value : relocation_reference)
            seed.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
        for (const auto &source : sources) {
            for (const auto value : source.key)
                seed.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
        }
        const auto digest = runtime::sha256 (seed);
        aggregate_id_t aggregate_id;
        std::copy_n (digest.begin (), aggregate_id.value.size (), aggregate_id.value.begin ());
        const auto generation = _next_generation.fetch_add (1, std::memory_order_relaxed);

        /* Location runtime §8.1: the one aggregate CAS moves the User Spot and
         * every member Actor to the new owner. Each row then holds the same
         * steady authority payload a standalone relocation publishes — the
         * target placement — so every reader resolves the moved objects. */
        std::vector<aggregate_participant_t> participants;
        participants.reserve (snapshots.size ());
        for (std::size_t index = 0; index != sources.size (); ++index) {
            auto payload = target_authority_payload (sources[index].kind, snapshots[index].payload,
                                                     target_node_id, target_owner,
                                                     target_node->lifecycle_generation);
            if (!payload)
                return {};
            participants.push_back (
              {public_authority_store_adapter_t::authority_key (sources[index]),
               snapshots[index].store_version,
               authority_generation_transition_t::new_owner,
               std::move (*payload),
               {}});
        }
        std::sort (
          participants.begin (), participants.end (),
          [] (const auto &left, const auto &right) { return left.key.value < right.key.value; });
        zlink::framework::inventory_digest_t public_digest;
        for (std::size_t index = 0; index != public_digest.value.size (); ++index)
            public_digest.value[index] = static_cast<std::byte> (inventory_digest[index]);
        aggregate_prepare_request_t request;
        request.aggregate_id = aggregate_id;
        request.aggregate_generation = generation;
        request.participants = std::move (participants);
        request.inventory_digest = public_digest;
        request.target_descriptor = {sources.front ().mesh_name, target_node->rid};
        request.target_descriptor_lifecycle_generation = target_node->lifecycle_generation;
        request.capacity_bundle = capacity;
        request.target_owner = target_owner;
        const auto prepared = infrastructure_result ([&] {
                                  return _store->prepare_aggregate (std::move (request));
                              }).value ();
        const auto *created = std::get_if<aggregate_prepared_t> (&prepared);
        const auto *existing = std::get_if<aggregate_already_prepared_t> (&prepared);
        if (!created && !existing)
            return {aggregate_publish_status_t::conflict, {}, {}};
        const auto public_fence = created ? created->fence : existing->fence;
        return {aggregate_publish_status_t::prepared,
                aggregate_relocation_fence_t{public_fence.aggregate_generation, public_fence},
                {}};
    }

    /* The committed aggregate record identifies the move: a row that holds
     * exactly the payload this aggregate prepared was written by its commit.
     * The result carries each committed target placement and owner. */
    aggregate_publish_result_t commit (aggregate_relocation_fence_t fence) override
    {
        if (fence.value == 0 || fence.durable_fence.aggregate_generation == 0)
            return {};
        const auto participants =
          infrastructure_result ([&] {
              return _store->read_aggregate_participants (fence.durable_fence);
          }).value ();
        if (!participants)
            return {};
        const auto committed = infrastructure_result ([&] {
                                   return _store->commit_aggregate (fence.durable_fence);
                               }).value ();
        if (committed != aggregate_commit_result_t::committed
            && committed != aggregate_commit_result_t::already_committed)
            return {aggregate_publish_status_t::conflict, fence, {}};
        std::vector<authority_relocation_reference_t> current;
        current.reserve (participants->size ());
        for (const auto &participant : *participants) {
            const auto read = infrastructure_result ([&] {
                                  return _store->read_authority (participant.key);
                              }).value ();
            const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
            const auto key =
              runtime::authority_key_codec_detail::decode_authority_key (participant.key.value);
            if (!snapshot || !key || snapshot->payload != participant.authority_payload)
                return {};
            authority_relocation_reference_t placement;
            placement.target = {key->kind == 'a' ? object_kind_t::actor : object_kind_t::user_spot,
                                key->object_id,
                                snapshot->object_generation,
                                snapshot->authority_owner_generation,
                                snapshot->allocation.target.mesh_name,
                                std::string (snapshot->allocation.target.node_rid.value ())};
            placement.target_owner = snapshot->owner;
            placement.application_payload = snapshot->payload;
            current.push_back (std::move (placement));
        }
        return {aggregate_publish_status_t::committed, fence, std::move (current)};
    }

    void abort (aggregate_relocation_fence_t fence) override
    {
        if (fence.value != 0 && fence.durable_fence.aggregate_generation != 0)
            (void) infrastructure_result ([&] {
                return _store->abort_aggregate (fence.durable_fence);
            }).value ();
    }

  private:
    static std::optional<std::vector<std::byte>>
    target_authority_payload (object_kind_t kind,
                              const std::vector<std::byte> &source_payload,
                              const std::string &target_node_id,
                              const location_owner_token_t &target_owner,
                              std::uint64_t target_node_generation)
    {
        const auto owner_lease = static_cast<std::uint64_t> (target_owner.lease_generation);
        if (kind == object_kind_t::actor) {
            auto payload = runtime::decode_direct_actor_authority_payload (source_payload);
            if (!payload)
                return std::nullopt;
            payload->owner_id = target_owner.owner_id;
            payload->owner_lease_generation = owner_lease;
            payload->node_rid = node_rid_t::from_string (target_node_id);
            payload->node_generation = target_node_generation;
            return runtime::encode_actor_authority_payload (*payload);
        }
        if (kind == object_kind_t::user_spot) {
            auto payload = runtime::decode_direct_user_spot_authority_payload (source_payload);
            if (!payload)
                return std::nullopt;
            payload->owner_id = target_owner.owner_id;
            payload->owner_lease_generation = owner_lease;
            payload->node_rid = node_rid_t::from_string (target_node_id);
            payload->node_generation = target_node_generation;
            return runtime::encode_user_spot_authority_payload (*payload);
        }
        return std::nullopt;
    }

    zlink::framework::location_repository_t *_store;
    std::atomic<std::uint64_t> _next_generation{1};
};

} // namespace zlink::framework::runtime::stateful
