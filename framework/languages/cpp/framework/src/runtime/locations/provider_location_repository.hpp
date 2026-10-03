/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/detail/binary_text_codec.hpp>

#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_record_fields.hpp"
#include "runtime/locations/aggregate_inventory.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/transport/endpoint_notation.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include "base64.hpp"
#include "sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <numeric>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime
{
namespace aggregate_status
{
inline constexpr char pending[] = "pending";
inline constexpr char preparing[] = "preparing";
inline constexpr char prepared[] = "prepared";
inline constexpr char committing[] = "committing";
inline constexpr char committed[] = "committed";
inline constexpr char aborted[] = "aborted";
}


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
            auto owner = co_await _store->read (owner_key);
            if (std::holds_alternative<store_found_t> (owner))
                co_return owner_lease_claim_result_t{owner_lease_conflict_t{}};

            auto counter = co_await _store->read (counter_key);
            std::int64_t generation = 1;
            if (const auto *found = std::get_if<store_found_t> (&counter))
                generation = parse_i64 (found->value.bytes);
            if (generation == std::numeric_limits<std::int64_t>::max ())
                co_return owner_lease_claim_result_t{owner_lease_generation_exhausted_t{}};

            const auto payload = owner_lease_bytes ({owner_id, generation});
            store_write_request_t request;
            request.conditions.push_back (missing_condition (owner_key));
            request.conditions.push_back (condition_for (counter_key, counter));
            request.mutations.push_back (store_put_t{owner_key, payload, lease_ttl});
            request.mutations.push_back (
              store_put_t{counter_key, to_bytes (std::to_string (generation + 1)), std::nullopt});
            auto written = co_await write_async (std::move (request));
            if (std::holds_alternative<store_write_conflict_t> (written))
                continue;
            const auto &applied = std::get<store_write_applied_t> (written);
            co_return owner_lease_claim_result_t{
              owner_lease_claimed_t{{std::move (owner_id), generation},
                                    applied.store_now + lease_ttl,
                                    applied.store_now}};
        }
    }

    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner_id) override
    {
        auto result = co_await _store->read (key_owner (owner_id));
        const auto *found = std::get_if<store_found_t> (&result);
        if (!found)
            co_return owner_lease_read_result_t{owner_lease_missing_t{}};
        auto lease = decode_owner_lease (*found);
        if (lease.token.owner_id != owner_id)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Location Store owner lease record is invalid");
        co_return owner_lease_read_result_t{std::move (lease)};
    }

    task_t<owner_lease_renew_result_t>
    renew_owner_lease (location_owner_token_t token, std::chrono::milliseconds lease_ttl) override
    {
        if (lease_ttl <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("owner lease TTL must be positive");
        const auto key = key_owner (token.owner_id);
        auto current = co_await _store->read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            co_return owner_lease_renew_result_t{owner_lease_stale_t{}};
        store_write_request_t request;
        request.conditions.push_back (version_condition (key, found->value.version));
        request.mutations.push_back (store_put_t{key, found->value.bytes, lease_ttl});
        auto result = co_await write_async (std::move (request));
        if (std::holds_alternative<store_write_conflict_t> (result))
            co_return owner_lease_renew_result_t{owner_lease_stale_t{}};
        const auto &applied = std::get<store_write_applied_t> (result);
        co_return owner_lease_renew_result_t{
          owner_lease_renewed_t{applied.store_now + lease_ttl, applied.store_now}};
    }

    task_t<owner_lease_release_result_t> release_owner_lease (location_owner_token_t token) override
    {
        const auto key = key_owner (token.owner_id);
        auto current = co_await _store->read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            co_return owner_lease_release_result_t{owner_lease_stale_t{}};
        store_write_request_t result_request{{version_condition (key, found->value.version)},
                                             {store_delete_t{key}}};
        auto result = co_await write_async (std::move (result_request));
        co_return std::holds_alternative<store_write_applied_t> (result)
          ? owner_lease_release_result_t{owner_lease_released_t{}}
          : owner_lease_release_result_t{owner_lease_stale_t{}};
    }

    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        const auto key = key_mesh (descriptor.mesh_name, descriptor.rid);
        auto current = co_await _store->read (key);
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            const auto stored = decode_mesh_descriptor (
              parse_canonical_record (found->value.bytes, "MeshNode descriptor")
                .at (location_record_fields::descriptor));
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
        co_return co_await update_descriptor (
          key, descriptor.owner_id, descriptor.lease_generation, descriptor.lifecycle_generation,
          descriptor.descriptor_revision, encode_mesh_record (1, descriptor), intent,
          [descriptor] (const nlohmann::json &record) {
              return same_mesh_immutable (
                decode_mesh_descriptor (record.at (location_record_fields::descriptor)),
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
              return decode_mesh_descriptor (record.at (location_record_fields::descriptor));
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
          intent, [descriptor] (const nlohmann::json &record) {
              const auto current =
                decode_client_server (record.at (location_record_fields::descriptor));
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
              return decode_client_server (record.at (location_record_fields::descriptor));
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
          intent, [descriptor] (const nlohmann::json &record) {
              const auto current = decode_fanout (record.at (location_record_fields::descriptor));
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
          prefix_fanout (channel_name), std::move (page), [] (const nlohmann::json &record) {
              return decode_fanout (record.at (location_record_fields::descriptor));
          });
    }

    task_t<authority_read_result_t> read_authority (authority_key_t key,
                                                    std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<authority_read_result_t> ();
        co_return co_await read_authority_value_async (std::move (key.value));
    }

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority (authority_key_t key,
                                std::string expected_store_version,
                                authority_mutation_t mutation,
                                std::stop_token cancellation = {}) override
    {
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<authority_compare_exchange_result_t> ();
            const auto row_key = key_authority (key.value);
            auto current = co_await _store->read (row_key);
            auto *found = std::get_if<store_found_t> (&current);
            if (!found)
                co_return co_await authority_conflict (std::move (current));
            if (co_await authority_mutation_locked_async (key.value))
                co_return authority_compare_exchange_result_t{
                  authority_conflict_t{co_await read_authority_value_async (key.value)}};
            auto snapshot =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot.store_version != expected_store_version)
                co_return co_await authority_conflict (std::move (current));

            const auto *retarget = std::get_if<authority_retarget_t> (&mutation);
            const auto qualification_owner = retarget ? retarget->target.owner : snapshot.owner;
            store_write_request_t write_request;
            if (std::holds_alternative<authority_delete_t> (mutation)) {
                if (snapshot.allocation.state != placement_allocation_state_t::active)
                    co_return co_await authority_conflict (std::move (current));
                if (!co_await owner_is_live_async (snapshot.owner))
                    co_return co_await authority_conflict (std::move (current));
                auto target =
                  co_await read_target_descriptor_async (snapshot.allocation.target, false);
                if (!target)
                    co_return co_await authority_conflict (std::move (current));
                auto capacity = co_await read_capacity_async (snapshot.allocation.target);
                if (!adjust_capacity (capacity.record, snapshot.allocation.capacity_bundle, 0, -1))
                    co_return co_await authority_conflict (std::move (current));
                const auto decoded_key =
                  authority_key_codec_detail::decode_authority_key (key.value);
                if (!decoded_key)
                    co_return co_await authority_conflict (std::move (current));
                write_request.conditions = {
                  version_condition (row_key, found->value.version),
                  owner_condition (snapshot.owner),
                  version_condition (target->key, target->provider_version), capacity.condition};
                write_request.mutations = {store_delete_t{row_key},
                                           store_put_t{capacity.key,
                                                       encode_capacity_record (capacity.record),
                                                       std::nullopt}};
            } else if (const auto *restore = std::get_if<authority_restore_t> (&mutation)) {
                if (!same_owner (snapshot.owner, restore->expected_owner))
                    co_return co_await authority_conflict (std::move (current));
                snapshot.payload = restore->payload;
                write_request = {
                  {version_condition (row_key, found->value.version)},
                  {store_put_t{row_key, encode_authority (snapshot), std::nullopt}}};
            } else if (retarget) {
                if (snapshot.allocation.state != placement_allocation_state_t::active)
                    co_return co_await authority_conflict (std::move (current));
                auto target_descriptor = co_await read_target_descriptor_async (retarget->target);
                if (!target_descriptor
                    || !target_accepts (target_descriptor->descriptor,
                                        snapshot.allocation.object_kind,
                                        snapshot.allocation.stable_type))
                    co_return co_await authority_conflict (std::move (current));
                const auto same_allocation_target =
                  same_target (snapshot.allocation.target, retarget->target);
                std::optional<stored_capacity_t> source_capacity;
                std::optional<stored_capacity_t> target_capacity;
                if (!same_allocation_target) {
                    source_capacity = co_await read_capacity_async (snapshot.allocation.target);
                    target_capacity = co_await read_capacity_async (retarget->target);
                    if (source_capacity->key.value == target_capacity->key.value)
                        target_capacity->record = source_capacity->record;
                    if (!capacity_available (target_descriptor->descriptor, target_capacity->record,
                                             snapshot.allocation.capacity_bundle)
                        || !adjust_capacity (source_capacity->record,
                                             snapshot.allocation.capacity_bundle, 0, -1))
                        co_return co_await authority_conflict (std::move (current));
                    if (source_capacity->key.value == target_capacity->key.value)
                        target_capacity->record = source_capacity->record;
                    if (!adjust_capacity (target_capacity->record,
                                          snapshot.allocation.capacity_bundle, 0, 1)) {
                        co_return co_await authority_conflict (std::move (current));
                    }
                    if (source_capacity->key.value == target_capacity->key.value)
                        source_capacity->record = target_capacity->record;
                }

                auto owner_generations = co_await _store->read (authority_owner_counter_key);
                const auto next_owner_generation = counter_next_value (owner_generations);
                if (next_owner_generation >= max_generation)
                    co_return authority_compare_exchange_result_t{
                      authority_generation_exhausted_t{}};
                snapshot.authority_owner_generation = next_owner_generation;
                snapshot.owner = retarget->target.owner;
                snapshot.allocation.target = retarget->target;
                snapshot.payload = retarget->payload;
                write_request.conditions = {
                  version_condition (row_key, found->value.version),
                  condition_for (authority_owner_counter_key, owner_generations),
                  owner_condition (retarget->target.owner),
                  version_condition (target_descriptor->key, target_descriptor->provider_version)};
                if (source_capacity) {
                    write_request.conditions.push_back (source_capacity->condition);
                    if (target_capacity->key.value != source_capacity->key.value)
                        write_request.conditions.push_back (target_capacity->condition);
                }
                write_request.mutations = {
                  store_put_t{row_key, encode_authority (snapshot), std::nullopt},
                  store_put_t{authority_owner_counter_key,
                              to_bytes (std::to_string (next_owner_generation + 1)), std::nullopt}};
                if (source_capacity) {
                    write_request.mutations.push_back (
                      store_put_t{source_capacity->key,
                                  encode_capacity_record (source_capacity->record), std::nullopt});
                    if (target_capacity->key.value != source_capacity->key.value)
                        write_request.mutations.push_back (store_put_t{
                          target_capacity->key, encode_capacity_record (target_capacity->record),
                          std::nullopt});
                }
            } else {
                if (snapshot.allocation.state != placement_allocation_state_t::active)
                    co_return co_await authority_conflict (std::move (current));
                auto live_owner = co_await read_live_owner_async (snapshot.owner);
                if (!live_owner)
                    co_return co_await authority_conflict (std::move (current));
                write_request.conditions = {version_condition (row_key, found->value.version),
                                            owner_condition (snapshot.owner)};
                if (auto *reincarnate = std::get_if<authority_reincarnate_t> (&mutation)) {
                    auto object_generations = co_await _store->read (object_counter_key);
                    auto owner_generations = co_await _store->read (authority_owner_counter_key);
                    const auto object_generation = counter_next_value (object_generations);
                    const auto owner_generation = counter_next_value (owner_generations);
                    if (object_generation >= max_generation || owner_generation >= max_generation)
                        co_return authority_compare_exchange_result_t{
                          authority_generation_exhausted_t{}};
                    snapshot.object_generation = object_generation;
                    snapshot.authority_owner_generation = owner_generation;
                    snapshot.payload = reincarnate->payload;
                    write_request.conditions.push_back (
                      condition_for (object_counter_key, object_generations));
                    write_request.conditions.push_back (
                      condition_for (authority_owner_counter_key, owner_generations));
                    write_request.mutations = {
                      store_put_t{object_counter_key, to_bytes (std::to_string (object_generation + 1)),
                                  std::nullopt},
                      store_put_t{authority_owner_counter_key,
                                  to_bytes (std::to_string (owner_generation + 1)), std::nullopt}};
                } else {
                    snapshot.payload = std::get<authority_put_t> (mutation).payload;
                }
                write_request.mutations.push_back (
                  store_put_t{row_key, encode_authority (snapshot), std::nullopt});
            }
            auto written = co_await write_async (std::move (write_request));
            const auto *applied = std::get_if<store_write_applied_t> (&written);
            if (!applied) {
                if (co_await conflict_qualification_unchanged (
                      row_key, found->value.version, qualification_owner, std::nullopt))
                    continue;
                co_return co_await authority_conflict (co_await _store->read (row_key));
            }
            if (std::holds_alternative<authority_delete_t> (mutation))
                co_return authority_compare_exchange_result_t{
                  authority_deleted_t{snapshot.store_version, applied->store_now}};
            snapshot.store_now = applied->store_now;
            snapshot.store_version = version_of (*applied, row_key);
            co_return authority_compare_exchange_result_t{authority_stored_t{std::move (snapshot)}};
        }
    }

    task_t<authority_scan_result_t> list_authorities (std::string key_prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit,
                                                      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<authority_scan_result_t> ();
        if (limit == 0 || limit > location_page_item_limit)
            throw std::invalid_argument ("authority scan limit must be between 1 and 1000");
        store_scan_request_t scan_request{
          prefix_authority (),
          cursor ? std::optional<store_scan_cursor_t>{store_scan_cursor_t{
                     std::string (cursor->encoded ())}}
                 : std::nullopt,
          static_cast<std::uint32_t> (limit)};
        auto result = co_await _store->scan (std::move (scan_request));
        const auto *page = std::get_if<store_scan_page_t> (&result);
        if (!page)
            co_return authority_scan_result_t{authority_scan_expired_t{}};
        authority_page_t output;
        output.items.reserve (page->items.size ());
        for (const auto &item : page->items) {
            // item.key.value is the §2.4 preimage "authority\0{actor|spot}\0{Id}".
            const auto &preimage = item.key.value;
            const auto domain_end = authority_domain.size ();
            if (preimage.size () <= domain_end + 1
                || preimage.compare (0, domain_end, authority_domain) != 0
                || preimage[domain_end] != '\0')
                continue;
            const auto kind_start = domain_end + 1;
            const auto kind_end = preimage.find ('\0', kind_start);
            if (kind_end == std::string::npos)
                continue;
            const auto kind_word = preimage.substr (kind_start, kind_end - kind_start);
            const auto object_id = preimage.substr (kind_end + 1);
            char kind = 0;
            if (kind_word == "actor")
                kind = 'a';
            else if (kind_word == "spot")
                kind = 's';
            else
                continue;
            const auto logical_key =
              (kind == 'a' ? actor_authority_key (object_id) : spot_authority_key (object_id))
                .value;
            if (!logical_key.starts_with (key_prefix))
                continue;
            auto snapshot = co_await effective_authority_async (
              logical_key, item.value.bytes, item.value.version, page->store_now);
            if (!snapshot)
                continue;
            output.items.push_back ({{logical_key}, std::move (*snapshot)});
        }
        if (page->next_cursor)
            output.next_cursor = authority_scan_cursor_t{page->next_cursor->value};
        co_return authority_scan_result_t{std::move (output)};
    }

    task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (creation_operation_identity_t operation,
                            std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return detail::result_access_t::failure<std::optional<creation_terminal_record_t>> (
              detail::make_cancellation_exception ("location store operation was cancelled"));
        auto result = co_await _store->read (key_creation_terminal (operation));
        const auto *found = std::get_if<store_found_t> (&result);
        if (!found)
            co_return std::optional<creation_terminal_record_t>{};
        if (!found->value.expires_at)
            throw std::invalid_argument ("creation terminal record is missing expiry");
        co_return std::optional<creation_terminal_record_t>{creation_terminal_record_t{
          operation, found->value.bytes,
          std::chrono::time_point_cast<std::chrono::milliseconds> (*found->value.expires_at)}};
    }

    task_t<object_reserve_result_t> reserve (object_reserve_request_t request,
                                             std::stop_token cancellation = {}) override
    {
        object_reservation_fence_t fence;
        for (;;) {
            bool retry_reclaim = false;
            auto result =
              co_await await_result (reserve_once (request, cancellation, &retry_reclaim, fence));
            if (!result)
                co_return std::move (result);

            if (!retry_reclaim)
                co_return std::move (result);
        }
    }

  private:
    task_t<bool> conflict_qualification_unchanged (
      store_key_t key,
      std::optional<store_version_t> original_version,
      location_owner_token_t owner,
      std::optional<std::chrono::system_clock::time_point> operation_deadline,
      const creation_terminal_record_t *terminal = nullptr,
      std::chrono::system_clock::time_point *terminal_store_now = nullptr)
    {
        if (terminal) {
            const auto completed =
              co_await _store->read (key_creation_terminal (terminal->operation));
            if (std::holds_alternative<store_found_t> (completed))
                co_return false;
            if (terminal_store_now)
                *terminal_store_now = std::get<store_missing_t> (completed).store_now;
        }
        const auto current = co_await _store->read (std::move (key));
        const auto *found = std::get_if<store_found_t> (&current);
        const bool unchanged = original_version
                                 ? found && found->value.version.value == original_version->value
                                 : std::holds_alternative<store_missing_t> (current);
        const auto store_now =
          found ? found->value.store_now : std::get<store_missing_t> (current).store_now;
        // The unchanged opaque version also preserves the reservation identity
        // checked before the operation constructs its conditional write.
        if (!unchanged || (operation_deadline && store_now >= *operation_deadline))
            co_return false;
        co_return co_await owner_is_live_async (owner);
    }

    enum class stale_authority_reclaim_result_t
    {
        owner_live,
        reclaimed,
        conflict,
        recovery_required
    };

    task_t<object_reserve_result_t> reserve_once (const object_reserve_request_t &request,
                                                  std::stop_token cancellation,
                                                  bool *retry_reclaim,
                                                  object_reservation_fence_t &fence)
    {
        if (cancellation.stop_requested ())
            co_return detail::result_access_t::failure<object_reserve_result_t> (
              detail::make_cancellation_exception ("location store operation was cancelled"));
        if (request.creating_payload.size () > location_record_payload_limit
            || request.intent.request_encoded_size > location_record_payload_limit)
            throw std::invalid_argument ("object reservation payload exceeds 1 MiB");
        if (!bundle_matches (request.capacity_bundle, request.key.kind, request.intent.stable_type))
            throw std::invalid_argument (
              "object reservation capacity bundle does not match the object");

        auto creation_target = request.target;
        const auto authority_key = key_authority (object_key (request.key));
        auto authority = co_await _store->read (authority_key);
        if (co_await authority_mutation_locked_async (object_key (request.key))) {
            auto current = co_await read_authority_value_async (object_key (request.key));
            co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
        }
        if (const auto *found = std::get_if<store_found_t> (&authority)) {
            auto current =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (current.allocation.stable_type != request.intent.stable_type)
                co_return object_reserve_result_t{object_type_mismatch_t{std::move (current)}};
            if (current.allocation.state == placement_allocation_state_t::active)
                co_return object_reserve_result_t{object_already_exists_t{std::move (current)}};
            const auto reclaim = co_await try_reclaim_reserved_authority_async (
              request.key, authority_key, *found, current);
            if (reclaim == stale_authority_reclaim_result_t::reclaimed
                || reclaim == stale_authority_reclaim_result_t::conflict) {
                *retry_reclaim = reclaim == stale_authority_reclaim_result_t::reclaimed;
                if (!*retry_reclaim)
                    *retry_reclaim = co_await conflict_qualification_unchanged (
                      authority_key, found->value.version, creation_target.owner,
                      request.operation_deadline);
                auto observed = co_await read_authority_value_async (object_key (request.key));
                co_return object_reserve_result_t{object_reserve_conflict_t{std::move (observed)}};
            }
            co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
        }

        auto target = co_await read_target_descriptor_async (creation_target);
        if (!target) {
            // A node lease renewal or descriptor publication can advance the
            // lifecycle/owner version between candidate enumeration and the
            // reservation CAS. Refresh the target by its stable node key
            // before classifying the candidate as unavailable.
            auto refreshed = co_await read_target_descriptor_async (creation_target, true, false);
            if (refreshed
                && target_accepts (refreshed->descriptor, request.key.kind,
                                   request.intent.stable_type)) {
                creation_target.node_lifecycle_generation =
                  refreshed->descriptor.lifecycle_generation;
                creation_target.owner = {refreshed->descriptor.owner_id,
                                         refreshed->descriptor.lease_generation};
                target = std::move (refreshed);
            }
        }
        bool target_available =
          target
          && target_accepts (target->descriptor, request.key.kind, request.intent.stable_type);
        if (target_available)
            target_available = co_await owner_is_live_async (creation_target.owner);
        if (!target_available)
            co_return object_reserve_result_t{object_reserve_conflict_t{
              authority_missing_t{std::get<store_missing_t> (authority).store_now}}};
        auto capacity = co_await read_capacity_async (creation_target);
        if (!capacity_available (target->descriptor, capacity.record, request.capacity_bundle)) {
            co_return object_reserve_result_t{object_placement_capacity_exhausted_t{}};
        }
        if (!adjust_capacity (capacity.record, request.capacity_bundle, 1, 0))
            co_return object_reserve_result_t{object_placement_capacity_exhausted_t{}};

        auto object_generations = co_await _store->read (object_counter_key);
        auto owner_generations = co_await _store->read (authority_owner_counter_key);
        const auto object_generation = counter_next_value (object_generations);
        const auto owner_generation_value = counter_next_value (owner_generations);
        if (object_generation >= max_generation || owner_generation_value >= max_generation)
            co_return object_reserve_result_t{authority_generation_exhausted_t{}};

        // The authority write assigns the opaque version.  The returned
        // fence receives it after the write; pendingCreation is the durable
        // reservation identity while the row remains reserved.
        if (fence.reservation_id.empty ())
            fence.reservation_id = "reservation-" + std::to_string (object_generation) + "-"
                                   + std::to_string (owner_generation_value);
        fence.expected_store_version = aggregate_status::pending;
        fence.object_generation = object_generation;
        fence.authority_owner_generation = owner_generation_value;
        fence.target = creation_target;
        fence.capacity_bundle = request.capacity_bundle;
        authority_snapshot_t creating{
          aggregate_status::pending,
          request.creating_payload,
          object_generation,
          owner_generation_value,
          creation_target.owner,
          {},
          {placement_allocation_state_t::reserved, request.key.kind, request.intent.stable_type,
           creation_target, request.capacity_bundle},
          pending_object_creation_t{
            fence.reservation_id, request.intent.request_content_reference,
            request.intent.request_sha256,
            static_cast<std::uint32_t> (request.intent.request_encoded_size)}};
        store_write_request_t write_request{
          {missing_condition (authority_key),
           condition_for (object_counter_key, object_generations),
           condition_for (authority_owner_counter_key, owner_generations),
           version_condition (target->key, target->provider_version),
           owner_condition (creation_target.owner), capacity.condition},
          {store_put_t{authority_key, encode_authority (creating), std::nullopt},
           store_put_t{object_counter_key, to_bytes (std::to_string (object_generation + 1)),
                       std::nullopt},
           store_put_t{authority_owner_counter_key,
                       to_bytes (std::to_string (owner_generation_value + 1)), std::nullopt},
           store_put_t{capacity.key, encode_capacity_record (capacity.record), std::nullopt}}};
        auto written = co_await write_async (std::move (write_request));
        const auto *applied = std::get_if<store_write_applied_t> (&written);
        if (!applied) {
            *retry_reclaim = co_await conflict_qualification_unchanged (
              authority_key, std::nullopt, creation_target.owner, request.operation_deadline);
            auto observed = co_await read_authority_value_async (object_key (request.key));
            co_return object_reserve_result_t{object_reserve_conflict_t{std::move (observed)}};
        }
        creating.store_now = applied->store_now;
        creating.store_version = version_of (*applied, authority_key);
        fence.expected_store_version = creating.store_version;
        co_return object_reserve_result_t{
          object_reserved_t{std::move (fence), std::move (creating)}};
    }

    task_t<stale_authority_reclaim_result_t>
    try_reclaim_reserved_authority_async (object_creation_key_t key,
                                          store_key_t authority_key,
                                          store_found_t stored_authority,
                                          authority_snapshot_t current)
    {
        const auto owner_key = key_owner (current.owner.owner_id);
        auto owner = co_await _store->read (owner_key);
        auto stale_owner_condition = missing_condition (owner_key);
        if (const auto *found = std::get_if<store_found_t> (&owner)) {
            const auto lease = decode_owner_lease (*found);
            if (lease.token.owner_id != current.owner.owner_id)
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Location Store owner lease record is invalid");
            if (lease.token.lease_generation == current.owner.lease_generation
                && lease.lease_expires_at > lease.store_now)
                co_return stale_authority_reclaim_result_t::owner_live;
            stale_owner_condition = owner_condition (lease.token);
        }

        // Relocation authority is recovered by its own protocol. Reserve may
        // reclaim only a steady pending creation whose owner lease ended.
        const auto actor = decode_direct_actor_authority_payload (current.payload);
        if (actor && actor->has_relocation_state)
            co_return stale_authority_reclaim_result_t::recovery_required;
        if (!current.pending_creation)
            co_return stale_authority_reclaim_result_t::recovery_required;

        const auto target =
          co_await read_target_descriptor_async (current.allocation.target, false, false);
        if (!target)
            co_return stale_authority_reclaim_result_t::recovery_required;
        auto capacity = co_await read_capacity_async (current.allocation.target);
        if (!adjust_capacity (capacity.record, current.allocation.capacity_bundle, -1, 0))
            co_return stale_authority_reclaim_result_t::recovery_required;

        store_write_request_t write_request{
          {version_condition (authority_key, stored_authority.value.version),
           std::move (stale_owner_condition), capacity.condition},
          {store_delete_t{authority_key},
           store_put_t{capacity.key, encode_capacity_record (capacity.record), std::nullopt}}};
        const auto written = co_await write_async (std::move (write_request));
        co_return std::holds_alternative<store_write_applied_t> (written)
          ? stale_authority_reclaim_result_t::reclaimed
          : stale_authority_reclaim_result_t::conflict;
    }

  public:
    task_t<object_complete_creation_result_t>
    complete_creation (object_complete_creation_request_t request,
                       std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return detail::result_access_t::failure<object_complete_creation_result_t> (
              detail::make_cancellation_exception ("location store operation was cancelled"));
        const auto publication =
          std::visit ([] (const auto &value) { return value.terminal; }, request.completion);
        if (publication.terminal_envelope.size () > location_record_payload_limit)
            throw std::invalid_argument ("creation terminal envelope is too large");
        const auto expires_at = publication.operation_deadline + creation_terminal_retention;
        const auto terminal_key = key_creation_terminal (publication.operation);
        auto existing = co_await _store->read (terminal_key);
        if (const auto *found = std::get_if<store_found_t> (&existing)) {
            if (!found->value.expires_at)
                throw std::invalid_argument ("creation terminal record is missing expiry");
            co_return object_complete_creation_result_t{object_creation_already_completed_result_t{
              creation_terminal_record_t{publication.operation, found->value.bytes,
                                         std::chrono::time_point_cast<std::chrono::milliseconds> (
                                           *found->value.expires_at)}}};
        }

        const auto store_now = std::get<store_missing_t> (existing).store_now;
        if (expires_at <= store_now)
            throw std::invalid_argument ("creation terminal expiry is not in the future");
        creation_terminal_record_t terminal{publication.operation, publication.terminal_envelope,
                                            expires_at};
        object_complete_creation_result_t result{object_creation_completion_stale_t{}};
        if (const auto *created = std::get_if<object_creation_completed_t> (&request.completion)) {
            object_commit_request_t commit_request{request.key, request.fence,
                                                   created->ready_payload};
            const auto committed =
              co_await commit (std::move (commit_request), cancellation, &terminal, store_now,
                               publication.operation_deadline);
            if (const auto *value = std::get_if<object_committed_t> (&committed))
                co_return object_complete_creation_result_t{
                  object_creation_completed_result_t{std::move (terminal), value->ready}};
            if (const auto *conflict = std::get_if<object_commit_conflict_t> (&committed))
                result = object_creation_completion_conflict_t{conflict->current};
            else if (std::holds_alternative<authority_generation_exhausted_t> (committed))
                result = authority_generation_exhausted_t{};
        } else {
            object_abort_request_t abort_request{request.key, request.fence};
            const auto aborted = co_await abort (std::move (abort_request), cancellation, &terminal,
                                                 store_now, publication.operation_deadline);
            if (std::holds_alternative<object_aborted_t> (aborted))
                co_return object_complete_creation_result_t{
                  object_creation_completed_result_t{std::move (terminal), std::nullopt}};
            if (const auto *conflict = std::get_if<object_abort_conflict_t> (&aborted))
                result = object_creation_completion_conflict_t{conflict->current};
        }
        // A competing completion may have won the same conditional write.
        // Only its stored terminal can resolve replay; Ready alone cannot.
        auto concurrent = co_await _store->read (terminal_key);
        if (const auto *found = std::get_if<store_found_t> (&concurrent)) {
            if (!found->value.expires_at)
                throw std::invalid_argument ("creation terminal record is missing expiry");
            co_return object_complete_creation_result_t{object_creation_already_completed_result_t{
              creation_terminal_record_t{publication.operation, found->value.bytes,
                                         std::chrono::time_point_cast<std::chrono::milliseconds> (
                                           *found->value.expires_at)}}};
        }
        co_return result;
    }

    task_t<object_commit_result_t>
    commit (object_commit_request_t request,
            std::stop_token cancellation = {},
            std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        return commit (std::move (request), cancellation, nullptr, {}, operation_deadline);
    }

    task_t<object_abort_result_t>
    abort (object_abort_request_t request,
           std::stop_token cancellation = {},
           std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        return abort (std::move (request), cancellation, nullptr, {}, operation_deadline);
    }

  private:
    task_t<object_commit_result_t> commit (object_commit_request_t request,
                                           std::stop_token cancellation,
                                           const creation_terminal_record_t *terminal,
                                           std::chrono::system_clock::time_point terminal_store_now,
                                           std::chrono::system_clock::time_point operation_deadline)
    {
        for (;;) {
            if (cancellation.stop_requested ())
                co_return detail::result_access_t::failure<object_commit_result_t> (
                  detail::make_cancellation_exception ("location store operation was cancelled"));
            if (request.ready_payload.size () > location_record_payload_limit)
                throw std::invalid_argument ("object commit payload exceeds 1 MiB");
            const auto authority_key = key_authority (object_key (request.key));
            auto authority = co_await _store->read (authority_key);
            if (co_await authority_mutation_locked_async (object_key (request.key))) {
                auto current = co_await read_authority_value_async (object_key (request.key));
                co_return object_commit_result_t{object_commit_conflict_t{std::move (current)}};
            }
            const auto *stored_authority = std::get_if<store_found_t> (&authority);
            if (!stored_authority)
                co_return object_commit_result_t{object_commit_conflict_t{
                  authority_missing_t{std::get<store_missing_t> (authority).store_now}}};
            auto snapshot =
              decode_authority (stored_authority->value.bytes, stored_authority->value.version,
                                stored_authority->value.store_now);
            const auto matches_reservation =
              snapshot.pending_creation
              && snapshot.object_generation == request.fence.object_generation
              && snapshot.authority_owner_generation == request.fence.authority_owner_generation
              && same_owner (snapshot.owner, request.fence.target.owner)
              && snapshot.pending_creation->reservation_id == request.fence.reservation_id;
            if (!matches_reservation) {
                if (!snapshot.pending_creation)
                    co_return object_commit_result_t{
                      object_already_committed_t{std::move (snapshot)}};
                co_return object_commit_result_t{object_commit_stale_t{}};
            }
            if (snapshot.allocation.state != placement_allocation_state_t::reserved)
                co_return object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}};
            auto target = co_await read_target_descriptor_async (request.fence.target);
            if (!target)
                co_return object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}};
            auto capacity = co_await read_capacity_async (request.fence.target, &*target);
            if (!adjust_capacity (capacity.record, request.fence.capacity_bundle, -1, 1))
                co_return object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}};
            snapshot.payload = std::move (request.ready_payload);
            snapshot.allocation.state = placement_allocation_state_t::active;
            snapshot.pending_creation.reset ();
            store_write_request_t write_request{
              {version_condition (authority_key, stored_authority->value.version),
               owner_condition (request.fence.target.owner),
               version_condition (target->key, target->provider_version), capacity.condition},
              {store_put_t{authority_key, encode_authority (snapshot), std::nullopt},
               store_put_t{capacity.key, encode_capacity_record (capacity.record), std::nullopt}}};
            auto written =
              co_await write_async (std::move (write_request), terminal, terminal_store_now);
            const auto *applied = std::get_if<store_write_applied_t> (&written);
            if (!applied) {
                if (co_await conflict_qualification_unchanged (
                      authority_key, stored_authority->value.version, request.fence.target.owner,
                      operation_deadline, terminal, &terminal_store_now)) {
                    request.ready_payload = std::move (snapshot.payload);
                    continue;
                }
                auto current = co_await read_authority_value_async (object_key (request.key));
                co_return object_commit_result_t{object_commit_conflict_t{std::move (current)}};
            }
            snapshot.store_now = applied->store_now;
            snapshot.store_version = version_of (*applied, authority_key);
            co_return object_commit_result_t{object_committed_t{std::move (snapshot)}};
        }
    }

    task_t<object_abort_result_t> abort (object_abort_request_t request,
                                         std::stop_token cancellation,
                                         const creation_terminal_record_t *terminal,
                                         std::chrono::system_clock::time_point terminal_store_now,
                                         std::chrono::system_clock::time_point operation_deadline)
    {
        for (;;) {
            if (cancellation.stop_requested ())
                co_return detail::result_access_t::failure<object_abort_result_t> (
                  detail::make_cancellation_exception ("location store operation was cancelled"));
            const auto authority_key = key_authority (object_key (request.key));
            auto authority = co_await _store->read (authority_key);
            if (co_await authority_mutation_locked_async (object_key (request.key))) {
                auto current = co_await read_authority_value_async (object_key (request.key));
                co_return object_abort_result_t{object_abort_conflict_t{std::move (current)}};
            }
            const auto *stored_authority = std::get_if<store_found_t> (&authority);
            if (!stored_authority)
                co_return object_abort_result_t{object_abort_conflict_t{
                  authority_missing_t{std::get<store_missing_t> (authority).store_now}}};
            const auto snapshot =
              decode_authority (stored_authority->value.bytes, stored_authority->value.version,
                                stored_authority->value.store_now);
            const auto matches_reservation =
              snapshot.pending_creation
              && snapshot.object_generation == request.fence.object_generation
              && snapshot.authority_owner_generation == request.fence.authority_owner_generation
              && same_owner (snapshot.owner, request.fence.target.owner)
              && snapshot.pending_creation->reservation_id == request.fence.reservation_id;
            if (!matches_reservation)
                co_return object_abort_result_t{object_abort_stale_t{}};
            auto target = co_await read_target_descriptor_async (request.fence.target, false);
            if (!target)
                co_return object_abort_result_t{object_abort_conflict_t{snapshot}};
            auto capacity = co_await read_capacity_async (request.fence.target, &*target);
            if (!adjust_capacity (capacity.record, request.fence.capacity_bundle, -1, 0))
                co_return object_abort_result_t{object_abort_conflict_t{snapshot}};
            store_write_request_t write_request{
              {version_condition (authority_key, stored_authority->value.version),
               version_condition (target->key, target->provider_version), capacity.condition},
              {store_delete_t{authority_key},
               store_put_t{capacity.key, encode_capacity_record (capacity.record), std::nullopt}}};
            auto written =
              co_await write_async (std::move (write_request), terminal, terminal_store_now);
            if (!std::holds_alternative<store_write_applied_t> (written)) {
                if (co_await conflict_qualification_unchanged (
                      authority_key, stored_authority->value.version, request.fence.target.owner,
                      operation_deadline, terminal, &terminal_store_now))
                    continue;
                auto current = co_await read_authority_value_async (object_key (request.key));
                co_return object_abort_result_t{object_abort_conflict_t{std::move (current)}};
            }
            co_return object_abort_result_t{object_aborted_t{}};
        }
    }

  public:
    task_t<aggregate_prepare_result_t>
    prepare_aggregate (aggregate_prepare_request_t request,
                       std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<aggregate_prepare_result_t> ();
        if (request.aggregate_generation == 0 || request.aggregate_generation > max_generation
            || request.participants.empty ()
            || std::all_of (request.aggregate_id.value.begin (), request.aggregate_id.value.end (),
                            [] (std::byte value) { return value == std::byte{0}; })
            || request.target_owner.owner_id.empty () || request.target_owner.lease_generation <= 0
            || request.capacity_bundle.spot_slots != 1 || !request.capacity_bundle.spot_type
            || request.capacity_bundle.spot_type->object_kind != placement_object_kind_t::user_spot)
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        if (std::any_of (request.participants.begin (), request.participants.end (),
                         [] (const aggregate_participant_t &participant) {
                             return !participant.membership_mutation.empty ();
                         }))
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        const auto inventory_tree = aggregate_inventory::build_tree (request.participants);
        if (!inventory_tree)
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        std::string previous;
        std::vector<std::pair<store_key_t, store_found_t>> authorities;
        placement_capacity_bundle_t inventory;
        authorities.reserve (request.participants.size ());
        for (const auto &participant : request.participants) {
            if (!previous.empty () && participant.key.value <= previous)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            previous = participant.key.value;
            const auto key = key_authority (participant.key.value);
            auto row = co_await _store->read (key);
            const auto *found = std::get_if<store_found_t> (&row);
            if (!found)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            const auto snapshot =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot.store_version != participant.expected_store_version
                || snapshot.allocation.state != placement_allocation_state_t::active
                || participant.owner_transition != authority_generation_transition_t::new_owner)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            inventory.actor_slots += snapshot.allocation.capacity_bundle.actor_slots;
            inventory.spot_slots += snapshot.allocation.capacity_bundle.spot_slots;
            if (snapshot.allocation.capacity_bundle.spot_type) {
                const auto &spot = *snapshot.allocation.capacity_bundle.spot_type;
                if (inventory.spot_type
                    && (inventory.spot_type->object_kind != spot.object_kind
                        || inventory.spot_type->stable_type != spot.stable_type))
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                if (!inventory.spot_type)
                    inventory.spot_type =
                      spot_type_capacity_delta_t{spot.object_kind, spot.stable_type, 0};
                inventory.spot_type->slots += spot.slots;
            }
            authorities.emplace_back (key, *found);
        }
        if (encode_bundle (inventory) != encode_bundle (request.capacity_bundle))
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        const auto row_key = key_aggregate (request.aggregate_id);
        auto current = co_await _store->read (row_key);
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto stored = parse_json (found->value.bytes);
            const auto status = stored.value (location_record_fields::status, "");
            if (!aggregate_record_matches_request (stored, request, *inventory_tree))
                co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
            if (status == aggregate_status::prepared || status == aggregate_status::committing
                || status == aggregate_status::committed)
                co_return aggregate_prepare_result_t{aggregate_already_prepared_t{
                  {request.aggregate_id, request.aggregate_generation, request.inventory_digest}}};
            if (status != aggregate_status::preparing)
                co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
        } else {
            // Claim the aggregate before writing any inventory or lock child.
            // A restart can now find a partial prepare and abort those children
            // without guessing whether the claim reached the provider.
            const auto preparing =
              encode_aggregate (request, aggregate_status::preparing, *inventory_tree);
            store_write_request_t claimed_request{
              {missing_condition (row_key)},
              {store_put_t{row_key, to_bytes (preparing.dump ()), std::nullopt}}};
            const auto claimed = co_await write_async (std::move (claimed_request));
            if (!std::holds_alternative<store_write_applied_t> (claimed))
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        }

        const auto encoded =
          encode_aggregate (request, aggregate_status::prepared, *inventory_tree);
        std::size_t participant_offset = 0;
        std::size_t page_index = 0;
        for (const auto &page : inventory_tree->pages) {
            const auto page_key = key_aggregate_inventory (request.aggregate_id, page_index++);
            auto page_current = co_await _store->read (page_key);
            if (const auto *found = std::get_if<store_found_t> (&page_current)) {
                if (found->value.bytes != page.encoded)
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
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
                      co_await write_async (std::move (page_request))))
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            }

            // A prepared aggregate reserves the authority rows even though the
            // participant bytes remain in their existing rows. This bounded
            // lookup prevents a concurrent single-authority write from
            // invalidating the inventory before the aggregate CAS.
            store_write_request_t lock_request;
            for (std::size_t index = 0; index < page.participants.size (); ++index) {
                const auto participant_index = participant_offset + index;
                const auto lock_key =
                  key_aggregate_lock (request.participants[participant_index].key.value);
                auto existing_lock = co_await _store->read (lock_key);
                if (const auto *found = std::get_if<store_found_t> (&existing_lock)) {
                    const auto lock = decode_aggregate_lock (found->value.bytes);
                    if (!lock
                        || lock->authority_key != request.participants[participant_index].key.value)
                        co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                    if (lock->aggregate_id.value == request.aggregate_id.value
                        && lock->aggregate_generation == request.aggregate_generation) {
                        if (lock->expected_store_version
                            != request.participants[participant_index].expected_store_version)
                            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                        continue;
                    }
                    auto old_aggregate = co_await _store->read (key_aggregate (lock->aggregate_id));
                    const auto *old_aggregate_found = std::get_if<store_found_t> (&old_aggregate);
                    if (!old_aggregate_found
                        || parse_json (old_aggregate_found->value.bytes)
                               .value (location_record_fields::status, "")
                             != aggregate_status::committed)
                        co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                    lock_request.conditions.push_back (
                      version_condition (lock_key, found->value.version));
                } else {
                    lock_request.conditions.push_back (missing_condition (lock_key));
                }
                lock_request.conditions.push_back (
                  version_condition (authorities[participant_index].first,
                                     authorities[participant_index].second.value.version));
                lock_request.mutations.push_back (store_put_t{
                  lock_key,
                  to_bytes (encode_aggregate_lock (
                              request.aggregate_id, request.aggregate_generation,
                              request.participants[participant_index].key.value,
                              request.participants[participant_index].expected_store_version,
                              aggregate_status::prepared, page_index - 1, index)
                              .dump ()),
                  std::nullopt});
            }
            bool lock_rejected = false;
            if (!lock_request.mutations.empty ())
                lock_rejected = !std::holds_alternative<store_write_applied_t> (
                  co_await write_async (std::move (lock_request)));
            if (lock_rejected) {
                bool adopted = true;
                for (std::size_t index = 0; index < page.participants.size (); ++index) {
                    const auto participant_index = participant_offset + index;
                    const auto &participant = request.participants[participant_index];
                    const auto raced_lock =
                      co_await _store->read (key_aggregate_lock (participant.key.value));
                    const auto *raced_found = std::get_if<store_found_t> (&raced_lock);
                    const auto raced =
                      raced_found ? decode_aggregate_lock (raced_found->value.bytes) : std::nullopt;
                    if (!raced || raced->aggregate_id.value != request.aggregate_id.value
                        || raced->aggregate_generation != request.aggregate_generation
                        || raced->authority_key != participant.key.value
                        || raced->expected_store_version != participant.expected_store_version) {
                        adopted = false;
                        break;
                    }
                }
                if (!adopted) {
                    const auto raced_aggregate = co_await _store->read (row_key);
                    const auto *raced_found = std::get_if<store_found_t> (&raced_aggregate);
                    if (raced_found) {
                        const auto raced = parse_json (raced_found->value.bytes);
                        const auto status = raced.value (location_record_fields::status, "");
                        if (aggregate_record_matches_request (raced, request, *inventory_tree)
                            && (status == aggregate_status::preparing
                                || status == aggregate_status::prepared))
                            co_return co_await prepare_aggregate (std::move (request),
                                                                  cancellation);
                    }
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                }
            }

            participant_offset += page.participants.size ();
        }

        for (const auto &index_page : inventory_tree->index_pages) {
            const auto index_key = key_aggregate_inventory_index (
              request.aggregate_id, index_page.level, index_page.page_index);
            auto current_index = co_await _store->read (index_key);
            if (const auto *found = std::get_if<store_found_t> (&current_index)) {
                if (found->value.bytes != index_page.encoded)
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                continue;
            }
            store_write_request_t index_request;
            index_request.conditions.push_back (missing_condition (index_key));
            index_request.mutations.push_back (
              store_put_t{index_key, index_page.encoded, std::nullopt});
            if (!std::holds_alternative<store_write_applied_t> (
                  co_await write_async (std::move (index_request))))
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        }

        const object_creation_target_t target{
          request.target_descriptor.mesh_name,
          node_rid_t::from_string (request.target_descriptor.rid.to_string ()),
          request.target_descriptor_lifecycle_generation, request.target_owner};
        auto target_descriptor = co_await read_target_descriptor_async (target);
        if (!target_descriptor)
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        auto capacity = co_await read_capacity_async (target);
        if (!capacity_available (target_descriptor->descriptor, capacity.record,
                                 request.capacity_bundle)
            || !adjust_capacity (capacity.record, request.capacity_bundle, 1, 0))
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        auto preparing_row = co_await _store->read (row_key);
        const auto *preparing_found = std::get_if<store_found_t> (&preparing_row);
        if (!preparing_found)
            co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
        const auto preparing_record = parse_json (preparing_found->value.bytes);
        if (preparing_record.value (location_record_fields::status, "")
            != aggregate_status::preparing) {
            if (preparing_record.value (location_record_fields::status, "")
                  == aggregate_status::prepared
                || preparing_record.value (location_record_fields::status, "")
                     == aggregate_status::committing
                || preparing_record.value (location_record_fields::status, "")
                     == aggregate_status::committed)
                co_return aggregate_prepare_result_t{aggregate_already_prepared_t{
                  {request.aggregate_id, request.aggregate_generation, request.inventory_digest}}};
            co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
        }
        store_write_request_t write_request;
        write_request.conditions.push_back (
          version_condition (row_key, preparing_found->value.version));
        write_request.conditions.push_back (
          version_condition (target_descriptor->key, target_descriptor->provider_version));
        write_request.conditions.push_back (owner_condition (request.target_owner));
        write_request.conditions.push_back (capacity.condition);
        write_request.mutations.push_back (
          store_put_t{row_key, to_bytes (encoded.dump ()), std::nullopt});
        write_request.mutations.push_back (
          store_put_t{capacity.key, encode_capacity_record (capacity.record), std::nullopt});
        auto written = co_await write_async (std::move (write_request));
        if (!std::holds_alternative<store_write_applied_t> (written))
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        co_return aggregate_prepare_result_t{aggregate_prepared_t{
          {request.aggregate_id, request.aggregate_generation, request.inventory_digest}}};
    }

    task_t<aggregate_commit_result_t> commit_aggregate (aggregate_fence_t fence,
                                                        std::stop_token cancellation = {}) override
    {
        const auto row_key = key_aggregate (fence.aggregate_id);
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<aggregate_commit_result_t> ();
            auto current = co_await _store->read (row_key);
            const auto *stored = std::get_if<store_found_t> (&current);
            if (!stored)
                co_return aggregate_commit_result_t::stale;
            auto record = parse_json (stored->value.bytes);
            if (record.at (location_record_fields::aggregateGeneration).get<std::uint64_t> ()
                != fence.aggregate_generation)
                co_return aggregate_commit_result_t::stale;
            const auto status = record.value (location_record_fields::status, "");
            if (status == aggregate_status::committed)
                co_return aggregate_commit_result_t::already_committed;
            if (status != aggregate_status::prepared && status != aggregate_status::committing)
                co_return aggregate_commit_result_t::stale;
            const auto target_owner =
              decode_owner (record.at (location_record_fields::targetOwner));
            const bool resuming_commit = status == aggregate_status::committing;
            const object_creation_target_t target{
              record.at (location_record_fields::targetMeshName).get<std::string> (),
              node_rid_t::from_string (
                record.at (location_record_fields::targetNodeRid).get<std::string> ()),
              record.at (location_record_fields::targetLifecycleGeneration).get<std::uint64_t> (),
              target_owner};
            // A staged aggregate retains its rollback path, but an unconfirmed
            // target commit still requires the original live target owner lease.
            auto target_descriptor =
              co_await read_target_descriptor_async (target, !resuming_commit);
            if (!target_descriptor)
                co_return aggregate_commit_result_t::stale;
            if (!co_await owner_is_live_async (target_owner))
                co_return aggregate_commit_result_t::stale;

            const auto inventory_count =
              record.value (location_record_fields::inventoryCount, std::size_t{0});
            const auto inventory_page_count =
              record.value (location_record_fields::inventoryPageCount, std::size_t{0});
            const auto expected_inventory_page_count =
              inventory_count / aggregate_inventory::page_item_limit
              + (inventory_count % aggregate_inventory::page_item_limit != 0 ? 1 : 0);
            if (inventory_count < 2 || inventory_page_count == 0
                || expected_inventory_page_count != inventory_page_count)
                co_return aggregate_commit_result_t::stale;
            std::vector<aggregate_participant_t> participants;
            participants.reserve (inventory_count);
            for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
                const auto page =
                  co_await _store->read (key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *found = std::get_if<store_found_t> (&page);
                if (!found)
                    co_return aggregate_commit_result_t::stale;
                const auto decoded =
                  aggregate_inventory::decode_page (found->value.bytes, page_index);
                if (!decoded)
                    co_return aggregate_commit_result_t::stale;
                participants.insert (participants.end (), decoded->begin (), decoded->end ());
            }
            if (participants.size () != inventory_count)
                co_return aggregate_commit_result_t::stale;
            const auto inventory_tree = aggregate_inventory::build_tree (participants);
            if (!inventory_tree || inventory_tree->participant_count != inventory_count
                || inventory_tree->pages.size () != inventory_page_count
                || hex (inventory_tree->root)
                     != record.at (location_record_fields::inventoryRoot).get<std::string> ())
                co_return aggregate_commit_result_t::stale;
            for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
                const auto page =
                  co_await _store->read (key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *found = std::get_if<store_found_t> (&page);
                if (!found || found->value.bytes != inventory_tree->pages[page_index].encoded
                    || sha256 (found->value.bytes) != inventory_tree->pages[page_index].digest)
                    co_return aggregate_commit_result_t::stale;
            }
            const auto inventory_index_page_count =
              record.value (location_record_fields::inventoryIndexPageCount, std::size_t{0});
            const auto inventory_index_level_count =
              record.value (location_record_fields::inventoryIndexLevelCount, std::size_t{0});
            if (inventory_index_page_count != inventory_tree->index_pages.size ()
                || inventory_index_level_count != inventory_tree->index_level_count)
                co_return aggregate_commit_result_t::stale;
            for (const auto &index_page : inventory_tree->index_pages) {
                const auto index = co_await _store->read (key_aggregate_inventory_index (
                  fence.aggregate_id, index_page.level, index_page.page_index));
                const auto *found = std::get_if<store_found_t> (&index);
                const auto decoded = found ? aggregate_inventory::decode_index_page (
                                               found->value.bytes, index_page.level,
                                               index_page.page_index, index_page.child_start)
                                           : std::optional<aggregate_inventory::index_page_t>{};
                if (!found || !decoded || found->value.bytes != index_page.encoded
                    || decoded->digest != index_page.digest)
                    co_return aggregate_commit_result_t::stale;
            }
            if (fence.inventory_digest) {
                const auto stored_digest = unhex_array<32> (
                  record.at (location_record_fields::inventoryDigest).get<std::string> ());
                if (stored_digest != fence.inventory_digest->value)
                    co_return aggregate_commit_result_t::stale;
            }

            const auto participant_count = participants.size ();
            if (participant_count > max_generation)
                co_return aggregate_commit_result_t::generation_exhausted;
            std::uint64_t owner_generation_start = 0;
            if (status == aggregate_status::prepared) {
                auto owner_generations = co_await _store->read (authority_owner_counter_key);
                const auto next_owner_generation = counter_next_value (owner_generations);
                if (next_owner_generation > max_generation - participant_count)
                    co_return aggregate_commit_result_t::generation_exhausted;
                owner_generation_start = next_owner_generation;
            } else {
                owner_generation_start =
                  record.value (location_record_fields::ownerGenerationStart, std::uint64_t{0});
                if (owner_generation_start == 0
                    || record.value (location_record_fields::ownerGenerationEnd, std::uint64_t{0})
                         != owner_generation_start + participant_count - 1)
                    co_return aggregate_commit_result_t::stale;
            }

            std::vector<aggregate_commit_entry_t> entries;
            entries.reserve (participants.size ());
            std::map<std::string, stored_target_t> descriptors;
            descriptors.emplace (target_descriptor->key.value, *target_descriptor);
            std::map<std::string, stored_capacity_t> capacities;
            auto target_capacity = co_await read_capacity_async (target, &*target_descriptor);
            const auto target_capacity_key = target_capacity.key.value;
            capacities.emplace (target_capacity_key, std::move (target_capacity));

            for (std::size_t participant_index = 0; participant_index < participants.size ();
                 ++participant_index) {
                const auto &participant = participants[participant_index];
                const auto authority_key = key_authority (participant.key.value);
                auto authority = co_await _store->read (authority_key);
                const auto *found = std::get_if<store_found_t> (&authority);
                if (!found)
                    co_return aggregate_commit_result_t::stale;
                auto lock_result =
                  co_await _store->read (key_aggregate_lock (participant.key.value));
                const auto *stored_lock = std::get_if<store_found_t> (&lock_result);
                if (!stored_lock)
                    co_return aggregate_commit_result_t::stale;
                const auto lock = decode_aggregate_lock (stored_lock->value.bytes);
                if (!lock || lock->aggregate_id.value != fence.aggregate_id.value
                    || lock->aggregate_generation != fence.aggregate_generation
                    || lock->authority_key != participant.key.value
                    || lock->expected_store_version != participant.expected_store_version
                    || (lock->status != aggregate_status::prepared
                        && lock->status != aggregate_status::committing))
                    co_return aggregate_commit_result_t::stale;

                aggregate_commit_entry_t entry;
                if (lock->status == aggregate_status::committing) {
                    if (!lock->page_index || !lock->entry_index)
                        co_return aggregate_commit_result_t::stale;
                    auto page = co_await _store->read (
                      key_aggregate_commit_page (fence.aggregate_id, *lock->page_index));
                    const auto *stored_page = std::get_if<store_found_t> (&page);
                    if (!stored_page)
                        co_return aggregate_commit_result_t::stale;
                    const auto decoded_page =
                      decode_aggregate_commit_page (stored_page->value.bytes);
                    if (!decoded_page || *lock->entry_index >= decoded_page->size ())
                        co_return aggregate_commit_result_t::stale;
                    entry = (*decoded_page)[*lock->entry_index];
                    if (entry.authority_key != participant.key.value
                        || found->value.bytes != entry.after)
                        co_return aggregate_commit_result_t::stale;
                } else {
                    auto before = decode_authority (found->value.bytes, found->value.version,
                                                    found->value.store_now);
                    if (before.store_version != participant.expected_store_version
                        || participant.owner_transition
                             != authority_generation_transition_t::new_owner)
                        co_return aggregate_commit_result_t::stale;
                    auto after = before;
                    after.authority_owner_generation = owner_generation_start + participant_index;
                    after.owner = target_owner;
                    after.allocation.target = target;
                    after.payload = participant.authority_payload;
                    // `after.store_version` is never read back out of this
                    // struct (encode_authority no longer serializes it, and the
                    // committing-page replay branch above re-decodes with the
                    // live provider version once the row is actually written),
                    // so there is nothing to advance here anymore.
                    entry = {participant.key.value, found->value.bytes, encode_authority (after)};
                }

                const auto before =
                  decode_authority (entry.before, found->value.version, found->value.store_now);
                const auto source =
                  co_await read_target_descriptor_async (before.allocation.target, false, false);
                if (source) {
                    auto [source_state, inserted] =
                      descriptors.emplace (source->key.value, *source);
                    if (!inserted
                        && source_state->second.provider_version != source->provider_version)
                        co_return aggregate_commit_result_t::stale;
                }
                auto source_capacity = co_await read_capacity_async (before.allocation.target,
                                                                     source ? &*source : nullptr);
                auto [capacity_state, capacity_inserted] =
                  capacities.emplace (source_capacity.key.value, std::move (source_capacity));
                (void) capacity_inserted;
                if (!adjust_capacity (capacity_state->second.record,
                                      before.allocation.capacity_bundle, 0, -1))
                    co_return aggregate_commit_result_t::stale;
                entries.push_back (std::move (entry));
            }

            // The final CAS contains the aggregate row, every distinct source or
            // target descriptor, and the target owner lease condition. Bound that
            // set before any committing page is installed; a provider must never
            // reject the terminal CAS after authority pages have been staged.
            if (descriptors.size () + capacities.size () + 2 > aggregate_commit_final_key_limit)
                co_return aggregate_commit_result_t::stale;

            const auto commit_pages = split_aggregate_commit_entries (entries);
            if (!commit_pages)
                co_return aggregate_commit_result_t::stale;
            if (status == aggregate_status::prepared) {
                auto owner_generations = co_await _store->read (authority_owner_counter_key);
                const auto current_owner_generation = counter_next_value (owner_generations);
                if (current_owner_generation != owner_generation_start)
                    continue;
                record[location_record_fields::status] = aggregate_status::committing;
                record[location_record_fields::ownerGenerationStart] = owner_generation_start;
                record[location_record_fields::ownerGenerationEnd] =
                  owner_generation_start + participant_count - 1;
                record[location_record_fields::commitPageCount] = commit_pages->size ();
                store_write_request_t transition_request{
                  {version_condition (row_key, stored->value.version),
                   condition_for (authority_owner_counter_key, owner_generations)},
                  {store_put_t{row_key, to_bytes (record.dump ()), std::nullopt},
                   store_put_t{
                     authority_owner_counter_key,
                     to_bytes (std::to_string (current_owner_generation + participant_count)),
                     std::nullopt}}};
                auto transition = co_await write_async (std::move (transition_request));
                if (!std::holds_alternative<store_write_applied_t> (transition))
                    continue;
            }

            std::size_t page_index = 0;
            for (; page_index < commit_pages->size (); ++page_index) {
                const auto page_key = key_aggregate_commit_page (fence.aggregate_id, page_index);
                const auto encoded_page = to_bytes (
                  encode_aggregate_commit_page (page_index, (*commit_pages)[page_index]).dump ());
                auto existing_page = co_await _store->read (page_key);
                if (const auto *found_page = std::get_if<store_found_t> (&existing_page)) {
                    if (found_page->value.bytes != encoded_page)
                        co_return aggregate_commit_result_t::stale;
                    continue;
                }
                store_write_request_t page_request;
                page_request.conditions.push_back (missing_condition (page_key));
                for (std::size_t entry_index = 0; entry_index < (*commit_pages)[page_index].size ();
                     ++entry_index) {
                    const auto participant_index =
                      std::accumulate (
                        commit_pages->begin (),
                        commit_pages->begin () + static_cast<std::ptrdiff_t> (page_index),
                        std::size_t{0},
                        [] (std::size_t count, const std::vector<aggregate_commit_entry_t> &page) {
                            return count + page.size ();
                        })
                      + entry_index;
                    const auto &participant = participants[participant_index];
                    const auto &entry = (*commit_pages)[page_index][entry_index];
                    auto authority = co_await _store->read (key_authority (participant.key.value));
                    const auto *found_authority = std::get_if<store_found_t> (&authority);
                    auto lock_result =
                      co_await _store->read (key_aggregate_lock (participant.key.value));
                    const auto *found_lock = std::get_if<store_found_t> (&lock_result);
                    if (!found_authority || !found_lock)
                        co_return aggregate_commit_result_t::stale;
                    const auto lock = decode_aggregate_lock (found_lock->value.bytes);
                    if (!lock || lock->status != aggregate_status::prepared)
                        co_return aggregate_commit_result_t::stale;
                    page_request.conditions.push_back (version_condition (
                      key_authority (participant.key.value), found_authority->value.version));
                    page_request.conditions.push_back (version_condition (
                      key_aggregate_lock (participant.key.value), found_lock->value.version));
                    page_request.mutations.push_back (store_put_t{
                      key_authority (participant.key.value), entry.after, std::nullopt});
                    page_request.mutations.push_back (store_put_t{
                      key_aggregate_lock (participant.key.value),
                      to_bytes (encode_aggregate_lock (
                                  fence.aggregate_id, fence.aggregate_generation,
                                  participant.key.value, participant.expected_store_version,
                                  aggregate_status::committing, page_index, entry_index)
                                  .dump ()),
                      std::nullopt});
                }
                page_request.mutations.push_back (
                  store_put_t{page_key, encoded_page, std::nullopt});
                if (!std::holds_alternative<store_write_applied_t> (
                      co_await write_async (std::move (page_request))))
                    break;
            }
            if (page_index != commit_pages->size ())
                continue;

            // current_record must outlive its use through `stored` at the version
            // condition below; a block-local copy here would leave `stored`
            // dangling past the branch. Both former branches were identical.
            auto current_record = co_await _store->read (row_key);
            const auto *current_found = std::get_if<store_found_t> (&current_record);
            if (!current_found)
                co_return aggregate_commit_result_t::stale;
            record = parse_json (current_found->value.bytes);
            stored = current_found;
            if (record.value (location_record_fields::status, "") != aggregate_status::committing)
                co_return aggregate_commit_result_t::stale;

            auto target_capacity_state = capacities.find (target_capacity_key);
            if (target_capacity_state == capacities.end ()
                || !adjust_capacity (
                  target_capacity_state->second.record,
                  decode_bundle (record.at (location_record_fields::capacityBundle)), -1, 1))
                co_return aggregate_commit_result_t::stale;
            store_write_request_t final_request;
            final_request.conditions.push_back (version_condition (row_key, stored->value.version));
            if (target_descriptor->owner_present)
                final_request.conditions.push_back (owner_condition (target_owner));
            for (auto &[descriptor_key, descriptor] : descriptors) {
                (void) descriptor_key;
                final_request.conditions.push_back (
                  version_condition (descriptor.key, descriptor.provider_version));
            }
            for (auto &[capacity_key_value, capacity] : capacities) {
                (void) capacity_key_value;
                final_request.conditions.push_back (capacity.condition);
                final_request.mutations.push_back (store_put_t{
                  capacity.key, encode_capacity_record (capacity.record), std::nullopt});
            }
            record[location_record_fields::status] = aggregate_status::committed;
            final_request.mutations.push_back (
              store_put_t{row_key, to_bytes (record.dump ()), std::nullopt});
            auto written = co_await write_async (std::move (final_request));
            if (std::holds_alternative<store_write_applied_t> (written))
                co_return aggregate_commit_result_t::committed;
        }
    }

    task_t<aggregate_abort_result_t> abort_aggregate (aggregate_fence_t fence,
                                                      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<aggregate_abort_result_t> ();
        const auto key = key_aggregate (fence.aggregate_id);
        auto current = co_await _store->read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return aggregate_abort_result_t::stale;
        auto record = parse_json (found->value.bytes);
        if (record.at (location_record_fields::aggregateGeneration).get<std::uint64_t> ()
            != fence.aggregate_generation)
            co_return aggregate_abort_result_t::stale;
        const auto status = record.value (location_record_fields::status, "");
        if (status == aggregate_status::aborted)
            co_return aggregate_abort_result_t::already_aborted;
        if (status != aggregate_status::preparing && status != aggregate_status::prepared
            && status != aggregate_status::committing)
            co_return aggregate_abort_result_t::stale;
        const object_creation_target_t target{
          record.at (location_record_fields::targetMeshName).get<std::string> (),
          node_rid_t::from_string (
            record.at (location_record_fields::targetNodeRid).get<std::string> ()),
          record.at (location_record_fields::targetLifecycleGeneration).get<std::uint64_t> (),
          decode_owner (record.at (location_record_fields::targetOwner))};
        if (status == aggregate_status::committing) {
            const auto commit_page_count =
              record.value (location_record_fields::commitPageCount, std::size_t{0});
            if (commit_page_count == 0)
                co_return aggregate_abort_result_t::stale;
            for (std::size_t page_index = 0; page_index < commit_page_count; ++page_index) {
                auto page = co_await _store->read (
                  key_aggregate_commit_page (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    co_return aggregate_abort_result_t::stale;
                const auto entries =
                  decode_aggregate_commit_page (stored_page->value.bytes, page_index);
                if (!entries)
                    co_return aggregate_abort_result_t::stale;
                store_write_request_t rollback;
                for (std::size_t entry_index = 0; entry_index < entries->size (); ++entry_index) {
                    const auto &entry = (*entries)[entry_index];
                    auto authority = co_await _store->read (key_authority (entry.authority_key));
                    const auto *stored_authority = std::get_if<store_found_t> (&authority);
                    auto lock = co_await _store->read (key_aggregate_lock (entry.authority_key));
                    const auto *stored_lock = std::get_if<store_found_t> (&lock);
                    if (!stored_authority)
                        co_return aggregate_abort_result_t::stale;
                    if (stored_authority->value.bytes != entry.before
                        && stored_authority->value.bytes != entry.after)
                        co_return aggregate_abort_result_t::stale;
                    if (stored_lock) {
                        const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                        if (!decoded_lock
                            || decoded_lock->aggregate_id.value != fence.aggregate_id.value
                            || decoded_lock->aggregate_generation != fence.aggregate_generation
                            || decoded_lock->authority_key != entry.authority_key
                            || decoded_lock->status != aggregate_status::committing
                            || decoded_lock->page_index != page_index
                            || decoded_lock->entry_index != entry_index)
                            co_return aggregate_abort_result_t::stale;
                        rollback.conditions.push_back (version_condition (
                          key_aggregate_lock (entry.authority_key), stored_lock->value.version));
                        rollback.mutations.push_back (
                          store_delete_t{key_aggregate_lock (entry.authority_key)});
                    } else if (stored_authority->value.bytes != entry.before) {
                        co_return aggregate_abort_result_t::stale;
                    }
                    if (stored_authority->value.bytes == entry.after) {
                        rollback.conditions.push_back (version_condition (
                          key_authority (entry.authority_key), stored_authority->value.version));
                        rollback.mutations.push_back (store_put_t{
                          key_authority (entry.authority_key), entry.before, std::nullopt});
                    }
                }
                if (!rollback.mutations.empty ()) {
                    const auto rolled_back = co_await write_async (std::move (rollback));
                    if (!std::holds_alternative<store_write_applied_t> (rolled_back))
                        co_return aggregate_abort_result_t::stale;
                }
            }
        }

        const bool target_capacity_reserved = status != aggregate_status::preparing;
        std::optional<stored_target_t> target_descriptor;
        std::optional<stored_capacity_t> target_capacity;
        if (target_capacity_reserved) {
            target_descriptor = co_await read_target_descriptor_async (target, false);
            if (!target_descriptor)
                co_return aggregate_abort_result_t::stale;
            target_capacity = co_await read_capacity_async (target);
            if (!adjust_capacity (
                  target_capacity->record,
                  decode_bundle (record.at (location_record_fields::capacityBundle)), -1, 0))
                co_return aggregate_abort_result_t::stale;
        }

        const auto inventory_page_count =
          record.value (location_record_fields::inventoryPageCount, std::size_t{0});
        for (std::size_t page_index = 0; page_index < inventory_page_count; ++page_index) {
            auto page =
              co_await _store->read (key_aggregate_inventory (fence.aggregate_id, page_index));
            const auto *stored_page = std::get_if<store_found_t> (&page);
            if (!stored_page) {
                // A preparing aggregate claims its row before the first
                // child page. Missing pages after that point cannot own a
                // lock, so the durable claim can still be marked aborted
                // without inventing participant keys.
                if (status == aggregate_status::preparing)
                    break;
                co_return aggregate_abort_result_t::stale;
            }
            const auto participants =
              aggregate_inventory::decode_page (stored_page->value.bytes, page_index);
            if (!participants)
                co_return aggregate_abort_result_t::stale;
            store_write_request_t cleanup;
            for (std::size_t entry_index = 0; entry_index < participants->size (); ++entry_index) {
                const auto &participant = (*participants)[entry_index];
                const auto lock_key = key_aggregate_lock (participant.key.value);
                auto lock = co_await _store->read (lock_key);
                if (const auto *stored_lock = std::get_if<store_found_t> (&lock)) {
                    const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                    if (!decoded_lock
                        || decoded_lock->aggregate_id.value != fence.aggregate_id.value
                        || decoded_lock->aggregate_generation != fence.aggregate_generation)
                        co_return aggregate_abort_result_t::stale;
                    cleanup.conditions.push_back (
                      version_condition (lock_key, stored_lock->value.version));
                    cleanup.mutations.push_back (store_delete_t{lock_key});
                }
            }
            if (!cleanup.mutations.empty ()) {
                const auto cleaned = co_await write_async (std::move (cleanup));
                if (!std::holds_alternative<store_write_applied_t> (cleaned))
                    co_return aggregate_abort_result_t::stale;
            }
        }
        record[location_record_fields::status] = aggregate_status::aborted;
        store_write_request_t final_request;
        final_request.conditions.push_back (version_condition (key, found->value.version));
        final_request.mutations.push_back (
          store_put_t{key, to_bytes (record.dump ()), std::nullopt});
        if (target_descriptor) {
            final_request.conditions.push_back (
              version_condition (target_descriptor->key, target_descriptor->provider_version));
            if (target_descriptor->owner_present)
                final_request.conditions.push_back (owner_condition (target.owner));
            final_request.conditions.push_back (target_capacity->condition);
            final_request.mutations.push_back (
              store_put_t{target_capacity->key, encode_capacity_record (target_capacity->record),
                          std::nullopt});
        }
        auto written = co_await write_async (std::move (final_request));
        co_return std::holds_alternative<store_write_applied_t> (written)
          ? aggregate_abort_result_t::aborted
          : aggregate_abort_result_t::stale;
    }

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (aggregate_fence_t fence,
                                 std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<std::optional<std::vector<aggregate_participant_t>>> ();
        try {
            const auto current = co_await _store->read (key_aggregate (fence.aggregate_id));
            const auto *found = std::get_if<store_found_t> (&current);
            if (!found)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            const auto record = parse_json (found->value.bytes);
            if (record.at (location_record_fields::aggregateGeneration).get<std::uint64_t> ()
                != fence.aggregate_generation)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            const auto count =
              record.at (location_record_fields::inventoryCount).get<std::size_t> ();
            const auto page_count =
              record.at (location_record_fields::inventoryPageCount).get<std::size_t> ();
            const auto expected_page_count =
              count / aggregate_inventory::page_item_limit
              + (count % aggregate_inventory::page_item_limit != 0 ? 1 : 0);
            if (count < 2 || page_count == 0 || expected_page_count != page_count)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            std::vector<aggregate_participant_t> participants;
            participants.reserve (count);
            for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
                const auto page =
                  co_await _store->read (key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    co_return std::optional<std::vector<aggregate_participant_t>>{};
                const auto decoded =
                  aggregate_inventory::decode_page (stored_page->value.bytes, page_index);
                if (!decoded)
                    co_return std::optional<std::vector<aggregate_participant_t>>{};
                participants.insert (participants.end (), decoded->begin (), decoded->end ());
            }
            if (participants.size () != count)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            const auto tree = aggregate_inventory::build_tree (participants);
            if (!tree || tree->participant_count != count || tree->pages.size () != page_count
                || hex (tree->root)
                     != record.at (location_record_fields::inventoryRoot).get<std::string> ())
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
                const auto page =
                  co_await _store->read (key_aggregate_inventory (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page || stored_page->value.bytes != tree->pages[page_index].encoded
                    || sha256 (stored_page->value.bytes) != tree->pages[page_index].digest)
                    co_return std::optional<std::vector<aggregate_participant_t>>{};
            }
            const auto index_page_count =
              record.value (location_record_fields::inventoryIndexPageCount, std::size_t{0});
            const auto index_level_count =
              record.value (location_record_fields::inventoryIndexLevelCount, std::size_t{0});
            if (index_page_count != tree->index_pages.size ()
                || index_level_count != tree->index_level_count)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            for (const auto &index_page : tree->index_pages) {
                const auto index = co_await _store->read (key_aggregate_inventory_index (
                  fence.aggregate_id, index_page.level, index_page.page_index));
                const auto *stored_index = std::get_if<store_found_t> (&index);
                const auto decoded = stored_index
                                       ? aggregate_inventory::decode_index_page (
                                           stored_index->value.bytes, index_page.level,
                                           index_page.page_index, index_page.child_start)
                                       : std::optional<aggregate_inventory::index_page_t>{};
                if (!stored_index || !decoded || stored_index->value.bytes != index_page.encoded
                    || decoded->digest != index_page.digest)
                    co_return std::optional<std::vector<aggregate_participant_t>>{};
            }
            if (fence.inventory_digest
                && unhex_array<32> (
                     record.at (location_record_fields::inventoryDigest).get<std::string> ())
                     != fence.inventory_digest->value)
                co_return std::optional<std::vector<aggregate_participant_t>>{};
            co_return std::optional<std::vector<aggregate_participant_t>>{std::move (participants)};
        }
        catch (...) {
            co_return std::optional<std::vector<aggregate_participant_t>>{};
        }
    }

    task_t<std::int64_t> remove_all_by_owner (location_owner_token_t owner) override
    {
        std::int64_t removed = 0;
        removed += co_await remove_owned (std::string ("mesh-node") + '\0', owner);
        removed += co_await remove_owned (std::string ("client-server") + '\0', owner);
        removed += co_await remove_owned (std::string ("fanout-publisher") + '\0', owner);
        co_return removed;
    }

  private:
    using json_t = nlohmann::json;
    static constexpr std::string_view prefix = "zlink:v11:";
    static constexpr std::uint64_t max_generation =
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    inline static const store_key_t counter_key{std::string (prefix) + "owner-counter"};
    // Spec 22 §7's Store-wide, next-to-issue counters.  These are canonical
    // cross-language rows: bare decimal bytes, not canonical JSON records.
    inline static const store_key_t object_counter_key{std::string (prefix) + "object-counter"};
    inline static const store_key_t authority_owner_counter_key{std::string (prefix)
                                                                + "authority-owner-counter"};

    struct stored_target_t
    {
        store_key_t key;
        std::string provider_version;
        bool owner_present;
        mesh_node_descriptor_t descriptor;
        json_t record;
    };

    enum class capacity_record_format_t
    {
        canonical,
        node_compatible
    };

    struct capacity_count_t
    {
        std::int64_t active = 0;
        std::int64_t pending = 0;
    };

    struct capacity_record_t
    {
        capacity_count_t actors;
        capacity_count_t spots;
        std::map<std::string, capacity_count_t> spot_types;
        capacity_record_format_t format = capacity_record_format_t::canonical;
    };

    struct stored_capacity_t
    {
        store_key_t key;
        store_condition_t condition;
        capacity_record_t record;
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
    static constexpr std::size_t aggregate_commit_page_byte_limit =
      aggregate_inventory::page_byte_limit;
    static constexpr std::size_t aggregate_commit_fixed_key_count = 3;
    static constexpr std::size_t aggregate_commit_final_key_limit = 2048;

    static json_t
    encode_aggregate_commit_page (std::size_t page_index,
                                  const std::vector<aggregate_commit_entry_t> &entries)
    {
        json_t encoded_entries = json_t::array ();
        for (const auto &entry : entries)
            encoded_entries.push_back ({{location_record_fields::authorityKey, entry.authority_key},
                                        {location_record_fields::before, hex (entry.before)},
                                        {location_record_fields::after, hex (entry.after)}});
        return {{location_record_fields::version, 1},
                {location_record_fields::pageIndex, page_index},
                {location_record_fields::entries, std::move (encoded_entries)}};
    }

    static std::optional<std::vector<aggregate_commit_entry_t>>
    decode_aggregate_commit_page (const std::vector<std::byte> &bytes,
                                  std::optional<std::size_t> expected_page_index = std::nullopt)
    {
        if (bytes.empty () || bytes.size () > aggregate_commit_page_byte_limit)
            return std::nullopt;
        try {
            const auto record = parse_json (bytes);
            if (record.value (location_record_fields::version, 0) != 1
                || !record.at (location_record_fields::pageIndex).is_number_unsigned ()
                || (expected_page_index
                    && record.at (location_record_fields::pageIndex).get<std::size_t> ()
                         != *expected_page_index)
                || !record.at (location_record_fields::entries).is_array ()
                || record.at (location_record_fields::entries).empty ()
                || record.at (location_record_fields::entries).size ()
                     > aggregate_commit_page_item_limit)
                return std::nullopt;
            std::vector<aggregate_commit_entry_t> result;
            result.reserve (record.at (location_record_fields::entries).size ());
            for (const auto &entry : record.at (location_record_fields::entries))
                result.push_back (
                  {entry.at (location_record_fields::authorityKey).get<std::string> (),
                   unhex (entry.at (location_record_fields::before).get<std::string> ()),
                   unhex (entry.at (location_record_fields::after).get<std::string> ())});
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
        for (std::size_t offset = 0; offset < entries.size ();) {
            const auto first = entries.begin () + static_cast<std::ptrdiff_t> (offset);
            std::vector<aggregate_commit_entry_t> current;
            const auto count = bounded_page_prefix (
              std::span<const aggregate_commit_entry_t> (entries).subspan (offset),
              aggregate_commit_page_item_limit, aggregate_commit_page_byte_limit,
              [] (const aggregate_commit_entry_t &entry) {
                  return std::array{entry.authority_key.size (), entry.before.size (),
                                    entry.after.size ()};
              },
              [&] (std::size_t candidate_count) {
                  current.assign (first, first + static_cast<std::ptrdiff_t> (candidate_count));
                  return encode_aggregate_commit_page (pages.size (), current).dump ().size ()
                         <= aggregate_commit_page_byte_limit;
              });
            if (!count)
                return std::nullopt;
            current.resize (*count);
            offset += *count;
            pages.push_back (std::move (current));
        }
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
        return {
          {location_record_fields::aggregateId, hex (aggregate_id.value)},
          {location_record_fields::aggregateGeneration, aggregate_generation},
          {location_record_fields::authorityKey, authority_key},
          {location_record_fields::expectedStoreVersion, expected_store_version},
          {location_record_fields::status, status},
          {location_record_fields::pageIndex, page_index ? json_t (*page_index) : json_t (nullptr)},
          {location_record_fields::entryIndex,
           entry_index ? json_t (*entry_index) : json_t (nullptr)}};
    }

    static std::optional<aggregate_lock_t>
    decode_aggregate_lock (const std::vector<std::byte> &bytes)
    {
        try {
            const auto record = parse_json (bytes);
            aggregate_lock_t result;
            result.aggregate_id.value =
              unhex_array<16> (record.at (location_record_fields::aggregateId).get<std::string> ());
            result.aggregate_generation =
              record.at (location_record_fields::aggregateGeneration).get<std::uint64_t> ();
            result.authority_key =
              record.at (location_record_fields::authorityKey).get<std::string> ();
            result.expected_store_version =
              record.at (location_record_fields::expectedStoreVersion).get<std::string> ();
            result.status = record.at (location_record_fields::status).get<std::string> ();
            if (!record.at (location_record_fields::pageIndex).is_null ())
                result.page_index =
                  record.at (location_record_fields::pageIndex).get<std::size_t> ();
            if (!record.at (location_record_fields::entryIndex).is_null ())
                result.entry_index =
                  record.at (location_record_fields::entryIndex).get<std::size_t> ();
            if (result.aggregate_generation == 0 || result.authority_key.empty ()
                || (result.status != aggregate_status::prepared
                    && result.status != aggregate_status::committing
                    && result.status != aggregate_status::committed))
                return std::nullopt;
            return result;
        }
        catch (...) {
            return std::nullopt;
        }
    }

    task_t<std::optional<aggregate_lock_t>> read_aggregate_lock_async (std::string authority_key)
    {
        auto value = co_await _store->read (key_aggregate_lock (authority_key));
        const auto *found = std::get_if<store_found_t> (&value);
        if (!found)
            co_return std::nullopt;
        auto lock = decode_aggregate_lock (found->value.bytes);
        if (!lock || lock->authority_key != authority_key)
            co_return std::nullopt;
        co_return lock;
    }

    task_t<std::optional<authority_snapshot_t>>
    effective_authority_async (std::string authority_key,
                               std::vector<std::byte> raw_bytes,
                               store_version_t raw_version,
                               std::chrono::system_clock::time_point store_now)
    {
        auto raw = decode_authority (raw_bytes, raw_version, store_now);
        const auto lock = co_await read_aggregate_lock_async (authority_key);
        if (!lock || lock->status != aggregate_status::committing)
            co_return raw;
        auto aggregate = co_await _store->read (key_aggregate (lock->aggregate_id));
        if (const auto *aggregate_found = std::get_if<store_found_t> (&aggregate)) {
            const auto aggregate_record = parse_json (aggregate_found->value.bytes);
            if (aggregate_record.value (location_record_fields::status, "")
                == aggregate_status::committed)
                co_return raw;
        }
        if (!lock->page_index || !lock->entry_index)
            co_return raw;
        auto page =
          co_await _store->read (key_aggregate_commit_page (lock->aggregate_id, *lock->page_index));
        const auto *found = std::get_if<store_found_t> (&page);
        if (!found)
            co_return std::nullopt;
        const auto entries = decode_aggregate_commit_page (found->value.bytes);
        if (!entries || *lock->entry_index >= entries->size ())
            co_return std::nullopt;
        const auto &entry = (*entries)[*lock->entry_index];
        if (entry.authority_key != authority_key)
            co_return std::nullopt;
        co_return decode_authority (entry.before, raw_version, store_now);
    }

    task_t<bool> authority_mutation_locked_async (std::string authority_key)
    {
        const auto lock = co_await read_aggregate_lock_async (authority_key);
        if (!lock
            || (lock->status != aggregate_status::prepared
                && lock->status != aggregate_status::committing))
            co_return false;
        if (lock->status == aggregate_status::committing) {
            auto aggregate = co_await _store->read (key_aggregate (lock->aggregate_id));
            if (const auto *found = std::get_if<store_found_t> (&aggregate)) {
                const auto record = parse_json (found->value.bytes);
                if (record.value (location_record_fields::status, "") == aggregate_status::committed
                    || record.value (location_record_fields::status, "")
                         == aggregate_status::aborted)
                    co_return false;
            }
        }
        co_return true;
    }

    task_t<store_write_result_t>
    write_async (store_write_request_t request,
                 const creation_terminal_record_t *terminal = nullptr,
                 std::chrono::system_clock::time_point terminal_store_now = {})
    {
        if (terminal) {
            const auto terminal_retention = std::chrono::ceil<std::chrono::milliseconds> (
              terminal->expires_at - terminal_store_now);
            if (terminal_retention <= std::chrono::milliseconds::zero ())
                throw std::invalid_argument (
                  "creation terminal retention elapsed before publication");
            const auto terminal_key = key_creation_terminal (terminal->operation);
            request.conditions.push_back (missing_condition (terminal_key));
            request.mutations.push_back (
              store_put_t{terminal_key, terminal->terminal_envelope, terminal_retention});
        }
        auto first = co_await await_result (_store->write (request));
        if (first)
            co_return first.value ();
        if (auto applied = co_await reconcile_write_async (request))
            co_return store_write_result_t{std::move (*applied)};
        if (first.error () != nullptr && detail::is_transient_error (first.error ()->kind ())) {
            auto retried = co_await await_result (_store->write (request));
            if (retried)
                co_return retried.value ();
            if (auto applied = co_await reconcile_write_async (request))
                co_return store_write_result_t{std::move (*applied)};
            co_return retried.value ();
        }
        co_return first.value ();
    }

    task_t<std::optional<store_write_applied_t>>
    reconcile_write_async (const store_write_request_t &request)
    {
        if (request.mutations.empty ())
            co_return std::nullopt;
        store_write_applied_t applied;
        for (const auto &mutation : request.mutations) {
            const auto key = std::visit ([] (const auto &value) { return value.key; }, mutation);
            auto observed = co_await await_result (_store->read (key));
            if (!observed)
                co_return std::nullopt;
            const auto &read = observed.value ();
            if (const auto *put = std::get_if<store_put_t> (&mutation)) {
                const auto *found = std::get_if<store_found_t> (&read);
                if (found == nullptr || found->value.bytes != put->bytes
                    || static_cast<bool> (found->value.expires_at)
                         != static_cast<bool> (put->retention))
                    co_return std::nullopt;
                if (put->retention && *found->value.expires_at <= found->value.store_now)
                    co_return std::nullopt;
                applied.put_versions.push_back ({key, found->value.version});
                applied.store_now = std::max (applied.store_now, found->value.store_now);
            } else {
                const auto *missing = std::get_if<store_missing_t> (&read);
                if (missing == nullptr)
                    co_return std::nullopt;
                applied.store_now = std::max (applied.store_now, missing->store_now);
            }
        }
        co_return applied;
    }

    static store_condition_t missing_condition (store_key_t key)
    {
        return store_missing_condition_t{std::move (key)};
    }

    static store_condition_t version_condition (store_key_t key, store_version_t version)
    {
        return store_version_condition_t{std::move (key), std::move (version)};
    }

    static store_condition_t version_condition (store_key_t key, std::string version)
    {
        return version_condition (std::move (key), store_version_t{std::move (version)});
    }

    static store_condition_t value_condition (store_key_t key, std::vector<std::byte> expected)
    {
        return store_value_condition_t{std::move (key), std::move (expected)};
    }

    // Value conditions compare these bytes with the lease other languages
    // wrote, so the field order is the golden's (store-record-v1.json).
    static std::vector<std::byte> owner_lease_bytes (const location_owner_token_t &owner)
    {
        const nlohmann::ordered_json record{
          {location_record_fields::recordVersion, 1},
          {location_record_fields::ownerId, owner.owner_id},
          {location_record_fields::leaseGeneration, std::to_string (owner.lease_generation)}};
        return to_bytes (record.dump ());
    }

    static store_condition_t owner_condition (const location_owner_token_t &owner)
    {
        return value_condition (key_owner (owner.owner_id), owner_lease_bytes (owner));
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

    // 21-location-runtime.md §2.4: the framework checks `recordVersion` on
    // every canonical opaque record and fails explicitly on an unrecognized
    // value instead of guessing how to read the record. A MISSING
    // recordVersion is just as unrecognized as a wrong one (fail-closed --
    // java is strict; dotnet went strict in f2dfa809e8). Every writer of
    // the five canonical rows (MeshNode, owner lease, ClientServer, fanout
    // publisher, authority) provably always emits the field, so this can
    // never fire on a record this repository wrote. Provider-private rows
    // (reservation, aggregate, terminal, counters, ...) are outside the
    // canonical contract and are not funneled through here.
    static void require_record_version (const json_t &record, const char *record_name)
    {
        if (!record.contains (location_record_fields::recordVersion)
            || !record.at (location_record_fields::recordVersion).is_number_integer ()
            || record.at (location_record_fields::recordVersion).get<std::int64_t> () != 1) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         std::string ("unrecognized ") + record_name
                                           + " recordVersion");
        }
    }

    static json_t parse_canonical_record (const std::vector<std::byte> &value,
                                          const char *record_name)
    {
        auto record = parse_json (value);
        require_record_version (record, record_name);
        return record;
    }

    static std::int64_t parse_i64 (const std::vector<std::byte> &value)
    {
        const auto decimal = to_string (value);
        if (decimal.empty () || decimal.front () == '0'
            || !std::all_of (decimal.begin (), decimal.end (), [] (unsigned char character) {
                   return character >= '0' && character <= '9';
               }))
            throw std::invalid_argument ("counter is not canonical decimal");
        const auto parsed = std::stoll (decimal);
        if (parsed <= 0)
            throw std::invalid_argument ("counter is outside its valid range");
        return parsed;
    }

    // Counter rows deliberately bypass parse_canonical_record(): §7 assigns
    // them a bare-decimal representation with no recordVersion envelope.
    static std::uint64_t counter_next_value (const store_read_result_t &counter)
    {
        if (const auto *found = std::get_if<store_found_t> (&counter))
            return static_cast<std::uint64_t> (parse_i64 (found->value.bytes));
        return 1;
    }

    static std::int64_t owner_generation (const std::vector<std::byte> &value)
    {
        return parse_i64_field (parse_canonical_record (value, "owner lease")
                                  .at (location_record_fields::leaseGeneration));
    }

    static owner_lease_found_t decode_owner_lease (const store_found_t &found)
    {
        if (!found.value.expires_at)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Location Store owner lease record is invalid");
        const auto record = parse_canonical_record (found.value.bytes, "owner lease");
        return {{record.at (location_record_fields::ownerId).get<std::string> (),
                 parse_i64_field (record.at (location_record_fields::leaseGeneration))},
                *found.value.expires_at,
                found.value.store_now};
    }

    static std::string segment (std::string_view value)
    {
        return std::to_string (value.size ()) + ":" + std::string (value) + ":";
    }

    // NUL-delimited logical-key preimages, 21-location-runtime.md#3.4. These
    // preimages -- not the internal "zlink:v11:..." scheme below -- are the
    // cross-language public contract for the opaque records (MeshNode, owner
    // lease, ClientServer, fanout publisher, authority, creation terminal).
    // The provider hashes whatever store_key_t it receives with SHA-256, so
    // provider-private keys (reservation, aggregate, lock, ...)
    // keep the existing "zlink:v11:" scheme unchanged -- only these records
    // need a byte-exact, cross-language preimage.
    static std::string preimage (std::initializer_list<std::string_view> segments)
    {
        std::string result;
        std::size_t size = segments.size () == 0 ? 0 : segments.size () - 1;
        for (const auto segment : segments)
            size += segment.size ();
        result.reserve (size);
        bool first = true;
        for (const auto segment : segments) {
            if (!first)
                result.push_back ('\0');
            result.append (segment);
            first = false;
        }
        return result;
    }

    static store_key_t key_owner (std::string_view owner_id)
    {
        return {preimage ({"owner-lease", owner_id})};
    }

    static constexpr std::string_view authority_domain = "authority";

    static std::string prefix_authority () { return preimage ({authority_domain, ""}); }

    static store_key_t key_authority (std::string_view value)
    {
        const auto decoded = authority_key_codec_detail::decode_authority_key (value);
        if (!decoded)
            throw std::invalid_argument ("authority store key requires a valid identity");
        return {preimage (
          {authority_domain, decoded->kind == 'a' ? "actor" : "spot", decoded->object_id})};
    }

    static store_key_t key_creation_terminal (const creation_operation_identity_t &operation)
    {
        const auto fixed_hex = [] (std::uint64_t value) {
            std::string result;
            result.reserve (16);
            for (int shift = 60; shift >= 0; shift -= 4)
                result.push_back (
                  zlink::framework::detail::lowercase_hex_digits[(value >> shift) & 0x0f]);
            return result;
        };
        const auto source_rid = hex (to_bytes (operation.source_node_rid.value ()));
        const auto source_generation = std::to_string (operation.source_node_generation);
        const auto operation_id =
          fixed_hex (operation.operation_id.high) + fixed_hex (operation.operation_id.low);
        return {preimage ({"creation-terminal", source_rid, source_generation, operation_id})};
    }

    static store_key_t key_aggregate (const aggregate_id_t &id)
    {
        return {std::string (prefix) + "aggregate:" + hex (id.value)};
    }

    static store_key_t key_aggregate_inventory (const aggregate_id_t &id, std::size_t page_index)
    {
        return {std::string (prefix) + "aggregate-inventory:" + hex (id.value) + ":"
                + std::to_string (page_index)};
    }

    static store_key_t key_aggregate_inventory_index (const aggregate_id_t &id,
                                                      std::size_t level,
                                                      std::size_t page_index)
    {
        return {std::string (prefix) + "aggregate-inventory-index:" + hex (id.value) + ":"
                + std::to_string (level) + ":" + std::to_string (page_index)};
    }

    static store_key_t key_aggregate_commit_page (const aggregate_id_t &id, std::size_t page_index)
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
        return (key.kind == placement_object_kind_t::actor ? actor_authority_key (key.global_id)
                                                           : spot_authority_key (key.global_id))
          .value;
    }

    template <std::size_t N> static std::string hex (const std::array<std::byte, N> &value)
    {
        return zlink::framework::detail::encode_hex (std::span<const std::byte> (value));
    }

    static std::string hex (const std::vector<std::byte> &value)
    {
        return zlink::framework::detail::encode_hex (std::span<const std::byte> (value));
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
        return preimage ({"mesh-node", mesh_name, ""});
    }

    static store_key_t key_mesh (std::string_view mesh_name, const zlink::routing_id_t &rid)
    {
        const auto rid_hex = rid.to_hex ();
        return {preimage ({"mesh-node", mesh_name, rid_hex})};
    }

    static store_key_t key_capacity (std::string_view mesh_name,
                                     const zlink::routing_id_t &rid,
                                     std::uint64_t lifecycle_generation)
    {
        return {std::string (prefix) + "capacity:" + segment (mesh_name) + segment (rid.to_hex ())
                + std::to_string (lifecycle_generation)};
    }

    static std::string encode_uri_component (std::string_view value)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string result;
        result.reserve (value.size ());
        for (const auto character : value) {
            const auto byte = static_cast<unsigned char> (character);
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.'
                || byte == '!' || byte == '~' || byte == '*' || byte == '\'' || byte == '('
                || byte == ')') {
                result.push_back (static_cast<char> (byte));
            } else {
                result.push_back ('%');
                result.push_back (digits[byte >> 4]);
                result.push_back (digits[byte & 0x0f]);
            }
        }
        return result;
    }

    static store_key_t key_node_capacity (std::string_view mesh_name, std::string_view node_rid)
    {
        return {std::string (prefix) + "capacity:" + encode_uri_component (mesh_name) + ":"
                + encode_uri_component (node_rid)};
    }

    static std::string prefix_client_server (std::string_view channel_name)
    {
        return preimage ({"client-server", channel_name, ""});
    }

    static store_key_t key_client_server (std::string_view channel_name,
                                          const zlink::routing_id_t &rid)
    {
        const auto rid_hex = rid.to_hex ();
        return {preimage ({"client-server", channel_name, rid_hex})};
    }

    static std::string prefix_fanout (std::string_view channel_name)
    {
        return preimage ({"fanout-publisher", channel_name, ""});
    }

    static store_key_t key_fanout (std::string_view channel_name, const zlink::routing_id_t &rid)
    {
        const auto rid_hex = rid.to_hex ();
        return {preimage ({"fanout-publisher", channel_name, rid_hex})};
    }

    task_t<location_write_result_t>
    update_descriptor (store_key_t row_key,
                       std::string owner_id,
                       std::int64_t lease_generation,
                       std::uint64_t lifecycle_generation,
                       std::uint64_t descriptor_revision,
                       json_t record,
                       location_write_intent_t intent,
                       std::function<bool (const json_t &)> immutable_fields_equal)
    {
        const auto lease_key = key_owner (owner_id);
        auto lease = co_await _store->read (lease_key);
        const auto *live_lease = std::get_if<store_found_t> (&lease);
        if (!live_lease || owner_generation (live_lease->value.bytes) != lease_generation)
            co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};

        auto current = co_await _store->read (row_key);
        // The write-generation counter is provider-private bookkeeping (it
        // only gates exhaustion on non-renew intents; §2.4's canonical
        // record shape has no room for it) reparented onto the provider's
        // own opaque per-key version -- the same trick checklist C-4d
        // applies to the authority store's store_version -- instead of a
        // value this store used to persist inside the record body as
        // "generation". `generation` here is therefore no longer 1:1 with
        // any prior call's return value; it is a best-effort exhaustion
        // bound, skipped when the provider version isn't a plain integer.
        std::uint64_t generation = 1;
        store_condition_t row_condition;
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto stored =
              parse_canonical_record (found->value.bytes, location_record_fields::descriptor);
            std::uint64_t provider_generation = 0;
            bool provider_generation_known = false;
            try {
                provider_generation = std::stoull (found->value.version.value);
                provider_generation_known = true;
            }
            catch (...) {
            }
            const auto stored_owner = record_owner_id (stored);
            const auto stored_lease = record_lease_generation (stored);
            const auto previous_owner = co_await _store->read (key_owner (stored_owner));
            const auto previous_owner_live = std::holds_alternative<store_found_t> (previous_owner);
            if (intent == location_write_intent_t::new_claim && previous_owner_live)
                co_return location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}};
            if (intent == location_write_intent_t::takeover && previous_owner_live)
                co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
            if (intent == location_write_intent_t::renew) {
                if (stored_owner != owner_id || stored_lease != lease_generation
                    || record_lifecycle_generation (stored) != lifecycle_generation
                    || descriptor_revision <= record_descriptor_revision (stored)
                    || !immutable_fields_equal (stored))
                    co_return location_write_result_t{
                      location_write_status_t::ignored_stale, 0, {}};
            } else if (provider_generation_known
                       && provider_generation == std::numeric_limits<std::uint64_t>::max ()) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "descriptor generation exhausted");
            }
            generation = provider_generation_known ? provider_generation + 1 : 1;
            row_condition = version_condition (row_key, found->value.version);
        } else {
            if (intent == location_write_intent_t::renew)
                co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
            row_condition = missing_condition (row_key);
        }
        auto encoded = to_bytes (record.dump ());
        store_write_request_t write_request{
          {owner_condition ({owner_id, lease_generation}), std::move (row_condition)},
          {store_put_t{row_key, std::move (encoded), std::nullopt}}};
        auto result = co_await write_async (std::move (write_request));
        if (const auto *applied = std::get_if<store_write_applied_t> (&result))
            co_return location_write_result_t::stored (static_cast<std::int64_t> (generation),
                                                       applied->store_now);
        co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
    }

    task_t<location_write_status_t> remove_descriptor (store_key_t row_key,
                                                       location_owner_token_t owner)
    {
        auto current = co_await _store->read (row_key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return location_write_status_t::ignored_stale;
        const auto record =
          parse_canonical_record (found->value.bytes, location_record_fields::descriptor);
        if (record_owner_id (record) != owner.owner_id
            || record_lease_generation (record) != owner.lease_generation)
            co_return location_write_status_t::ignored_stale;
        store_write_request_t result_request{{version_condition (row_key, found->value.version)},
                                             {store_delete_t{row_key}}};
        auto result = co_await write_async (std::move (result_request));
        co_return std::holds_alternative<store_write_applied_t> (result)
          ? location_write_status_t::stored
          : location_write_status_t::ignored_stale;
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
        auto result = co_await _store->scan (std::move (request));
        const auto *found = std::get_if<store_scan_page_t> (&result);
        if (!found)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Location Store scan cursor expired");
        location_page_t<T> output;
        output.items.reserve (found->items.size ());
        for (const auto &item : found->items)
            output.items.push_back (decode (
              parse_canonical_record (item.value.bytes, location_record_fields::descriptor)));
        if (found->next_cursor)
            output.continuation_token = found->next_cursor->value;
        co_return std::move (output);
    }

    task_t<std::int64_t> remove_owned (std::string row_prefix, location_owner_token_t owner)
    {
        std::int64_t removed = 0;
        std::optional<store_scan_cursor_t> cursor;
        do {
            store_scan_request_t scan_request{row_prefix, cursor, location_page_item_limit};
            auto result = co_await _store->scan (std::move (scan_request));
            const auto *page = std::get_if<store_scan_page_t> (&result);
            if (!page)
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Location Store scan cursor expired");
            for (const auto &item : page->items) {
                const auto record =
                  parse_canonical_record (item.value.bytes, location_record_fields::descriptor);
                if (record_owner_id (record) != owner.owner_id
                    || record_lease_generation (record) != owner.lease_generation)
                    continue;
                store_write_request_t delete_request{
                  {version_condition (item.key, item.value.version)}, {store_delete_t{item.key}}};
                auto written = co_await write_async (std::move (delete_request));
                if (std::holds_alternative<store_write_applied_t> (written))
                    ++removed;
            }
            cursor = page->next_cursor;
        } while (cursor);
        co_return removed;
    }

    // §2.4 generation fields are JSON strings; the descriptor sub-object's
    // own copies of these fields (kept for `same_mesh_immutable`/decode
    // round-tripping) still use plain JSON numbers, so this accepts both.
    static std::int64_t parse_i64_field (const json_t &value)
    {
        return value.is_string () ? std::stoll (value.get<std::string> ())
                                  : value.get<std::int64_t> ();
    }

    static std::uint64_t parse_u64_field (const json_t &value)
    {
        return value.is_string () ? std::stoull (value.get<std::string> ())
                                  : value.get<std::uint64_t> ();
    }

    static std::string record_owner_id (const json_t &record)
    {
        if (record.contains (location_record_fields::ownerId))
            return record.at (location_record_fields::ownerId).get<std::string> ();
        return record.at (location_record_fields::descriptor)
          .at (location_record_fields::ownerId)
          .get<std::string> ();
    }

    static std::int64_t record_lease_generation (const json_t &record)
    {
        if (record.contains (location_record_fields::leaseGeneration))
            return parse_i64_field (record.at (location_record_fields::leaseGeneration));
        return parse_i64_field (record.at (location_record_fields::descriptor)
                                  .at (location_record_fields::leaseGeneration));
    }

    static std::uint64_t record_lifecycle_generation (const json_t &record)
    {
        if (record.contains (location_record_fields::lifecycleGeneration))
            return parse_u64_field (record.at (location_record_fields::lifecycleGeneration));
        return parse_u64_field (record.at (location_record_fields::descriptor)
                                  .at (location_record_fields::lifecycleGeneration));
    }

    static std::uint64_t record_descriptor_revision (const json_t &record)
    {
        if (record.contains (location_record_fields::descriptorRevision))
            return parse_u64_field (record.at (location_record_fields::descriptorRevision));
        return parse_u64_field (record.at (location_record_fields::descriptor)
                                  .at (location_record_fields::descriptorRevision));
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
        return {{location_record_fields::ownerId, value.owner_id},
                {location_record_fields::leaseGeneration, value.lease_generation}};
    }

    static location_owner_token_t decode_owner (const json_t &value)
    {
        return {value.at (location_record_fields::ownerId).get<std::string> (),
                parse_i64_field (value.at (location_record_fields::leaseGeneration))};
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

    task_t<bool> owner_is_live_async (location_owner_token_t owner)
    {
        co_return (co_await read_live_owner_async (std::move (owner))).has_value ();
    }

    task_t<std::optional<store_found_t>> read_live_owner_async (location_owner_token_t owner)
    {
        auto current = co_await _store->read (key_owner (owner.owner_id));
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return std::nullopt;
        const auto lease = decode_owner_lease (*found);
        if (lease.token.owner_id != owner.owner_id)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Location Store owner lease record is invalid");
        if (lease.token.lease_generation != owner.lease_generation
            || lease.lease_expires_at <= lease.store_now)
            co_return std::nullopt;
        co_return *found;
    }

    static json_t encode_target (const object_creation_target_t &value)
    {
        // nodeGeneration is the 21-location-runtime.md#2.4 canonical alias
        // for nodeLifecycleGeneration (kept for decode_target/same_target).
        return {{location_record_fields::meshName, value.mesh_name},
                {location_record_fields::nodeRid, value.node_rid.value ()},
                {location_record_fields::nodeLifecycleGeneration, value.node_lifecycle_generation},
                {location_record_fields::nodeGeneration,
                 generation_string (value.node_lifecycle_generation)},
                {location_record_fields::owner, encode_owner (value.owner)}};
    }

    static object_creation_target_t decode_target (const json_t &value)
    {
        return {
          value.at (location_record_fields::meshName).get<std::string> (),
          node_rid_t::from_string (value.at (location_record_fields::nodeRid).get<std::string> ()),
          value.contains (location_record_fields::nodeLifecycleGeneration)
            ? value.at (location_record_fields::nodeLifecycleGeneration).get<std::uint64_t> ()
            : parse_u64_field (value.at (location_record_fields::nodeGeneration)),
          decode_owner (value.at (location_record_fields::owner))};
    }

    // 21-location-runtime.md#2.4's canonical objectKind spelling for the
    // three-way placement kind (actor|userSpot|instanceSpot), shared by
    // encode_bundle's spotType and encode_allocation below.
    template <typename T, std::size_t N>
    static const char *
    store_enum_name (T value, const std::array<std::pair<T, const char *>, N> &names, T fallback)
    {
        for (const auto &[kind, name] : names)
            if (kind == value)
                return name;
        return std::find_if (names.begin (), names.end (),
                             [fallback] (const auto &entry) { return entry.first == fallback; })
          ->second;
    }

    template <typename T, std::size_t N>
    static T parse_store_enum (std::string_view value,
                               const std::array<std::pair<T, const char *>, N> &names,
                               T fallback)
    {
        for (const auto &[kind, name] : names)
            if (value == name)
                return kind;
        return fallback;
    }

    static inline constexpr std::array<std::pair<placement_object_kind_t, const char *>, 3>
      object_kind_names{{
        {placement_object_kind_t::actor, "actor"},
        {placement_object_kind_t::instance_spot, "instanceSpot"},
        {placement_object_kind_t::user_spot, "userSpot"},
      }};

    static const char *object_kind3_name (placement_object_kind_t value)
    {
        return store_enum_name (value, object_kind_names, placement_object_kind_t::user_spot);
    }

    static placement_object_kind_t parse_object_kind3 (const std::string &value)
    {
        return parse_store_enum (value, object_kind_names, placement_object_kind_t::user_spot);
    }

    // `actorSlots`/`spotSlots`/`spotType{objectKind:int,...,slots}`).
    static json_t encode_bundle (const placement_capacity_bundle_t &value)
    {
        json_t spot_type = nullptr;
        if (value.spot_type)
            spot_type = {{location_record_fields::objectKind,
                          object_kind3_name (value.spot_type->object_kind)},
                         {location_record_fields::stableType, value.spot_type->stable_type},
                         {location_record_fields::count, value.spot_type->slots}};
        return {{location_record_fields::actors, value.actor_slots},
                {location_record_fields::spots, value.spot_slots},
                {location_record_fields::spotType, std::move (spot_type)}};
    }

    static placement_capacity_bundle_t decode_bundle (const json_t &value)
    {
        placement_capacity_bundle_t result;
        result.actor_slots = value.at (location_record_fields::actors).get<std::uint32_t> ();
        result.spot_slots = value.at (location_record_fields::spots).get<std::uint32_t> ();
        if (value.contains (location_record_fields::spotType)
            && !value.at (location_record_fields::spotType).is_null ()) {
            const auto &spot = value.at (location_record_fields::spotType);
            result.spot_type = spot_type_capacity_delta_t{
              parse_object_kind3 (spot.at (location_record_fields::objectKind).get<std::string> ()),
              spot.at (location_record_fields::stableType).get<std::string> (),
              static_cast<std::uint32_t> (
                spot.at (location_record_fields::count).get<std::uint64_t> ())};
        }
        return result;
    }

    static inline constexpr std::array<std::pair<placement_allocation_state_t, const char *>, 2>
      allocation_state_names{{
        {placement_allocation_state_t::active, "active"},
        {placement_allocation_state_t::reserved, "reserved"},
      }};

    static const char *allocation_state_name (placement_allocation_state_t value)
    {
        return store_enum_name (value, allocation_state_names,
                                placement_allocation_state_t::reserved);
    }

    static placement_allocation_state_t parse_allocation_state (const json_t &value)
    {
        if (value.is_string ())
            return parse_store_enum (value.get<std::string> (), allocation_state_names,
                                     placement_allocation_state_t::reserved);
        return static_cast<placement_allocation_state_t> (value.get<int> ());
    }

    // §2.4's canonical allocation.descriptor is just {meshName,routingIdHex}
    // -- no owner, no nodeLifecycleGeneration -- because the allocation is
    // always written together with the authority envelope's ownerId/
    // ownerLeaseGeneration, and authority_retarget_t/reserve_once/aggregate
    // commit all set snapshot.owner and snapshot.allocation.target.owner to
    // the identical owner token in the same mutation, so the envelope owner
    // losslessly reconstructs it on decode. This is a separate helper from
    // encode_target (kept for the fence/terminal-record shape, which is not
    // golden-pinned and still needs owner+nodeLifecycleGeneration for its
    // own CAS bookkeeping).
    //
    // object_creation_target_t::node_rid is an opaque raw-bytes token
    // (read_target_descriptor reconstructs a routing_id_t from it via
    // `routing_id_t::from(std::string(node_rid.value()))`, i.e. the bytes
    // are used verbatim, not parsed as hex/decimal/UTF-8) -- so the decode
    // side below must hand back those same raw bytes, not
    // routing_id_t::to_string()'s printable/4-byte-decimal/UUID
    // special-casing, or a non-printable/non-4/16-byte id round-trips to a
    // node_rid_t that no longer resolves to the same mesh row.
    static json_t encode_target_descriptor (const object_creation_target_t &value)
    {
        return {{location_record_fields::meshName, value.mesh_name},
                {location_record_fields::routingIdHex,
                 zlink::routing_id_t::from (std::string (value.node_rid.value ())).to_hex ()}};
    }

    static object_creation_target_t decode_target_descriptor (
      const json_t &value, location_owner_token_t owner, std::uint64_t node_lifecycle_generation)
    {
        const auto rid = zlink::routing_id_t::from_hex (
          value.at (location_record_fields::routingIdHex).get<std::string> ());
        return {value.at (location_record_fields::meshName).get<std::string> (),
                node_rid_t::from_string (
                  std::string (reinterpret_cast<const char *> (rid.data ()), rid.size ())),
                node_lifecycle_generation, std::move (owner)};
    }

    // §2.4's canonical allocation shape uses `state`/`objectKind` as strings
    // (objectKind is the three-way actor|userSpot|instanceSpot spelling,
    // shared with encode_bundle's spotType -- no separate spotKind field).
    static json_t encode_allocation (const placement_allocation_t &value)
    {
        return {{location_record_fields::state, allocation_state_name (value.state)},
                {location_record_fields::objectKind, object_kind3_name (value.object_kind)},
                {location_record_fields::stableType, value.stable_type},
                {location_record_fields::descriptor, encode_target_descriptor (value.target)},
                {location_record_fields::descriptorLifecycleGeneration,
                 generation_string (value.target.node_lifecycle_generation)},
                {location_record_fields::capacity, encode_bundle (value.capacity_bundle)}};
    }

    static placement_allocation_t decode_allocation (const json_t &value,
                                                     const location_owner_token_t &owner)
    {
        return {
          parse_allocation_state (value.at (location_record_fields::state)),
          parse_object_kind3 (value.at (location_record_fields::objectKind).get<std::string> ()),
          value.at (location_record_fields::stableType).get<std::string> (),
          decode_target_descriptor (
            value.at (location_record_fields::descriptor), owner,
            parse_u64_field (value.at (location_record_fields::descriptorLifecycleGeneration))),
          decode_bundle (value.at (location_record_fields::capacity))};
    }

    static json_t encode_pending (const std::optional<pending_object_creation_t> &value)
    {
        if (!value)
            return nullptr;
        return {{location_record_fields::reservationId, value->reservation_id},
                {location_record_fields::requestContentReference, value->request_content_reference},
                {location_record_fields::requestSha256, hex (value->request_sha256)},
                {location_record_fields::requestEncodedSize,
                 static_cast<std::uint64_t> (value->request_encoded_size)}};
    }

    static std::optional<pending_object_creation_t> decode_pending (const json_t &value)
    {
        if (value.is_null ())
            return std::nullopt;
        return pending_object_creation_t{
          value.at (location_record_fields::reservationId).get<std::string> (),
          value.at (location_record_fields::requestContentReference).get<std::string> (),
          unhex_array<32> (value.at (location_record_fields::requestSha256).get<std::string> ()),
          static_cast<std::uint32_t> (
            parse_u64_field (value.at (location_record_fields::requestEncodedSize)))};
    }

    // 21-location-runtime.md#2.4's canonical authority envelope. `payload`
    // is base64 (was hex -- checklist C-4's explicitly named cpp
    // conversion); `objectGeneration`/`authorityOwnerGeneration` and the
    // flat `ownerId`/`ownerLeaseGeneration` (not a nested `owner` object,
    // unlike the generic record) are JSON strings. There is no `storeVersion`
    // field (checklist C-4d): the CAS token is reparented onto the
    // provider's own opaque per-key version (store_found_t.value.version /
    // store_write_applied_t.put_versions), threaded in via decode_authority's
    // `provider_version` parameter and compare_exchange_authority's post-write
    // lookup, instead of a counter this store used to maintain inside
    // the record body.
    static std::vector<std::byte> encode_authority (const authority_snapshot_t &value)
    {
        return to_bytes (json_t{
          {location_record_fields::recordVersion, 1},
          {location_record_fields::payload, base64_encode (value.payload)},
          {location_record_fields::objectGeneration, generation_string (value.object_generation)},
          {location_record_fields::authorityOwnerGeneration,
           generation_string (value.authority_owner_generation)},
          {location_record_fields::ownerId, value.owner.owner_id},
          {location_record_fields::ownerLeaseGeneration,
           generation_string (value.owner.lease_generation)},
          {location_record_fields::allocation, encode_allocation (value.allocation)},
          {location_record_fields::pendingCreation, encode_pending (value.pending_creation)}}
                           .dump ());
    }

    static authority_snapshot_t decode_authority (const std::vector<std::byte> &bytes,
                                                  std::string provider_version,
                                                  std::chrono::system_clock::time_point store_now)
    {
        // Fail-closed on absent AND present-unknown recordVersion (spec 21
        // §2.4); tolerating absence would guess at the record layout.
        const auto value = parse_canonical_record (bytes, location_record_fields::authority);
        const auto owner =
          value.contains (location_record_fields::owner)
            ? decode_owner (value.at (location_record_fields::owner))
            : location_owner_token_t{
                value.at (location_record_fields::ownerId).get<std::string> (),
                parse_i64_field (value.at (location_record_fields::ownerLeaseGeneration))};
        // Clean break (checklist C-4): payload was hex, now base64. There is
        // no dual-read fallback -- the whole opaque-record key/value scheme
        // changed in the same conversion, so no pre-conversion record can be
        // found under these keys to begin with.
        return {std::move (provider_version),
                base64_decode (value.at (location_record_fields::payload).get<std::string> ()),
                parse_u64_field (value.at (location_record_fields::objectGeneration)),
                parse_u64_field (value.at (location_record_fields::authorityOwnerGeneration)),
                owner,
                store_now,
                decode_allocation (value.at (location_record_fields::allocation), owner),
                decode_pending (value.at (location_record_fields::pendingCreation))};
    }

    static authority_snapshot_t decode_authority (const std::vector<std::byte> &bytes,
                                                  const store_version_t &provider_version,
                                                  std::chrono::system_clock::time_point store_now)
    {
        return decode_authority (bytes, provider_version.value, store_now);
    }

    // checklist C-4d's matches_reservation(): identifies a reservation by
    // reservation_id (already unique per attempt -- derived from
    // object_generation/authority_owner_generation, both also compared
    // below) rather than the placeholder expected_store_version the
    // reservation record was written with before the real value was known
    // (see reserve_once).
    static bool same_fence (const object_reservation_fence_t &left,
                            const object_reservation_fence_t &right)
    {
        return left.reservation_id == right.reservation_id
               && left.object_generation == right.object_generation
               && left.authority_owner_generation == right.authority_owner_generation
               && left.target.mesh_name == right.target.mesh_name
               && left.target.node_rid.value () == right.target.node_rid.value ()
               && left.target.node_lifecycle_generation == right.target.node_lifecycle_generation
               && same_owner (left.target.owner, right.target.owner)
               && encode_bundle (left.capacity_bundle) == encode_bundle (right.capacity_bundle);
    }

    task_t<authority_compare_exchange_result_t> authority_conflict (store_read_result_t current)
    {
        if (const auto *found = std::get_if<store_found_t> (&current))
            return completed (
              authority_compare_exchange_result_t{authority_conflict_t{decode_authority (
                found->value.bytes, found->value.version, found->value.store_now)}});
        return completed (authority_compare_exchange_result_t{authority_conflict_t{
          authority_missing_t{std::get<store_missing_t> (current).store_now}}});
    }

    // The post-write CAS token for a row: the provider's own opaque
    // per-key version for `key` out of a successful write's put_versions,
    // i.e. the value this store now uses as authority_snapshot_t::
    // store_version (checklist C-4d's reparenting away from a record-body
    // counter).
    static std::string version_of (const store_write_applied_t &applied, const store_key_t &key)
    {
        for (const auto &entry : applied.put_versions)
            if (entry.key.value == key.value)
                return entry.version.value;
        return {};
    }

    static json_t encode (const capacity_usage_t &value)
    {
        return {{location_record_fields::active, value.active},
                {location_record_fields::reserved, value.reserved},
                {location_record_fields::limit, value.limit}};
    }

    static capacity_usage_t decode_capacity_usage (const json_t &value)
    {
        return {value.at (location_record_fields::active).get<std::uint64_t> (),
                value.at (location_record_fields::reserved).get<std::uint64_t> (),
                value.at (location_record_fields::limit).get<std::int32_t> ()};
    }

    static inline constexpr std::array<std::pair<framework_runtime_state_t, const char *>, 7>
      runtime_state_names{{
        {framework_runtime_state_t::preparing, "preparing"},
        {framework_runtime_state_t::serving, "serving"},
        {framework_runtime_state_t::relocating, "relocating"},
        {framework_runtime_state_t::relocated, "relocated"},
        {framework_runtime_state_t::draining, "draining"},
        {framework_runtime_state_t::stopped, "stopped"},
        {framework_runtime_state_t::error, "error"},
      }};

    static const char *runtime_state_name (framework_runtime_state_t value)
    {
        return store_enum_name (value, runtime_state_names, framework_runtime_state_t::preparing);
    }

    static framework_runtime_state_t parse_runtime_state (const std::string &value)
    {
        return parse_store_enum (value, runtime_state_names, framework_runtime_state_t::preparing);
    }

    static inline constexpr std::array<std::pair<maintenance_policy_kind_t, const char *>, 3>
      maintenance_policy_names{{
        {maintenance_policy_kind_t::disabled, "disabled"},
        {maintenance_policy_kind_t::recreate, "recreate"},
        {maintenance_policy_kind_t::snapshot, "snapshot"},
      }};

    static const char *maintenance_policy_name (maintenance_policy_kind_t value)
    {
        return store_enum_name (value, maintenance_policy_names,
                                maintenance_policy_kind_t::disabled);
    }

    static maintenance_policy_kind_t parse_maintenance_policy (const std::string &value)
    {
        return parse_store_enum (value, maintenance_policy_names,
                                 maintenance_policy_kind_t::disabled);
    }

    static inline constexpr std::array<const char *, 3> object_role_names{"none", "client",
                                                                          "server"};

    static const char *object_role_name (object_role_t value)
    {
        return object_role_names.at (static_cast<std::size_t> (value));
    }

    static object_role_t parse_object_role (const std::string &value)
    {
        const auto found = std::find (object_role_names.begin (), object_role_names.end (), value);
        return found == object_role_names.end ()
                 ? object_role_t::none
                 : static_cast<object_role_t> (std::distance (object_role_names.begin (), found));
    }

    // 21-location-runtime.md#2.4's canonical mesh node descriptor shape:
    // camelCase field names, generation-typed fields as JSON strings,
    // enums spelled out (objectKind/policy/objectRole/state), capacity's
    // spotTypes entries flattened (no nested "usage"), and no `rid`/
    // `nodeGeneration`/`role` provider-internal aliases (checklist C-4d).
    static json_t encode (const mesh_node_descriptor_t &value)
    {
        json_t capabilities = json_t::array ();
        for (const auto &item : value.object_capabilities)
            capabilities.push_back (
              {{location_record_fields::objectKind, object_kind3_name (item.object_kind)},
               {location_record_fields::stableType, item.stable_type},
               {location_record_fields::policy, maintenance_policy_name (item.policy)},
               {location_record_fields::hasSnapshotAdapter, item.has_snapshot_adapter},
               {location_record_fields::limit, item.spot_limit}});
        json_t spot_types = json_t::array ();
        for (const auto &item : value.capacity.spot_types)
            spot_types.push_back (
              {{location_record_fields::objectKind, object_kind3_name (item.object_kind)},
               {location_record_fields::stableType, item.stable_type},
               {location_record_fields::active, item.usage.active},
               {location_record_fields::reserved, item.usage.reserved},
               {location_record_fields::limit, item.usage.limit}});
        return {
          {location_record_fields::meshName, value.mesh_name},
          {location_record_fields::routingIdHex, value.rid.to_hex ()},
          {location_record_fields::lifecycleGeneration,
           generation_string (value.lifecycle_generation)},
          {location_record_fields::descriptorRevision,
           generation_string (value.descriptor_revision)},
          {location_record_fields::endpoint, value.endpoint},
          {location_record_fields::entrySpotId,
           value.entry_spot_id ? json_t (*value.entry_spot_id) : json_t (nullptr)},
          {location_record_fields::channelWeights, value.channel_weights},
          {location_record_fields::applicationVersion,
           generation_string (value.application_version)},
          {location_record_fields::objectCapabilities, std::move (capabilities)},
          {location_record_fields::objectRole, object_role_name (value.object_role)},
          {location_record_fields::placementWeight, value.placement_weight},
          {location_record_fields::capacity,
           {{location_record_fields::actors, encode (value.capacity.actors)},
            {location_record_fields::spots, encode (value.capacity.spots)},
            {location_record_fields::spotTypes, std::move (spot_types)}}},
          {location_record_fields::activationConcurrency,
           {{location_record_fields::active, value.activation_concurrency.active},
            {location_record_fields::limit, value.activation_concurrency.limit}}},
          {location_record_fields::maintenanceWave,
           value.maintenance_wave ? json_t (*value.maintenance_wave) : json_t (nullptr)},
          {location_record_fields::state, runtime_state_name (value.state)},
          {location_record_fields::securityIdentity, value.security_identity},
          {location_record_fields::ownerId, value.owner_id},
          {location_record_fields::leaseGeneration, generation_string (value.lease_generation)},
          {location_record_fields::updatedAtEpochMs,
           generation_string (unix_ms (value.updated_at))}};
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

    task_t<std::optional<stored_target_t>> read_target_descriptor_async (
      object_creation_target_t target, bool require_live = true, bool require_identity = true)
    {
        const auto row_key = key_mesh (
          target.mesh_name, zlink::routing_id_t::from (std::string (target.node_rid.value ())));
        auto row = co_await _store->read (row_key);
        const auto *found = std::get_if<store_found_t> (&row);
        if (!found)
            co_return std::nullopt;
        const auto record = parse_canonical_record (found->value.bytes, "MeshNode descriptor");
        auto descriptor = decode_mesh_descriptor (record.at (location_record_fields::descriptor));
        if ((require_identity
             && (descriptor.lifecycle_generation != target.node_lifecycle_generation
                 || descriptor.owner_id != target.owner.owner_id
                 || descriptor.lease_generation != target.owner.lease_generation))
            || (require_live && descriptor.state != framework_runtime_state_t::serving))
            co_return std::nullopt;
        const auto owner_id = require_identity ? target.owner.owner_id : descriptor.owner_id;
        const auto owner = co_await _store->read (key_owner (owner_id));
        const auto *live = std::get_if<store_found_t> (&owner);
        if (require_live
            && (!live || owner_generation (live->value.bytes) != descriptor.lease_generation))
            co_return std::nullopt;
        co_return stored_target_t{row_key, found->value.version.value, live != nullptr,
                                  std::move (descriptor), record};
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

    static std::string canonical_capacity_type_key (placement_object_kind_t kind,
                                                    std::string_view stable_type)
    {
        return std::to_string (static_cast<int> (kind)) + ":" + std::string (stable_type);
    }

    static std::string node_capacity_type_key (placement_object_kind_t kind,
                                               std::string_view stable_type)
    {
        const auto object_kind =
          kind == placement_object_kind_t::user_spot
            ? "user_spot"
            : (kind == placement_object_kind_t::instance_spot ? "instance_spot" : "actor");
        return std::string (object_kind) + '\0' + std::string (stable_type);
    }

    static capacity_record_t decode_capacity_record (const std::vector<std::byte> &bytes)
    {
        const auto value = parse_json (bytes);
        capacity_record_t result;
        if (value.contains (location_record_fields::active)
            && value.contains (location_record_fields::pending)) {
            result.format = capacity_record_format_t::node_compatible;
            const auto decode_usage = [&] (const json_t &usage, bool active) {
                auto &actors = active ? result.actors.active : result.actors.pending;
                auto &spots = active ? result.spots.active : result.spots.pending;
                actors = usage.at (location_record_fields::actors).get<std::int64_t> ();
                spots = usage.at (location_record_fields::spots).get<std::int64_t> ();
                for (const auto &[key, count] :
                     usage.at (location_record_fields::spotTypes).items ()) {
                    auto &typed = result.spot_types[key];
                    (active ? typed.active : typed.pending) = count.get<std::int64_t> ();
                }
            };
            decode_usage (value.at (location_record_fields::active), true);
            decode_usage (value.at (location_record_fields::pending), false);
        } else {
            result.actors.active =
              value.at (location_record_fields::actorsActive).get<std::int64_t> ();
            result.actors.pending =
              value.at (location_record_fields::actorsPending).get<std::int64_t> ();
            result.spots.active =
              value.at (location_record_fields::spotsActive).get<std::int64_t> ();
            result.spots.pending =
              value.at (location_record_fields::spotsPending).get<std::int64_t> ();
            for (const auto &[key, count] : value.at (location_record_fields::spotTypes).items ())
                result.spot_types.emplace (
                  key, capacity_count_t{
                         count.at (location_record_fields::active).get<std::int64_t> (),
                         count.at (location_record_fields::pending).get<std::int64_t> ()});
        }
        const auto non_negative = [] (const capacity_count_t &count) {
            return count.active >= 0 && count.pending >= 0;
        };
        if (!non_negative (result.actors) || !non_negative (result.spots)
            || std::any_of (result.spot_types.begin (), result.spot_types.end (),
                            [&] (const auto &item) { return !non_negative (item.second); }))
            throw std::invalid_argument ("Location Store capacity row is inconsistent");
        return result;
    }

    static std::vector<std::byte> encode_capacity_record (const capacity_record_t &record)
    {
        json_t value;
        if (record.format == capacity_record_format_t::node_compatible) {
            json_t active_types = json_t::object ();
            json_t pending_types = json_t::object ();
            for (const auto &[key, count] : record.spot_types) {
                if (count.active != 0)
                    active_types[key] = count.active;
                if (count.pending != 0)
                    pending_types[key] = count.pending;
            }
            value = {{location_record_fields::active,
                      {{location_record_fields::actors, record.actors.active},
                       {location_record_fields::spots, record.spots.active},
                       {location_record_fields::spotTypes, std::move (active_types)}}},
                     {location_record_fields::pending,
                      {{location_record_fields::actors, record.actors.pending},
                       {location_record_fields::spots, record.spots.pending},
                       {location_record_fields::spotTypes, std::move (pending_types)}}}};
        } else {
            json_t typed = json_t::object ();
            for (const auto &[key, count] : record.spot_types)
                typed[key] = {{location_record_fields::active, count.active},
                              {location_record_fields::pending, count.pending}};
            value = {{location_record_fields::actorsActive, record.actors.active},
                     {location_record_fields::actorsPending, record.actors.pending},
                     {location_record_fields::spotsActive, record.spots.active},
                     {location_record_fields::spotsPending, record.spots.pending},
                     {location_record_fields::spotTypes, std::move (typed)}};
        }
        return to_bytes (value.dump ());
    }

    task_t<stored_capacity_t> read_capacity_async (object_creation_target_t target,
                                                   const stored_target_t *descriptor = nullptr)
    {
        const auto canonical_key = key_capacity (
          descriptor ? descriptor->descriptor.mesh_name : target.mesh_name,
          descriptor ? descriptor->descriptor.rid
                     : zlink::routing_id_t::from (std::string (target.node_rid.value ())),
          target.node_lifecycle_generation);
        auto current = co_await _store->read (canonical_key);
        if (const auto *found = std::get_if<store_found_t> (&current))
            co_return stored_capacity_t{canonical_key,
                                        version_condition (canonical_key, found->value.version),
                                        decode_capacity_record (found->value.bytes)};

        // Node's current repository uses a node-keyed active/pending JSON
        // shape. Preserve that row's key and encoding when it owns the
        // allocation so a cross-language retarget can update its source
        // accounting in the same CAS.
        const auto node_key = key_node_capacity (target.mesh_name, target.node_rid.value ());
        auto node_current = co_await _store->read (node_key);
        if (const auto *found = std::get_if<store_found_t> (&node_current))
            co_return stored_capacity_t{node_key,
                                        version_condition (node_key, found->value.version),
                                        decode_capacity_record (found->value.bytes)};
        co_return stored_capacity_t{canonical_key, missing_condition (canonical_key),
                                    capacity_record_t{}};
    }

    static bool capacity_available (const mesh_node_descriptor_t &descriptor,
                                    const capacity_record_t &capacity,
                                    const placement_capacity_bundle_t &bundle)
    {
        const auto enough = [] (const capacity_count_t &usage, std::int32_t limit,
                                std::uint32_t requested) {
            if (requested == 0 || limit == 0)
                return true;
            return usage.active <= limit && usage.pending <= limit
                   && static_cast<std::uint64_t> (usage.active)
                          + static_cast<std::uint64_t> (usage.pending) + requested
                        <= static_cast<std::uint64_t> (limit);
        };
        if (!enough (capacity.actors, descriptor.capacity.actors.limit, bundle.actor_slots)
            || !enough (capacity.spots, descriptor.capacity.spots.limit, bundle.spot_slots))
            return false;
        if (!bundle.spot_type)
            return true;
        const auto limit = std::find_if (
          descriptor.capacity.spot_types.begin (), descriptor.capacity.spot_types.end (),
          [&] (const spot_type_capacity_t &item) {
              return item.object_kind == bundle.spot_type->object_kind
                     && item.stable_type == bundle.spot_type->stable_type;
          });
        if (limit == descriptor.capacity.spot_types.end ())
            return false;
        const auto key =
          capacity.format == capacity_record_format_t::node_compatible
            ? node_capacity_type_key (bundle.spot_type->object_kind, bundle.spot_type->stable_type)
            : canonical_capacity_type_key (bundle.spot_type->object_kind,
                                           bundle.spot_type->stable_type);
        const auto found = capacity.spot_types.find (key);
        return enough (found == capacity.spot_types.end () ? capacity_count_t{} : found->second,
                       limit->usage.limit, bundle.spot_type->slots);
    }

    static bool adjust_capacity (capacity_record_t &record,
                                 const placement_capacity_bundle_t &bundle,
                                 std::int64_t pending_delta,
                                 std::int64_t active_delta)
    {
        const auto adjust = [] (capacity_count_t &usage, std::uint32_t slots,
                                std::int64_t pending_change, std::int64_t active_change) {
            const auto apply = [slots] (std::int64_t current, std::int64_t change,
                                        std::int64_t &next) {
                if (change < -1 || change > 1)
                    return false;
                const auto amount = static_cast<std::int64_t> (slots);
                if ((change < 0 && current < amount)
                    || (change > 0 && current > std::numeric_limits<std::int64_t>::max () - amount))
                    return false;
                next = current + change * amount;
                return true;
            };
            std::int64_t pending = 0;
            std::int64_t active = 0;
            if (!apply (usage.pending, pending_change, pending)
                || !apply (usage.active, active_change, active))
                return false;
            usage.pending = pending;
            usage.active = active;
            return true;
        };
        if (!adjust (record.actors, bundle.actor_slots, pending_delta, active_delta)
            || !adjust (record.spots, bundle.spot_slots, pending_delta, active_delta))
            return false;
        if (!bundle.spot_type)
            return true;
        const auto key =
          record.format == capacity_record_format_t::node_compatible
            ? node_capacity_type_key (bundle.spot_type->object_kind, bundle.spot_type->stable_type)
            : canonical_capacity_type_key (bundle.spot_type->object_kind,
                                           bundle.spot_type->stable_type);
        auto &typed = record.spot_types[key];
        if (!adjust (typed, bundle.spot_type->slots, pending_delta, active_delta))
            return false;
        if (record.format == capacity_record_format_t::node_compatible && typed.active == 0
            && typed.pending == 0)
            record.spot_types.erase (key);
        return true;
    }

    static std::vector<std::byte> encode_target_record (stored_target_t target)
    {
        target.record[location_record_fields::descriptor] = encode (target.descriptor);
        return to_bytes (target.record.dump ());
    }

    task_t<authority_read_result_t> read_authority_value_async (std::string key)
    {
        auto current = co_await _store->read (key_authority (key));
        if (const auto *found = std::get_if<store_found_t> (&current)) {
            auto snapshot = co_await effective_authority_async (
              key, found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot)
                co_return authority_read_result_t{std::move (*snapshot)};
            co_return authority_read_result_t{authority_missing_t{found->value.store_now}};
        }
        co_return authority_read_result_t{
          authority_missing_t{std::get<store_missing_t> (current).store_now}};
    }

    static json_t encode_aggregate (const aggregate_prepare_request_t &request,
                                    std::string_view status,
                                    const aggregate_inventory::tree_t &inventory)
    {
        return {{location_record_fields::status, status},
                {location_record_fields::aggregateId, hex (request.aggregate_id.value)},
                {location_record_fields::aggregateGeneration, request.aggregate_generation},
                {location_record_fields::inventoryRoot, hex (inventory.root)},
                {location_record_fields::inventoryCount, inventory.participant_count},
                {location_record_fields::inventoryPageCount, inventory.pages.size ()},
                {location_record_fields::inventoryIndexPageCount, inventory.index_pages.size ()},
                {location_record_fields::inventoryIndexLevelCount, inventory.index_level_count},
                {location_record_fields::inventoryDigest, hex (request.inventory_digest.value)},
                {location_record_fields::targetMeshName, request.target_descriptor.mesh_name},
                {location_record_fields::targetNodeRid, request.target_descriptor.rid.to_string ()},
                {location_record_fields::targetLifecycleGeneration,
                 request.target_descriptor_lifecycle_generation},
                {location_record_fields::capacityBundle, encode_bundle (request.capacity_bundle)},
                {location_record_fields::targetOwner, encode_owner (request.target_owner)}};
    }

    static bool aggregate_record_matches_request (const json_t &record,
                                                  const aggregate_prepare_request_t &request,
                                                  const aggregate_inventory::tree_t &inventory)
    {
        try {
            const auto expected = encode_aggregate (
              request, record.value (location_record_fields::status, ""), inventory);
            for (const auto *field :
                 {location_record_fields::aggregateId, location_record_fields::aggregateGeneration,
                  location_record_fields::inventoryRoot, location_record_fields::inventoryCount,
                  location_record_fields::inventoryPageCount,
                  location_record_fields::inventoryIndexPageCount,
                  location_record_fields::inventoryIndexLevelCount,
                  location_record_fields::inventoryDigest, location_record_fields::targetMeshName,
                  location_record_fields::targetNodeRid,
                  location_record_fields::targetLifecycleGeneration,
                  location_record_fields::capacityBundle, location_record_fields::targetOwner}) {
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
        result.mesh_name = value.at (location_record_fields::meshName).get<std::string> ();
        result.rid = zlink::routing_id_t::from_hex (
          value.at (location_record_fields::routingIdHex).get<std::string> ());
        result.lifecycle_generation =
          parse_u64_field (value.at (location_record_fields::lifecycleGeneration));
        result.descriptor_revision =
          parse_u64_field (value.at (location_record_fields::descriptorRevision));
        result.endpoint = transport::normalize_endpoint (
          value.at (location_record_fields::endpoint).get<std::string> ());
        if (value.contains (location_record_fields::entrySpotId)
            && !value.at (location_record_fields::entrySpotId).is_null ())
            result.entry_spot_id =
              value.at (location_record_fields::entrySpotId).get<std::string> ();
        result.channel_weights =
          value.at (location_record_fields::channelWeights).get<std::map<std::string, int>> ();
        result.application_version = static_cast<std::int64_t> (
          parse_u64_field (value.at (location_record_fields::applicationVersion)));
        for (const auto &item : value.at (location_record_fields::objectCapabilities))
            result.object_capabilities.push_back (
              {parse_object_kind3 (
                 item.at (location_record_fields::objectKind).get<std::string> ()),
               item.at (location_record_fields::stableType).get<std::string> (),
               parse_maintenance_policy (
                 item.at (location_record_fields::policy).get<std::string> ()),
               item.at (location_record_fields::hasSnapshotAdapter).get<bool> (),
               item.at (location_record_fields::limit).get<std::int32_t> ()});
        result.object_role =
          parse_object_role (value.at (location_record_fields::objectRole).get<std::string> ());
        result.placement_weight = value.at (location_record_fields::placementWeight).get<int> ();
        const auto &capacity = value.at (location_record_fields::capacity);
        result.capacity.actors =
          decode_capacity_usage (capacity.at (location_record_fields::actors));
        result.capacity.spots = decode_capacity_usage (capacity.at (location_record_fields::spots));
        for (const auto &item : capacity.at (location_record_fields::spotTypes))
            result.capacity.spot_types.push_back (
              {parse_object_kind3 (
                 item.at (location_record_fields::objectKind).get<std::string> ()),
               item.at (location_record_fields::stableType).get<std::string> (),
               {item.at (location_record_fields::active).get<std::uint64_t> (),
                item.at (location_record_fields::reserved).get<std::uint64_t> (),
                item.at (location_record_fields::limit).get<std::int32_t> ()}});
        const auto &activation = value.at (location_record_fields::activationConcurrency);
        result.activation_concurrency.active =
          activation.at (location_record_fields::active).get<std::uint32_t> ();
        result.activation_concurrency.limit =
          activation.at (location_record_fields::limit).get<std::int32_t> ();
        if (value.contains (location_record_fields::maintenanceWave)
            && !value.at (location_record_fields::maintenanceWave).is_null ())
            result.maintenance_wave =
              value.at (location_record_fields::maintenanceWave).get<std::string> ();
        result.state =
          parse_runtime_state (value.at (location_record_fields::state).get<std::string> ());
        result.security_identity =
          value.at (location_record_fields::securityIdentity).get<std::string> ();
        result.owner_id = value.at (location_record_fields::ownerId).get<std::string> ();
        result.lease_generation = static_cast<std::int64_t> (
          parse_u64_field (value.at (location_record_fields::leaseGeneration)));
        result.updated_at = from_unix_ms (static_cast<std::int64_t> (
          parse_u64_field (value.at (location_record_fields::updatedAtEpochMs))));
        return result;
    }

    static json_t encode (const client_server_server_descriptor_t &value)
    {
        return {{location_record_fields::channelName, value.channel_name},
                {location_record_fields::serverRid, value.server_rid.to_hex ()},
                {location_record_fields::lifecycleGeneration, value.lifecycle_generation},
                {location_record_fields::descriptorRevision, value.descriptor_revision},
                {location_record_fields::endpoint, value.endpoint},
                {location_record_fields::weight, value.weight},
                {location_record_fields::state, static_cast<int> (value.state)},
                {location_record_fields::securityIdentity, value.security_identity},
                {location_record_fields::ownerId, value.owner_id},
                {location_record_fields::leaseGeneration, value.lease_generation},
                {location_record_fields::updatedAt, unix_ms (value.updated_at)}};
    }

    static client_server_server_descriptor_t decode_client_server (const json_t &value)
    {
        client_server_server_descriptor_t result;
        result.channel_name = value.at (location_record_fields::channelName).get<std::string> ();
        result.server_rid = zlink::routing_id_t::from_hex (
          value.at (location_record_fields::serverRid).get<std::string> ());
        result.lifecycle_generation =
          value.at (location_record_fields::lifecycleGeneration).get<std::uint64_t> ();
        result.descriptor_revision =
          value.at (location_record_fields::descriptorRevision).get<std::uint64_t> ();
        result.endpoint = transport::normalize_endpoint (
          value.at (location_record_fields::endpoint).get<std::string> ());
        result.weight = value.at (location_record_fields::weight).get<int> ();
        result.state = static_cast<framework_runtime_state_t> (
          value.at (location_record_fields::state).get<int> ());
        result.security_identity =
          value.at (location_record_fields::securityIdentity).get<std::string> ();
        result.owner_id = value.at (location_record_fields::ownerId).get<std::string> ();
        result.lease_generation =
          value.at (location_record_fields::leaseGeneration).get<std::int64_t> ();
        result.updated_at =
          from_unix_ms (value.at (location_record_fields::updatedAt).get<std::int64_t> ());
        return result;
    }

    static json_t encode (const fanout_publisher_descriptor_t &value)
    {
        return {{location_record_fields::channelName, value.channel_name},
                {location_record_fields::publisherRid, value.publisher_rid.to_hex ()},
                {location_record_fields::lifecycleGeneration, value.lifecycle_generation},
                {location_record_fields::descriptorRevision, value.descriptor_revision},
                {location_record_fields::endpoint, value.endpoint},
                {location_record_fields::state, static_cast<int> (value.state)},
                {location_record_fields::securityIdentity, value.security_identity},
                {location_record_fields::ownerId, value.owner_id},
                {location_record_fields::leaseGeneration, value.lease_generation},
                {location_record_fields::updatedAt, unix_ms (value.updated_at)}};
    }

    static fanout_publisher_descriptor_t decode_fanout (const json_t &value)
    {
        fanout_publisher_descriptor_t result;
        result.channel_name = value.at (location_record_fields::channelName).get<std::string> ();
        result.publisher_rid = zlink::routing_id_t::from_hex (
          value.at (location_record_fields::publisherRid).get<std::string> ());
        result.lifecycle_generation =
          value.at (location_record_fields::lifecycleGeneration).get<std::uint64_t> ();
        result.descriptor_revision =
          value.at (location_record_fields::descriptorRevision).get<std::uint64_t> ();
        result.endpoint = transport::normalize_endpoint (
          value.at (location_record_fields::endpoint).get<std::string> ());
        result.state = static_cast<framework_runtime_state_t> (
          value.at (location_record_fields::state).get<int> ());
        result.security_identity =
          value.at (location_record_fields::securityIdentity).get<std::string> ();
        result.owner_id = value.at (location_record_fields::ownerId).get<std::string> ();
        result.lease_generation =
          value.at (location_record_fields::leaseGeneration).get<std::int64_t> ();
        result.updated_at =
          from_unix_ms (value.at (location_record_fields::updatedAt).get<std::int64_t> ());
        return result;
    }

    // §2.4 requires generation-typed fields as JSON strings (64-bit values
    // can exceed JSON number precision), while `recordVersion` and plain
    // counters like `placementWeight`/capacity slots stay JSON numbers.
    static std::string generation_string (std::uint64_t value) { return std::to_string (value); }
    static std::string generation_string (std::int64_t value) { return std::to_string (value); }

    // §2.4's canonical envelope for the generic opaque record (MeshNode,
    // ClientServer, fanout publisher). The internal write-generation counter
    // (distinct from the host-issued `descriptorRevision`; used to gate
    // `renew` intent and exhaustion) is no longer persisted here -- it is
    // reparented onto the provider's own opaque per-key version by
    // update_descriptor (checklist C-4d), so the `generation` parameter is
    // unused; kept only so call sites don't need touching.
    static json_t encode_mesh_record (std::uint64_t generation,
                                      const mesh_node_descriptor_t &descriptor)
    {
        (void) generation;
        return {{location_record_fields::recordVersion, 1},
                {location_record_fields::ownerId, descriptor.owner_id},
                {location_record_fields::leaseGeneration,
                 generation_string (descriptor.lease_generation)},
                {location_record_fields::descriptorRevision,
                 generation_string (descriptor.descriptor_revision)},
                {location_record_fields::descriptor, encode (descriptor)}};
    }

    static json_t encode_descriptor_record (std::uint64_t generation,
                                            std::string owner_id,
                                            std::int64_t lease_generation,
                                            std::uint64_t lifecycle_generation,
                                            std::uint64_t descriptor_revision,
                                            json_t descriptor)
    {
        (void) generation;
        return {
          {location_record_fields::recordVersion, 1},
          {location_record_fields::ownerId, std::move (owner_id)},
          {location_record_fields::leaseGeneration, generation_string (lease_generation)},
          {location_record_fields::lifecycleGeneration, lifecycle_generation},
          {location_record_fields::descriptorRevision, generation_string (descriptor_revision)},
          {location_record_fields::descriptor, std::move (descriptor)}};
    }

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    template <typename T> static task_t<T> unavailable (std::string message)
    {
        return task_t<T> (
          result_t<T>::failure (framework_error_kind_t::internal_failure, std::move (message)));
    }

    template <typename T> static task_t<T> cancelled ()
    {
        return task_t<T> (detail::result_access_t::failure<T> (
          detail::make_cancellation_exception ("location store operation was cancelled")));
    }

    location_store_t *_store;
};

} // namespace zlink::framework::runtime
