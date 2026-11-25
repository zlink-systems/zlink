/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/authority_aggregate_relocation.hpp"

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

    authority_publish_result_t
    publish (const object_ref_t &source,
             const object_ref_t &target,
             location_owner_token_t target_owner,
             object_creation_target_t target_placement,
             std::string relocation_reference,
             std::uint32_t checksum_crc32c,
             inventory_digest_t inventory_digest,
             std::vector<std::byte> target_application_payload = {},
             std::string expected_store_version = {},
             protocol::relocation_id_t relocation = {},
             location_owner_token_t source_owner = {},
             const protocol::relocation_prepare_t *prepare = nullptr) override
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
        // A Spot Actor join commit fences only the source StoreVersion
        // (05-spot-actor-membership :48); a relocation carries its coordinator fence
        // (04-relocation-flow §3, §4.5).
        const bool relocating = relocation.high != 0 || relocation.low != 0;
        const relocation_authority_fence_t fence =
          prepare ? relocation_authority_fence_t::from_prepare (source, *prepare)
                  : relocation_authority_fence_t{source.kind,  source.key,   expected_store_version,
                                                 source_owner, target_owner, relocation};
        const bool prepare_matches =
          !prepare
          || (source_claim_matches (*prepare) && fence.relocation == relocation
              && fence.expected_store_version == expected_store_version
              && same_owner (fence.source_owner, source_owner)
              && same_owner (fence.target_owner, target_owner)
              && fence.target_node_generation == target_placement.node_lifecycle_generation
              && zlink::routing_id_t::from (fence.target_node_routing_id).to_string ()
                   == target.node_id);
        if (relocating ? !prepare_matches || !holds_target_precommit_fence (*snapshot, fence)
                       : !expected_store_version.empty ()
                           && snapshot->store_version != expected_store_version)
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
        authority_retarget_t mutation{source.kind == object_kind_t::actor
                                        ? reference.application_payload
                                        : authority_relocation_payload_codec_t::encode (reference),
                                      target_placement};
        if (prepare)
            mutation.replace_aggregate =
              source.kind == object_kind_t::actor
                ? authority_aggregate_codec::replace (
                    snapshot->aggregate,
                    target_record (*snapshot, key, mutation.payload, *prepare, "committed"))
                : authority_aggregate_codec::without_capture (snapshot->aggregate);
        const auto exchanged =
          infrastructure_result ([&] {
              return _store->compare_exchange_authority (key, cas_version, std::move (mutation));
          }).value ();
        if (const auto *stored = std::get_if<authority_stored_t> (&exchanged)) {
            if (source.kind == object_kind_t::actor) {
                reference.application_payload = stored->snapshot.payload;
                reference.ready = stored->snapshot;
                if (!same_owner (stored->snapshot.owner, target_owner))
                    return {authority_publish_status_t::failed, std::nullopt};
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
    task_t<std::optional<std::string>>
    begin_source_capture (const std::vector<object_ref_t> &participants,
                          const relocation_authority_fence_t &fence,
                          const std::string &target_node_id,
                          std::uint64_t target_node_generation,
                          std::uint64_t application_version) override
    {
        try {
            const auto &primary = relocation_primary (participants);
            std::optional<authority_snapshot_t> original;
            auto inventory = nlohmann::json::array ();
            for (const auto &participant : participants) {
                const auto key = authority_key (participant);
                auto read = co_await _store->read_authority (key);
                auto *snapshot = std::get_if<authority_snapshot_t> (&read);
                if (!snapshot || snapshot->aggregate
                    || (&participant == &primary
                        && snapshot->store_version != fence.expected_store_version)
                    || snapshot->object_generation != participant.object_generation
                    || snapshot->authority_owner_generation
                         != participant.authority_owner_generation
                    || !holds_source_fence (*snapshot, participant.key, fence.relocation,
                                            fence.source_owner, fence.expected_store_version,
                                            fence.source_node_routing_id,
                                            fence.source_node_generation, &participant == &primary))
                    co_return std::optional<std::string>{};
                auto payload =
                  target_authority_payload (participant.kind, snapshot->payload, target_node_id,
                                            fence.target_owner, target_node_generation);
                if (!payload)
                    co_return std::optional<std::string>{};
                inventory.push_back (inventory_entry (key, snapshot->store_version, *payload));
                if (&participant == &primary)
                    original = std::move (*snapshot);
            }
            const auto digest = inventory_digest (std::move (inventory));
            const auto coordinator = protocol::relocation_coordinator_fence_t{
              .owner_id = fence.source_owner.owner_id,
              .lease_generation = static_cast<std::uint64_t> (fence.source_owner.lease_generation),
              .node_routing_id = fence.source_node_routing_id,
              .node_generation = fence.source_node_generation,
              .expected_authority_store_version = fence.expected_store_version};
            auto record = relocation_record (fence.relocation, coordinator, digest,
                                             application_version, "preparing");
            const auto result = co_await write_relocation (
              authority_key (primary), std::move (*original), std::move (record));
            if (const auto *stored = std::get_if<authority_stored_t> (&result))
                co_return std::optional{stored->snapshot.store_version};
            co_return std::optional<std::string>{};
        }
        catch (const std::exception &) {
            co_return std::optional<std::string>{};
        }
    }

    task_t<bool> capture_source (const object_ref_t &primary,
                                 const std::string &preparing_version) override
    {
        try {
            const auto key = authority_key (primary);
            auto read = co_await _store->read_authority (key);
            auto *snapshot = std::get_if<authority_snapshot_t> (&read);
            if (!snapshot || snapshot->store_version != preparing_version || !snapshot->aggregate
                || !snapshot->aggregate->relocation
                || snapshot->aggregate->relocation->phase != "preparing")
                co_return false;
            auto record = snapshot->aggregate->encoded.at ("relocation");
            record["phase"] = "captured";
            co_return std::holds_alternative<authority_stored_t> (
              co_await write_relocation (key, std::move (*snapshot), std::move (record)));
        }
        catch (const std::exception &) {
            co_return false;
        }
    }

    task_t<bool> record_target_prepared (const object_ref_t &source,
                                         const protocol::relocation_prepare_t &prepare) override
    {
        const auto key = authority_key (source);
        auto read = co_await _store->read_authority (key);
        auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        const auto fence = relocation_authority_fence_t::from_prepare (source, prepare);
        if (!snapshot || snapshot->object_generation != source.object_generation
            || snapshot->authority_owner_generation != source.authority_owner_generation
            || !source_claim_matches (prepare) || !holds_target_precommit_fence (*snapshot, fence))
            co_return false;
        // Prepared follows the source's durable Captured (01-location-runtime §3.4 1466-1467, 1493).
        if (!snapshot->aggregate || !snapshot->aggregate->relocation)
            co_return false;
        if (snapshot->aggregate->relocation->phase == "prepared")
            co_return true;
        if (snapshot->aggregate->relocation->phase != "captured")
            co_return false;
        auto record = target_record (*snapshot, key, {}, prepare, "prepared");
        co_return std::holds_alternative<authority_stored_t> (
          co_await write_relocation (key, std::move (*snapshot), std::move (record)));
    }

    bool cancel_source_capture (const relocation_authority_fence_t &fence) override
    {
        try {
            const auto key = authority_key (object_ref_t{.kind = fence.kind, .key = fence.key});
            const auto read =
              infrastructure_result ([&] { return _store->read_authority (key); }).value ();
            const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
            if (!snapshot || !snapshot->aggregate || !snapshot->aggregate->relocation)
                return true;
            const auto &record = *snapshot->aggregate->relocation;
            if (record.relocation_id
                  != authority_aggregate_codec::wire_uuid (fence.relocation.high,
                                                           fence.relocation.low)
                || !source_precommit (record.phase))
                return true;
            if (!holds_source_fence (*snapshot, fence.key, fence.relocation, fence.source_owner,
                                     fence.expected_store_version, fence.source_node_routing_id,
                                     fence.source_node_generation))
                return false;
            const auto result = preserve_source (key, *snapshot);
            return std::holds_alternative<authority_stored_t> (result);
        }
        catch (const std::exception &) {
            return false;
        }
    }

    bool finish_relocation (const object_ref_t &target,
                            const protocol::relocation_prepare_t &prepare) override
    {
        const auto key = authority_key (target);
        const auto read =
          infrastructure_result ([&] { return _store->read_authority (key); }).value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot || snapshot->object_generation != target.object_generation
            || snapshot->authority_owner_generation != target.authority_owner_generation)
            return false;
        if (!snapshot->aggregate || !snapshot->aggregate->relocation)
            return true;
        const auto &record = *snapshot->aggregate->relocation;
        const auto &durable_target = snapshot->aggregate->encoded.at ("relocation").at ("target");
        if (record.relocation_id
              != authority_aggregate_codec::wire_uuid (prepare.relocation.high,
                                                       prepare.relocation.low)
            || record.phase != "committed"
            || authority_aggregate_codec::number (durable_target, "attemptGeneration")
                 != prepare.target_attempt_generation
            || !same_owner (snapshot->owner, {prepare.target.target_owner_id,
                                              static_cast<std::int64_t> (
                                                prepare.target.target_owner_lease_generation)}))
            return false;
        return std::holds_alternative<authority_stored_t> (
          infrastructure_result ([&] {
              return _store->compare_exchange_authority (
                key, snapshot->store_version,
                authority_put_t{snapshot->payload,
                                {},
                                {},
                                authority_aggregate_codec::replace (snapshot->aggregate, nullptr)});
          }).value ());
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
                    const auto kind =
                      decoded_key->kind == 'a' ? object_kind_t::actor
                      : snapshot.allocation.object_kind == placement_object_kind_t::instance_spot
                        ? object_kind_t::instance_spot
                        : object_kind_t::user_spot;
                    identity.owner = current_owner (snapshot, kind, decoded_key->object_id);
                    identity.stable_type = snapshot.allocation.stable_type;
                    if (kind == object_kind_t::actor) {
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
                const auto exchanged = preserve_source (key, snapshot);
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
        if (auto relocation = decode_current (result))
            return relocation;
        const auto *snapshot = std::get_if<authority_snapshot_t> (&result);
        if (!snapshot || kind != object_kind_t::user_spot)
            return std::nullopt;
        const auto ready = authority_relocation_payload_codec_t::decode_ready_user_spot (*snapshot);
        if (!ready || ready->spot_id != key)
            return std::nullopt;
        const auto current = current_owner (*snapshot, kind, key);
        // Steady authority names the current owner; it carries no predecessor claim.
        return authority_relocation_reference_t{.source = current,
                                                .target = current,
                                                .target_owner = snapshot->owner,
                                                .application_payload = snapshot->payload,
                                                .ready = *snapshot};
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

    static void clear_legacy_capture (std::vector<std::byte> &payload)
    {
        const auto actor = runtime::decode_direct_actor_authority_payload (payload);
        if (actor && actor->has_relocation_state)
            payload = runtime::encode_actor_authority_payload (*actor);
    }

    authority_compare_exchange_result_t preserve_source (const authority_key_t &key,
                                                         const authority_snapshot_t &snapshot)
    {
        authority_put_t preserved{snapshot.payload};
        if (snapshot.aggregate && snapshot.aggregate->relocation)
            preserved.replace_aggregate =
              authority_aggregate_codec::replace (snapshot.aggregate, nullptr);
        else
            clear_legacy_capture (preserved.payload);
        return infrastructure_result ([&] {
                   return _store->compare_exchange_authority (key, snapshot.store_version,
                                                              std::move (preserved));
               })
          .value ();
    }

    static bool same_owner (const location_owner_token_t &left,
                            const location_owner_token_t &right) noexcept
    {
        return left.owner_id == right.owner_id && left.lease_generation == right.lease_generation;
    }

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
        if (kind == object_kind_t::instance_spot) {
            auto payload = runtime::decode_instance_spot_authority_payload (source_payload);
            if (!payload)
                return std::nullopt;
            payload->owner_id = target_owner.owner_id;
            payload->owner_lease_generation = owner_lease;
            payload->node_rid = node_rid_t::from_string (target_node_id);
            payload->node_generation = target_node_generation;
            return runtime::encode_instance_spot_authority_payload (*payload);
        }
        return std::nullopt;
    }

    static authority_aggregate_codec::json_t encoded_fence (const std::vector<std::uint8_t> &rid,
                                                            std::uint64_t generation,
                                                            const std::string &owner,
                                                            std::uint64_t lease)
    {
        return {{"nodeRid", zlink::routing_id_t::from (rid).to_hex ()},
                {"nodeGeneration", std::to_string (generation)},
                {"ownerId", owner},
                {"ownerLeaseGeneration", std::to_string (lease)}};
    }

    static bool source_claim_matches (const protocol::relocation_prepare_t &prepare)
    {
        return prepare.source_node_routing_id == prepare.coordinator.node_routing_id
               && prepare.source_node_generation == prepare.coordinator.node_generation;
    }

    static authority_aggregate_codec::json_t
    relocation_record (const protocol::relocation_id_t &relocation,
                       const protocol::relocation_coordinator_fence_t &coordinator,
                       std::string digest,
                       std::uint64_t application_version,
                       std::string_view phase)
    {
        auto source = encoded_fence (coordinator.node_routing_id, coordinator.node_generation,
                                     coordinator.owner_id, coordinator.lease_generation);
        auto coordinator_record = source;
        coordinator_record["expectedStoreVersion"] = coordinator.expected_authority_store_version;
        return {
          {"relocationId", authority_aggregate_codec::wire_uuid (relocation.high, relocation.low)},
          {"phase", phase},
          {"source", std::move (source)},
          {"target", nullptr},
          {"coordinator", std::move (coordinator_record)},
          {"sourceInventorySha256", std::move (digest)},
          {"applicationVersion", std::to_string (application_version)}};
    }

    static authority_aggregate_codec::json_t
    encoded_target (const protocol::relocation_prepare_t &prepare)
    {
        auto target = encoded_fence (
          prepare.target.target_node_routing_id, prepare.target.target_node_generation,
          prepare.target.target_owner_id, prepare.target.target_owner_lease_generation);
        target["attemptGeneration"] = std::to_string (prepare.target_attempt_generation);
        return target;
    }

    static authority_aggregate_codec::json_t
    target_record (const authority_snapshot_t &snapshot,
                   const authority_key_t &key,
                   const std::vector<std::byte> &payload,
                   const protocol::relocation_prepare_t &prepare,
                   std::string_view phase)
    {
        auto record =
          snapshot.aggregate && snapshot.aggregate->relocation
            ? snapshot.aggregate->encoded.at ("relocation")
            : relocation_record (
                prepare.relocation, prepare.coordinator,
                inventory_digest (nlohmann::json::array ({inventory_entry (
                  key, prepare.coordinator.expected_authority_store_version, payload)})),
                prepare.application_version, phase);
        record["phase"] = phase;
        record["target"] = encoded_target (prepare);
        return record;
    }

    task_t<authority_compare_exchange_result_t>
    write_relocation (const authority_key_t &key,
                      authority_snapshot_t snapshot,
                      authority_aggregate_codec::json_t record)
    {
        authority_put_t put{std::move (snapshot.payload)};
        put.replace_aggregate =
          authority_aggregate_codec::replace (snapshot.aggregate, std::move (record));
        return _store->compare_exchange_authority (key, std::move (snapshot.store_version),
                                                   std::move (put));
    }

    static nlohmann::json inventory_entry (const authority_key_t &key,
                                           const std::string &store_version,
                                           std::span<const std::byte> target_payload)
    {
        static const auto empty_membership_hash = zlink::framework::detail::sha256_hex ("[]");
        return {{"authorityKey", key.value},
                {"authorityPayloadSha256",
                 zlink::framework::detail::encode_hex (runtime::sha256 (target_payload))},
                {"expectedStoreVersion", store_version},
                {"membershipMutationSha256", empty_membership_hash},
                {"ownerTransition", "newOwner"}};
    }

    static std::string inventory_digest (nlohmann::json entries)
    {
        using json_t = nlohmann::json;
        constexpr std::size_t page_items = 1024;
        const auto participants = entries.size ();
        std::sort (entries.begin (), entries.end (), [] (const auto &left, const auto &right) {
            return left.at ("authorityKey") < right.at ("authorityKey");
        });
        for (std::size_t index = 0; index != participants; ++index)
            entries[index]["index"] = index;
        for (std::size_t level = 0;; ++level) {
            auto references = json_t::array ();
            for (std::size_t first = 0; first < entries.size ();) {
                auto count = std::min (page_items, entries.size () - first);
                const auto start_index =
                  level == 0 ? first : entries[first].at ("startIndex").get<std::size_t> ();
                for (;;) {
                    const auto last = first + count;
                    const auto end_index =
                      level == 0 ? last
                                 : entries[last - 1].at ("startIndex").get<std::size_t> ()
                                     + entries[last - 1].at ("entryCount").get<std::size_t> ();
                    auto items = json_t::array ();
                    for (auto index = first; index < last; ++index)
                        items.push_back (entries[index]);
                    const json_t page{{"children", level == 0 ? json_t::array () : items},
                                      {"entries", level == 0 ? items : json_t::array ()},
                                      {"entryCount", end_index - start_index},
                                      {"index", references.size ()},
                                      {"level", level},
                                      {"startIndex", start_index}};
                    const auto encoded = page.dump ();
                    if (encoded.size () <= location_record_payload_limit) {
                        references.push_back (
                          {{"entryCount", end_index - start_index},
                           {"index", references.size ()},
                           {"level", level},
                           {"sha256", zlink::framework::detail::sha256_hex (encoded)},
                           {"startIndex", start_index}});
                        first = last;
                        break;
                    }
                    if (count == 1)
                        throw std::length_error ("Relocation inventory entry exceeds page limit");
                    count /= 2;
                }
            }
            if (references.size () <= page_items) {
                const json_t root{
                  {"topLevel", level}, {"topPages", references}, {"totalCount", participants}};
                return zlink::framework::detail::sha256_hex (root.dump ());
            }
            entries = std::move (references);
        }
    }

    static bool source_precommit (std::string_view phase)
    {
        return phase == "preparing" || phase == "captured" || phase == "prepared";
    }

    static bool prepared_target_matches (const authority_snapshot_t &snapshot,
                                         const relocation_authority_fence_t &fence)
    {
        if (!snapshot.aggregate || !snapshot.aggregate->relocation
            || snapshot.aggregate->relocation->phase != "prepared")
            return true;
        const auto &target = snapshot.aggregate->encoded.at ("relocation").at ("target");
        const auto durable = authority_aggregate_codec::read_fence (target);
        return durable.owner_id == fence.target_owner.owner_id
               && durable.owner_lease_generation
                    == static_cast<std::uint64_t> (fence.target_owner.lease_generation)
               && authority_aggregate_codec::number (target, "attemptGeneration")
                    == fence.target_attempt_generation
               && (fence.target_node_routing_id.empty ()
                   || durable.node_rid_hex
                        == zlink::routing_id_t::from (fence.target_node_routing_id).to_hex ())
               && (fence.target_node_generation == 0
                   || durable.node_generation == fence.target_node_generation);
    }

    static bool holds_target_precommit_fence (const authority_snapshot_t &snapshot,
                                              const relocation_authority_fence_t &fence,
                                              bool require_steady_version = true)
    {
        return holds_source_fence (snapshot, fence.key, fence.relocation, fence.source_owner,
                                   fence.expected_store_version, fence.source_node_routing_id,
                                   fence.source_node_generation, require_steady_version)
               && (!snapshot.aggregate || !snapshot.aggregate->relocation
                   || snapshot.aggregate->relocation->phase != "preparing")
               && prepared_target_matches (snapshot, fence);
    }

    static bool holds_source_fence (const authority_snapshot_t &snapshot,
                                    std::string_view actor_id,
                                    const protocol::relocation_id_t &relocation,
                                    const location_owner_token_t &source_owner,
                                    std::string_view original_version,
                                    std::span<const std::uint8_t> expected_node = {},
                                    std::uint64_t expected_node_generation = 0,
                                    bool require_steady_version = true)
    {
        if (source_owner.owner_id.empty () || source_owner.lease_generation <= 0
            || !same_owner (snapshot.owner, source_owner))
            return false;
        const auto &placement = snapshot.allocation.target;
        const auto rid = placement.node_rid.value ();
        const auto rid_bytes = std::as_bytes (std::span{rid.data (), rid.size ()});
        if ((!expected_node.empty ()
             && !std::ranges::equal (std::as_bytes (expected_node), rid_bytes))
            || (expected_node_generation != 0
                && expected_node_generation != placement.node_lifecycle_generation))
            return false;
        const auto lease = static_cast<std::uint64_t> (source_owner.lease_generation);
        const authority_relocation_fence_t source{zlink::framework::detail::encode_hex (rid_bytes),
                                                  placement.node_lifecycle_generation,
                                                  source_owner.owner_id, lease};
        if (snapshot.aggregate && snapshot.aggregate->relocation) {
            const auto &record = *snapshot.aggregate->relocation;
            return source_precommit (record.phase)
                   && record.relocation_id
                        == authority_aggregate_codec::wire_uuid (relocation.high, relocation.low)
                   && record.source == source && record.coordinator == source
                   && record.expected_store_version == original_version;
        }
        runtime::actor_authority_detail::relocation_capture_identity_t identity;
        const auto actor =
          runtime::decode_direct_actor_authority_payload (snapshot.payload, &identity);
        if (!actor || !actor->has_relocation_state)
            return !require_steady_version || snapshot.store_version == original_version;
        return actor->actor_id == actor_id
               && actor->relocation_phase
                    == runtime::actor_authority_detail::captured_relocation_phase
               && identity.high == relocation.high && identity.low == relocation.low
               && actor->owner_id == source_owner.owner_id && actor->owner_lease_generation == lease
               && identity.source_owner_id == source_owner.owner_id
               && identity.source_owner_lease_generation == lease
               && identity.coordinator_owner_id == source_owner.owner_id
               && identity.coordinator_lease_generation == lease
               && identity.source_node_rid == placement.node_rid.value ()
               && identity.source_node_generation == placement.node_lifecycle_generation
               && identity.coordinator_node_rid == placement.node_rid.value ()
               && identity.coordinator_node_generation == placement.node_lifecycle_generation
               && actor->relocation_expected_store_version == original_version;
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
        if (same_owner (snapshot->owner, fence.target_owner)) {
            if (!snapshot->aggregate || !snapshot->aggregate->relocation)
                return relocation_authority_t::target_committed;
            const auto &record = *snapshot->aggregate->relocation;
            if (record.relocation_id
                  != authority_aggregate_codec::wire_uuid (fence.relocation.high,
                                                           fence.relocation.low)
                || record.expected_store_version != fence.expected_store_version
                || (record.phase != "committed" && record.phase != "activating"
                    && record.phase != "activated" && record.phase != "cleaning"
                    && record.phase != "completed"))
                return relocation_authority_t::source_preserved;
            const auto &target = snapshot->aggregate->encoded.at ("relocation").at ("target");
            const auto &placement = snapshot->allocation.target;
            const auto rid = placement.node_rid.value ();
            const authority_relocation_fence_t actual{
              zlink::framework::detail::encode_hex (
                std::as_bytes (std::span{rid.data (), rid.size ()})),
              placement.node_lifecycle_generation, snapshot->owner.owner_id,
              static_cast<std::uint64_t> (snapshot->owner.lease_generation)};
            return authority_aggregate_codec::read_fence (target) == actual
                       && (fence.target_attempt_generation == 0
                           || authority_aggregate_codec::number (target, "attemptGeneration")
                                == fence.target_attempt_generation)
                     ? relocation_authority_t::target_committed
                     : relocation_authority_t::source_preserved;
        }
        return holds_source_fence (*snapshot, fence.key, fence.relocation, fence.source_owner,
                                   fence.expected_store_version, fence.source_node_routing_id,
                                   fence.source_node_generation)
                   && prepared_target_matches (*snapshot, fence)
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
        return {holds_source_fence (*current, fence.key, fence.relocation, fence.source_owner,
                                    fence.expected_store_version, fence.source_node_routing_id,
                                    fence.source_node_generation)
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
    static object_ref_t
    current_owner (const authority_snapshot_t &snapshot, object_kind_t kind, const std::string &key)
    {
        return {kind,
                key,
                snapshot.object_generation,
                snapshot.authority_owner_generation,
                snapshot.allocation.target.mesh_name,
                std::string (snapshot.allocation.target.node_rid.value ())};
    }

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
                                        relocation_authority_fence_t source_fence) override
    {
        if (sources.size () < 2 || target_node_id.empty () || target_owner.owner_id.empty ()
            || target_owner.lease_generation <= 0 || source_fence.expected_store_version.empty ())
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
            auto participant_fence = source_fence;
            participant_fence.key = source.key;
            if (!snapshot || snapshot->object_generation != source.object_generation
                || snapshot->authority_owner_generation != source.authority_owner_generation
                || !public_authority_store_adapter_t::holds_target_precommit_fence (
                  *snapshot, participant_fence, &source == &primary))
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
            auto payload = public_authority_store_adapter_t::target_authority_payload (
              sources[index].kind, snapshots[index].payload, target_node_id, target_owner,
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
            placement.ready = *snapshot;
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
    zlink::framework::location_repository_t *_store;
    std::atomic<std::uint64_t> _next_generation{1};
};

} // namespace zlink::framework::runtime::stateful
