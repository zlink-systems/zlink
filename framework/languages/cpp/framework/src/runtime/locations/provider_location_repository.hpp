/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/aggregate_inventory.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include "sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <numeric>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime
{

/*
 * Framework-owned domain repository over the public opaque Store SPI.
 * Providers never receive descriptor, lease, authority or placement DTOs.
 */
class provider_location_repository_t final : public location_repository_t
{
  public:
    explicit provider_location_repository_t (location_store_t &store) noexcept : _store (&store) {}

    task_t<owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override
    {
        if (owner_id.empty () || lease_ttl <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("owner lease claim is incomplete");
        const auto owner_key = key_owner (owner_id);
        for (;;) {
            auto owner = read (owner_key);
            if (std::holds_alternative<store_found_t> (owner))
                return completed (owner_lease_claim_result_t{owner_lease_conflict_t{}});

            auto counter = read (counter_key);
            std::int64_t generation = 1;
            if (const auto *found = std::get_if<store_found_t> (&counter))
                generation = parse_i64 (found->value.bytes);
            if (generation == std::numeric_limits<std::int64_t>::max ())
                return completed (owner_lease_claim_result_t{owner_lease_generation_exhausted_t{}});

            const auto payload = to_bytes (
              nlohmann::json{{"ownerId", owner_id}, {"leaseGeneration", generation}}.dump ());
            store_write_request_t request;
            request.conditions.push_back (missing_condition (owner_key));
            request.conditions.push_back (condition_for (counter_key, counter));
            request.mutations.push_back (store_put_t{owner_key, payload, lease_ttl});
            request.mutations.push_back (
              store_put_t{counter_key, to_bytes (std::to_string (generation + 1)), std::nullopt});
            auto written = write (std::move (request));
            if (std::holds_alternative<store_write_conflict_t> (written))
                continue;
            const auto &applied = std::get<store_write_applied_t> (written);
            return completed (
              owner_lease_claim_result_t{owner_lease_claimed_t{{std::move (owner_id), generation},
                                                               applied.store_now + lease_ttl,
                                                               applied.store_now}});
        }
    }

    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner_id) override
    {
        auto result = read (key_owner (owner_id));
        const auto *found = std::get_if<store_found_t> (&result);
        if (!found || !found->value.expires_at)
            return completed (owner_lease_read_result_t{owner_lease_missing_t{}});
        const auto record = parse_json (found->value.bytes);
        return completed (owner_lease_read_result_t{
          owner_lease_found_t{{record.at ("ownerId").get<std::string> (),
                               record.at ("leaseGeneration").get<std::int64_t> ()},
                              *found->value.expires_at,
                              found->value.store_now}});
    }

    task_t<owner_lease_renew_result_t>
    renew_owner_lease (location_owner_token_t token, std::chrono::milliseconds lease_ttl) override
    {
        if (lease_ttl <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("owner lease TTL must be positive");
        const auto key = key_owner (token.owner_id);
        auto current = read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            return completed (owner_lease_renew_result_t{owner_lease_stale_t{}});
        auto result = write ({{version_condition (key, found->value.version)},
                              {store_put_t{key, found->value.bytes, lease_ttl}}});
        if (std::holds_alternative<store_write_conflict_t> (result))
            return completed (owner_lease_renew_result_t{owner_lease_stale_t{}});
        const auto &applied = std::get<store_write_applied_t> (result);
        return completed (owner_lease_renew_result_t{
          owner_lease_renewed_t{applied.store_now + lease_ttl, applied.store_now}});
    }

    task_t<owner_lease_release_result_t> release_owner_lease (location_owner_token_t token) override
    {
        const auto key = key_owner (token.owner_id);
        auto current = read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            return completed (owner_lease_release_result_t{owner_lease_stale_t{}});
        auto result =
          write ({{version_condition (key, found->value.version)}, {store_delete_t{key}}});
        return completed (std::holds_alternative<store_write_applied_t> (result)
                            ? owner_lease_release_result_t{owner_lease_released_t{}}
                            : owner_lease_release_result_t{owner_lease_stale_t{}});
    }

    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        const auto key = key_mesh (descriptor.mesh_name, descriptor.rid);
        auto current = read (key);
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            const auto stored =
              decode_mesh_descriptor (parse_json (found->value.bytes).at ("descriptor"));
            descriptor.capacity.actors.active = stored.capacity.actors.active;
            descriptor.capacity.actors.reserved = stored.capacity.actors.reserved;
            descriptor.capacity.spots.active = stored.capacity.spots.active;
            descriptor.capacity.spots.reserved = stored.capacity.spots.reserved;
            for (auto &typed : descriptor.capacity.spot_types) {
                const auto existing = std::find_if (
                  stored.capacity.spot_types.begin (), stored.capacity.spot_types.end (),
                  [&] (const spot_type_capacity_t &item) {
                      return item.object_kind == typed.object_kind
                             && item.stable_type == typed.stable_type;
                  });
                if (existing != stored.capacity.spot_types.end ()) {
                    typed.usage.active = existing->usage.active;
                    typed.usage.reserved = existing->usage.reserved;
                }
            }
        }
        return update_descriptor (
          key, descriptor.owner_id, descriptor.lease_generation, descriptor.lifecycle_generation,
          descriptor.descriptor_revision, encode_mesh_record (1, descriptor), intent,
          [&] (const nlohmann::json &record) {
              return same_mesh_immutable (decode_mesh_descriptor (record.at ("descriptor")),
                                          descriptor);
          });
    }

    task_t<location_write_status_t> remove_mesh_node (mesh_node_descriptor_key_t key,
                                                      location_owner_token_t owner) override
    {
        return remove_descriptor (key_mesh (key.mesh_name, key.rid), std::move (owner));
    }

    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, location_page_request_t page = {}) override
    {
        return list_descriptors<mesh_node_descriptor_t> (
          prefix_mesh (mesh_name), std::move (page), [] (const nlohmann::json &record) {
              return decode_mesh_descriptor (record.at ("descriptor"));
          });
    }

    task_t<location_write_result_t>
    update_client_server (client_server_server_descriptor_t descriptor,
                          location_write_intent_t intent) override
    {
        const auto key = key_client_server (descriptor.channel_name, descriptor.server_rid);
        return update_descriptor (
          key, descriptor.owner_id, descriptor.lease_generation, descriptor.lifecycle_generation,
          descriptor.descriptor_revision,
          encode_descriptor_record (1, descriptor.owner_id, descriptor.lease_generation,
                                    descriptor.lifecycle_generation, descriptor.descriptor_revision,
                                    encode (descriptor)),
          intent, [&] (const nlohmann::json &record) {
              const auto current = decode_client_server (record.at ("descriptor"));
              return current.endpoint == descriptor.endpoint
                     && current.security_identity == descriptor.security_identity;
          });
    }

    task_t<location_write_status_t> remove_client_server (client_server_server_descriptor_key_t key,
                                                          location_owner_token_t owner) override
    {
        return remove_descriptor (key_client_server (key.channel_name, key.server_rid),
                                  std::move (owner));
    }

    task_t<location_page_t<client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name, location_page_request_t page = {}) override
    {
        return list_descriptors<client_server_server_descriptor_t> (
          prefix_client_server (channel_name), std::move (page), [] (const nlohmann::json &record) {
              return decode_client_server (record.at ("descriptor"));
          });
    }

    task_t<location_write_result_t>
    update_fanout_publisher (fanout_publisher_descriptor_t descriptor,
                             location_write_intent_t intent) override
    {
        const auto key = key_fanout (descriptor.channel_name, descriptor.publisher_rid);
        return update_descriptor (
          key, descriptor.owner_id, descriptor.lease_generation, descriptor.lifecycle_generation,
          descriptor.descriptor_revision,
          encode_descriptor_record (1, descriptor.owner_id, descriptor.lease_generation,
                                    descriptor.lifecycle_generation, descriptor.descriptor_revision,
                                    encode (descriptor)),
          intent, [&] (const nlohmann::json &record) {
              const auto current = decode_fanout (record.at ("descriptor"));
              return current.endpoint == descriptor.endpoint
                     && current.security_identity == descriptor.security_identity;
          });
    }

    task_t<location_write_status_t> remove_fanout_publisher (fanout_publisher_descriptor_key_t key,
                                                             location_owner_token_t owner) override
    {
        return remove_descriptor (key_fanout (key.channel_name, key.publisher_rid),
                                  std::move (owner));
    }

    task_t<location_page_t<fanout_publisher_descriptor_t>>
    list_fanout_publishers (std::string channel_name, location_page_request_t page = {}) override
    {
        return list_descriptors<fanout_publisher_descriptor_t> (
          prefix_fanout (channel_name), std::move (page),
          [] (const nlohmann::json &record) { return decode_fanout (record.at ("descriptor")); });
    }

    task_t<authority_read_result_t> read_authority (authority_key_t key,
                                                    std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_read_result_t> ();
        auto current = read (key_authority (key.value));
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto snapshot = effective_authority (key.value, found->value.bytes,
                                                 found->value.version, found->value.store_now);
            if (!snapshot)
                return completed (authority_read_result_t{
                  authority_missing_t{found->value.store_now}});
            return completed (authority_read_result_t{std::move (*snapshot)});
        }
        return completed (authority_read_result_t{
          authority_missing_t{std::get<store_missing_t> (current).store_now}});
    }

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority (authority_key_t key,
                                std::string expected_store_version,
                                authority_mutation_t mutation,
                                std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_compare_exchange_result_t> ();
        const auto row_key = key_authority (key.value);
        auto current = read (row_key);
        auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            return authority_conflict (std::move (current));
        if (authority_mutation_locked (key.value))
            return completed (authority_compare_exchange_result_t{authority_conflict_t{
              read_authority_value (key.value)}});
        auto snapshot =
          decode_authority (found->value.bytes, found->value.version, found->value.store_now);
        if (snapshot.store_version != expected_store_version)
            return authority_conflict (std::move (current));

        if (std::holds_alternative<authority_delete_t> (mutation)) {
            if (snapshot.allocation.state != placement_allocation_state_t::active
                || !owner_is_live (snapshot.owner))
                return authority_conflict (std::move (current));
            auto target = read_target_descriptor (snapshot.allocation.target, false);
            if (!target
                || !adjust_capacity (target->descriptor, snapshot.allocation.capacity_bundle, 0,
                                     -1))
                return authority_conflict (std::move (current));
            if (!advance_store_version (snapshot))
                return completed (
                  authority_compare_exchange_result_t{authority_generation_exhausted_t{}});
            auto written =
              write ({{version_condition (row_key, found->value.version),
                       version_condition (key_owner (snapshot.owner.owner_id),
                                          target->owner_provider_version),
                       version_condition (target->key, target->provider_version)},
                      {store_delete_t{row_key},
                       store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
            if (const auto *applied = std::get_if<store_write_applied_t> (&written))
                return completed (authority_compare_exchange_result_t{
                  authority_deleted_t{snapshot.store_version, applied->store_now}});
            return authority_conflict (read (row_key));
        }

        if (const auto *restore = std::get_if<authority_restore_t> (&mutation)) {
            if (!same_owner (snapshot.owner, restore->expected_owner))
                return authority_conflict (std::move (current));
            snapshot.payload = restore->payload;
            if (!advance_store_version (snapshot))
                return completed (
                  authority_compare_exchange_result_t{authority_generation_exhausted_t{}});
            auto written =
              write ({{version_condition (row_key, found->value.version)},
                      {store_put_t{row_key, encode_authority (snapshot), std::nullopt}}});
            return authority_write_result (row_key, snapshot, std::move (written));
        }

        auto put = std::get<authority_put_t> (std::move (mutation));
        if (snapshot.allocation.state != placement_allocation_state_t::active)
            return authority_conflict (std::move (current));
        if (put.generation_transition == authority_generation_transition_t::preserve) {
            if (put.target_owner || put.relocation_capacity_fence
                || !owner_is_live (snapshot.owner))
                return authority_conflict (std::move (current));
        } else {
            if (!put.target_owner || !put.relocation_capacity_fence
                || !owner_is_live (*put.target_owner))
                return authority_conflict (std::move (current));
            const auto relocation_key =
              key_relocation_capacity (put.relocation_capacity_fence->value);
            auto relocation = read (relocation_key);
            const auto *stored = std::get_if<store_found_t> (&relocation);
            if (!stored)
                return authority_conflict (std::move (current));
            auto record = parse_json (stored->value.bytes);
            if (record.value ("status", "") != "reserved"
                || record.at ("authorityKey").get<std::string> () != key.value
                || record.at ("expectedStoreVersion").get<std::string> () != expected_store_version
                || decode_owner (record.at ("sourceOwner")).owner_id != snapshot.owner.owner_id
                || !same_owner (decode_owner (record.at ("targetOwner")), *put.target_owner))
                return authority_conflict (std::move (current));
            auto generations = read (generation_counter_key);
            std::uint64_t object_generation = 0;
            std::uint64_t next_owner_generation = 0;
            if (const auto *generation = std::get_if<store_found_t> (&generations)) {
                const auto counters = parse_json (generation->value.bytes);
                object_generation = counters.at ("objectGeneration").get<std::uint64_t> ();
                next_owner_generation =
                  counters.at ("authorityOwnerGeneration").get<std::uint64_t> ();
            }
            if (next_owner_generation >= max_generation)
                return completed (
                  authority_compare_exchange_result_t{authority_generation_exhausted_t{}});
            auto source_descriptor =
              read_target_descriptor (decode_target (record.at ("source")), false);
            auto target_descriptor = read_target_descriptor (decode_target (record.at ("target")));
            const auto bundle = decode_bundle (record.at ("capacityBundle"));
            if (!source_descriptor || !target_descriptor)
                return authority_conflict (std::move (current));
            if (source_descriptor->key.value == target_descriptor->key.value) {
                if (!adjust_capacity (source_descriptor->descriptor, bundle, -1, 0))
                    return authority_conflict (std::move (current));
                if (!adjust_capacity (source_descriptor->descriptor, bundle, 0, 0))
                    return authority_conflict (std::move (current));
                target_descriptor = source_descriptor;
            } else {
                if (!adjust_capacity (source_descriptor->descriptor, bundle, 0, -1)
                    || !adjust_capacity (target_descriptor->descriptor, bundle, -1, 1))
                    return authority_conflict (std::move (current));
            }
            snapshot.authority_owner_generation = ++next_owner_generation;
            snapshot.owner = *put.target_owner;
            snapshot.allocation.target = decode_target (record.at ("target"));
            snapshot.allocation.state = placement_allocation_state_t::active;
            record["status"] = "committed";
            snapshot.payload = std::move (put.payload);
            if (!advance_store_version (snapshot))
                return completed (
                  authority_compare_exchange_result_t{authority_generation_exhausted_t{}});
            store_write_request_t write_request;
            write_request.conditions = {
              version_condition (row_key, found->value.version),
              version_condition (relocation_key, stored->value.version),
              condition_for (generation_counter_key, generations),
              version_condition (key_owner (put.target_owner->owner_id),
                                 target_descriptor->owner_provider_version),
              version_condition (source_descriptor->key, source_descriptor->provider_version)};
            if (source_descriptor->key.value != target_descriptor->key.value)
                write_request.conditions.push_back (
                  version_condition (target_descriptor->key, target_descriptor->provider_version));
            write_request.mutations = {
              store_put_t{row_key, encode_authority (snapshot), std::nullopt},
              store_put_t{relocation_key, to_bytes (record.dump ()), std::nullopt},
              store_put_t{generation_counter_key,
                          to_bytes (json_t{{"objectGeneration", object_generation},
                                           {"authorityOwnerGeneration", next_owner_generation}}
                                      .dump ()),
                          std::nullopt},
              store_put_t{source_descriptor->key, encode_target_record (*source_descriptor),
                          std::nullopt}};
            if (source_descriptor->key.value != target_descriptor->key.value)
                write_request.mutations.push_back (store_put_t{
                  target_descriptor->key, encode_target_record (*target_descriptor), std::nullopt});
            auto written = write (std::move (write_request));
            return authority_write_result (row_key, snapshot, std::move (written));
        }
        snapshot.payload = std::move (put.payload);
        if (!advance_store_version (snapshot))
            return completed (
              authority_compare_exchange_result_t{authority_generation_exhausted_t{}});
        auto live_owner = read_live_owner (snapshot.owner);
        if (!live_owner)
            return authority_conflict (std::move (current));
        auto written = write (
          {{version_condition (row_key, found->value.version),
            version_condition (key_owner (snapshot.owner.owner_id), live_owner->value.version)},
           {store_put_t{row_key, encode_authority (snapshot), std::nullopt}}});
        return authority_write_result (row_key, snapshot, std::move (written));
    }

    task_t<authority_scan_result_t> list_authorities (std::string key_prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit,
                                                      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_scan_result_t> ();
        if (limit == 0 || limit > 1000)
            throw std::invalid_argument ("authority scan limit must be between 1 and 1000");
        auto result = _store
                        ->scan ({std::string (authority_prefix),
                                 cursor ? std::optional<store_scan_cursor_t>{store_scan_cursor_t{
                                            std::string (cursor->encoded ())}}
                                        : std::nullopt,
                                 static_cast<std::uint32_t> (limit)})
                        .result ()
                        .value ();
        const auto *page = std::get_if<store_scan_page_t> (&result);
        if (!page)
            return completed (authority_scan_result_t{authority_scan_expired_t{}});
        authority_page_t output;
        output.items.reserve (page->items.size ());
        for (const auto &item : page->items) {
            const auto encoded = item.key.value.substr (authority_prefix.size ());
            const auto separator = encoded.find (':');
            if (separator == std::string::npos)
                continue;
            const auto length =
              static_cast<std::size_t> (std::stoull (encoded.substr (0, separator)));
            const auto start = separator + 1;
            if (start + length > encoded.size ())
                continue;
            const auto logical_key = encoded.substr (start, length);
            if (!logical_key.starts_with (key_prefix))
                continue;
            auto snapshot = effective_authority (logical_key, item.value.bytes, item.value.version,
                                                 page->store_now);
            if (!snapshot)
                continue;
            output.items.push_back ({{logical_key}, std::move (*snapshot)});
        }
        if (page->next_cursor)
            output.next_cursor = authority_scan_cursor_t{page->next_cursor->value};
        return completed (authority_scan_result_t{std::move (output)});
    }

    task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (creation_operation_identity_t operation,
                            std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<std::optional<creation_terminal_record_t>> ();
        auto result = read (key_creation_terminal (operation));
        const auto *found = std::get_if<store_found_t> (&result);
        if (!found)
            return completed (std::optional<creation_terminal_record_t>{});
        return completed (std::optional<creation_terminal_record_t>{
          decode_terminal (parse_json (found->value.bytes))});
    }

    task_t<object_reserve_result_t> reserve (object_reserve_request_t request,
                                             std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_reserve_result_t> ();
        if (request.creating_payload.size () > 1024u * 1024u
            || request.intent.request_encoded_size > 1024u * 1024u)
            throw std::invalid_argument ("object reservation payload exceeds 1 MiB");
        if (!bundle_matches (request.capacity_bundle, request.key.kind, request.intent.stable_type))
            throw std::invalid_argument (
              "object reservation capacity bundle does not match the object");

        const auto authority_key = key_authority (object_key (request.key));
        auto authority = read (authority_key);
        if (authority_mutation_locked (object_key (request.key)))
            return completed (object_reserve_result_t{object_reserve_conflict_t{
              read_authority_value (object_key (request.key))}});
        if (const auto *found = std::get_if<store_found_t> (&authority)) {
            auto current =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (current.allocation.stable_type != request.intent.stable_type)
                return completed (
                  object_reserve_result_t{object_type_mismatch_t{std::move (current)}});
            if (current.allocation.state == placement_allocation_state_t::active)
                return completed (
                  object_reserve_result_t{object_already_exists_t{std::move (current)}});
            return completed (
              object_reserve_result_t{object_reserve_conflict_t{std::move (current)}});
        }

        auto target = read_target_descriptor (request.target);
        if (!target
            || !target_accepts (target->descriptor, request.key.kind, request.intent.stable_type)
            || !owner_is_live (request.target.owner))
            return completed (object_reserve_result_t{object_reserve_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}});
        if (!capacity_available (target->descriptor, request.capacity_bundle))
            return completed (object_reserve_result_t{object_placement_capacity_exhausted_t{}});
        if (!adjust_capacity (target->descriptor, request.capacity_bundle, 1, 0))
            return completed (object_reserve_result_t{object_placement_capacity_exhausted_t{}});

        auto generations = read (generation_counter_key);
        std::uint64_t object_generation = 0;
        std::uint64_t owner_generation_value = 0;
        if (const auto *found = std::get_if<store_found_t> (&generations)) {
            const auto record = parse_json (found->value.bytes);
            object_generation = record.at ("objectGeneration").get<std::uint64_t> ();
            owner_generation_value = record.at ("authorityOwnerGeneration").get<std::uint64_t> ();
        }
        if (object_generation >= max_generation || owner_generation_value >= max_generation)
            return completed (object_reserve_result_t{authority_generation_exhausted_t{}});
        ++object_generation;
        ++owner_generation_value;

        const auto reservation_key = key_reservation (request.key);
        auto old_reservation = read (reservation_key);
        if (std::holds_alternative<store_found_t> (old_reservation))
            return completed (object_reserve_result_t{object_reserve_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}});

        object_reservation_fence_t fence{"reservation-" + std::to_string (object_generation) + "-"
                                           + std::to_string (owner_generation_value),
                                         "1",
                                         object_generation,
                                         owner_generation_value,
                                         request.target,
                                         request.capacity_bundle};
        authority_snapshot_t creating{
          "1",
          request.creating_payload,
          object_generation,
          owner_generation_value,
          request.target.owner,
          {},
          {placement_allocation_state_t::reserved, request.key.kind, request.intent.stable_type,
           request.target, request.capacity_bundle},
          pending_object_creation_t{
            fence.reservation_id, request.intent.request_content_reference,
            request.intent.request_sha256,
            static_cast<std::uint32_t> (request.intent.request_encoded_size)}};
        auto reservation_record = encode_reservation (request, fence, creating, "prepared");
        auto written = write (
          {{missing_condition (authority_key), missing_condition (reservation_key),
            condition_for (generation_counter_key, generations),
            version_condition (target->key, target->provider_version),
            version_condition (key_owner (request.target.owner.owner_id),
                               target->owner_provider_version)},
           {store_put_t{authority_key, encode_authority (creating), std::nullopt},
            store_put_t{reservation_key, to_bytes (reservation_record.dump ()), std::nullopt},
            store_put_t{generation_counter_key,
                        to_bytes (json_t{{"objectGeneration", object_generation},
                                         {"authorityOwnerGeneration", owner_generation_value}}
                                    .dump ()),
                        std::nullopt},
            store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
        if (!std::holds_alternative<store_write_applied_t> (written))
            return completed (object_reserve_result_t{
              object_reserve_conflict_t{read_authority_value (object_key (request.key))}});
        creating.store_now = std::get<store_write_applied_t> (written).store_now;
        return completed (
          object_reserve_result_t{object_reserved_t{std::move (fence), std::move (creating)}});
    }

    task_t<object_complete_creation_result_t>
    complete_creation (object_complete_creation_request_t request,
                       std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_complete_creation_result_t> ();
        const auto publication =
          std::visit ([] (const auto &value) { return value.terminal; }, request.completion);
        if (publication.terminal_envelope.size () > 1024u * 1024u
            || sha256 (publication.terminal_envelope) != publication.sha256)
            throw std::invalid_argument ("creation terminal envelope or SHA-256 is invalid");
        const auto expires_at = publication.operation_deadline + std::chrono::minutes (5);
        const auto now = std::chrono::system_clock::now ();
        if (expires_at <= now)
            throw std::invalid_argument ("creation terminal expiry is not in the future");
        const auto terminal_key = key_creation_terminal (publication.operation);
        auto existing = read (terminal_key);
        if (const auto *found = std::get_if<store_found_t> (&existing))
            return completed (
              object_complete_creation_result_t{object_creation_already_completed_result_t{
                decode_terminal (parse_json (found->value.bytes))}});

        creation_terminal_record_t terminal{
          publication.operation,
          request.key,
          request.fence,
          std::holds_alternative<object_creation_completed_t> (request.completion)
            ? creation_terminal_state_t::created
            : (std::holds_alternative<object_creation_rejected_t> (request.completion)
                 ? creation_terminal_state_t::rejected
                 : creation_terminal_state_t::failed),
          publication.terminal_envelope,
          publication.sha256,
          expires_at};
        std::optional<authority_snapshot_t> ready;
        if (const auto *created = std::get_if<object_creation_completed_t> (&request.completion)) {
            auto committed =
              commit ({request.key, request.fence, created->ready_payload}, cancellation)
                .result ()
                .value ();
            if (const auto *value = std::get_if<object_committed_t> (&committed))
                ready = value->ready;
            else if (const auto *value = std::get_if<object_already_committed_t> (&committed))
                ready = value->ready;
            else if (std::holds_alternative<object_commit_stale_t> (committed))
                return completed (
                  object_complete_creation_result_t{object_creation_completion_stale_t{}});
            else if (const auto *conflict = std::get_if<object_commit_conflict_t> (&committed))
                return completed (object_complete_creation_result_t{
                  object_creation_completion_conflict_t{conflict->current}});
            else
                return completed (
                  object_complete_creation_result_t{authority_generation_exhausted_t{}});
        } else {
            auto aborted = abort ({request.key, request.fence}, cancellation).result ().value ();
            if (std::holds_alternative<object_abort_stale_t> (aborted))
                return completed (
                  object_complete_creation_result_t{object_creation_completion_stale_t{}});
            if (const auto *conflict = std::get_if<object_abort_conflict_t> (&aborted))
                return completed (object_complete_creation_result_t{
                  object_creation_completion_conflict_t{conflict->current}});
        }
        const auto retention = std::chrono::duration_cast<std::chrono::milliseconds> (
          expires_at - std::chrono::system_clock::now ());
        auto stored = write (
          {{missing_condition (terminal_key)},
           {store_put_t{terminal_key, to_bytes (encode_terminal (terminal).dump ()), retention}}});
        if (!std::holds_alternative<store_write_applied_t> (stored)) {
            auto concurrent = read (terminal_key);
            if (const auto *found = std::get_if<store_found_t> (&concurrent))
                return completed (
                  object_complete_creation_result_t{object_creation_already_completed_result_t{
                    decode_terminal (parse_json (found->value.bytes))}});
            return completed (
              object_complete_creation_result_t{object_creation_completion_stale_t{}});
        }
        return completed (object_complete_creation_result_t{
          object_creation_completed_result_t{std::move (terminal), std::move (ready)}});
    }

    task_t<object_commit_result_t> commit (object_commit_request_t request,
                                           std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_commit_result_t> ();
        if (request.ready_payload.size () > 1024u * 1024u)
            throw std::invalid_argument ("object commit payload exceeds 1 MiB");
        const auto reservation_key = key_reservation (request.key);
        auto reservation = read (reservation_key);
        const auto *stored_reservation = std::get_if<store_found_t> (&reservation);
        if (!stored_reservation)
            return completed (object_commit_result_t{object_commit_stale_t{}});
        auto record = parse_json (stored_reservation->value.bytes);
        const auto stored_fence = decode_fence (record.at ("fence"));
        if (!same_fence (stored_fence, request.fence))
            return completed (object_commit_result_t{object_commit_stale_t{}});
        const auto status = record.at ("status").get<std::string> ();
        if (status == "committed")
            return completed (object_commit_result_t{object_already_committed_t{decode_authority (
              to_bytes (record.at ("snapshot").dump ()), std::string{},
              stored_reservation->value.store_now)}});
        if (status != "prepared")
            return completed (object_commit_result_t{object_commit_stale_t{}});

        const auto authority_key = key_authority (object_key (request.key));
        auto authority = read (authority_key);
        if (authority_mutation_locked (object_key (request.key)))
            return completed (object_commit_result_t{object_commit_conflict_t{
              read_authority_value (object_key (request.key))}});
        const auto *stored_authority = std::get_if<store_found_t> (&authority);
        if (!stored_authority)
            return completed (object_commit_result_t{object_commit_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}});
        auto snapshot =
          decode_authority (stored_authority->value.bytes, stored_authority->value.version,
                            stored_authority->value.store_now);
        if (snapshot.store_version != request.fence.expected_store_version
            || snapshot.allocation.state != placement_allocation_state_t::reserved)
            return completed (
              object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}});
        auto target = read_target_descriptor (request.fence.target);
        if (!target)
            return completed (
              object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}});
        if (!adjust_capacity (target->descriptor, request.fence.capacity_bundle, -1, 1))
            return completed (
              object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}});
        if (!advance_store_version (snapshot))
            return completed (object_commit_result_t{authority_generation_exhausted_t{}});
        snapshot.payload = std::move (request.ready_payload);
        snapshot.allocation.state = placement_allocation_state_t::active;
        snapshot.pending_creation.reset ();
        record["status"] = "committed";
        record["snapshot"] = parse_json (encode_authority (snapshot));
        auto written =
          write ({{version_condition (authority_key, stored_authority->value.version),
                   version_condition (reservation_key, stored_reservation->value.version),
                   version_condition (key_owner (request.fence.target.owner.owner_id),
                                      target->owner_provider_version),
                   version_condition (target->key, target->provider_version)},
                  {store_put_t{authority_key, encode_authority (snapshot), std::nullopt},
                   store_put_t{reservation_key, to_bytes (record.dump ()), std::nullopt},
                   store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
        if (!std::holds_alternative<store_write_applied_t> (written))
            return completed (object_commit_result_t{
              object_commit_conflict_t{read_authority_value (object_key (request.key))}});
        snapshot.store_now = std::get<store_write_applied_t> (written).store_now;
        return completed (object_commit_result_t{object_committed_t{std::move (snapshot)}});
    }

    task_t<object_abort_result_t> abort (object_abort_request_t request,
                                         std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_abort_result_t> ();
        const auto reservation_key = key_reservation (request.key);
        auto reservation = read (reservation_key);
        const auto *stored_reservation = std::get_if<store_found_t> (&reservation);
        if (!stored_reservation)
            return completed (object_abort_result_t{object_abort_stale_t{}});
        auto record = parse_json (stored_reservation->value.bytes);
        if (!same_fence (decode_fence (record.at ("fence")), request.fence))
            return completed (object_abort_result_t{object_abort_stale_t{}});
        const auto status = record.at ("status").get<std::string> ();
        if (status == "aborted")
            return completed (object_abort_result_t{object_already_aborted_t{}});
        if (status != "prepared")
            return completed (object_abort_result_t{object_abort_stale_t{}});
        const auto authority_key = key_authority (object_key (request.key));
        auto authority = read (authority_key);
        if (authority_mutation_locked (object_key (request.key)))
            return completed (object_abort_result_t{object_abort_conflict_t{
              read_authority_value (object_key (request.key))}});
        const auto *stored_authority = std::get_if<store_found_t> (&authority);
        if (!stored_authority)
            return completed (object_abort_result_t{object_abort_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}});
        const auto snapshot =
          decode_authority (stored_authority->value.bytes, stored_authority->value.version,
                            stored_authority->value.store_now);
        if (snapshot.store_version != request.fence.expected_store_version)
            return completed (object_abort_result_t{object_abort_conflict_t{snapshot}});
        auto target = read_target_descriptor (request.fence.target, false);
        if (!target || !adjust_capacity (target->descriptor, request.fence.capacity_bundle, -1, 0))
            return completed (object_abort_result_t{object_abort_conflict_t{snapshot}});
        record["status"] = "aborted";
        auto written =
          write ({{version_condition (authority_key, stored_authority->value.version),
                   version_condition (reservation_key, stored_reservation->value.version),
                   version_condition (target->key, target->provider_version)},
                  {store_delete_t{authority_key},
                   store_put_t{reservation_key, to_bytes (record.dump ()), std::nullopt},
                   store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
        if (!std::holds_alternative<store_write_applied_t> (written))
            return completed (object_abort_result_t{
              object_abort_conflict_t{read_authority_value (object_key (request.key))}});
        return completed (object_abort_result_t{object_aborted_t{}});
    }

    task_t<relocation_capacity_reserve_result_t>
    reserve_relocation_capacity (relocation_capacity_reserve_request_t request,
                                 std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_capacity_reserve_result_t> ();
        if (std::all_of (request.reservation_id.begin (), request.reservation_id.end (),
                         [] (std::byte value) { return value == std::byte{0}; })
            || request.key.value.empty () || request.expected_store_version.empty ()
            || request.stable_type.empty ()
            || !bundle_matches (request.capacity_bundle, request.object_kind, request.stable_type))
            throw std::invalid_argument ("relocation capacity reservation is incomplete");
        const auto id_key = key_relocation_capacity_id (request.reservation_id);
        auto existing_id = read (id_key);
        if (const auto *found = std::get_if<store_found_t> (&existing_id)) {
            const auto fence = to_string (found->value.bytes);
            auto existing = read (key_relocation_capacity (fence));
            if (const auto *stored = std::get_if<store_found_t> (&existing);
                stored && relocation_request_equal (parse_json (stored->value.bytes), request))
                return completed (relocation_capacity_reserve_result_t{
                  relocation_capacity_already_reserved_t{{fence}}});
            return completed (relocation_capacity_reserve_result_t{
              relocation_capacity_conflict_t{read_authority_value (request.key.value)}});
        }

        const auto authority_key = key_authority (request.key.value);
        auto authority = read (authority_key);
        if (authority_mutation_locked (request.key.value))
            return completed (relocation_capacity_reserve_result_t{
              relocation_capacity_conflict_t{read_authority_value (request.key.value)}});
        const auto *stored_authority = std::get_if<store_found_t> (&authority);
        if (!stored_authority)
            return completed (relocation_capacity_reserve_result_t{relocation_capacity_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}});
        const auto snapshot =
          decode_authority (stored_authority->value.bytes, stored_authority->value.version,
                            stored_authority->value.store_now);
        if (snapshot.store_version != request.expected_store_version
            || !same_owner (snapshot.owner, request.source.owner)
            || snapshot.allocation.target.mesh_name != request.source.mesh_name
            || snapshot.allocation.target.node_rid.value () != request.source.node_rid.value ()
            || snapshot.allocation.target.node_lifecycle_generation
                 != request.source.node_lifecycle_generation)
            return completed (
              relocation_capacity_reserve_result_t{relocation_capacity_conflict_t{snapshot}});
        auto target = read_target_descriptor (request.target);
        if (!target
            || !target_accepts (target->descriptor, request.object_kind, request.stable_type))
            return completed (
              relocation_capacity_reserve_result_t{relocation_capacity_target_unavailable_t{}});
        if (!capacity_available (target->descriptor, request.capacity_bundle))
            return completed (
              relocation_capacity_reserve_result_t{relocation_capacity_exhausted_t{}});
        if (!adjust_capacity (target->descriptor, request.capacity_bundle, 1, 0))
            return completed (
              relocation_capacity_reserve_result_t{relocation_capacity_exhausted_t{}});
        relocation_capacity_fence_t fence{"relocation-" + hex (request.reservation_id)};
        const auto row_key = key_relocation_capacity (fence.value);
        const auto encoded = encode_relocation_capacity (request, "reserved");
        auto written =
          write ({{missing_condition (id_key), missing_condition (row_key),
                   version_condition (authority_key, stored_authority->value.version),
                   version_condition (target->key, target->provider_version),
                   version_condition (key_owner (request.target.owner.owner_id),
                                      target->owner_provider_version)},
                  {store_put_t{id_key, to_bytes (fence.value), std::nullopt},
                   store_put_t{row_key, to_bytes (encoded.dump ()), std::nullopt},
                   store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
        if (!std::holds_alternative<store_write_applied_t> (written))
            return completed (relocation_capacity_reserve_result_t{
              relocation_capacity_conflict_t{read_authority_value (request.key.value)}});
        return completed (
          relocation_capacity_reserve_result_t{relocation_capacity_reserved_t{std::move (fence)}});
    }

    task_t<relocation_capacity_abort_result_t>
    abort_relocation_capacity (relocation_capacity_fence_t fence,
                               std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_capacity_abort_result_t> ();
        const auto key = key_relocation_capacity (fence.value);
        auto current = read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            return completed (relocation_capacity_abort_result_t::stale);
        auto record = parse_json (found->value.bytes);
        const auto status = record.value ("status", "");
        if (status == "committed")
            return completed (relocation_capacity_abort_result_t::already_committed);
        if (status == "aborted")
            return completed (relocation_capacity_abort_result_t::already_aborted);
        if (record.contains ("aggregateId")) {
            try {
                aggregate_id_t aggregate_id;
                aggregate_id.value =
                  unhex_array<16> (record.at ("aggregateId").get<std::string> ());
                auto aggregate = read (key_aggregate (aggregate_id));
                if (const auto *aggregate_found = std::get_if<store_found_t> (&aggregate)) {
                    const auto aggregate_record = parse_json (aggregate_found->value.bytes);
                    const auto aggregate_status = aggregate_record.value ("status", "");
                    if (aggregate_status == "committed")
                        return completed (relocation_capacity_abort_result_t::already_committed);
                    if (aggregate_status == "aborted")
                        return completed (relocation_capacity_abort_result_t::already_aborted);
                    if (aggregate_status == "prepared" || aggregate_status == "committing")
                        return completed (relocation_capacity_abort_result_t::stale);
                }
            }
            catch (...) {
                return completed (relocation_capacity_abort_result_t::stale);
            }
        }
        if (status != "reserved")
            return completed (relocation_capacity_abort_result_t::stale);
        auto target = read_target_descriptor (decode_target (record.at ("target")), false);
        if (!target
            || !adjust_capacity (target->descriptor, decode_bundle (record.at ("capacityBundle")),
                                 -1, 0))
            return completed (relocation_capacity_abort_result_t::stale);
        record["status"] = "aborted";
        auto written =
          write ({{version_condition (key, found->value.version),
                   version_condition (target->key, target->provider_version)},
                  {store_put_t{key, to_bytes (record.dump ()), std::nullopt},
                   store_put_t{target->key, encode_target_record (*target), std::nullopt}}});
        return completed (std::holds_alternative<store_write_applied_t> (written)
                            ? relocation_capacity_abort_result_t::aborted
                            : relocation_capacity_abort_result_t::stale);
    }

    task_t<aggregate_prepare_result_t>
    prepare_aggregate (aggregate_prepare_request_t request,
                       std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_prepare_result_t> ();
        if (request.aggregate_generation == 0 || request.aggregate_generation > max_generation
            || request.participants.empty ()
            || std::all_of (request.aggregate_id.value.begin (), request.aggregate_id.value.end (),
                            [] (std::byte value) { return value == std::byte{0}; })
            || request.target_owner.owner_id.empty () || request.target_owner.lease_generation <= 0
            || request.capacity_bundle.spot_slots != 1 || !request.capacity_bundle.spot_type
            || request.capacity_bundle.spot_type->object_kind != placement_object_kind_t::user_spot)
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        if (std::any_of (
              request.participants.begin (), request.participants.end (),
              [] (const aggregate_participant_t &participant) {
                  return !participant.membership_mutation.empty ();
              }))
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        const auto inventory_tree = aggregate_inventory::build_tree (request.participants);
        if (!inventory_tree)
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        if (!request.capacity_fences.empty ()
            && request.capacity_fences.size () != request.participants.size ())
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        for (std::size_t index = 0; index < request.participants.size (); ++index) {
            const auto &participant = request.participants[index];
            if (request.capacity_fences.empty ()) {
                if (participant.capacity_fence)
                    return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            } else if (!participant.capacity_fence
                       || participant.capacity_fence->value
                            != request.capacity_fences[index].value) {
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            }
        }
        std::string previous;
        std::vector<std::pair<store_key_t, store_found_t>> authorities;
        placement_capacity_bundle_t inventory;
        authorities.reserve (request.participants.size ());
        std::size_t participant_index = 0;
        for (const auto &participant : request.participants) {
            if (!previous.empty () && participant.key.value <= previous)
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            previous = participant.key.value;
            const auto key = key_authority (participant.key.value);
            auto row = read (key);
            const auto *found = std::get_if<store_found_t> (&row);
            if (!found)
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            const auto snapshot =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot.store_version != participant.expected_store_version
                || snapshot.allocation.state != placement_allocation_state_t::active
                || participant.owner_transition != authority_generation_transition_t::new_owner)
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            if (!request.capacity_fences.empty ()) {
                auto reservation = read (key_relocation_capacity (
                  request.capacity_fences[participant_index].value));
                const auto *stored_reservation =
                  std::get_if<store_found_t> (&reservation);
                if (!stored_reservation)
                    return completed (
                      aggregate_prepare_result_t{
                        aggregate_prepare_conflict_t{}});
                const auto record = parse_json (stored_reservation->value.bytes);
                const auto source = decode_target (record.at ("source"));
                const auto target = decode_target (record.at ("target"));
                if (record.value ("status", "") != "reserved"
                    || record.at ("authorityKey").get<std::string> ()
                         != participant.key.value
                    || record.at ("expectedStoreVersion").get<std::string> ()
                         != participant.expected_store_version
                    || encode_target (source)
                         != encode_target (snapshot.allocation.target)
                    || !same_owner (target.owner, request.target_owner)
                    || target.mesh_name != request.target_descriptor.mesh_name
                    || target.node_rid.value ()
                         != request.target_descriptor.rid.to_string ()
                    || target.node_lifecycle_generation
                         != request.target_descriptor_lifecycle_generation
                    || encode_bundle (record.at ("capacityBundle").is_object ()
                                        ? decode_bundle (record.at ("capacityBundle"))
                                        : placement_capacity_bundle_t{})
                         != encode_bundle (
                              snapshot.allocation.capacity_bundle))
                    return completed (
                      aggregate_prepare_result_t{
                        aggregate_prepare_conflict_t{}});
                if (record.contains ("aggregateId")
                    && record.at ("aggregateId").get<std::string> ()
                         != hex (request.aggregate_id.value))
                    return completed (
                      aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            }
            inventory.actor_slots += snapshot.allocation.capacity_bundle.actor_slots;
            inventory.spot_slots += snapshot.allocation.capacity_bundle.spot_slots;
            if (snapshot.allocation.capacity_bundle.spot_type) {
                const auto &spot = *snapshot.allocation.capacity_bundle.spot_type;
                if (inventory.spot_type
                    && (inventory.spot_type->object_kind != spot.object_kind
                        || inventory.spot_type->stable_type != spot.stable_type))
                    return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                if (!inventory.spot_type)
                    inventory.spot_type =
                      spot_type_capacity_delta_t{spot.object_kind, spot.stable_type, 0};
                inventory.spot_type->slots += spot.slots;
            }
            authorities.emplace_back (key, *found);
            ++participant_index;
        }
        if (encode_bundle (inventory) != encode_bundle (request.capacity_bundle))
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        const auto row_key = key_aggregate (request.aggregate_id);
        auto current = read (row_key);
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto stored = parse_json (found->value.bytes);
            const auto status = stored.value ("status", "");
            if (!aggregate_record_matches_request (stored, request, *inventory_tree))
                return completed (aggregate_prepare_result_t{aggregate_prepare_stale_t{}});
            if (status == "prepared" || status == "committing" || status == "committed")
                return completed (aggregate_prepare_result_t{aggregate_already_prepared_t{
                  {request.aggregate_id, request.aggregate_generation,
                   request.inventory_digest}}});
            if (status != "preparing")
                return completed (aggregate_prepare_result_t{aggregate_prepare_stale_t{}});
        } else {
            // Claim the aggregate before writing any inventory, lock or
            // reservation child. A restart can now find a partial prepare
            // and abort those children without guessing whether the claim
            // reached the provider.
            const auto preparing = encode_aggregate (request, "preparing", *inventory_tree);
            const auto claimed = write ({
              {missing_condition (row_key)},
              {store_put_t{row_key, to_bytes (preparing.dump ()), std::nullopt}}});
            if (!std::holds_alternative<store_write_applied_t> (claimed))
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        }

        const auto encoded = encode_aggregate (request, "prepared", *inventory_tree);
        std::size_t participant_offset = 0;
        std::size_t page_index = 0;
        for (const auto &page : inventory_tree->pages) {
            const auto page_key = key_aggregate_inventory (request.aggregate_id, page_index++);
            auto page_current = read (page_key);
            if (const auto *found = std::get_if<store_found_t> (&page_current)) {
                if (found->value.bytes != page.encoded)
                    return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            } else {
                store_write_request_t page_request;
                page_request.conditions.push_back (missing_condition (page_key));
                for (std::size_t index = 0; index < page.participants.size (); ++index)
                    page_request.conditions.push_back (version_condition (
                      authorities[participant_offset + index].first,
                      authorities[participant_offset + index].second.value.version));
                page_request.mutations.push_back (
                  store_put_t{page_key, page.encoded, std::nullopt});
                if (!std::holds_alternative<store_write_applied_t> (
                      write (std::move (page_request))))
                    return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            }

            // A prepared aggregate reserves the authority rows even though the
            // participant bytes remain in their existing rows. This bounded
            // lookup prevents a concurrent single-authority write from
            // invalidating the inventory before the aggregate CAS.
            store_write_request_t lock_request;
            for (std::size_t index = 0; index < page.participants.size (); ++index) {
                const auto participant_index = participant_offset + index;
                const auto lock_key = key_aggregate_lock (
                  request.participants[participant_index].key.value);
                auto existing_lock = read (lock_key);
                if (const auto *found = std::get_if<store_found_t> (&existing_lock)) {
                    const auto lock = decode_aggregate_lock (found->value.bytes);
                    if (!lock
                        || lock->authority_key != request.participants[participant_index].key.value)
                        return completed (
                          aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                    if (lock->aggregate_id.value == request.aggregate_id.value
                        && lock->aggregate_generation == request.aggregate_generation) {
                        if (lock->expected_store_version
                            != request.participants[participant_index].expected_store_version)
                            return completed (
                              aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                        continue;
                    }
                    auto old_aggregate = read (key_aggregate (lock->aggregate_id));
                    const auto *old_aggregate_found =
                      std::get_if<store_found_t> (&old_aggregate);
                    if (!old_aggregate_found
                        || parse_json (old_aggregate_found->value.bytes).value ("status", "")
                             != "committed")
                        return completed (
                          aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                    lock_request.conditions.push_back (
                      version_condition (lock_key, found->value.version));
                } else {
                    lock_request.conditions.push_back (missing_condition (lock_key));
                }
                lock_request.conditions.push_back (version_condition (
                  authorities[participant_index].first,
                  authorities[participant_index].second.value.version));
                lock_request.mutations.push_back (store_put_t{
                  lock_key,
                  to_bytes (encode_aggregate_lock (
                              request.aggregate_id, request.aggregate_generation,
                              request.participants[participant_index].key.value,
                              request.participants[participant_index].expected_store_version,
                              "prepared", page_index - 1, index)
                              .dump ()),
                  std::nullopt});
            }
            if (!lock_request.mutations.empty ()
                && !std::holds_alternative<store_write_applied_t> (
                  write (std::move (lock_request))))
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});

            store_write_request_t reservation_request;
            for (std::size_t index = 0; index < page.participants.size (); ++index) {
                const auto participant_index = participant_offset + index;
                const auto &participant = request.participants[participant_index];
                if (!participant.capacity_fence)
                    continue;
                const auto reservation_key =
                  key_relocation_capacity (participant.capacity_fence->value);
                auto reservation = read (reservation_key);
                const auto *stored_reservation = std::get_if<store_found_t> (&reservation);
                if (!stored_reservation)
                    return completed (
                      aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                auto reservation_record = parse_json (stored_reservation->value.bytes);
                if (reservation_record.contains ("aggregateId")) {
                    if (reservation_record.at ("aggregateId").get<std::string> ()
                            != hex (request.aggregate_id.value)
                        || reservation_record.value ("aggregateGeneration", 0ull)
                             != request.aggregate_generation
                        || reservation_record.value ("status", "") != "prepared")
                        return completed (
                          aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                    continue;
                }
                reservation_record["aggregateId"] = hex (request.aggregate_id.value);
                reservation_record["aggregateGeneration"] = request.aggregate_generation;
                reservation_record["status"] = "prepared";
                reservation_request.conditions.push_back (
                  version_condition (reservation_key, stored_reservation->value.version));
                reservation_request.mutations.push_back (store_put_t{
                  reservation_key, to_bytes (reservation_record.dump ()), std::nullopt});
            }
            if (!reservation_request.mutations.empty ()
                && !std::holds_alternative<store_write_applied_t> (
                  write (std::move (reservation_request))))
                return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
            participant_offset += page.participants.size ();
        }

        for (const auto &index_page : inventory_tree->index_pages) {
            const auto index_key = key_aggregate_inventory_index (
              request.aggregate_id, index_page.level, index_page.page_index);
            auto current_index = read (index_key);
            if (const auto *found = std::get_if<store_found_t> (&current_index)) {
                if (found->value.bytes != index_page.encoded)
                    return completed (
                      aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
                continue;
            }
            store_write_request_t index_request;
            index_request.conditions.push_back (missing_condition (index_key));
            index_request.mutations.push_back (
              store_put_t{index_key, index_page.encoded, std::nullopt});
            if (!std::holds_alternative<store_write_applied_t> (
                  write (std::move (index_request))))
                return completed (
                  aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        }

        const object_creation_target_t target{
          request.target_descriptor.mesh_name,
          node_rid_t::from_string (request.target_descriptor.rid.to_string ()),
          request.target_descriptor_lifecycle_generation, request.target_owner};
        auto target_descriptor = read_target_descriptor (target);
        if (!target_descriptor
            || (!request.capacity_fences.empty ()
                && request.capacity_fences.size () != request.participants.size ())
            || (request.capacity_fences.empty ()
                && !capacity_available (target_descriptor->descriptor, request.capacity_bundle)))
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        if (request.capacity_fences.empty ()
            && !adjust_capacity (target_descriptor->descriptor, request.capacity_bundle, 1, 0))
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        auto preparing_row = read (row_key);
        const auto *preparing_found = std::get_if<store_found_t> (&preparing_row);
        if (!preparing_found)
            return completed (aggregate_prepare_result_t{aggregate_prepare_stale_t{}});
        const auto preparing_record = parse_json (preparing_found->value.bytes);
        if (preparing_record.value ("status", "") != "preparing") {
            if (preparing_record.value ("status", "") == "prepared"
                || preparing_record.value ("status", "") == "committing"
                || preparing_record.value ("status", "") == "committed")
                return completed (aggregate_prepare_result_t{aggregate_already_prepared_t{
                  {request.aggregate_id, request.aggregate_generation,
                   request.inventory_digest}}});
            return completed (aggregate_prepare_result_t{aggregate_prepare_stale_t{}});
        }
        store_write_request_t write_request;
        write_request.conditions.push_back (
          version_condition (row_key, preparing_found->value.version));
        write_request.conditions.push_back (
          version_condition (target_descriptor->key, target_descriptor->provider_version));
        write_request.conditions.push_back (version_condition (
          key_owner (request.target_owner.owner_id), target_descriptor->owner_provider_version));
        write_request.mutations.push_back (
          store_put_t{row_key, to_bytes (encoded.dump ()), std::nullopt});
        write_request.mutations.push_back (store_put_t{
          target_descriptor->key, encode_target_record (*target_descriptor), std::nullopt});
        auto written = write (std::move (write_request));
        if (!std::holds_alternative<store_write_applied_t> (written))
            return completed (aggregate_prepare_result_t{aggregate_prepare_conflict_t{}});
        return completed (aggregate_prepare_result_t{
          aggregate_prepared_t{{request.aggregate_id, request.aggregate_generation,
                                request.inventory_digest}}});
    }

    task_t<aggregate_commit_result_t> commit_aggregate (aggregate_fence_t fence,
                                                        std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_commit_result_t> ();
        const auto row_key = key_aggregate (fence.aggregate_id);
        auto current = read (row_key);
        const auto *stored = std::get_if<store_found_t> (&current);
        if (!stored)
            return completed (aggregate_commit_result_t::stale);
        auto record = parse_json (stored->value.bytes);
        if (record.at ("aggregateGeneration").get<std::uint64_t> () != fence.aggregate_generation)
            return completed (aggregate_commit_result_t::stale);
        const auto status = record.value ("status", "");
        if (status == "committed")
            return completed (aggregate_commit_result_t::already_committed);
        if (status != "prepared" && status != "committing")
            return completed (aggregate_commit_result_t::stale);
        const auto target_owner = decode_owner (record.at ("targetOwner"));
        const bool resuming_commit = status == "committing";
        const object_creation_target_t target{
          record.at ("targetMeshName").get<std::string> (),
          node_rid_t::from_string (record.at ("targetNodeRid").get<std::string> ()),
          record.at ("targetLifecycleGeneration").get<std::uint64_t> (), target_owner};
        // Before the aggregate enters committing, the target lease is part of
        // the admission fence. Once committing is durable, the authority rows
        // are still hidden by their locks, so the same target descriptor can
        // finish or be rolled back even if its lease expires between retries.
        auto target_descriptor = read_target_descriptor (target, !resuming_commit);
        if (!target_descriptor)
            return completed (aggregate_commit_result_t::stale);
        if (!resuming_commit && !owner_is_live (target_owner))
            return completed (aggregate_commit_result_t::stale);

        const auto inventory_count = record.value ("inventoryCount", std::size_t{0});
        const auto inventory_page_count = record.value ("inventoryPageCount", std::size_t{0});
        const auto expected_inventory_page_count =
          inventory_count / aggregate_inventory::page_item_limit
          + (inventory_count % aggregate_inventory::page_item_limit != 0 ? 1 : 0);
        if (inventory_count < 2 || inventory_page_count == 0
            || expected_inventory_page_count != inventory_page_count)
            return completed (aggregate_commit_result_t::stale);
        std::vector<aggregate_participant_t> participants;
        participants.reserve (inventory_count);
        for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
            const auto page = read (key_aggregate_inventory (fence.aggregate_id, page_index));
            const auto *found = std::get_if<store_found_t> (&page);
            if (!found)
                return completed (aggregate_commit_result_t::stale);
            const auto decoded = aggregate_inventory::decode_page (
              found->value.bytes, page_index);
            if (!decoded)
                return completed (aggregate_commit_result_t::stale);
            participants.insert (participants.end (), decoded->begin (), decoded->end ());
        }
        if (participants.size () != inventory_count)
            return completed (aggregate_commit_result_t::stale);
        const auto inventory_tree = aggregate_inventory::build_tree (participants);
        if (!inventory_tree || inventory_tree->participant_count != inventory_count
            || inventory_tree->pages.size () != inventory_page_count
            || hex (inventory_tree->root) != record.at ("inventoryRoot").get<std::string> ())
            return completed (aggregate_commit_result_t::stale);
        for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
            const auto page = read (key_aggregate_inventory (fence.aggregate_id, page_index));
            const auto *found = std::get_if<store_found_t> (&page);
            if (!found
                || found->value.bytes != inventory_tree->pages[page_index].encoded
                || sha256 (found->value.bytes) != inventory_tree->pages[page_index].digest)
                return completed (aggregate_commit_result_t::stale);
        }
        const auto inventory_index_page_count =
          record.value ("inventoryIndexPageCount", std::size_t{0});
        const auto inventory_index_level_count =
          record.value ("inventoryIndexLevelCount", std::size_t{0});
        if (inventory_index_page_count != inventory_tree->index_pages.size ()
            || inventory_index_level_count != inventory_tree->index_level_count)
            return completed (aggregate_commit_result_t::stale);
        for (const auto &index_page : inventory_tree->index_pages) {
            const auto index = read (key_aggregate_inventory_index (
              fence.aggregate_id, index_page.level, index_page.page_index));
            const auto *found = std::get_if<store_found_t> (&index);
            const auto decoded = found
                                   ? aggregate_inventory::decode_index_page (
                                       found->value.bytes, index_page.level,
                                       index_page.page_index,
                                       index_page.child_start)
                                   : std::optional<aggregate_inventory::index_page_t>{};
            if (!found || !decoded
                || found->value.bytes != index_page.encoded
                || decoded->digest != index_page.digest)
                return completed (aggregate_commit_result_t::stale);
        }
        if (fence.inventory_digest) {
            const auto stored_digest =
              unhex_array<32> (record.at ("inventoryDigest").get<std::string> ());
            if (stored_digest != fence.inventory_digest->value)
                return completed (aggregate_commit_result_t::stale);
        }

        const auto participant_count = participants.size ();
        if (participant_count > max_generation)
            return completed (aggregate_commit_result_t::generation_exhausted);
        std::uint64_t owner_generation_start = 0;
        if (status == "prepared") {
            auto generations = read (generation_counter_key);
            std::uint64_t next_owner_generation = 0;
            if (const auto *generation = std::get_if<store_found_t> (&generations))
                next_owner_generation =
                  parse_json (generation->value.bytes).at ("authorityOwnerGeneration")
                    .get<std::uint64_t> ();
            if (next_owner_generation > max_generation - participant_count)
                return completed (aggregate_commit_result_t::generation_exhausted);
            owner_generation_start = next_owner_generation + 1;
        } else {
            owner_generation_start =
              record.value ("ownerGenerationStart", std::uint64_t{0});
            if (owner_generation_start == 0
                || record.value ("ownerGenerationEnd", std::uint64_t{0})
                     != owner_generation_start + participant_count - 1)
                return completed (aggregate_commit_result_t::stale);
        }

        std::vector<aggregate_commit_entry_t> entries;
        entries.reserve (participants.size ());
        std::map<std::string, stored_target_t> descriptors;
        descriptors.emplace (target_descriptor->key.value, *target_descriptor);

        const auto valid_capacity_fence = [&] (const aggregate_participant_t &participant,
                                               const authority_snapshot_t &snapshot) {
            if (!participant.capacity_fence)
                return true;
            auto reservation = read (key_relocation_capacity (
              participant.capacity_fence->value));
            const auto *stored_reservation = std::get_if<store_found_t> (&reservation);
            if (!stored_reservation)
                return false;
            const auto reservation_record = parse_json (stored_reservation->value.bytes);
            if (reservation_record.value ("status", "") != "prepared"
                || reservation_record.at ("authorityKey").get<std::string> ()
                     != participant.key.value
                || reservation_record.at ("expectedStoreVersion").get<std::string> ()
                     != participant.expected_store_version
                || !same_target (decode_target (reservation_record.at ("target")), target)
                || !same_owner (decode_owner (reservation_record.at ("targetOwner")),
                                target_owner)
                || encode_bundle (decode_bundle (reservation_record.at ("capacityBundle")))
                     != encode_bundle (snapshot.allocation.capacity_bundle))
                return false;
            if (reservation_record.contains ("aggregateId")
                && reservation_record.at ("aggregateId").get<std::string> ()
                     != hex (fence.aggregate_id.value))
                return false;
            return true;
        };

        for (std::size_t participant_index = 0;
             participant_index < participants.size ();
             ++participant_index) {
            const auto &participant = participants[participant_index];
            const auto authority_key = key_authority (participant.key.value);
            auto authority = read (authority_key);
            const auto *found = std::get_if<store_found_t> (&authority);
            if (!found)
                return completed (aggregate_commit_result_t::stale);
            auto lock_result = read (key_aggregate_lock (participant.key.value));
            const auto *stored_lock = std::get_if<store_found_t> (&lock_result);
            if (!stored_lock)
                return completed (aggregate_commit_result_t::stale);
            const auto lock = decode_aggregate_lock (stored_lock->value.bytes);
            if (!lock || lock->aggregate_id.value != fence.aggregate_id.value
                || lock->aggregate_generation != fence.aggregate_generation
                || lock->authority_key != participant.key.value
                || lock->expected_store_version != participant.expected_store_version
                || (lock->status != "prepared" && lock->status != "committing"))
                return completed (aggregate_commit_result_t::stale);

            aggregate_commit_entry_t entry;
            if (lock->status == "committing") {
                if (!lock->page_index || !lock->entry_index)
                    return completed (aggregate_commit_result_t::stale);
                auto page = read (key_aggregate_commit_page (
                  fence.aggregate_id, *lock->page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    return completed (aggregate_commit_result_t::stale);
                const auto decoded_page = decode_aggregate_commit_page (stored_page->value.bytes);
                if (!decoded_page || *lock->entry_index >= decoded_page->size ())
                    return completed (aggregate_commit_result_t::stale);
                entry = (*decoded_page)[*lock->entry_index];
                if (entry.authority_key != participant.key.value
                    || found->value.bytes != entry.after)
                    return completed (aggregate_commit_result_t::stale);
            } else {
                auto before = decode_authority (found->value.bytes, found->value.version,
                                                found->value.store_now);
                if (before.store_version != participant.expected_store_version
                    || participant.owner_transition
                         != authority_generation_transition_t::new_owner
                    || !valid_capacity_fence (participant, before)
                    || !owner_is_live (before.owner))
                    return completed (aggregate_commit_result_t::stale);
                auto after = before;
                after.authority_owner_generation = owner_generation_start + participant_index;
                after.owner = target_owner;
                after.allocation.target = target;
                after.payload = participant.authority_payload;
                if (!advance_store_version (after))
                    return completed (aggregate_commit_result_t::generation_exhausted);
                entry = {participant.key.value, found->value.bytes, encode_authority (after)};
            }

            const auto before = decode_authority (entry.before, found->value.version,
                                                 found->value.store_now);
            const auto source = read_target_descriptor (before.allocation.target, false);
            if (!source)
                return completed (aggregate_commit_result_t::stale);
            auto [source_state, inserted] = descriptors.emplace (source->key.value, *source);
            if (!inserted && source_state->second.provider_version != source->provider_version)
                return completed (aggregate_commit_result_t::stale);
            if (!adjust_capacity (source_state->second.descriptor,
                                  before.allocation.capacity_bundle, 0, -1))
                return completed (aggregate_commit_result_t::stale);
            entries.push_back (std::move (entry));
        }

        // The final CAS contains the aggregate row, every distinct source or
        // target descriptor, and the target owner lease condition. Bound that
        // set before any committing page is installed; a provider must never
        // reject the terminal CAS after authority pages have been staged.
        if (descriptors.size () + 2 > aggregate_commit_final_key_limit)
            return completed (aggregate_commit_result_t::stale);

        const auto commit_pages = split_aggregate_commit_entries (entries);
        if (!commit_pages)
            return completed (aggregate_commit_result_t::stale);
        if (status == "prepared") {
            auto generations = read (generation_counter_key);
            std::uint64_t object_generation = 0;
            std::uint64_t current_owner_generation = 0;
            if (const auto *generation = std::get_if<store_found_t> (&generations)) {
                const auto counters = parse_json (generation->value.bytes);
                object_generation = counters.at ("objectGeneration").get<std::uint64_t> ();
                current_owner_generation =
                  counters.at ("authorityOwnerGeneration").get<std::uint64_t> ();
            }
            if (current_owner_generation + participant_count
                != owner_generation_start + participant_count - 1)
                return completed (aggregate_commit_result_t::stale);
            record["status"] = "committing";
            record["ownerGenerationStart"] = owner_generation_start;
            record["ownerGenerationEnd"] = owner_generation_start + participant_count - 1;
            record["commitPageCount"] = commit_pages->size ();
            auto transition = write (
              {{version_condition (row_key, stored->value.version),
                condition_for (generation_counter_key, generations)},
               {store_put_t{row_key, to_bytes (record.dump ()), std::nullopt},
                store_put_t{generation_counter_key,
                            to_bytes (json_t{{"objectGeneration", object_generation},
                                              {"authorityOwnerGeneration",
                                               current_owner_generation + participant_count}}
                                        .dump ()),
                            std::nullopt}}});
            if (!std::holds_alternative<store_write_applied_t> (transition))
                return completed (aggregate_commit_result_t::stale);
        }

        for (std::size_t page_index = 0; page_index < commit_pages->size (); ++page_index) {
            const auto page_key = key_aggregate_commit_page (fence.aggregate_id, page_index);
            const auto encoded_page = to_bytes (
              encode_aggregate_commit_page (page_index, (*commit_pages)[page_index]).dump ());
            auto existing_page = read (page_key);
            if (const auto *found_page = std::get_if<store_found_t> (&existing_page)) {
                if (found_page->value.bytes != encoded_page)
                    return completed (aggregate_commit_result_t::stale);
                continue;
            }
            store_write_request_t page_request;
            page_request.conditions.push_back (missing_condition (page_key));
            for (std::size_t entry_index = 0;
                 entry_index < (*commit_pages)[page_index].size ();
                 ++entry_index) {
                const auto participant_index =
                  std::accumulate (commit_pages->begin (),
                                   commit_pages->begin () + static_cast<std::ptrdiff_t> (page_index),
                                   std::size_t{0},
                                   [] (std::size_t count,
                                       const std::vector<aggregate_commit_entry_t> &page) {
                                       return count + page.size ();
                                   })
                  + entry_index;
                const auto &participant = participants[participant_index];
                const auto &entry = (*commit_pages)[page_index][entry_index];
                auto authority = read (key_authority (participant.key.value));
                const auto *found_authority = std::get_if<store_found_t> (&authority);
                auto lock_result = read (key_aggregate_lock (participant.key.value));
                const auto *found_lock = std::get_if<store_found_t> (&lock_result);
                if (!found_authority || !found_lock)
                    return completed (aggregate_commit_result_t::stale);
                const auto lock = decode_aggregate_lock (found_lock->value.bytes);
                if (!lock || lock->status != "prepared")
                    return completed (aggregate_commit_result_t::stale);
                page_request.conditions.push_back (
                  version_condition (key_authority (participant.key.value),
                                     found_authority->value.version));
                page_request.conditions.push_back (
                  version_condition (key_aggregate_lock (participant.key.value),
                                     found_lock->value.version));
                page_request.mutations.push_back (
                  store_put_t{key_authority (participant.key.value), entry.after, std::nullopt});
                page_request.mutations.push_back (store_put_t{
                  key_aggregate_lock (participant.key.value),
                  to_bytes (encode_aggregate_lock (
                              fence.aggregate_id, fence.aggregate_generation,
                              participant.key.value, participant.expected_store_version,
                              "committing", page_index, entry_index)
                              .dump ()),
                  std::nullopt});
                if (participant.capacity_fence) {
                    auto reservation = read (key_relocation_capacity (
                      participant.capacity_fence->value));
                    const auto *stored_reservation =
                      std::get_if<store_found_t> (&reservation);
                    if (!stored_reservation)
                        return completed (aggregate_commit_result_t::stale);
                    auto reservation_record = parse_json (stored_reservation->value.bytes);
                    if (reservation_record.value ("status", "") != "prepared"
                        || reservation_record.at ("authorityKey").get<std::string> ()
                             != participant.key.value
                        || (reservation_record.contains ("aggregateId")
                            && reservation_record.at ("aggregateId").get<std::string> ()
                                 != hex (fence.aggregate_id.value)))
                        return completed (aggregate_commit_result_t::stale);
                    // Keep the reservation logically prepared until the
                    // aggregate row becomes committed. The aggregate terminal
                    // state is the authoritative commit marker, so a failed
                    // final CAS can still release every prepared reservation.
                }
            }
            page_request.mutations.push_back (store_put_t{page_key, encoded_page, std::nullopt});
            if (!std::holds_alternative<store_write_applied_t> (
                  write (std::move (page_request))))
                return completed (aggregate_commit_result_t::stale);
        }

        if (status == "prepared") {
            auto current_record = read (row_key);
            const auto *current_found = std::get_if<store_found_t> (&current_record);
            if (!current_found)
                return completed (aggregate_commit_result_t::stale);
            record = parse_json (current_found->value.bytes);
            stored = current_found;
        } else {
            auto current_record = read (row_key);
            const auto *current_found = std::get_if<store_found_t> (&current_record);
            if (!current_found)
                return completed (aggregate_commit_result_t::stale);
            stored = current_found;
            record = parse_json (current_found->value.bytes);
        }
        if (record.value ("status", "") != "committing")
            return completed (aggregate_commit_result_t::stale);

        auto target_state = descriptors.find (target_descriptor->key.value);
        if (target_state == descriptors.end ()
            || !adjust_capacity (target_state->second.descriptor,
                                 decode_bundle (record.at ("capacityBundle")), -1, 1))
            return completed (aggregate_commit_result_t::stale);
        store_write_request_t final_request;
        final_request.conditions.push_back (version_condition (
          row_key, stored->value.version));
        if (!target_descriptor->owner_provider_version.empty ())
            final_request.conditions.push_back (
              version_condition (key_owner (target_owner.owner_id),
                                 target_descriptor->owner_provider_version));
        for (auto &[descriptor_key, descriptor] : descriptors) {
            (void) descriptor_key;
            final_request.conditions.push_back (
              version_condition (descriptor.key, descriptor.provider_version));
            final_request.mutations.push_back (
              store_put_t{descriptor.key, encode_target_record (descriptor), std::nullopt});
        }
        record["status"] = "committed";
        final_request.mutations.push_back (
          store_put_t{row_key, to_bytes (record.dump ()), std::nullopt});
        auto written = write (std::move (final_request));
        return completed (std::holds_alternative<store_write_applied_t> (written)
                            ? aggregate_commit_result_t::committed
                            : aggregate_commit_result_t::stale);
    }

    task_t<aggregate_abort_result_t> abort_aggregate (aggregate_fence_t fence,
                                                      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_abort_result_t> ();
        const auto key = key_aggregate (fence.aggregate_id);
        auto current = read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            return completed (aggregate_abort_result_t::stale);
        auto record = parse_json (found->value.bytes);
        if (record.at ("aggregateGeneration").get<std::uint64_t> () != fence.aggregate_generation)
            return completed (aggregate_abort_result_t::stale);
        const auto status = record.value ("status", "");
        if (status == "aborted")
            return completed (aggregate_abort_result_t::already_aborted);
        if (status != "preparing" && status != "prepared" && status != "committing")
            return completed (aggregate_abort_result_t::stale);
        const object_creation_target_t target{
          record.at ("targetMeshName").get<std::string> (),
          node_rid_t::from_string (record.at ("targetNodeRid").get<std::string> ()),
          record.at ("targetLifecycleGeneration").get<std::uint64_t> (),
          decode_owner (record.at ("targetOwner"))};

        if (status == "committing") {
            const auto commit_page_count = record.value ("commitPageCount", std::size_t{0});
            if (commit_page_count == 0)
                return completed (aggregate_abort_result_t::stale);
            for (std::size_t page_index = 0; page_index < commit_page_count; ++page_index) {
                auto page = read (key_aggregate_commit_page (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    return completed (aggregate_abort_result_t::stale);
                const auto entries = decode_aggregate_commit_page (
                  stored_page->value.bytes, page_index);
                if (!entries)
                    return completed (aggregate_abort_result_t::stale);
                store_write_request_t rollback;
                for (std::size_t entry_index = 0; entry_index < entries->size (); ++entry_index) {
                    const auto &entry = (*entries)[entry_index];
                    auto authority = read (key_authority (entry.authority_key));
                    const auto *stored_authority = std::get_if<store_found_t> (&authority);
                    auto lock = read (key_aggregate_lock (entry.authority_key));
                    const auto *stored_lock = std::get_if<store_found_t> (&lock);
                    if (!stored_authority)
                        return completed (aggregate_abort_result_t::stale);
                    if (stored_authority->value.bytes != entry.before
                        && stored_authority->value.bytes != entry.after)
                        return completed (aggregate_abort_result_t::stale);
                    if (stored_lock) {
                        const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                        if (!decoded_lock
                            || decoded_lock->aggregate_id.value != fence.aggregate_id.value
                            || decoded_lock->aggregate_generation != fence.aggregate_generation
                            || decoded_lock->authority_key != entry.authority_key
                            || decoded_lock->status != "committing"
                            || decoded_lock->page_index != page_index
                            || decoded_lock->entry_index != entry_index)
                            return completed (aggregate_abort_result_t::stale);
                        rollback.conditions.push_back (version_condition (
                          key_aggregate_lock (entry.authority_key), stored_lock->value.version));
                        rollback.mutations.push_back (
                          store_delete_t{key_aggregate_lock (entry.authority_key)});
                    } else if (stored_authority->value.bytes != entry.before) {
                        return completed (aggregate_abort_result_t::stale);
                    }
                    if (stored_authority->value.bytes == entry.after) {
                        rollback.conditions.push_back (version_condition (
                          key_authority (entry.authority_key), stored_authority->value.version));
                        rollback.mutations.push_back (store_put_t{
                          key_authority (entry.authority_key), entry.before, std::nullopt});
                    }
                }
                if (!rollback.mutations.empty ()
                    && !std::holds_alternative<store_write_applied_t> (
                      write (std::move (rollback))))
                    return completed (aggregate_abort_result_t::stale);
            }
        }

        const bool target_capacity_reserved = status != "preparing";
        std::optional<stored_target_t> target_descriptor;
        if (target_capacity_reserved) {
            target_descriptor = read_target_descriptor (target, false);
            if (!target_descriptor
                || !adjust_capacity (target_descriptor->descriptor,
                                     decode_bundle (record.at ("capacityBundle")), -1, 0))
                return completed (aggregate_abort_result_t::stale);
        }

        const auto inventory_page_count = record.value ("inventoryPageCount", std::size_t{0});
        for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
            auto page = read (key_aggregate_inventory (fence.aggregate_id, page_index));
            const auto *stored_page = std::get_if<store_found_t> (&page);
            if (!stored_page) {
                // A preparing aggregate claims its row before the first
                // child page. Missing pages after that point cannot own a
                // lock or reservation, so the durable claim can still be
                // marked aborted without inventing participant keys.
                if (status == "preparing")
                    break;
                return completed (aggregate_abort_result_t::stale);
            }
            const auto participants = aggregate_inventory::decode_page (
              stored_page->value.bytes, page_index);
            if (!participants)
                return completed (aggregate_abort_result_t::stale);
            store_write_request_t cleanup;
            for (std::size_t entry_index = 0; entry_index < participants->size (); ++entry_index) {
                const auto &participant = (*participants)[entry_index];
                const auto lock_key = key_aggregate_lock (participant.key.value);
                auto lock = read (lock_key);
                if (const auto *stored_lock = std::get_if<store_found_t> (&lock)) {
                    const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                    if (!decoded_lock
                        || decoded_lock->aggregate_id.value != fence.aggregate_id.value
                        || decoded_lock->aggregate_generation != fence.aggregate_generation)
                        return completed (aggregate_abort_result_t::stale);
                    cleanup.conditions.push_back (
                      version_condition (lock_key, stored_lock->value.version));
                    cleanup.mutations.push_back (store_delete_t{lock_key});
                }
                if (!participant.capacity_fence)
                    continue;
                const auto reservation_key =
                  key_relocation_capacity (participant.capacity_fence->value);
                auto reservation = read (reservation_key);
                const auto *stored_reservation =
                  std::get_if<store_found_t> (&reservation);
                if (!stored_reservation) {
                    if (status == "preparing")
                        continue;
                    return completed (aggregate_abort_result_t::stale);
                }
                auto reservation_record = parse_json (stored_reservation->value.bytes);
                if (!reservation_record.contains ("aggregateId")
                    || reservation_record.at ("aggregateId").get<std::string> ()
                         != hex (fence.aggregate_id.value))
                    return completed (aggregate_abort_result_t::stale);
                const auto reservation_status = reservation_record.value ("status", "");
                if (reservation_status == "committed")
                    return completed (aggregate_abort_result_t::stale);
                if (reservation_status == "prepared") {
                    reservation_record["status"] = "aborted";
                    cleanup.conditions.push_back (
                      version_condition (reservation_key, stored_reservation->value.version));
                    cleanup.mutations.push_back (store_put_t{
                      reservation_key, to_bytes (reservation_record.dump ()), std::nullopt});
                } else if (reservation_status != "aborted") {
                    return completed (aggregate_abort_result_t::stale);
                }
            }
            if (!cleanup.mutations.empty ()
                && !std::holds_alternative<store_write_applied_t> (
                  write (std::move (cleanup))))
                return completed (aggregate_abort_result_t::stale);
        }
        record["status"] = "aborted";
        store_write_request_t final_request;
        final_request.conditions.push_back (version_condition (key, found->value.version));
        final_request.mutations.push_back (
          store_put_t{key, to_bytes (record.dump ()), std::nullopt});
        if (target_descriptor) {
            final_request.conditions.push_back (
              version_condition (target_descriptor->key, target_descriptor->provider_version));
            if (!target_descriptor->owner_provider_version.empty ())
                final_request.conditions.push_back (version_condition (
                  key_owner (target.owner.owner_id), target_descriptor->owner_provider_version));
            final_request.mutations.push_back (store_put_t{
              target_descriptor->key, encode_target_record (*target_descriptor), std::nullopt});
        }
        auto written = write (std::move (final_request));
        return completed (std::holds_alternative<store_write_applied_t> (written)
                            ? aggregate_abort_result_t::aborted
                            : aggregate_abort_result_t::stale);
    }

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<std::optional<std::vector<aggregate_participant_t>>> ();
        try {
            const auto current = read (key_aggregate (fence.aggregate_id));
            const auto *found = std::get_if<store_found_t> (&current);
            if (!found)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            const auto record = parse_json (found->value.bytes);
            if (record.at ("aggregateGeneration").get<std::uint64_t> ()
                    != fence.aggregate_generation)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            const auto count = record.at ("inventoryCount").get<std::size_t> ();
            const auto page_count =
              record.at ("inventoryPageCount").get<std::size_t> ();
            const auto expected_page_count =
              count / aggregate_inventory::page_item_limit
              + (count % aggregate_inventory::page_item_limit != 0 ? 1 : 0);
            if (count < 2 || page_count == 0 || expected_page_count != page_count)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            std::vector<aggregate_participant_t> participants;
            participants.reserve (count);
            for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
                const auto page = read (
                  key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    return completed (
                      std::optional<std::vector<aggregate_participant_t>>{});
                const auto decoded = aggregate_inventory::decode_page (
                  stored_page->value.bytes, page_index);
                if (!decoded)
                    return completed (
                      std::optional<std::vector<aggregate_participant_t>>{});
                participants.insert (
                  participants.end (), decoded->begin (), decoded->end ());
            }
            if (participants.size () != count)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            const auto tree = aggregate_inventory::build_tree (participants);
            if (!tree || tree->participant_count != count
                || tree->pages.size () != page_count
                || hex (tree->root)
                     != record.at ("inventoryRoot").get<std::string> ())
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
                const auto page = read (
                  key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page
                    || stored_page->value.bytes
                         != tree->pages[page_index].encoded
                    || sha256 (stored_page->value.bytes)
                         != tree->pages[page_index].digest)
                    return completed (
                      std::optional<std::vector<aggregate_participant_t>>{});
            }
            const auto index_page_count =
              record.value ("inventoryIndexPageCount", std::size_t{0});
            const auto index_level_count =
              record.value ("inventoryIndexLevelCount", std::size_t{0});
            if (index_page_count != tree->index_pages.size ()
                || index_level_count != tree->index_level_count)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            for (const auto &index_page : tree->index_pages) {
                const auto index = read (key_aggregate_inventory_index (
                  fence.aggregate_id, index_page.level, index_page.page_index));
                const auto *stored_index = std::get_if<store_found_t> (&index);
                const auto decoded = stored_index
                                       ? aggregate_inventory::decode_index_page (
                                           stored_index->value.bytes,
                                           index_page.level,
                                           index_page.page_index,
                                           index_page.child_start)
                                       : std::optional<aggregate_inventory::index_page_t>{};
                if (!stored_index || !decoded
                    || stored_index->value.bytes != index_page.encoded
                    || decoded->digest != index_page.digest)
                    return completed (
                      std::optional<std::vector<aggregate_participant_t>>{});
            }
            if (fence.inventory_digest
                && unhex_array<32> (
                     record.at ("inventoryDigest").get<std::string> ())
                     != fence.inventory_digest->value)
                return completed (
                  std::optional<std::vector<aggregate_participant_t>>{});
            return completed (
              std::optional<std::vector<aggregate_participant_t>>{
                std::move (participants)});
        }
        catch (...) {
            return completed (
              std::optional<std::vector<aggregate_participant_t>>{});
        }
    }

    task_t<std::int64_t> remove_all_by_owner (location_owner_token_t owner) override
    {
        std::int64_t removed = 0;
        removed += remove_owned (std::string (prefix) + "mesh:", owner);
        removed += remove_owned (std::string (prefix) + "client-server:", owner);
        removed += remove_owned (std::string (prefix) + "fanout:", owner);
        return completed (removed);
    }

  private:
    using json_t = nlohmann::json;
    static constexpr std::string_view prefix = "zlink:v11:";
    static constexpr std::string_view authority_prefix = "zlink:v11:authority:";
    static constexpr std::uint64_t max_generation =
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    inline static const store_key_t counter_key{std::string (prefix) + "owner-counter"};
    inline static const store_key_t generation_counter_key{std::string (prefix)
                                                           + "authority-generations"};

    struct stored_target_t
    {
        store_key_t key;
        std::string provider_version;
        std::string owner_provider_version;
        mesh_node_descriptor_t descriptor;
        json_t record;
    };

    struct aggregate_commit_entry_t
    {
        std::string authority_key;
        std::vector<std::byte> before;
        std::vector<std::byte> after;
    };

    struct aggregate_lock_t
    {
        aggregate_id_t aggregate_id;
        std::uint64_t aggregate_generation = 0;
        std::string authority_key;
        std::string expected_store_version;
        std::string status;
        std::optional<std::size_t> page_index;
        std::optional<std::size_t> entry_index;
    };

    static constexpr std::size_t aggregate_commit_page_item_limit = 512;
    static constexpr std::size_t aggregate_commit_page_byte_limit = 1024u * 1024u;
    static constexpr std::size_t aggregate_commit_final_key_limit = 2048;

    static json_t encode_aggregate_commit_page (
      std::size_t page_index,
      const std::vector<aggregate_commit_entry_t> &entries)
    {
        json_t encoded_entries = json_t::array ();
        for (const auto &entry : entries)
            encoded_entries.push_back (
              { {"authorityKey", entry.authority_key},
                {"before", hex (entry.before)},
                {"after", hex (entry.after)} });
        return {{"version", 1},
                {"pageIndex", page_index},
                {"entries", std::move (encoded_entries)}};
    }

    static std::optional<std::vector<aggregate_commit_entry_t>>
    decode_aggregate_commit_page (
      const std::vector<std::byte> &bytes,
      std::optional<std::size_t> expected_page_index = std::nullopt)
    {
        if (bytes.empty () || bytes.size () > aggregate_commit_page_byte_limit)
            return std::nullopt;
        try {
            const auto record = parse_json (bytes);
            if (record.value ("version", 0) != 1 || !record.at ("pageIndex").is_number_unsigned ()
                || (expected_page_index && record.at ("pageIndex").get<std::size_t> ()
                    != *expected_page_index)
                || !record.at ("entries").is_array ()
                || record.at ("entries").empty ()
                || record.at ("entries").size () > aggregate_commit_page_item_limit)
                return std::nullopt;
            std::vector<aggregate_commit_entry_t> result;
            result.reserve (record.at ("entries").size ());
            for (const auto &entry : record.at ("entries"))
                result.push_back ({entry.at ("authorityKey").get<std::string> (),
                                   unhex (entry.at ("before").get<std::string> ()),
                                   unhex (entry.at ("after").get<std::string> ())});
            return result;
        }
        catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<std::vector<std::vector<aggregate_commit_entry_t>>>
    split_aggregate_commit_entries (const std::vector<aggregate_commit_entry_t> &entries)
    {
        if (entries.empty ())
            return std::nullopt;
        std::vector<std::vector<aggregate_commit_entry_t>> pages;
        std::vector<aggregate_commit_entry_t> current;
        current.reserve (aggregate_commit_page_item_limit);
        const auto finish = [&] {
            if (current.empty ())
                return true;
            const auto encoded = encode_aggregate_commit_page (pages.size (), current).dump ();
            if (encoded.size () > aggregate_commit_page_byte_limit)
                return false;
            pages.push_back (std::move (current));
            current.clear ();
            current.reserve (aggregate_commit_page_item_limit);
            return true;
        };
        for (const auto &entry : entries) {
            if (current.size () == aggregate_commit_page_item_limit && !finish ())
                return std::nullopt;
            current.push_back (entry);
            if (encode_aggregate_commit_page (pages.size (), current).dump ().size ()
                <= aggregate_commit_page_byte_limit)
                continue;
            current.pop_back ();
            if (!finish ())
                return std::nullopt;
            current.push_back (entry);
            if (encode_aggregate_commit_page (pages.size (), current).dump ().size ()
                > aggregate_commit_page_byte_limit)
                return std::nullopt;
        }
        if (!finish ())
            return std::nullopt;
        return pages;
    }

    static json_t encode_aggregate_lock (const aggregate_id_t &aggregate_id,
                                          std::uint64_t aggregate_generation,
                                          std::string_view authority_key,
                                          std::string_view expected_store_version,
                                          std::string_view status,
                                          std::optional<std::size_t> page_index = std::nullopt,
                                          std::optional<std::size_t> entry_index = std::nullopt)
    {
        return {{"aggregateId", hex (aggregate_id.value)},
                {"aggregateGeneration", aggregate_generation},
                {"authorityKey", authority_key},
                {"expectedStoreVersion", expected_store_version},
                {"status", status},
                {"pageIndex", page_index ? json_t (*page_index) : json_t (nullptr)},
                {"entryIndex", entry_index ? json_t (*entry_index) : json_t (nullptr)}};
    }

    static std::optional<aggregate_lock_t>
    decode_aggregate_lock (const std::vector<std::byte> &bytes)
    {
        try {
            const auto record = parse_json (bytes);
            aggregate_lock_t result;
            result.aggregate_id.value =
              unhex_array<16> (record.at ("aggregateId").get<std::string> ());
            result.aggregate_generation =
              record.at ("aggregateGeneration").get<std::uint64_t> ();
            result.authority_key = record.at ("authorityKey").get<std::string> ();
            result.expected_store_version =
              record.at ("expectedStoreVersion").get<std::string> ();
            result.status = record.at ("status").get<std::string> ();
            if (!record.at ("pageIndex").is_null ())
                result.page_index = record.at ("pageIndex").get<std::size_t> ();
            if (!record.at ("entryIndex").is_null ())
                result.entry_index = record.at ("entryIndex").get<std::size_t> ();
            if (result.aggregate_generation == 0 || result.authority_key.empty ()
                || (result.status != "prepared" && result.status != "committing"
                    && result.status != "committed"))
                return std::nullopt;
            return result;
        }
        catch (...) {
            return std::nullopt;
        }
    }

    store_read_result_t read (const store_key_t &key)
    {
        return _store->read (key).result ().value ();
    }

    std::optional<aggregate_lock_t> read_aggregate_lock (std::string_view authority_key)
    {
        auto value = read (key_aggregate_lock (authority_key));
        const auto *found = std::get_if<store_found_t> (&value);
        if (!found)
            return std::nullopt;
        auto lock = decode_aggregate_lock (found->value.bytes);
        if (!lock || lock->authority_key != authority_key)
            return std::nullopt;
        return lock;
    }

    std::optional<authority_snapshot_t>
    effective_authority (std::string_view authority_key,
                         const std::vector<std::byte> &raw_bytes,
                         const store_version_t &raw_version,
                         std::chrono::system_clock::time_point store_now)
    {
        auto raw = decode_authority (raw_bytes, raw_version, store_now);
        const auto lock = read_aggregate_lock (authority_key);
        if (!lock || lock->status != "committing")
            return raw;
        auto aggregate = read (key_aggregate (lock->aggregate_id));
        if (const auto *aggregate_found = std::get_if<store_found_t> (&aggregate)) {
            const auto aggregate_record = parse_json (aggregate_found->value.bytes);
            if (aggregate_record.value ("status", "") == "committed")
                return raw;
        }
        if (!lock->page_index || !lock->entry_index)
            return raw;
        auto page = read (key_aggregate_commit_page (lock->aggregate_id, *lock->page_index));
        const auto *found = std::get_if<store_found_t> (&page);
        if (!found)
            return std::nullopt;
        const auto entries = decode_aggregate_commit_page (found->value.bytes);
        if (!entries || *lock->entry_index >= entries->size ())
            return std::nullopt;
        const auto &entry = (*entries)[*lock->entry_index];
        if (entry.authority_key != authority_key)
            return std::nullopt;
        return decode_authority (entry.before, raw_version, store_now);
    }

    bool authority_mutation_locked (std::string_view authority_key)
    {
        const auto lock = read_aggregate_lock (authority_key);
        if (!lock || (lock->status != "prepared" && lock->status != "committing"))
            return false;
        if (lock->status == "committing") {
            auto aggregate = read (key_aggregate (lock->aggregate_id));
            if (const auto *found = std::get_if<store_found_t> (&aggregate)) {
                const auto record = parse_json (found->value.bytes);
                if (record.value ("status", "") == "committed"
                    || record.value ("status", "") == "aborted")
                    return false;
            }
        }
        return true;
    }

    store_write_result_t write (store_write_request_t request)
    {
        auto first = _store->write (request).result ();
        if (first)
            return first.value ();
        if (auto applied = reconcile_write (request))
            return store_write_result_t{std::move (*applied)};
        if (first.error () != nullptr
            && detail::is_transient_error (first.error ()->kind ())) {
            auto retried = _store->write (request).result ();
            if (retried)
                return retried.value ();
            if (auto applied = reconcile_write (request))
                return store_write_result_t{
                  std::move (*applied)};
            return retried.value ();
        }
        return first.value ();
    }

    std::optional<store_write_applied_t>
    reconcile_write (
      const store_write_request_t &request)
    {
        if (request.mutations.empty ())
            return std::nullopt;
        store_write_applied_t applied;
        for (const auto &mutation : request.mutations) {
            const auto key = std::visit (
              [] (const auto &value) {
                  return value.key;
              },
              mutation);
            auto observed = _store->read (key).result ();
            if (!observed)
                return std::nullopt;
            const auto &read = observed.value ();
            if (const auto *put =
                  std::get_if<store_put_t> (
                    &mutation)) {
                const auto *found =
                  std::get_if<store_found_t> (&read);
                if (found == nullptr
                    || found->value.bytes != put->bytes
                    || static_cast<bool> (
                         found->value.expires_at)
                         != static_cast<bool> (
                           put->retention))
                    return std::nullopt;
                if (put->retention
                    && *found->value.expires_at
                         <= found->value.store_now)
                    return std::nullopt;
                applied.put_versions.push_back (
                  {key, found->value.version});
                applied.store_now =
                  std::max (
                    applied.store_now,
                    found->value.store_now);
            }
            else {
                const auto *missing =
                  std::get_if<store_missing_t> (&read);
                if (missing == nullptr)
                    return std::nullopt;
                applied.store_now =
                  std::max (
                    applied.store_now,
                    missing->store_now);
            }
        }
        return applied;
    }

    static store_condition_t missing_condition (store_key_t key)
    {
        return store_missing_condition_t{
          std::move (key)};
    }

    static store_condition_t version_condition (
      store_key_t key,
      store_version_t version)
    {
        return store_version_condition_t{
          std::move (key), std::move (version)};
    }

    static store_condition_t version_condition (
      store_key_t key,
      std::string version)
    {
        return version_condition (
          std::move (key),
          store_version_t{std::move (version)});
    }

    static store_condition_t condition_for (const store_key_t &key,
                                            const store_read_result_t &result)
    {
        if (const auto *found = std::get_if<store_found_t> (&result))
            return version_condition (key, found->value.version);
        return missing_condition (key);
    }

    static std::vector<std::byte> to_bytes (std::string_view value)
    {
        const auto *first = reinterpret_cast<const std::byte *> (value.data ());
        return {first, first + value.size ()};
    }

    static std::string to_string (const std::vector<std::byte> &value)
    {
        return {reinterpret_cast<const char *> (value.data ()), value.size ()};
    }

    static json_t parse_json (const std::vector<std::byte> &value)
    {
        return json_t::parse (to_string (value));
    }

    static std::int64_t parse_i64 (const std::vector<std::byte> &value)
    {
        return std::stoll (to_string (value));
    }

    static std::int64_t owner_generation (const std::vector<std::byte> &value)
    {
        return parse_json (value).at ("leaseGeneration").get<std::int64_t> ();
    }

    static std::string segment (std::string_view value)
    {
        return std::to_string (value.size ()) + ":" + std::string (value) + ":";
    }

    static store_key_t key_owner (std::string_view owner_id)
    {
        return {std::string (prefix) + "owner:" + segment (owner_id)};
    }

    static store_key_t key_authority (std::string_view value)
    {
        return {std::string (authority_prefix) + segment (value)};
    }

    static store_key_t key_reservation (const object_creation_key_t &key)
    {
        return {std::string (prefix) + "creation-reservation:" + segment (object_key (key))};
    }

    static store_key_t key_creation_terminal (const creation_operation_identity_t &operation)
    {
        return {std::string (prefix)
                + "creation-terminal:" + segment (operation.source_node_rid.value ())
                + std::to_string (operation.source_node_generation) + ":"
                + std::to_string (operation.operation_id.high) + ":"
                + std::to_string (operation.operation_id.low)};
    }

    static store_key_t key_relocation_capacity (std::string_view fence)
    {
        return {std::string (prefix) + "relocation-capacity:" + segment (fence)};
    }

    static store_key_t key_relocation_capacity_id (const std::array<std::byte, 16> &id)
    {
        return {std::string (prefix) + "relocation-capacity-id:" + hex (id)};
    }

    static store_key_t key_aggregate (const aggregate_id_t &id)
    {
        return {std::string (prefix) + "aggregate:" + hex (id.value)};
    }

    static store_key_t key_aggregate_inventory (const aggregate_id_t &id,
                                                std::size_t page_index)
    {
        return {std::string (prefix) + "aggregate-inventory:" + hex (id.value) + ":"
                + std::to_string (page_index)};
    }

    static store_key_t key_aggregate_inventory_index (
      const aggregate_id_t &id,
      std::size_t level,
      std::size_t page_index)
    {
        return {std::string (prefix) + "aggregate-inventory-index:"
                + hex (id.value) + ":" + std::to_string (level) + ":"
                + std::to_string (page_index)};
    }

    static store_key_t key_aggregate_commit_page (const aggregate_id_t &id,
                                                  std::size_t page_index)
    {
        return {std::string (prefix) + "aggregate-commit:" + hex (id.value) + ":"
                + std::to_string (page_index)};
    }

    // The lookup key is deliberately independent of the aggregate id. It lets
    // every authority mutation reject a prepared aggregate without scanning
    // all aggregate records. The logical authority key remains in the value
    // so a digest collision cannot silently authorize a different authority.
    static store_key_t key_aggregate_lock (std::string_view authority_key)
    {
        const auto digest = sha256 (to_bytes (authority_key));
        return {std::string (prefix) + "aggregate-lock:" + hex (digest)};
    }

    static std::string object_key (const object_creation_key_t &key)
    {
        return std::to_string (static_cast<int> (key.kind)) + ":" + key.global_id;
    }

    template <std::size_t N> static std::string hex (const std::array<std::byte, N> &value)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve (N * 2);
        for (const auto item : value) {
            const auto byte = std::to_integer<unsigned char> (item);
            result.push_back (digits[byte >> 4]);
            result.push_back (digits[byte & 0x0f]);
        }
        return result;
    }

    static std::string hex (const std::vector<std::byte> &value)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve (value.size () * 2);
        for (const auto item : value) {
            const auto byte = std::to_integer<unsigned char> (item);
            result.push_back (digits[byte >> 4]);
            result.push_back (digits[byte & 0x0f]);
        }
        return result;
    }

    static std::vector<std::byte> unhex (std::string_view value)
    {
        if (value.size () % 2 != 0)
            throw std::invalid_argument ("hex payload has an odd length");
        const auto digit = [] (char item) -> unsigned {
            if (item >= '0' && item <= '9')
                return static_cast<unsigned> (item - '0');
            if (item >= 'a' && item <= 'f')
                return static_cast<unsigned> (item - 'a' + 10);
            if (item >= 'A' && item <= 'F')
                return static_cast<unsigned> (item - 'A' + 10);
            throw std::invalid_argument ("hex payload contains an invalid digit");
        };
        std::vector<std::byte> result;
        result.reserve (value.size () / 2);
        for (std::size_t index = 0; index < value.size (); index += 2)
            result.push_back (
              static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
        return result;
    }

    template <std::size_t N> static std::array<std::byte, N> unhex_array (std::string_view value)
    {
        const auto decoded = unhex (value);
        if (decoded.size () != N)
            throw std::invalid_argument ("hex value has an invalid length");
        std::array<std::byte, N> result{};
        std::copy (decoded.begin (), decoded.end (), result.begin ());
        return result;
    }

    static std::string prefix_mesh (std::string_view mesh_name)
    {
        return std::string (prefix) + "mesh:" + segment (mesh_name);
    }

    static store_key_t key_mesh (std::string_view mesh_name, const zlink::routing_id_t &rid)
    {
        return {prefix_mesh (mesh_name) + segment (rid.to_hex ())};
    }

    static std::string prefix_client_server (std::string_view channel_name)
    {
        return std::string (prefix) + "client-server:" + segment (channel_name);
    }

    static store_key_t key_client_server (std::string_view channel_name,
                                          const zlink::routing_id_t &rid)
    {
        return {prefix_client_server (channel_name) + segment (rid.to_hex ())};
    }

    static std::string prefix_fanout (std::string_view channel_name)
    {
        return std::string (prefix) + "fanout:" + segment (channel_name);
    }

    static store_key_t key_fanout (std::string_view channel_name, const zlink::routing_id_t &rid)
    {
        return {prefix_fanout (channel_name) + segment (rid.to_hex ())};
    }

    template <typename TImmutable>
    task_t<location_write_result_t> update_descriptor (const store_key_t &row_key,
                                                       const std::string &owner_id,
                                                       std::int64_t lease_generation,
                                                       std::uint64_t lifecycle_generation,
                                                       std::uint64_t descriptor_revision,
                                                       json_t record,
                                                       location_write_intent_t intent,
                                                       TImmutable immutable_fields_equal)
    {
        const auto lease_key = key_owner (owner_id);
        auto lease = read (lease_key);
        const auto *live_lease = std::get_if<store_found_t> (&lease);
        if (!live_lease || owner_generation (live_lease->value.bytes) != lease_generation)
            return completed (
              location_write_result_t{location_write_status_t::ignored_stale, 0, {}});

        auto current = read (row_key);
        std::uint64_t generation = 1;
        store_condition_t row_condition;
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto stored = parse_json (found->value.bytes);
            generation = stored.at ("generation").get<std::uint64_t> ();
            const auto stored_owner = record_owner_id (stored);
            const auto stored_lease = record_lease_generation (stored);
            const auto previous_owner = read (key_owner (stored_owner));
            const auto previous_owner_live = std::holds_alternative<store_found_t> (previous_owner);
            if (intent == location_write_intent_t::new_claim && previous_owner_live)
                return completed (
                  location_write_result_t{location_write_status_t::rejected_conflict, 0, {}});
            if (intent == location_write_intent_t::takeover && previous_owner_live)
                return completed (
                  location_write_result_t{location_write_status_t::ignored_stale, 0, {}});
            if (intent == location_write_intent_t::renew) {
                if (stored_owner != owner_id || stored_lease != lease_generation
                    || record_lifecycle_generation (stored) != lifecycle_generation
                    || descriptor_revision <= record_descriptor_revision (stored)
                    || !immutable_fields_equal (stored))
                    return completed (
                      location_write_result_t{location_write_status_t::ignored_stale, 0, {}});
            } else {
                if (generation == std::numeric_limits<std::uint64_t>::max ())
                    return unavailable<location_write_result_t> ("descriptor generation exhausted");
                ++generation;
            }
            row_condition = version_condition (row_key, found->value.version);
        } else {
            if (intent == location_write_intent_t::renew)
                return completed (
                  location_write_result_t{location_write_status_t::ignored_stale, 0, {}});
            row_condition = missing_condition (row_key);
        }
        record["generation"] = generation;
        auto encoded = to_bytes (record.dump ());
        auto result = write (
          {{version_condition (lease_key, live_lease->value.version), std::move (row_condition)},
           {store_put_t{row_key, encoded, std::nullopt}}});
        if (const auto *applied = std::get_if<store_write_applied_t> (&result))
            return completed (location_write_result_t::stored (
              static_cast<std::int64_t> (generation), applied->store_now));
        return completed (location_write_result_t{location_write_status_t::ignored_stale, 0, {}});
    }

    task_t<location_write_status_t> remove_descriptor (const store_key_t &row_key,
                                                       location_owner_token_t owner)
    {
        auto current = read (row_key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            return completed (location_write_status_t::ignored_stale);
        const auto record = parse_json (found->value.bytes);
        if (record_owner_id (record) != owner.owner_id
            || record_lease_generation (record) != owner.lease_generation)
            return completed (location_write_status_t::ignored_stale);
        auto result =
          write ({{version_condition (row_key, found->value.version)}, {store_delete_t{row_key}}});
        return completed (std::holds_alternative<store_write_applied_t> (result)
                            ? location_write_status_t::stored
                            : location_write_status_t::ignored_stale);
    }

    template <typename T, typename TDecode>
    task_t<location_page_t<T>>
    list_descriptors (std::string row_prefix, location_page_request_t page, TDecode decode)
    {
        store_scan_request_t request{
          std::move (row_prefix),
          page.continuation_token
            ? std::optional<store_scan_cursor_t>{store_scan_cursor_t{*page.continuation_token}}
            : std::nullopt,
          page.page_size > 0 ? static_cast<std::uint32_t> (page.page_size) : 256u};
        auto result = _store->scan (std::move (request)).result ().value ();
        const auto *found = std::get_if<store_scan_page_t> (&result);
        if (!found)
            return unavailable<location_page_t<T>> ("Location Store scan cursor expired");
        location_page_t<T> output;
        output.items.reserve (found->items.size ());
        for (const auto &item : found->items)
            output.items.push_back (decode (parse_json (item.value.bytes)));
        if (found->next_cursor)
            output.continuation_token = found->next_cursor->value;
        return completed (std::move (output));
    }

    std::int64_t remove_owned (const std::string &row_prefix, const location_owner_token_t &owner)
    {
        std::int64_t removed = 0;
        std::optional<store_scan_cursor_t> cursor;
        do {
            auto result = _store->scan ({row_prefix, cursor, 1000}).result ().value ();
            const auto *page = std::get_if<store_scan_page_t> (&result);
            if (!page)
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Location Store scan cursor expired");
            for (const auto &item : page->items) {
                const auto record = parse_json (item.value.bytes);
                if (record_owner_id (record) != owner.owner_id
                    || record_lease_generation (record) != owner.lease_generation)
                    continue;
                auto written = write (
                  {{version_condition (item.key, item.value.version)}, {store_delete_t{item.key}}});
                if (std::holds_alternative<store_write_applied_t> (written))
                    ++removed;
            }
            cursor = page->next_cursor;
        } while (cursor);
        return removed;
    }

    static std::string record_owner_id (const json_t &record)
    {
        if (record.contains ("ownerId"))
            return record.at ("ownerId").get<std::string> ();
        return record.at ("descriptor").at ("ownerId").get<std::string> ();
    }

    static std::int64_t record_lease_generation (const json_t &record)
    {
        if (record.contains ("leaseGeneration"))
            return record.at ("leaseGeneration").get<std::int64_t> ();
        return record.at ("descriptor").at ("leaseGeneration").get<std::int64_t> ();
    }

    static std::uint64_t record_lifecycle_generation (const json_t &record)
    {
        if (record.contains ("lifecycleGeneration"))
            return record.at ("lifecycleGeneration").get<std::uint64_t> ();
        return record.at ("descriptor").at ("lifecycleGeneration").get<std::uint64_t> ();
    }

    static std::uint64_t record_descriptor_revision (const json_t &record)
    {
        if (record.contains ("descriptorRevision"))
            return record.at ("descriptorRevision").get<std::uint64_t> ();
        return record.at ("descriptor").at ("descriptorRevision").get<std::uint64_t> ();
    }

    static std::int64_t unix_ms (std::chrono::system_clock::time_point value)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (value.time_since_epoch ())
          .count ();
    }

    static std::chrono::system_clock::time_point from_unix_ms (std::int64_t value)
    {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
    }

    static json_t encode_owner (const location_owner_token_t &value)
    {
        return {{"ownerId", value.owner_id}, {"leaseGeneration", value.lease_generation}};
    }

    static location_owner_token_t decode_owner (const json_t &value)
    {
        return {value.at ("ownerId").get<std::string> (),
                value.at ("leaseGeneration").get<std::int64_t> ()};
    }

    static bool same_owner (const location_owner_token_t &left, const location_owner_token_t &right)
    {
        return left.owner_id == right.owner_id && left.lease_generation == right.lease_generation;
    }

    static bool same_target (const object_creation_target_t &left,
                             const object_creation_target_t &right)
    {
        return left.mesh_name == right.mesh_name
               && left.node_rid.value () == right.node_rid.value ()
               && left.node_lifecycle_generation == right.node_lifecycle_generation
               && same_owner (left.owner, right.owner);
    }

    bool owner_is_live (const location_owner_token_t &owner)
    {
        return read_live_owner (owner).has_value ();
    }

    std::optional<store_found_t> read_live_owner (const location_owner_token_t &owner)
    {
        auto current = read (key_owner (owner.owner_id));
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != owner.lease_generation)
            return std::nullopt;
        return *found;
    }

    static json_t encode_target (const object_creation_target_t &value)
    {
        return {{"meshName", value.mesh_name},
                {"nodeRid", value.node_rid.value ()},
                {"nodeLifecycleGeneration", value.node_lifecycle_generation},
                {"owner", encode_owner (value.owner)}};
    }

    static object_creation_target_t decode_target (const json_t &value)
    {
        return {value.at ("meshName").get<std::string> (),
                node_rid_t::from_string (value.at ("nodeRid").get<std::string> ()),
                value.at ("nodeLifecycleGeneration").get<std::uint64_t> (),
                decode_owner (value.at ("owner"))};
    }

    static json_t encode_bundle (const placement_capacity_bundle_t &value)
    {
        json_t spot_type = nullptr;
        if (value.spot_type)
            spot_type = {{"objectKind", static_cast<int> (value.spot_type->object_kind)},
                         {"stableType", value.spot_type->stable_type},
                         {"slots", value.spot_type->slots}};
        return {{"actorSlots", value.actor_slots},
                {"spotSlots", value.spot_slots},
                {"spotType", std::move (spot_type)}};
    }

    static placement_capacity_bundle_t decode_bundle (const json_t &value)
    {
        placement_capacity_bundle_t result;
        result.actor_slots = value.at ("actorSlots").get<std::uint32_t> ();
        result.spot_slots = value.at ("spotSlots").get<std::uint32_t> ();
        if (value.contains ("spotType") && !value.at ("spotType").is_null ()) {
            const auto &spot = value.at ("spotType");
            result.spot_type = spot_type_capacity_delta_t{
              static_cast<placement_object_kind_t> (spot.at ("objectKind").get<int> ()),
              spot.at ("stableType").get<std::string> (), spot.at ("slots").get<std::uint32_t> ()};
        }
        return result;
    }

    static json_t encode_allocation (const placement_allocation_t &value)
    {
        return {{"state", static_cast<int> (value.state)},
                {"objectKind", static_cast<int> (value.object_kind)},
                {"stableType", value.stable_type},
                {"target", encode_target (value.target)},
                {"capacityBundle", encode_bundle (value.capacity_bundle)}};
    }

    static placement_allocation_t decode_allocation (const json_t &value)
    {
        return {static_cast<placement_allocation_state_t> (value.at ("state").get<int> ()),
                static_cast<placement_object_kind_t> (value.at ("objectKind").get<int> ()),
                value.at ("stableType").get<std::string> (), decode_target (value.at ("target")),
                decode_bundle (value.at ("capacityBundle"))};
    }

    static json_t encode_pending (const std::optional<pending_object_creation_t> &value)
    {
        if (!value)
            return nullptr;
        return {{"reservationId", value->reservation_id},
                {"requestContentReference", value->request_content_reference},
                {"requestSha256", hex (value->request_sha256)},
                {"requestEncodedSize", value->request_encoded_size}};
    }

    static std::optional<pending_object_creation_t> decode_pending (const json_t &value)
    {
        if (value.is_null ())
            return std::nullopt;
        return pending_object_creation_t{
          value.at ("reservationId").get<std::string> (),
          value.at ("requestContentReference").get<std::string> (),
          unhex_array<32> (value.at ("requestSha256").get<std::string> ()),
          value.at ("requestEncodedSize").get<std::uint32_t> ()};
    }

    static std::vector<std::byte> encode_authority (const authority_snapshot_t &value)
    {
        return to_bytes (json_t{{"storeVersion", value.store_version},
                                {"payload", hex (value.payload)},
                                {"objectGeneration", value.object_generation},
                                {"authorityOwnerGeneration", value.authority_owner_generation},
                                {"owner", encode_owner (value.owner)},
                                {"allocation", encode_allocation (value.allocation)},
                                {"pendingCreation", encode_pending (value.pending_creation)}}
                           .dump ());
    }

    static authority_snapshot_t decode_authority (const std::vector<std::byte> &bytes,
                                                  std::string provider_version,
                                                  std::chrono::system_clock::time_point store_now)
    {
        (void) provider_version;
        const auto value = parse_json (bytes);
        return {value.at ("storeVersion").get<std::string> (),
                unhex (value.at ("payload").get<std::string> ()),
                value.at ("objectGeneration").get<std::uint64_t> (),
                value.at ("authorityOwnerGeneration").get<std::uint64_t> (),
                decode_owner (value.at ("owner")),
                store_now,
                decode_allocation (value.at ("allocation")),
                decode_pending (value.at ("pendingCreation"))};
    }

    static authority_snapshot_t decode_authority (
      const std::vector<std::byte> &bytes,
      const store_version_t &provider_version,
      std::chrono::system_clock::time_point store_now)
    {
        return decode_authority (
          bytes, provider_version.value, store_now);
    }

    static json_t encode_creation_key (const object_creation_key_t &value)
    {
        return {{"kind", static_cast<int> (value.kind)}, {"globalId", value.global_id}};
    }

    static object_creation_key_t decode_creation_key (const json_t &value)
    {
        return {static_cast<placement_object_kind_t> (value.at ("kind").get<int> ()),
                value.at ("globalId").get<std::string> ()};
    }

    static json_t encode_operation (const creation_operation_identity_t &value)
    {
        return {{"sourceNodeRid", value.source_node_rid.value ()},
                {"sourceNodeGeneration", value.source_node_generation},
                {"operationHigh", value.operation_id.high},
                {"operationLow", value.operation_id.low}};
    }

    static creation_operation_identity_t decode_operation (const json_t &value)
    {
        return {node_rid_t::from_string (value.at ("sourceNodeRid").get<std::string> ()),
                value.at ("sourceNodeGeneration").get<std::uint64_t> (),
                {value.at ("operationHigh").get<std::uint64_t> (),
                 value.at ("operationLow").get<std::uint64_t> ()}};
    }

    static json_t encode_fence (const object_reservation_fence_t &value)
    {
        return {{"reservationId", value.reservation_id},
                {"expectedStoreVersion", value.expected_store_version},
                {"objectGeneration", value.object_generation},
                {"authorityOwnerGeneration", value.authority_owner_generation},
                {"target", encode_target (value.target)},
                {"capacityBundle", encode_bundle (value.capacity_bundle)}};
    }

    static object_reservation_fence_t decode_fence (const json_t &value)
    {
        return {value.at ("reservationId").get<std::string> (),
                value.at ("expectedStoreVersion").get<std::string> (),
                value.at ("objectGeneration").get<std::uint64_t> (),
                value.at ("authorityOwnerGeneration").get<std::uint64_t> (),
                decode_target (value.at ("target")),
                decode_bundle (value.at ("capacityBundle"))};
    }

    static bool same_fence (const object_reservation_fence_t &left,
                            const object_reservation_fence_t &right)
    {
        return left.reservation_id == right.reservation_id
               && left.expected_store_version == right.expected_store_version
               && left.object_generation == right.object_generation
               && left.authority_owner_generation == right.authority_owner_generation
               && left.target.mesh_name == right.target.mesh_name
               && left.target.node_rid.value () == right.target.node_rid.value ()
               && left.target.node_lifecycle_generation == right.target.node_lifecycle_generation
               && same_owner (left.target.owner, right.target.owner)
               && encode_bundle (left.capacity_bundle) == encode_bundle (right.capacity_bundle);
    }

    static json_t encode_terminal (const creation_terminal_record_t &value)
    {
        return {{"operation", encode_operation (value.operation)},
                {"object", encode_creation_key (value.object)},
                {"reservation", encode_fence (value.reservation)},
                {"state", static_cast<int> (value.state)},
                {"terminalEnvelope", hex (value.terminal_envelope)},
                {"sha256", hex (value.sha256)},
                {"expiresAt", unix_ms (value.expires_at)}};
    }

    static creation_terminal_record_t decode_terminal (const json_t &value)
    {
        return {decode_operation (value.at ("operation")),
                decode_creation_key (value.at ("object")),
                decode_fence (value.at ("reservation")),
                static_cast<creation_terminal_state_t> (value.at ("state").get<int> ()),
                unhex (value.at ("terminalEnvelope").get<std::string> ()),
                unhex_array<32> (value.at ("sha256").get<std::string> ()),
                from_unix_ms (value.at ("expiresAt").get<std::int64_t> ())};
    }

    task_t<authority_compare_exchange_result_t> authority_conflict (store_read_result_t current)
    {
        if (const auto *found = std::get_if<store_found_t> (&current))
            return completed (authority_compare_exchange_result_t{authority_conflict_t{
              decode_authority (found->value.bytes, found->value.version, found->value.store_now)}});
        return completed (authority_compare_exchange_result_t{authority_conflict_t{
          authority_missing_t{std::get<store_missing_t> (current).store_now}}});
    }

    task_t<authority_compare_exchange_result_t> authority_write_result (
      const store_key_t &row_key, authority_snapshot_t snapshot, store_write_result_t written)
    {
        if (const auto *applied = std::get_if<store_write_applied_t> (&written)) {
            (void) row_key;
            snapshot.store_now = applied->store_now;
            return completed (
              authority_compare_exchange_result_t{authority_stored_t{std::move (snapshot)}});
        }
        return authority_conflict (read (row_key));
    }

    static bool advance_store_version (authority_snapshot_t &snapshot)
    {
        std::uint64_t current = 0;
        try {
            current = static_cast<std::uint64_t> (std::stoull (snapshot.store_version));
        }
        catch (...) {
            return false;
        }
        if (current >= max_generation)
            return false;
        snapshot.store_version = std::to_string (current + 1);
        return true;
    }

    static json_t encode (const capacity_usage_t &value)
    {
        return {{"active", value.active}, {"reserved", value.reserved}, {"limit", value.limit}};
    }

    static capacity_usage_t decode_capacity_usage (const json_t &value)
    {
        return {value.at ("active").get<std::uint64_t> (),
                value.at ("reserved").get<std::uint64_t> (),
                value.at ("limit").get<std::int32_t> ()};
    }

    static json_t encode (const mesh_node_descriptor_t &value)
    {
        json_t capabilities = json_t::array ();
        for (const auto &item : value.object_capabilities)
            capabilities.push_back ({{"objectKind", static_cast<int> (item.object_kind)},
                                     {"stableType", item.stable_type},
                                     {"policy", static_cast<int> (item.policy)},
                                     {"hasSnapshotAdapter", item.has_snapshot_adapter},
                                     {"spotLimit", item.spot_limit}});
        json_t spot_types = json_t::array ();
        for (const auto &item : value.capacity.spot_types)
            spot_types.push_back ({{"objectKind", static_cast<int> (item.object_kind)},
                                   {"stableType", item.stable_type},
                                   {"usage", encode (item.usage)}});
        return {
          {"meshName", value.mesh_name},
          {"rid", value.rid.to_hex ()},
          {"lifecycleGeneration", value.lifecycle_generation},
          {"descriptorRevision", value.descriptor_revision},
          {"endpoint", value.endpoint},
          {"entrySpotId", value.entry_spot_id ? json_t (*value.entry_spot_id) : json_t (nullptr)},
          {"channelWeights", value.channel_weights},
          {"applicationVersion", value.application_version},
          {"objectCapabilities", std::move (capabilities)},
          {"objectRole", static_cast<int> (value.object_role)},
          {"placementWeight", value.placement_weight},
          {"capacity",
           {{"actors", encode (value.capacity.actors)},
            {"spots", encode (value.capacity.spots)},
            {"spotTypes", std::move (spot_types)}}},
          {"activationConcurrency",
           {{"active", value.activation_concurrency.active},
            {"limit", value.activation_concurrency.limit}}},
          {"maintenanceWave",
           value.maintenance_wave ? json_t (*value.maintenance_wave) : json_t (nullptr)},
          {"state", static_cast<int> (value.state)},
          {"securityIdentity", value.security_identity},
          {"ownerId", value.owner_id},
          {"leaseGeneration", value.lease_generation},
          {"updatedAt", unix_ms (value.updated_at)}};
    }

    static bool same_mesh_immutable (const mesh_node_descriptor_t &left,
                                     const mesh_node_descriptor_t &right)
    {
        if (left.mesh_name != right.mesh_name || left.rid.to_hex () != right.rid.to_hex ()
            || left.lifecycle_generation != right.lifecycle_generation
            || left.endpoint != right.endpoint || left.entry_spot_id != right.entry_spot_id
            || left.security_identity != right.security_identity
            || left.application_version != right.application_version
            || left.object_role != right.object_role
            || left.capacity.actors.limit != right.capacity.actors.limit
            || left.capacity.spots.limit != right.capacity.spots.limit
            || left.activation_concurrency.limit != right.activation_concurrency.limit
            || left.object_capabilities.size () != right.object_capabilities.size ()
            || left.capacity.spot_types.size () != right.capacity.spot_types.size ())
            return false;
        const auto capabilities_equal = [] (const object_capability_t &lhs,
                                            const object_capability_t &rhs) {
            return lhs.object_kind == rhs.object_kind && lhs.stable_type == rhs.stable_type
                   && lhs.policy == rhs.policy
                   && lhs.has_snapshot_adapter == rhs.has_snapshot_adapter
                   && lhs.spot_limit == rhs.spot_limit;
        };
        return std::equal (left.object_capabilities.begin (), left.object_capabilities.end (),
                           right.object_capabilities.begin (), capabilities_equal)
               && std::equal (
                 left.capacity.spot_types.begin (), left.capacity.spot_types.end (),
                 right.capacity.spot_types.begin (),
                 [] (const spot_type_capacity_t &lhs, const spot_type_capacity_t &rhs) {
                     return lhs.object_kind == rhs.object_kind && lhs.stable_type == rhs.stable_type
                            && lhs.usage.limit == rhs.usage.limit;
                 });
    }

    std::optional<stored_target_t> read_target_descriptor (const object_creation_target_t &target,
                                                           bool require_live = true)
    {
        const auto row_key = key_mesh (
          target.mesh_name, zlink::routing_id_t::from (std::string (target.node_rid.value ())));
        auto row = read (row_key);
        const auto *found = std::get_if<store_found_t> (&row);
        if (!found)
            return std::nullopt;
        const auto record = parse_json (found->value.bytes);
        auto descriptor = decode_mesh_descriptor (record.at ("descriptor"));
        if (descriptor.lifecycle_generation != target.node_lifecycle_generation
            || descriptor.owner_id != target.owner.owner_id
            || descriptor.lease_generation != target.owner.lease_generation
            || (require_live && descriptor.state != framework_runtime_state_t::serving))
            return std::nullopt;
        auto owner = read (key_owner (target.owner.owner_id));
        const auto *live = std::get_if<store_found_t> (&owner);
        if (require_live
            && (!live || owner_generation (live->value.bytes) != target.owner.lease_generation))
            return std::nullopt;
        return stored_target_t{row_key, found->value.version.value,
                               live ? live->value.version.value : std::string{},
                               std::move (descriptor),
                               record};
    }

    static bool target_accepts (const mesh_node_descriptor_t &descriptor,
                                placement_object_kind_t kind,
                                const std::string &stable_type)
    {
        return std::any_of (
          descriptor.object_capabilities.begin (), descriptor.object_capabilities.end (),
          [&] (const object_capability_t &capability) {
              return capability.object_kind == kind && capability.stable_type == stable_type;
          });
    }

    static bool bundle_matches (const placement_capacity_bundle_t &bundle,
                                placement_object_kind_t kind,
                                const std::string &stable_type)
    {
        if (kind == placement_object_kind_t::actor)
            return bundle.actor_slots == 1 && bundle.spot_slots == 0 && !bundle.spot_type;
        return bundle.actor_slots == 0 && bundle.spot_slots == 1 && bundle.spot_type
               && bundle.spot_type->object_kind == kind
               && bundle.spot_type->stable_type == stable_type && bundle.spot_type->slots == 1;
    }

    static bool capacity_available (const mesh_node_descriptor_t &descriptor,
                                    const placement_capacity_bundle_t &bundle)
    {
        const auto enough = [] (const capacity_usage_t &usage, std::uint32_t requested) {
            return requested == 0 || usage.limit == 0
                   || usage.active + usage.reserved + requested
                        <= static_cast<std::uint64_t> (usage.limit);
        };
        if (!enough (descriptor.capacity.actors, bundle.actor_slots)
            || !enough (descriptor.capacity.spots, bundle.spot_slots))
            return false;
        if (!bundle.spot_type)
            return true;
        const auto found = std::find_if (
          descriptor.capacity.spot_types.begin (), descriptor.capacity.spot_types.end (),
          [&] (const spot_type_capacity_t &item) {
              return item.object_kind == bundle.spot_type->object_kind
                     && item.stable_type == bundle.spot_type->stable_type;
          });
        return found != descriptor.capacity.spot_types.end ()
               && enough (found->usage, bundle.spot_type->slots);
    }

    static bool adjust_capacity (mesh_node_descriptor_t &descriptor,
                                 const placement_capacity_bundle_t &bundle,
                                 std::int64_t reserved_delta,
                                 std::int64_t active_delta)
    {
        const auto adjust = [] (capacity_usage_t &usage, std::uint32_t slots,
                                std::int64_t reserved_change, std::int64_t active_change) {
            const auto reserved = static_cast<std::int64_t> (usage.reserved)
                                  + reserved_change * static_cast<std::int64_t> (slots);
            const auto active = static_cast<std::int64_t> (usage.active)
                                + active_change * static_cast<std::int64_t> (slots);
            if (reserved < 0 || active < 0)
                return false;
            if (usage.limit > 0
                && static_cast<std::uint64_t> (reserved + active)
                     > static_cast<std::uint64_t> (usage.limit))
                return false;
            usage.reserved = static_cast<std::uint64_t> (reserved);
            usage.active = static_cast<std::uint64_t> (active);
            return true;
        };
        if (!adjust (descriptor.capacity.actors, bundle.actor_slots, reserved_delta, active_delta)
            || !adjust (descriptor.capacity.spots, bundle.spot_slots, reserved_delta, active_delta))
            return false;
        if (!bundle.spot_type)
            return true;
        const auto found = std::find_if (
          descriptor.capacity.spot_types.begin (), descriptor.capacity.spot_types.end (),
          [&] (const spot_type_capacity_t &item) {
              return item.object_kind == bundle.spot_type->object_kind
                     && item.stable_type == bundle.spot_type->stable_type;
          });
        return found != descriptor.capacity.spot_types.end ()
               && adjust (found->usage, bundle.spot_type->slots, reserved_delta, active_delta);
    }

    static std::vector<std::byte> encode_target_record (stored_target_t target)
    {
        target.record["descriptor"] = encode (target.descriptor);
        return to_bytes (target.record.dump ());
    }

    authority_read_result_t read_authority_value (std::string_view key)
    {
        auto current = read (key_authority (key));
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto snapshot = effective_authority (key, found->value.bytes, found->value.version,
                                                 found->value.store_now);
            if (snapshot)
                return std::move (*snapshot);
            return authority_missing_t{found->value.store_now};
        }
        return authority_missing_t{std::get<store_missing_t> (current).store_now};
    }

    static json_t encode_reservation (const object_reserve_request_t &request,
                                      const object_reservation_fence_t &fence,
                                      const authority_snapshot_t &snapshot,
                                      std::string_view status)
    {
        return {{"status", status},
                {"object", encode_creation_key (request.key)},
                {"stableType", request.intent.stable_type},
                {"fence", encode_fence (fence)},
                {"snapshot", parse_json (encode_authority (snapshot))}};
    }

    static json_t encode_relocation_capacity (const relocation_capacity_reserve_request_t &request,
                                              std::string_view status)
    {
        return {{"status", status},
                {"reservationId", hex (request.reservation_id)},
                {"authorityKey", request.key.value},
                {"expectedStoreVersion", request.expected_store_version},
                {"objectKind", static_cast<int> (request.object_kind)},
                {"stableType", request.stable_type},
                {"source", encode_target (request.source)},
                {"sourceOwner", encode_owner (request.source.owner)},
                {"target", encode_target (request.target)},
                {"targetOwner", encode_owner (request.target.owner)},
                {"capacityBundle", encode_bundle (request.capacity_bundle)}};
    }

    static bool relocation_request_equal (const json_t &stored,
                                          const relocation_capacity_reserve_request_t &request)
    {
        auto expected = encode_relocation_capacity (request, stored.value ("status", "reserved"));
        return stored == expected;
    }

    static json_t encode_aggregate (const aggregate_prepare_request_t &request,
                                    std::string_view status,
                                    const aggregate_inventory::tree_t &inventory)
    {
        return {{"status", status},
                {"aggregateId", hex (request.aggregate_id.value)},
                {"aggregateGeneration", request.aggregate_generation},
                {"inventoryRoot", hex (inventory.root)},
                {"inventoryCount", inventory.participant_count},
                {"inventoryPageCount", inventory.pages.size ()},
                {"inventoryIndexPageCount", inventory.index_pages.size ()},
                {"inventoryIndexLevelCount", inventory.index_level_count},
                {"inventoryDigest", hex (request.inventory_digest.value)},
                {"targetMeshName", request.target_descriptor.mesh_name},
                {"targetNodeRid", request.target_descriptor.rid.to_string ()},
                {"targetLifecycleGeneration", request.target_descriptor_lifecycle_generation},
                {"capacityBundle", encode_bundle (request.capacity_bundle)},
                {"targetOwner", encode_owner (request.target_owner)}};
    }

    static bool aggregate_record_matches_request (
      const json_t &record,
      const aggregate_prepare_request_t &request,
      const aggregate_inventory::tree_t &inventory)
    {
        try {
            const auto expected = encode_aggregate (request, record.value ("status", ""), inventory);
            for (const auto *field : {"aggregateId", "aggregateGeneration", "inventoryRoot",
                                      "inventoryCount", "inventoryPageCount",
                                      "inventoryIndexPageCount", "inventoryIndexLevelCount",
                                      "inventoryDigest", "targetMeshName", "targetNodeRid",
                                      "targetLifecycleGeneration", "capacityBundle",
                                      "targetOwner"}) {
                if (!record.contains (field) || record.at (field) != expected.at (field))
                    return false;
            }
            return true;
        }
        catch (...) {
            return false;
        }
    }

    static mesh_node_descriptor_t decode_mesh_descriptor (const json_t &value)
    {
        mesh_node_descriptor_t result;
        result.mesh_name = value.at ("meshName").get<std::string> ();
        result.rid = zlink::routing_id_t::from_hex (value.at ("rid").get<std::string> ());
        result.lifecycle_generation = value.at ("lifecycleGeneration").get<std::uint64_t> ();
        result.descriptor_revision = value.at ("descriptorRevision").get<std::uint64_t> ();
        result.endpoint = value.at ("endpoint").get<std::string> ();
        if (value.contains ("entrySpotId") && !value.at ("entrySpotId").is_null ())
            result.entry_spot_id = value.at ("entrySpotId").get<std::string> ();
        result.channel_weights = value.at ("channelWeights").get<std::map<std::string, int>> ();
        result.application_version = value.at ("applicationVersion").get<std::int64_t> ();
        for (const auto &item : value.at ("objectCapabilities"))
            result.object_capabilities.push_back (
              {static_cast<placement_object_kind_t> (item.at ("objectKind").get<int> ()),
               item.at ("stableType").get<std::string> (),
               static_cast<maintenance_policy_kind_t> (item.at ("policy").get<int> ()),
               item.at ("hasSnapshotAdapter").get<bool> (),
               item.at ("spotLimit").get<std::int32_t> ()});
        result.object_role = static_cast<object_role_t> (value.at ("objectRole").get<int> ());
        result.placement_weight = value.at ("placementWeight").get<int> ();
        const auto &capacity = value.at ("capacity");
        result.capacity.actors = decode_capacity_usage (capacity.at ("actors"));
        result.capacity.spots = decode_capacity_usage (capacity.at ("spots"));
        for (const auto &item : capacity.at ("spotTypes"))
            result.capacity.spot_types.push_back (
              {static_cast<placement_object_kind_t> (item.at ("objectKind").get<int> ()),
               item.at ("stableType").get<std::string> (),
               decode_capacity_usage (item.at ("usage"))});
        const auto &activation = value.at ("activationConcurrency");
        result.activation_concurrency.active = activation.at ("active").get<std::uint32_t> ();
        result.activation_concurrency.limit = activation.at ("limit").get<std::int32_t> ();
        if (value.contains ("maintenanceWave") && !value.at ("maintenanceWave").is_null ())
            result.maintenance_wave = value.at ("maintenanceWave").get<std::string> ();
        result.state = static_cast<framework_runtime_state_t> (value.at ("state").get<int> ());
        result.security_identity = value.at ("securityIdentity").get<std::string> ();
        result.owner_id = value.at ("ownerId").get<std::string> ();
        result.lease_generation = value.at ("leaseGeneration").get<std::int64_t> ();
        result.updated_at = from_unix_ms (value.at ("updatedAt").get<std::int64_t> ());
        return result;
    }

    static json_t encode (const client_server_server_descriptor_t &value)
    {
        return {{"channelName", value.channel_name},
                {"serverRid", value.server_rid.to_hex ()},
                {"lifecycleGeneration", value.lifecycle_generation},
                {"descriptorRevision", value.descriptor_revision},
                {"endpoint", value.endpoint},
                {"weight", value.weight},
                {"state", static_cast<int> (value.state)},
                {"securityIdentity", value.security_identity},
                {"ownerId", value.owner_id},
                {"leaseGeneration", value.lease_generation},
                {"updatedAt", unix_ms (value.updated_at)}};
    }

    static client_server_server_descriptor_t decode_client_server (const json_t &value)
    {
        client_server_server_descriptor_t result;
        result.channel_name = value.at ("channelName").get<std::string> ();
        result.server_rid =
          zlink::routing_id_t::from_hex (value.at ("serverRid").get<std::string> ());
        result.lifecycle_generation = value.at ("lifecycleGeneration").get<std::uint64_t> ();
        result.descriptor_revision = value.at ("descriptorRevision").get<std::uint64_t> ();
        result.endpoint = value.at ("endpoint").get<std::string> ();
        result.weight = value.at ("weight").get<int> ();
        result.state = static_cast<framework_runtime_state_t> (value.at ("state").get<int> ());
        result.security_identity = value.at ("securityIdentity").get<std::string> ();
        result.owner_id = value.at ("ownerId").get<std::string> ();
        result.lease_generation = value.at ("leaseGeneration").get<std::int64_t> ();
        result.updated_at = from_unix_ms (value.at ("updatedAt").get<std::int64_t> ());
        return result;
    }

    static json_t encode (const fanout_publisher_descriptor_t &value)
    {
        return {{"channelName", value.channel_name},
                {"publisherRid", value.publisher_rid.to_hex ()},
                {"lifecycleGeneration", value.lifecycle_generation},
                {"descriptorRevision", value.descriptor_revision},
                {"endpoint", value.endpoint},
                {"state", static_cast<int> (value.state)},
                {"securityIdentity", value.security_identity},
                {"ownerId", value.owner_id},
                {"leaseGeneration", value.lease_generation},
                {"updatedAt", unix_ms (value.updated_at)}};
    }

    static fanout_publisher_descriptor_t decode_fanout (const json_t &value)
    {
        fanout_publisher_descriptor_t result;
        result.channel_name = value.at ("channelName").get<std::string> ();
        result.publisher_rid =
          zlink::routing_id_t::from_hex (value.at ("publisherRid").get<std::string> ());
        result.lifecycle_generation = value.at ("lifecycleGeneration").get<std::uint64_t> ();
        result.descriptor_revision = value.at ("descriptorRevision").get<std::uint64_t> ();
        result.endpoint = value.at ("endpoint").get<std::string> ();
        result.state = static_cast<framework_runtime_state_t> (value.at ("state").get<int> ());
        result.security_identity = value.at ("securityIdentity").get<std::string> ();
        result.owner_id = value.at ("ownerId").get<std::string> ();
        result.lease_generation = value.at ("leaseGeneration").get<std::int64_t> ();
        result.updated_at = from_unix_ms (value.at ("updatedAt").get<std::int64_t> ());
        return result;
    }

    static json_t encode_mesh_record (std::uint64_t generation,
                                      const mesh_node_descriptor_t &descriptor)
    {
        return {{"generation", generation}, {"descriptor", encode (descriptor)}};
    }

    static json_t encode_descriptor_record (std::uint64_t generation,
                                            std::string owner_id,
                                            std::int64_t lease_generation,
                                            std::uint64_t lifecycle_generation,
                                            std::uint64_t descriptor_revision,
                                            json_t descriptor)
    {
        return {{"generation", generation},
                {"ownerId", std::move (owner_id)},
                {"leaseGeneration", lease_generation},
                {"lifecycleGeneration", lifecycle_generation},
                {"descriptorRevision", descriptor_revision},
                {"descriptor", std::move (descriptor)}};
    }

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    template <typename T> static task_t<T> unavailable (std::string message)
    {
        return task_t<T> (result_t<T>::failure (framework_error_kind_t::internal_failure,
                                                std::move (message)));
    }

    template <typename T> static task_t<T> cancelled ()
    {
        return task_t<T> (detail::boundary_failure<T> (detail::boundary_error_t::cancelled,
                                                       "location store operation was cancelled"));
    }

    location_store_t *_store;
};

} // namespace zlink::framework::runtime
