/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/authority_aggregate_relocation.hpp"

#include <zlink/framework/detail/binary_text_codec.hpp>

#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_record_fields.hpp"
#include "runtime/locations/aggregate_inventory.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/relocation_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/location_store_write.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/transport/endpoint_notation.hpp"
#include "runtime/utils/uuid.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include "base64.hpp"
#include "sha256.hpp"
#include "runtime/diagnostics/capacity_metrics.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
    class provider_boundary_t final : public location_store_t
    {
      public:
        explicit provider_boundary_t (location_store_t &provider,
                                      provider_location_repository_t &repository) noexcept :
            _provider (provider), _repository (repository)
        {
        }

        task_t<store_read_result_t> read (store_key_t key) override
        {
            try {
                co_return co_await _provider.read (std::move (key));
            }
            catch (...) {
                _repository.record_store_error ("read");
                rethrow_provider_failure ();
            }
        }

        task_t<store_write_result_t> write (store_write_request_t request) override
        {
            return write_as (std::move (request), "compare_exchange");
        }

        task_t<store_write_result_t> write_as (store_write_request_t request, const char *operation)
        {
            try {
                co_return co_await _provider.write (std::move (request));
            }
            catch (...) {
                _repository.record_store_error (operation);
                rethrow_provider_failure ();
            }
        }

        task_t<store_scan_result_t> scan (store_scan_request_t request) override
        {
            try {
                co_return co_await _provider.scan (std::move (request));
            }
            catch (...) {
                _repository.record_store_error ("read");
                rethrow_provider_failure ();
            }
        }

      private:
        [[noreturn]] static void rethrow_provider_failure ()
        {
            try {
                throw;
            }
            catch (const framework_exception_t &) {
                throw;
            }
            catch (const std::invalid_argument &) {
                throw;
            }
            catch (const std::out_of_range &) {
                throw;
            }
            catch (const std::exception &error) {
                if (detail::is_cancellation_exception (error))
                    throw;
            }
            catch (...) {
                // The active provider exception is retained by throw_with_nested below.
            }
            std::throw_with_nested (framework_exception_t (
              framework_error_kind_t::unavailable, "Location Store provider is unavailable."));
        }

        location_store_t &_provider;
        provider_location_repository_t &_repository;
    };

  public:
    explicit provider_location_repository_t (
      location_store_t &store,
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring = {}) :
        _monitoring (monitoring
                       ? std::move (monitoring)
                       : std::make_shared<framework::detail::monitoring_runtime_state_t> ()),
        _capacity_metrics (_monitoring),
        _store (store, *this)
    {
    }

    task_t<void> ensure_store_format (std::stop_token cancellation) override;

    task_t<void> refresh_capacity_metrics (std::string mesh_name, zlink::routing_id_t rid) override
    {
        const auto scope = capacity_prefix (mesh_name, rid.to_hex ());
        if (_capacity_metrics.contains (scope))
            co_await confirm_capacity (scope);
    }

    task_t<owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override
    {
        if (owner_id.empty () || lease_ttl <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("owner lease claim is incomplete");
        const auto owner_key = key_owner (owner_id);
        for (;;) {
            auto owner = co_await _store.read (owner_key);
            if (std::holds_alternative<store_found_t> (owner))
                co_return owner_lease_claim_result_t{owner_lease_conflict_t{}};

            auto counter = co_await _store.read (counter_key);
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
        auto result = co_await _store.read (key_owner (owner_id));
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
        auto current = co_await _store.read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            co_return owner_lease_renew_result_t{owner_lease_stale_t{}};
        store_write_request_t request;
        request.conditions.push_back (version_condition (key, found->value.version));
        request.mutations.push_back (store_put_t{key, found->value.bytes, lease_ttl});
        auto result = co_await write_async (std::move (request), nullptr, {}, "lease_renew");
        if (std::holds_alternative<store_write_conflict_t> (result))
            co_return owner_lease_renew_result_t{owner_lease_stale_t{}};
        const auto &applied = std::get<store_write_applied_t> (result);
        co_return owner_lease_renew_result_t{
          owner_lease_renewed_t{applied.store_now + lease_ttl, applied.store_now}};
    }

    task_t<owner_lease_release_result_t> release_owner_lease (location_owner_token_t token) override
    {
        return release_owner_lease (std::move (token),
                                    std::chrono::steady_clock::time_point::max ());
    }

    task_t<owner_lease_release_result_t>
    release_owner_lease (location_owner_token_t token,
                         std::chrono::steady_clock::time_point deadline) override
    {
        if (std::chrono::steady_clock::now () >= deadline)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         "owner cleanup deadline expired");
        const auto key = key_owner (token.owner_id);
        auto current = co_await _store.read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found || owner_generation (found->value.bytes) != token.lease_generation)
            co_return owner_lease_release_result_t{owner_lease_stale_t{}};
        store_write_request_t result_request{{version_condition (key, found->value.version)},
                                             {store_delete_t{key}}};
        if (std::chrono::steady_clock::now () >= deadline)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         "owner cleanup deadline expired");
        auto result =
          co_await write_async (std::move (result_request), nullptr, {}, "release", deadline);
        co_return std::holds_alternative<store_write_applied_t> (result)
          ? owner_lease_release_result_t{owner_lease_released_t{}}
          : owner_lease_release_result_t{owner_lease_stale_t{}};
    }

    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        _capacity_metrics.register_local (
          capacity_prefix (descriptor.mesh_name, descriptor.rid.to_hex ()), descriptor);
        const auto key = key_mesh (descriptor.mesh_name, descriptor.rid);
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
        return remove_mesh_node (std::move (key), std::move (owner),
                                 std::chrono::steady_clock::time_point::max ());
    }

    task_t<location_write_status_t>
    remove_mesh_node (mesh_node_descriptor_key_t key,
                      location_owner_token_t owner,
                      std::chrono::steady_clock::time_point deadline) override
    {
        return remove_descriptor (key_mesh (key.mesh_name, key.rid), std::move (owner), deadline);
    }

    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh_name, location_page_request_t page = {}) override
    {
        auto listed = co_await list_descriptors<mesh_node_descriptor_t> (
          prefix_mesh (mesh_name), std::move (page), [] (const nlohmann::json &record) {
              return decode_mesh_descriptor (record.at (location_record_fields::descriptor));
          });
        for (auto &descriptor : listed.items) {
            const auto scope = capacity_prefix (descriptor.mesh_name, descriptor.rid.to_hex ());
            project_capacity (scope, descriptor.capacity, co_await read_capacity (scope));
        }
        co_return listed;
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

    task_t<authority_read_result_t> read_authority_core (authority_key_t key,
                                                         std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<authority_read_result_t> ();
        co_return co_await read_authority_value_async (std::move (key.value));
    }

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected_store_version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override
    {
        return compare_exchange_authority_core (std::move (key), std::move (expected_store_version),
                                                std::move (mutation), cancellation, std::nullopt);
    }

  private:
    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected_store_version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation,
                                     std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        const auto *put = std::get_if<authority_put_t> (&mutation);
        const auto operation_deadline = put ? put->operation_deadline : deadline;
        const auto check_deadline = [&] {
            if (operation_deadline && std::chrono::steady_clock::now () >= *operation_deadline)
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "Actor membership operation deadline expired");
        };
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<authority_compare_exchange_result_t> ();
            check_deadline ();
            const auto row_key = key_authority (key.value);
            auto current = co_await _store.read (row_key);
            auto *found = std::get_if<store_found_t> (&current);
            if (!found)
                co_return co_await authority_conflict (std::move (current));
            std::optional<store_condition_t> retarget_lock;
            if (std::holds_alternative<authority_retarget_t> (mutation))
                retarget_lock.emplace ();
            if (co_await authority_mutation_locked_async (key.value, retarget_lock ? &*retarget_lock
                                                                                   : nullptr))
                co_return authority_compare_exchange_result_t{
                  authority_conflict_t{co_await read_authority_value_async (key.value)}};
            auto snapshot =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot.store_version != expected_store_version)
                co_return co_await authority_conflict (std::move (current));

            const auto old_member =
              snapshot.allocation.object_kind == placement_object_kind_t::actor
                ? decode_direct_actor_authority_payload (snapshot.payload)
                : std::nullopt;
            const auto *retarget = std::get_if<authority_retarget_t> (&mutation);
            const auto qualification_owner = retarget                   ? retarget->target.owner
                                             : put && put->preservation ? put->preservation->second
                                                                        : snapshot.owner;
            store_write_request_t write_request;
            std::optional<placement_allocation_t> source_allocation;
            std::optional<stored_target_t> relocation_target;
            if (std::holds_alternative<authority_delete_t> (mutation)) {
                if (snapshot.allocation.state != placement_allocation_state_t::active)
                    co_return co_await authority_conflict (std::move (current));
                if (!co_await owner_is_live_async (snapshot.owner))
                    co_return co_await authority_conflict (std::move (current));
                const auto descriptor_key =
                  key_mesh (snapshot.allocation.target.mesh_name,
                            zlink::routing_id_t::from (
                              std::string (snapshot.allocation.target.node_rid.value ())));
                const auto descriptor = co_await _store.read (descriptor_key);
                const auto decoded_key =
                  authority_key_codec_detail::decode_authority_key (key.value);
                if (!decoded_key)
                    co_return co_await authority_conflict (std::move (current));
                write_request.conditions = {version_condition (row_key, found->value.version),
                                            owner_condition (snapshot.owner),
                                            condition_for (descriptor_key, descriptor)};
                write_request.mutations = {store_delete_t{row_key}};
                capacity_changes_t capacity;
                capacity.change (snapshot.allocation.target, snapshot.allocation.capacity_bundle, 0,
                                 -1);
                capacity.append_to (write_request);
            } else if (put && put->preservation) {
                if (!put->matches_preserved (snapshot))
                    co_return co_await authority_conflict (std::move (current));
                if (!co_await owner_is_live_async (qualification_owner))
                    co_return co_await authority_conflict (std::move (current));
                auto target = snapshot.allocation.target;
                target.owner = qualification_owner;
                const auto descriptor =
                  co_await read_target_descriptor_async (target, false, false);
                if (!descriptor || descriptor->descriptor.owner_id != qualification_owner.owner_id
                    || descriptor->descriptor.lifecycle_generation
                         != target.node_lifecycle_generation)
                    co_return co_await authority_conflict (std::move (current));
                snapshot.owner = qualification_owner;
                snapshot.allocation.target.owner = qualification_owner;
                snapshot.payload = put->payload;
                write_request = {
                  {version_condition (row_key, found->value.version),
                   owner_condition (qualification_owner),
                   version_condition (descriptor->key, descriptor->provider_version)},
                  {store_put_t{row_key, encode_authority (snapshot), std::nullopt}}};
            } else if (const auto *restore = std::get_if<authority_restore_t> (&mutation)) {
                if (!same_owner (snapshot.owner, restore->expected_owner))
                    co_return co_await authority_conflict (std::move (current));
                snapshot.payload = restore->payload;
                write_request = {{version_condition (row_key, found->value.version)},
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
                if (capacity_scope (snapshot.allocation.target)
                    != capacity_scope (retarget->target)) {
                    source_allocation = snapshot.allocation;
                    relocation_target = target_descriptor;
                }
                write_request.conditions = {version_condition (row_key, found->value.version),
                                            owner_condition (retarget->target.owner),
                                            std::move (*retarget_lock)};
                if (!source_allocation)
                    write_request.conditions.push_back (version_condition (
                      target_descriptor->key, target_descriptor->provider_version));
                const auto owner_generation =
                  co_await issue_generation (row_key, generation_kind_t::owner, write_request);
                if (!owner_generation)
                    co_return authority_compare_exchange_result_t{
                      authority_generation_exhausted_t{}};
                snapshot.authority_owner_generation = *owner_generation;
                snapshot.owner = retarget->target.owner;
                snapshot.allocation.target = retarget->target;
                snapshot.payload = retarget->payload;
                if (retarget->replace_aggregate)
                    snapshot.aggregate = *retarget->replace_aggregate;
                write_request.mutations.insert (
                  write_request.mutations.begin (),
                  store_put_t{row_key, encode_authority (snapshot), std::nullopt});
            } else {
                if (snapshot.allocation.state != placement_allocation_state_t::active)
                    co_return co_await authority_conflict (std::move (current));
                auto live_owner = co_await read_live_owner_async (snapshot.owner);
                if (!live_owner)
                    co_return co_await authority_conflict (std::move (current));
                write_request.conditions = {version_condition (row_key, found->value.version),
                                            owner_condition (snapshot.owner)};
                if (auto *reincarnate = std::get_if<authority_reincarnate_t> (&mutation)) {
                    const auto object_generation =
                      co_await issue_generation (row_key, generation_kind_t::object, write_request);
                    const auto owner_generation =
                      co_await issue_generation (row_key, generation_kind_t::owner, write_request);
                    if (!object_generation || !owner_generation)
                        co_return authority_compare_exchange_result_t{
                          authority_generation_exhausted_t{}};
                    snapshot.object_generation = *object_generation;
                    snapshot.authority_owner_generation = *owner_generation;
                    snapshot.payload = reincarnate->payload;
                } else {
                    const auto &replacement = std::get<authority_put_t> (mutation);
                    snapshot.payload = replacement.payload;
                    if (replacement.replace_aggregate)
                        snapshot.aggregate = *replacement.replace_aggregate;
                }
                write_request.mutations.push_back (
                  store_put_t{row_key, encode_authority (snapshot), std::nullopt});
            }
            if (snapshot.allocation.object_kind == placement_object_kind_t::actor) {
                const auto membership_failure = co_await append_user_membership_conditions (
                  old_member, snapshot.payload, write_request);
                if (membership_failure)
                    co_return co_await authority_conflict (std::move (current), membership_failure);
            }
            check_deadline ();
            if (source_allocation) {
                co_return co_await publish_retarget (
                  key, row_key, found->value.version, std::move (snapshot), *source_allocation,
                  *relocation_target, std::move (write_request), cancellation,
                  operation_deadline.value_or (std::chrono::steady_clock::time_point::max ()));
            }
            auto written = co_await write_async (
              std::move (write_request), nullptr, {}, "compare_exchange",
              operation_deadline.value_or (std::chrono::steady_clock::time_point::max ()));
            const auto *applied = std::get_if<store_write_applied_t> (&written);
            if (!applied) {
                if (co_await conflict_qualification_unchanged (row_key, found->value.version,
                                                               qualification_owner, std::nullopt))
                    continue;
                co_return co_await authority_conflict (co_await _store.read (row_key));
            }
            if (std::holds_alternative<authority_delete_t> (mutation)) {
                co_return authority_compare_exchange_result_t{
                  authority_deleted_t{snapshot.store_version, applied->store_now}};
            }
            snapshot.store_now = applied->store_now;
            snapshot.store_version = version_of (*applied, row_key);
            co_return authority_compare_exchange_result_t{authority_stored_t{std::move (snapshot)}};
        }
    }

  public:
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
          prefix_authority (key_prefix),
          cursor ? std::optional<store_scan_cursor_t>{store_scan_cursor_t{
                     std::string (cursor->encoded ())}}
                 : std::nullopt,
          static_cast<std::uint32_t> (limit)};
        auto result = co_await _store.scan (std::move (scan_request));
        const auto *page = std::get_if<store_scan_page_t> (&result);
        if (!page)
            co_return authority_scan_result_t{authority_scan_expired_t{}};
        authority_page_t output;
        output.items.reserve (page->items.size ());
        for (const auto &item : page->items) {
            const auto key = authority_from_store_key (item.key);
            if (!key)
                continue;
            const auto &logical_key = key->value;
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
        auto result = co_await _store.read (key_creation_terminal (operation));
        const auto *found = std::get_if<store_found_t> (&result);
        if (!found)
            co_return std::optional<creation_terminal_record_t>{};
        if (!found->value.expires_at)
            throw std::invalid_argument ("creation terminal record is missing expiry");
        co_return std::optional<creation_terminal_record_t>{creation_terminal_record_t{
          operation, found->value.bytes,
          std::chrono::time_point_cast<std::chrono::milliseconds> (*found->value.expires_at)}};
    }

    task_t<object_reserve_result_t> reserve_core (object_reserve_request_t request,
                                                  std::stop_token cancellation = {}) override
    {
        object_reservation_fence_t fence;
        std::optional<store_version_t> reclaim_version;
        for (;;) {
            bool retry_reclaim = false;
            auto result = co_await await_result (
              reserve_once (request, cancellation, &retry_reclaim, fence, reclaim_version));
            if (!result)
                co_return std::move (result);

            if (!retry_reclaim)
                co_return std::move (result);
        }
    }

  private:
    static bool matches_store_version (const store_read_result_t &current,
                                       const std::optional<store_version_t> &original_version)
    {
        const auto *found = std::get_if<store_found_t> (&current);
        return original_version ? found && found->value.version.value == original_version->value
                                : std::holds_alternative<store_missing_t> (current);
    }

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
              co_await _store.read (key_creation_terminal (terminal->operation));
            if (std::holds_alternative<store_found_t> (completed))
                co_return false;
            if (terminal_store_now)
                *terminal_store_now = std::get<store_missing_t> (completed).store_now;
        }
        const auto current = co_await _store.read (std::move (key));
        const auto *found = std::get_if<store_found_t> (&current);
        const bool unchanged = matches_store_version (current, original_version);
        const auto store_now =
          found ? found->value.store_now : std::get<store_missing_t> (current).store_now;
        // The unchanged opaque version also preserves the reservation identity
        // checked before the operation constructs its conditional write. Direct
        // reserve/commit/abort calls use the epoch as the default (no deadline).
        if (!unchanged
            || (operation_deadline && *operation_deadline != decltype (store_now){}
                && store_now >= *operation_deadline))
            co_return false;
        co_return co_await owner_is_live_async (owner);
    }

    task_t<std::optional<framework_error_kind_t>>
    append_user_membership_conditions (const std::optional<actor_authority_payload_t> &old_member,
                                       const std::vector<std::byte> &payload,
                                       store_write_request_t &write_request)
    {
        const auto member = decode_direct_actor_authority_payload (payload);
        if (member && member->current_spot_kind == actor_authority_spot_kind_t::user
            && (!old_member || old_member->current_spot_kind != member->current_spot_kind
                || old_member->current_spot_id != member->current_spot_id
                || old_member->current_spot_generation != member->current_spot_generation)) {
            const auto spot_key =
              key_authority (spot_authority_key (member->current_spot_id).value);
            const auto spot_read = co_await _store.read (spot_key);
            const auto *spot_row = std::get_if<store_found_t> (&spot_read);
            if (!spot_row)
                co_return framework_error_kind_t::not_found;
            store_condition_t aggregate_condition;
            if (co_await authority_mutation_locked_async (
                  spot_authority_key (member->current_spot_id).value, &aggregate_condition))
                co_return framework_error_kind_t::unavailable;
            write_request.conditions.push_back (std::move (aggregate_condition));
            const auto spot = decode_authority (spot_row->value.bytes, spot_row->value.version,
                                                spot_row->value.store_now);
            const auto ready =
              stateful::authority_relocation_payload_codec_t::decode_ready_user_spot (spot);
            if (!ready || ready->spot_id != member->current_spot_id
                || spot.allocation.object_kind != placement_object_kind_t::user_spot
                || spot.allocation.state != placement_allocation_state_t::active
                || spot.object_generation != member->current_spot_generation)
                co_return framework_error_kind_t::unavailable;
            const auto owner_live = co_await owner_is_live_async (spot.owner);
            if (!owner_live)
                co_return framework_error_kind_t::unavailable;
            write_request.conditions.push_back (
              version_condition (spot_key, spot_row->value.version));
            auto required_owner = owner_condition (spot.owner);
            const auto &owner_value = std::get<store_value_condition_t> (required_owner);
            const auto existing_owner =
              std::find_if (write_request.conditions.begin (), write_request.conditions.end (),
                            [&] (const auto &condition) {
                                return std::visit (
                                  [&] (const auto &value) {
                                      if constexpr (std::is_same_v<std::decay_t<decltype (value)>,
                                                                   store_sum_at_most_condition_t>)
                                          return false;
                                      else
                                          return value.key.value == owner_value.key.value;
                                  },
                                  condition);
                            });
            if (existing_owner == write_request.conditions.end ())
                write_request.conditions.push_back (std::move (required_owner));
            else {
                const auto *existing_value =
                  std::get_if<store_value_condition_t> (&*existing_owner);
                if (!existing_value || existing_value->expected != owner_value.expected)
                    co_return framework_error_kind_t::unavailable;
            }
        }
        co_return std::nullopt;
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
                                                  object_reservation_fence_t &fence,
                                                  std::optional<store_version_t> &reclaim_version)
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
        auto authority = co_await _store.read (authority_key);
        const auto store_now = std::holds_alternative<store_found_t> (authority)
                                 ? std::get<store_found_t> (authority).value.store_now
                                 : std::get<store_missing_t> (authority).store_now;
        if (request.operation_deadline != std::chrono::system_clock::time_point{}
            && std::chrono::system_clock::now () >= request.operation_deadline)
            co_return detail::result_access_t::failure<object_reserve_result_t> (
              std::make_exception_ptr (framework_exception_t (
                framework_error_kind_t::deadline_exceeded, "Object creation deadline expired")));
        std::optional<authority_snapshot_t> decoded_current;
        if (const auto *found = std::get_if<store_found_t> (&authority)) {
            decoded_current =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (!authority_matches_creation_type (*decoded_current, request))
                co_return object_reserve_result_t{
                  object_type_mismatch_t{std::move (*decoded_current)}};
        }
        if (!decoded_current
            || decoded_current->allocation.state != placement_allocation_state_t::active) {
            const auto mutation_locked =
              co_await authority_mutation_locked_async (object_key (request.key));
            if (mutation_locked) {
                auto current = co_await read_authority_value_async (object_key (request.key));
                co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
            }
        }
        if (const auto *found = std::get_if<store_found_t> (&authority)) {
            auto current = std::move (*decoded_current);
            if (reclaim_version && found->value.version.value != reclaim_version->value)
                co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
            const auto reclaim = co_await try_reclaim_reserved_authority_async (
              authority_key, *found, current, &request, cancellation);
            if (reclaim == stale_authority_reclaim_result_t::reclaimed
                || reclaim == stale_authority_reclaim_result_t::conflict) {
                reclaim_version = reclaim == stale_authority_reclaim_result_t::conflict
                                    ? std::make_optional (found->value.version)
                                    : std::nullopt;
                *retry_reclaim = true;
                co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
            }
            if (current.allocation.state == placement_allocation_state_t::active)
                co_return object_reserve_result_t{object_already_exists_t{std::move (current)}};
            co_return object_reserve_result_t{object_reserve_conflict_t{std::move (current)}};
        }

        if (reclaim_version)
            co_return object_reserve_result_t{
              object_reserve_conflict_t{authority_missing_t{store_now}}};

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
        store_write_request_t write_request{
          {missing_condition (authority_key),
           version_condition (target->key, target->provider_version),
           owner_condition (creation_target.owner)},
          {}};
        const auto object_generation =
          co_await issue_generation (authority_key, generation_kind_t::object, write_request);
        const auto owner_generation =
          co_await issue_generation (authority_key, generation_kind_t::owner, write_request);
        if (!object_generation || !owner_generation)
            co_return object_reserve_result_t{authority_generation_exhausted_t{}};

        // The authority write assigns the opaque version.  The returned
        // fence receives it after the write; pendingCreation is the durable
        // reservation identity while the row remains reserved.
        if (fence.reservation_id.empty ())
            fence.reservation_id = detail::new_uuid_v4 ();
        fence.expected_store_version = aggregate_status::pending;
        fence.object_generation = *object_generation;
        fence.authority_owner_generation = *owner_generation;
        fence.target = creation_target;
        fence.capacity_bundle = request.capacity_bundle;
        authority_snapshot_t creating{
          aggregate_status::pending,
          request.creating_payload,
          *object_generation,
          *owner_generation,
          creation_target.owner,
          {},
          {placement_allocation_state_t::reserved, request.key.kind, request.intent.stable_type,
           creation_target, request.capacity_bundle},
          pending_object_creation_t{
            fence.reservation_id, request.intent.request_content_reference,
            request.intent.request_sha256,
            static_cast<std::uint32_t> (request.intent.request_encoded_size)}};
        write_request.mutations.insert (
          write_request.mutations.begin (),
          store_put_t{authority_key, encode_authority (creating), std::nullopt});
        capacity_changes_t capacity;
        if (!capacity.reserve (creation_target, request.capacity_bundle, target->descriptor))
            co_return object_reserve_result_t{object_placement_capacity_exhausted_t{}};
        capacity.append_to (write_request);
        auto written = co_await write_async (std::move (write_request));
        const auto *applied = std::get_if<store_write_applied_t> (&written);
        if (!applied) {
            *retry_reclaim = co_await conflict_qualification_unchanged (
              authority_key, std::nullopt, creation_target.owner, request.operation_deadline);
            if (*retry_reclaim && capacity.limited ()) {
                const auto qualified = co_await read_target_descriptor_async (creation_target);
                *retry_reclaim = qualified
                                 && target_accepts (qualified->descriptor, request.key.kind,
                                                    request.intent.stable_type);
                if (*retry_reclaim && qualified->provider_version == target->provider_version) {
                    const auto available = co_await capacity_still_available (capacity);
                    if (!available) {
                        *retry_reclaim = false;
                        co_return object_reserve_result_t{object_placement_capacity_exhausted_t{}};
                    }
                }
            }
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
    try_reclaim_reserved_authority_async (store_key_t authority_key,
                                          store_found_t stored_authority,
                                          authority_snapshot_t current,
                                          const object_reserve_request_t *request = nullptr,
                                          std::stop_token cancellation = {},
                                          const location_owner_token_t *required_owner = nullptr,
                                          const authority_snapshot_t *discarded = nullptr)
    {
        const bool active = current.allocation.state == placement_allocation_state_t::active;
        if (discarded && (!active || !discarded->same_incarnation (current)))
            co_return stale_authority_reclaim_result_t::recovery_required;
        if (active && !request && !discarded)
            co_return stale_authority_reclaim_result_t::recovery_required;
        // Relocation authority retains its own recovery protocol.
        const auto actor = decode_direct_actor_authority_payload (current.payload);
        if (!active && (current.aggregate || (actor && actor->has_relocation_state)))
            co_return stale_authority_reclaim_result_t::recovery_required;
        const auto owner_key = key_owner (current.owner.owner_id);
        auto owner = co_await _store.read (owner_key);
        auto stale_owner_condition = missing_condition (owner_key);
        bool owner_live = false;
        if (const auto *found = std::get_if<store_found_t> (&owner)) {
            const auto lease = decode_owner_lease (*found);
            if (lease.token.owner_id != current.owner.owner_id)
                throw framework_exception_t (framework_error_kind_t::data_lost,
                                             "Location Store owner lease record is invalid");
            owner_live = owner_lease_is_live (lease, current.owner);
            stale_owner_condition = version_condition (owner_key, found->value.version);
        }
        if (required_owner && (!owner_live || !same_owner (current.owner, *required_owner)))
            co_return stale_authority_reclaim_result_t::recovery_required;
        std::vector<authority_entry_t> members;
        std::optional<store_condition_t> authority_lock_condition;
        std::vector<store_condition_t> member_lock_conditions;
        if (active) {
            if (owner_live)
                co_return stale_authority_reclaim_result_t::owner_live;
            const auto mutation_locked = co_await authority_mutation_locked_async (
              (request ? object_key (request->key)
                       : authority_from_store_key (authority_key)->value),
              &authority_lock_condition.emplace ());
            if (current.aggregate || (actor && actor->has_relocation_state) || mutation_locked)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Object authority requires relocation recovery");
            if (!discarded
                && request->relocation_policy != detail::factory_relocation_kind_t::disabled)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Object owner lease is unavailable");
            if (!discarded && current.allocation.object_kind == placement_object_kind_t::actor
                && (!actor || actor->state != actor_authority_state_t::ready
                    || actor->actor_id != request->key.global_id
                    || actor->stable_type != request->intent.stable_type))
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Actor is not steady Ready");
            if (!discarded
                && current.allocation.object_kind == placement_object_kind_t::user_spot) {
                const auto spot =
                  stateful::authority_relocation_payload_codec_t::decode_ready_user_spot (current);
                if (!spot || spot->spot_id != request->key.global_id
                    || spot->stable_type != request->intent.stable_type)
                    throw framework_exception_t (framework_error_kind_t::unavailable,
                                                 "User Spot is not steady Ready");
                // Location runtime §6.1: read every membership of this incarnation from one
                // complete snapshot; an expired snapshot discards earlier pages and restarts.
                std::optional<authority_scan_cursor_t> cursor;
                do {
                    if (request->operation_deadline != std::chrono::system_clock::time_point{}
                        && std::chrono::system_clock::now () >= request->operation_deadline)
                        throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                                     "User Spot membership scan deadline elapsed");
                    auto scanned = co_await list_authorities ("", cursor, location_page_item_limit,
                                                              cancellation);
                    const auto *page = std::get_if<authority_page_t> (&scanned);
                    if (!page) {
                        cursor.reset ();
                        members.clear ();
                        member_lock_conditions.clear ();
                        continue;
                    }
                    for (const auto &entry : page->items) {
                        const auto member =
                          decode_direct_actor_authority_payload (entry.snapshot.payload);
                        if (!member
                            || member->current_spot_kind != actor_authority_spot_kind_t::user
                            || member->current_spot_id != spot->spot_id
                            || member->current_spot_generation != current.object_generation)
                            continue;
                        // A relocating, aggregate-locked or unactivated member keeps the Spot.
                        bool member_unavailable =
                          entry.snapshot.allocation.state != placement_allocation_state_t::active
                          || member->state != actor_authority_state_t::ready
                          || member->has_relocation_state || entry.snapshot.aggregate;
                        store_condition_t lock_condition;
                        if (!member_unavailable)
                            member_unavailable = co_await authority_mutation_locked_async (
                              entry.key.value, &lock_condition);
                        if (member_unavailable)
                            throw framework_exception_t (
                              framework_error_kind_t::unavailable,
                              "User Spot still has a relocating or unactivated member");
                        members.push_back (entry);
                        member_lock_conditions.push_back (std::move (lock_condition));
                    }
                    cursor = page->next_cursor;
                    if (!cursor)
                        break;
                } while (true);
            } else if (!discarded
                       && current.allocation.object_kind
                            == placement_object_kind_t::instance_spot) {
                const auto spot = decode_instance_spot_authority_payload (current.payload);
                if (!spot || spot->state != instance_spot_authority_state_t::ready
                    || spot->activation_recovery || spot->spot_id != request->key.global_id
                    || spot->stable_type != request->intent.stable_type)
                    throw framework_exception_t (framework_error_kind_t::unavailable,
                                                 "Instance Spot is not steady Ready");
            }
        }
        const bool wait_elapsed = !active || authority_release_wait_elapsed (current.owner);
        if (!wait_elapsed) {
            const auto observed = co_await _store.read (authority_key);
            if (!matches_store_version (observed, stored_authority.value.version))
                co_return stale_authority_reclaim_result_t::conflict;
        }
        const auto descriptor_key = key_mesh (
          current.allocation.target.mesh_name,
          zlink::routing_id_t::from (std::string (current.allocation.target.node_rid.value ())));
        const auto descriptor_read = co_await _store.read (descriptor_key);
        auto descriptor_condition = missing_condition (descriptor_key);
        if (const auto *found = std::get_if<store_found_t> (&descriptor_read)) {
            const auto record = parse_canonical_record (found->value.bytes, "MeshNode descriptor");
            const auto descriptor =
              decode_mesh_descriptor (record.at (location_record_fields::descriptor));
            if (owner_live
                && descriptor.lifecycle_generation
                     == current.allocation.target.node_lifecycle_generation)
                co_return stale_authority_reclaim_result_t::owner_live;
            descriptor_condition = version_condition (descriptor_key, found->value.version);
        }

        if (!active && !current.pending_creation)
            co_return stale_authority_reclaim_result_t::recovery_required;

        store_write_request_t write_request{
          {version_condition (authority_key, stored_authority.value.version),
           std::move (stale_owner_condition), std::move (descriptor_condition)},
          {store_delete_t{authority_key}}};
        if (authority_lock_condition)
            write_request.conditions.push_back (std::move (*authority_lock_condition));
        for (auto &condition : member_lock_conditions)
            write_request.conditions.push_back (std::move (condition));
        // §6.1: each ended-owner member is released in the same batch, fenced by its
        // authority version and the observed (invalid) state of its own owner lease.
        const auto *spot_owner = std::get_if<store_found_t> (&owner);
        std::map<std::string, std::optional<store_found_t>> member_owners{
          {current.owner.owner_id,
           spot_owner ? std::optional<store_found_t>{*spot_owner} : std::nullopt}};
        bool members_wait_elapsed = true;
        for (const auto &member : members) {
            const auto member_key = key_authority (member.key.value);
            const auto &member_owner = member.snapshot.owner;
            if (!member_owners.contains (member_owner.owner_id)) {
                const auto member_owner_key = key_owner (member_owner.owner_id);
                auto lease_read = co_await _store.read (member_owner_key);
                const auto *found = std::get_if<store_found_t> (&lease_read);
                write_request.conditions.push_back (
                  found ? version_condition (member_owner_key, found->value.version)
                        : missing_condition (member_owner_key));
                member_owners.emplace (member_owner.owner_id,
                                       found ? std::optional<store_found_t>{*found} : std::nullopt);
            }
            if (const auto &found = member_owners.at (member_owner.owner_id)) {
                const auto lease = decode_owner_lease (*found);
                if (lease.token.owner_id != member_owner.owner_id)
                    throw framework_exception_t (framework_error_kind_t::data_lost,
                                                 "Location Store owner lease record is invalid");
                if (owner_lease_is_live (lease, member_owner))
                    throw framework_exception_t (
                      framework_error_kind_t::unavailable,
                      "User Spot still has a member with a live owner lease");
            }
            if (!authority_release_wait_elapsed (member_owner)) {
                const auto observed = co_await _store.read (member_key);
                if (!matches_store_version (observed,
                                            store_version_t{member.snapshot.store_version}))
                    co_return stale_authority_reclaim_result_t::conflict;
                members_wait_elapsed = false;
            }
            write_request.conditions.push_back (
              version_condition (member_key, store_version_t{member.snapshot.store_version}));
            write_request.mutations.push_back (store_delete_t{member_key});
        }
        if (discarded && !wait_elapsed)
            co_return stale_authority_reclaim_result_t::recovery_required;
        if (!wait_elapsed || !members_wait_elapsed)
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Owner lease release observation is still waiting");
        capacity_changes_t capacity;
        capacity.change (current.allocation.target, current.allocation.capacity_bundle,
                         active ? 0 : -1, active ? -1 : 0);
        for (const auto &member : members)
            capacity.change (member.snapshot.allocation.target,
                             member.snapshot.allocation.capacity_bundle, 0, -1);
        capacity.append_to (write_request);
        if (!batch_fits (write_request))
            throw framework_exception_t (
              framework_error_kind_t::unavailable,
              "User Spot member release exceeds one Location Store batch");
        const auto result = co_await await_result (write_async (std::move (write_request)));
        if (request)
            record_instance_takeover (
              *request, !result ? "failed"
                        : std::holds_alternative<store_write_applied_t> (result.value ())
                          ? "claimed"
                          : "lost");
        const auto written = result.value ();
        co_return std::holds_alternative<store_write_applied_t> (written)
          ? stale_authority_reclaim_result_t::reclaimed
          : stale_authority_reclaim_result_t::conflict;
    }

  public:
    task_t<bool> release_ended_reservation (
      authority_key_t key,
      std::string expected_store_version,
      std::stop_token cancellation = {},
      std::optional<object_reserve_request_t> request = std::nullopt,
      std::optional<authority_snapshot_t> discarded = std::nullopt) override
    {
        return release_ended_reservation (std::move (key), std::move (expected_store_version),
                                          cancellation, std::move (request), std::nullopt,
                                          std::move (discarded));
    }

  private:
    task_t<bool>
    release_ended_reservation (authority_key_t key,
                               std::string expected_store_version,
                               std::stop_token cancellation,
                               std::optional<object_reserve_request_t> request,
                               std::optional<location_owner_token_t> required_owner,
                               std::optional<authority_snapshot_t> discarded = std::nullopt)
    {
        const auto row_key = key_authority (key.value);
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<bool> ();
            if (request && request->operation_deadline != std::chrono::system_clock::time_point{}
                && std::chrono::system_clock::now () >= request->operation_deadline)
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "Object authority release deadline expired");
            const auto read = co_await _store.read (row_key);
            const auto *found = std::get_if<store_found_t> (&read);
            if (!found || found->value.version.value != expected_store_version)
                co_return false;
            const auto current =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (request && !authority_matches_creation_type (current, *request))
                throw framework_exception_t (framework_error_kind_t::type_mismatch,
                                             "Object type does not match the current authority");
            if (current.allocation.state != placement_allocation_state_t::active) {
                const auto locked = co_await authority_mutation_locked_async (key.value);
                if (locked)
                    co_return false;
            }
            const auto reclaimed = co_await try_reclaim_reserved_authority_async (
              row_key, *found, current, request ? &*request : nullptr, cancellation,
              required_owner ? &*required_owner : nullptr, discarded ? &*discarded : nullptr);
            if (reclaimed == stale_authority_reclaim_result_t::conflict)
                continue;
            co_return reclaimed == stale_authority_reclaim_result_t::reclaimed;
        }
    }

  public:
    task_t<object_complete_creation_result_t>
    complete_creation_core (object_complete_creation_request_t request,
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
        auto existing = co_await _store.read (terminal_key);
        if (const auto *found = std::get_if<store_found_t> (&existing)) {
            if (!found->value.expires_at)
                throw std::invalid_argument ("creation terminal record is missing expiry");
            std::optional<authority_snapshot_t> ready;
            if (std::holds_alternative<object_creation_completed_t> (request.completion))
                ready = creation_replay_ready (
                  request, co_await read_authority_core (authority_key_t{object_key (request.key)},
                                                         cancellation));
            co_return object_complete_creation_result_t{object_creation_already_completed_result_t{
              creation_terminal_record_t{
                publication.operation, found->value.bytes,
                std::chrono::time_point_cast<std::chrono::milliseconds> (*found->value.expires_at)},
              std::move (ready)}};
        }

        const auto store_now = std::get<store_missing_t> (existing).store_now;
        if (expires_at <= store_now)
            throw std::invalid_argument ("creation terminal expiry is not in the future");
        creation_terminal_record_t terminal{publication.operation, publication.terminal_envelope,
                                            expires_at};
        object_complete_creation_result_t result{object_creation_completion_stale_t{}};
        std::optional<authority_snapshot_t> observed_ready;
        if (const auto *created = std::get_if<object_creation_completed_t> (&request.completion)) {
            object_commit_request_t commit_request{request.key, request.fence,
                                                   created->ready_payload};
            const auto committed =
              co_await commit_core (std::move (commit_request), cancellation, &terminal, store_now,
                                    publication.operation_deadline);
            if (const auto *value = std::get_if<object_committed_t> (&committed))
                co_return object_complete_creation_result_t{
                  object_creation_completed_result_t{std::move (terminal), value->ready}};
            if (const auto *already = std::get_if<object_already_committed_t> (&committed))
                observed_ready = creation_replay_ready (request, already->ready);
            if (const auto *conflict = std::get_if<object_commit_conflict_t> (&committed)) {
                observed_ready = creation_replay_ready (request, conflict->current);
                result = object_creation_completion_conflict_t{conflict->current};
            } else if (std::holds_alternative<authority_generation_exhausted_t> (committed))
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
        auto concurrent = co_await _store.read (terminal_key);
        if (const auto *found = std::get_if<store_found_t> (&concurrent)) {
            if (!found->value.expires_at)
                throw std::invalid_argument ("creation terminal record is missing expiry");
            auto ready = std::move (observed_ready);
            if (!ready && std::holds_alternative<object_creation_completed_t> (request.completion))
                ready = creation_replay_ready (
                  request, co_await read_authority_core (authority_key_t{object_key (request.key)},
                                                         cancellation));
            co_return object_complete_creation_result_t{object_creation_already_completed_result_t{
              creation_terminal_record_t{
                publication.operation, found->value.bytes,
                std::chrono::time_point_cast<std::chrono::milliseconds> (*found->value.expires_at)},
              std::move (ready)}};
        }
        co_return result;
    }

    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
                 std::stop_token cancellation = {},
                 std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        return commit_core (std::move (request), cancellation, nullptr, {}, operation_deadline);
    }

    task_t<object_abort_result_t>
    abort (object_abort_request_t request,
           std::stop_token cancellation = {},
           std::chrono::system_clock::time_point operation_deadline = {}) override
    {
        return abort (std::move (request), cancellation, nullptr, {}, operation_deadline);
    }

  private:
    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
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
            auto authority = co_await _store.read (authority_key);
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
            auto target = co_await read_target_descriptor_async (request.fence.target, false);
            if (!target)
                co_return object_commit_result_t{object_commit_conflict_t{std::move (snapshot)}};
            store_write_request_t write_request{
              {version_condition (authority_key, stored_authority->value.version),
               owner_condition (request.fence.target.owner),
               version_condition (target->key, target->provider_version)},
              {}};
            if (snapshot.allocation.object_kind == placement_object_kind_t::actor) {
                const auto membership_failure = co_await append_user_membership_conditions (
                  std::nullopt, request.ready_payload, write_request);
                if (membership_failure)
                    co_return object_commit_result_t{
                      object_commit_conflict_t{std::move (snapshot)}};
            }
            snapshot.payload = std::move (request.ready_payload);
            snapshot.allocation.state = placement_allocation_state_t::active;
            snapshot.pending_creation.reset ();
            write_request.mutations.push_back (
              store_put_t{authority_key, encode_authority (snapshot), std::nullopt});
            capacity_changes_t capacity;
            capacity.change (snapshot.allocation.target, snapshot.allocation.capacity_bundle, -1,
                             1);
            capacity.append_to (write_request);
            if (operation_deadline != std::chrono::system_clock::time_point{}
                && std::chrono::system_clock::now () >= operation_deadline)
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "Object creation deadline expired");
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
                                         std::chrono::system_clock::time_point operation_deadline,
                                         std::chrono::steady_clock::time_point deadline =
                                           std::chrono::steady_clock::time_point::max ())
    {
        for (;;) {
            if (cancellation.stop_requested ())
                co_return detail::result_access_t::failure<object_abort_result_t> (
                  detail::make_cancellation_exception ("location store operation was cancelled"));
            const auto authority_key = key_authority (object_key (request.key));
            auto authority = co_await _store.read (authority_key);
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
            store_write_request_t write_request{
              {version_condition (authority_key, stored_authority->value.version),
               owner_condition (request.fence.target.owner),
               version_condition (target->key, target->provider_version)},
              {store_delete_t{authority_key}}};
            capacity_changes_t capacity;
            capacity.change (snapshot.allocation.target, snapshot.allocation.capacity_bundle, -1,
                             0);
            capacity.append_to (write_request);
            auto written = co_await write_async (std::move (write_request), terminal,
                                                 terminal_store_now, "compare_exchange", deadline);
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
    prepare_aggregate_core (aggregate_prepare_request_t request,
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
            auto row = co_await _store.read (key);
            const auto *found = std::get_if<store_found_t> (&row);
            if (!found)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            const auto snapshot =
              decode_authority (found->value.bytes, found->value.version, found->value.store_now);
            if (snapshot.store_version != participant.expected_store_version
                || snapshot.allocation.state != placement_allocation_state_t::active)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            authorities.emplace_back (key, *found);
            if (participant.owner_transition == authority_generation_transition_t::preserve)
                continue;
            if (participant.owner_transition != authority_generation_transition_t::new_owner)
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
        }
        if (encode_bundle (inventory) != encode_bundle (request.capacity_bundle))
            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        const auto row_key = key_aggregate (request.aggregate_id);
        auto current = co_await _store.read (row_key);
        store_version_t preparing_version;
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
            preparing_version = found->value.version;
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
            const auto *applied = std::get_if<store_write_applied_t> (&claimed);
            if (!applied)
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            preparing_version = {version_of (*applied, row_key)};
        }

        const auto encoded =
          encode_aggregate (request, aggregate_status::prepared, *inventory_tree);
        std::size_t participant_offset = 0;
        std::size_t page_index = 0;
        for (const auto &page : inventory_tree->pages) {
            const auto page_key = key_aggregate_inventory (request.aggregate_id, page_index++);
            auto page_current = co_await _store.read (page_key);
            if (const auto *found = std::get_if<store_found_t> (&page_current)) {
                if (found->value.bytes != page.encoded)
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
            } else {
                store_write_request_t page_request;
                page_request.conditions.push_back (missing_condition (page_key));
                page_request.conditions.push_back (version_condition (row_key, preparing_version));
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
            constexpr auto lock_batch_size = (location_store_limits::write_keys - 1) / 2;
            for (std::size_t lock_offset = 0; lock_offset < page.participants.size ();
                 lock_offset += lock_batch_size) {
                const auto lock_end =
                  std::min (lock_offset + lock_batch_size, page.participants.size ());
                store_write_request_t lock_request;
                lock_request.conditions.push_back (version_condition (row_key, preparing_version));
                for (std::size_t index = lock_offset; index < lock_end; ++index) {
                    const auto participant_index = participant_offset + index;
                    const auto lock_key =
                      key_aggregate_lock (request.participants[participant_index].key.value);
                    auto existing_lock = co_await _store.read (lock_key);
                    if (const auto *found = std::get_if<store_found_t> (&existing_lock)) {
                        const auto lock = decode_aggregate_lock (found->value.bytes);
                        if (!lock
                            || lock->authority_key
                                 != request.participants[participant_index].key.value)
                            co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                        if (lock->aggregate_id.value == request.aggregate_id.value
                            && lock->aggregate_generation == request.aggregate_generation) {
                            if (lock->expected_store_version
                                != request.participants[participant_index].expected_store_version)
                                co_return aggregate_prepare_result_t{
                                  aggregate_prepare_conflict_t{}};
                            continue;
                        }
                        if (co_await aggregate_lock_holds_async (*lock))
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
                    for (std::size_t index = lock_offset; index < lock_end; ++index) {
                        const auto participant_index = participant_offset + index;
                        const auto &participant = request.participants[participant_index];
                        const auto raced_lock =
                          co_await _store.read (key_aggregate_lock (participant.key.value));
                        const auto *raced_found = std::get_if<store_found_t> (&raced_lock);
                        const auto raced = raced_found
                                             ? decode_aggregate_lock (raced_found->value.bytes)
                                             : std::nullopt;
                        if (!raced || raced->aggregate_id.value != request.aggregate_id.value
                            || raced->aggregate_generation != request.aggregate_generation
                            || raced->authority_key != participant.key.value
                            || raced->expected_store_version
                                 != participant.expected_store_version) {
                            adopted = false;
                            break;
                        }
                    }
                    if (!adopted) {
                        const auto raced_aggregate = co_await _store.read (row_key);
                        const auto *raced_found = std::get_if<store_found_t> (&raced_aggregate);
                        if (raced_found) {
                            const auto raced = parse_json (raced_found->value.bytes);
                            const auto status = raced.value (location_record_fields::status, "");
                            if (aggregate_record_matches_request (raced, request, *inventory_tree)
                                && (status == aggregate_status::preparing
                                    || status == aggregate_status::prepared))
                                co_return co_await prepare_aggregate_core (std::move (request),
                                                                           cancellation);
                        }
                        co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                    }
                }
            }

            participant_offset += page.participants.size ();
        }

        for (const auto &index_page : inventory_tree->index_pages) {
            const auto index_key = key_aggregate_inventory_index (
              request.aggregate_id, index_page.level, index_page.page_index);
            auto current_index = co_await _store.read (index_key);
            if (const auto *found = std::get_if<store_found_t> (&current_index)) {
                if (found->value.bytes != index_page.encoded)
                    co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
                continue;
            }
            store_write_request_t index_request;
            index_request.conditions.push_back (missing_condition (index_key));
            index_request.conditions.push_back (version_condition (row_key, preparing_version));
            index_request.mutations.push_back (
              store_put_t{index_key, index_page.encoded, std::nullopt});
            if (!std::holds_alternative<store_write_applied_t> (
                  co_await write_async (std::move (index_request))))
                co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
        }

        const object_creation_target_t target{
          request.target_descriptor.mesh_name,
          node_rid_t::from_string (raw_rid (request.target_descriptor.rid)),
          request.target_descriptor_lifecycle_generation, request.target_owner};
        auto target_descriptor = co_await read_target_descriptor_async (target);
        const aggregate_fence_t reservation{request.aggregate_id, request.aggregate_generation,
                                            request.inventory_digest};
        while (target_descriptor) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<aggregate_prepare_result_t> ();
            auto preparing_row = co_await _store.read (row_key);
            const auto *preparing_found = std::get_if<store_found_t> (&preparing_row);
            if (!preparing_found)
                co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
            const auto preparing_record = parse_json (preparing_found->value.bytes);
            const auto status = preparing_record.value (location_record_fields::status, "");
            if (status != aggregate_status::preparing) {
                if (status == aggregate_status::prepared || status == aggregate_status::committing
                    || status == aggregate_status::committed)
                    co_return aggregate_prepare_result_t{aggregate_already_prepared_t{
                      {request.aggregate_id, request.aggregate_generation,
                       request.inventory_digest}}};
                co_return aggregate_prepare_result_t{aggregate_prepare_stale_t{}};
            }
            store_write_request_t write_request{
              {version_condition (row_key, preparing_found->value.version),
               version_condition (target_descriptor->key, target_descriptor->provider_version),
               owner_condition (request.target_owner)},
              {store_put_t{row_key, to_bytes (encoded.dump ()), std::nullopt}}};
            capacity_changes_t capacity;
            if (!capacity.reserve (target, request.capacity_bundle, target_descriptor->descriptor))
                break;
            capacity.append_to (write_request);
            auto written = co_await write_async (std::move (write_request));
            if (std::holds_alternative<store_write_applied_t> (written))
                co_return aggregate_prepare_result_t{aggregate_prepared_t{
                  {request.aggregate_id, request.aggregate_generation, request.inventory_digest}}};
            const auto current = co_await _store.read (row_key);
            if (!matches_store_version (current, preparing_found->value.version))
                continue;
            bool authority_qualified = true;
            for (const auto &[key, expected] : authorities)
                if (!matches_store_version (co_await _store.read (key), expected.value.version)) {
                    authority_qualified = false;
                    break;
                }
            if (!authority_qualified)
                break;
            auto qualified = co_await read_target_descriptor_async (target);
            if (!qualified)
                break;
            if (qualified->provider_version == target_descriptor->provider_version) {
                const auto available = co_await capacity_still_available (capacity);
                if (!available)
                    break;
            }
            target_descriptor = std::move (qualified);
        }
        (void) co_await abort_aggregate (reservation);
        co_return aggregate_prepare_result_t{aggregate_prepare_conflict_t{}};
    }

    task_t<aggregate_commit_result_t> commit_aggregate (aggregate_fence_t fence,
                                                        std::stop_token cancellation = {}) override
    {
        const auto row_key = key_aggregate (fence.aggregate_id);
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<aggregate_commit_result_t> ();
            auto current = co_await _store.read (row_key);
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
              decode_aggregate_target_rid (record),
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
                  co_await _store.read (key_aggregate_inventory (fence.aggregate_id, page_index));
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
                  co_await _store.read (key_aggregate_inventory (fence.aggregate_id, page_index));
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
                const auto index = co_await _store.read (key_aggregate_inventory_index (
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

            std::vector<aggregate_commit_entry_t> entries;
            entries.reserve (participants.size ());
            std::map<std::string, stored_target_t> descriptors;
            descriptors.emplace (target_descriptor->key.value, *target_descriptor);
            capacity_changes_t capacity;
            capacity.change (
              target, decode_bundle (record.at (location_record_fields::capacityBundle)), -1, 1);
            std::vector<store_write_request_t> staged_generations (participants.size ());

            for (std::size_t participant_index = 0; participant_index < participants.size ();
                 ++participant_index) {
                const auto &participant = participants[participant_index];
                const auto authority_key = key_authority (participant.key.value);
                auto authority = co_await _store.read (authority_key);
                const auto *found = std::get_if<store_found_t> (&authority);
                if (!found)
                    co_return aggregate_commit_result_t::stale;
                auto lock_result =
                  co_await _store.read (key_aggregate_lock (participant.key.value));
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

                const bool new_owner =
                  participant.owner_transition == authority_generation_transition_t::new_owner;
                aggregate_commit_entry_t entry;
                if (lock->status == aggregate_status::committing) {
                    if (!lock->page_index || !lock->entry_index)
                        co_return aggregate_commit_result_t::stale;
                    auto page = co_await _store.read (
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
                    if (before.store_version != participant.expected_store_version)
                        co_return aggregate_commit_result_t::stale;
                    auto after = before;
                    if (new_owner) {
                        const auto generation =
                          co_await issue_generation (authority_key, generation_kind_t::owner,
                                                     staged_generations[participant_index]);
                        if (!generation)
                            co_return aggregate_commit_result_t::generation_exhausted;
                        after.authority_owner_generation = *generation;
                        after.owner = target_owner;
                        after.allocation.target = target;
                        after.aggregate =
                          authority_aggregate_codec::without_capture (before.aggregate);
                    }
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
                if (new_owner)
                    capacity.change (before.allocation.target, before.allocation.capacity_bundle, 0,
                                     -1);
                entries.push_back (std::move (entry));
            }

            record[location_record_fields::status] = aggregate_status::committed;
            store_write_request_t final_request{
              {version_condition (row_key, stored->value.version)},
              {store_put_t{row_key, to_bytes (record.dump ()), std::nullopt}}};
            if (target_descriptor->owner_present)
                final_request.conditions.push_back (owner_condition (target_owner));
            for (const auto &[descriptor_key, descriptor] : descriptors)
                final_request.conditions.push_back (
                  version_condition (descriptor.key, descriptor.provider_version));
            capacity.append_to (final_request);

            const auto commit_pages = split_aggregate_commit_entries (entries);
            if (!commit_pages)
                co_return aggregate_commit_result_t::stale;
            record[location_record_fields::commitPageCount] = commit_pages->size ();
            // Validate the actual final write before staging authority pages.
            std::get<store_put_t> (final_request.mutations.front ()).bytes =
              to_bytes (record.dump ());
            if (!batch_fits (final_request))
                co_return aggregate_commit_result_t::stale;
            auto committing_version = stored->value.version;
            if (status == aggregate_status::prepared) {
                record[location_record_fields::status] = aggregate_status::committing;
                store_write_request_t transition_request{
                  {version_condition (row_key, stored->value.version)},
                  {store_put_t{row_key, to_bytes (record.dump ()), std::nullopt}}};
                auto transition = co_await write_async (std::move (transition_request));
                const auto *applied = std::get_if<store_write_applied_t> (&transition);
                if (!applied)
                    continue;
                committing_version = {version_of (*applied, row_key)};
            }

            std::size_t page_index = 0;
            std::size_t offset = 0;
            for (; page_index < commit_pages->size ();
                 offset += (*commit_pages)[page_index++].size ()) {
                const auto page_key = key_aggregate_commit_page (fence.aggregate_id, page_index);
                const auto encoded_page = to_bytes (
                  encode_aggregate_commit_page (page_index, (*commit_pages)[page_index]).dump ());
                auto existing_page = co_await _store.read (page_key);
                if (const auto *found_page = std::get_if<store_found_t> (&existing_page)) {
                    if (found_page->value.bytes != encoded_page)
                        co_return aggregate_commit_result_t::stale;
                    continue;
                }
                store_write_request_t page_request;
                page_request.conditions.push_back (missing_condition (page_key));
                page_request.conditions.push_back (version_condition (row_key, committing_version));
                std::size_t entry_index = 0;
                for (; entry_index < (*commit_pages)[page_index].size (); ++entry_index) {
                    const auto participant_index = offset + entry_index;
                    const auto &participant = participants[participant_index];
                    const auto &entry = (*commit_pages)[page_index][entry_index];
                    auto authority = co_await _store.read (key_authority (participant.key.value));
                    const auto *found_authority = std::get_if<store_found_t> (&authority);
                    auto lock_result =
                      co_await _store.read (key_aggregate_lock (participant.key.value));
                    const auto *found_lock = std::get_if<store_found_t> (&lock_result);
                    if (!found_authority || !found_lock)
                        co_return aggregate_commit_result_t::stale;
                    const auto lock = decode_aggregate_lock (found_lock->value.bytes);
                    if (!lock || lock->status != aggregate_status::prepared
                        || found_authority->value.bytes != entry.before
                        || found_authority->value.version.value
                             != participant.expected_store_version)
                        break;
                    auto &generation = staged_generations[participant_index];
                    page_request.conditions.insert (page_request.conditions.end (),
                                                    generation.conditions.begin (),
                                                    generation.conditions.end ());
                    page_request.mutations.insert (page_request.mutations.end (),
                                                   generation.mutations.begin (),
                                                   generation.mutations.end ());
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
                if (entry_index != (*commit_pages)[page_index].size ())
                    break;
                page_request.mutations.push_back (
                  store_put_t{page_key, encoded_page, std::nullopt});
                if (!batch_fits (page_request))
                    co_return aggregate_commit_result_t::stale;
                if (!std::holds_alternative<store_write_applied_t> (
                      co_await write_async (std::move (page_request))))
                    break;
            }
            if (page_index != commit_pages->size ())
                continue;

            final_request.conditions.front () = version_condition (row_key, committing_version);
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
        auto current = co_await _store.read (key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return aggregate_abort_result_t::stale;
        auto record = parse_json (found->value.bytes);
        if (record.at (location_record_fields::aggregateGeneration).get<std::uint64_t> ()
            != fence.aggregate_generation)
            co_return aggregate_abort_result_t::stale;
        const auto status = record.value (location_record_fields::status, "");
        const bool already_aborted = status == aggregate_status::aborted;
        if (!already_aborted && status != aggregate_status::preparing
            && status != aggregate_status::prepared && status != aggregate_status::committing)
            co_return aggregate_abort_result_t::stale;
        if (!already_aborted) {
            record[location_record_fields::status] = aggregate_status::aborted;
            store_write_request_t abort_request{
              {version_condition (key, found->value.version)},
              {store_put_t{key, to_bytes (record.dump ()), std::nullopt}}};
            if (status != aggregate_status::preparing) {
                const object_creation_target_t target{
                  record.at (location_record_fields::targetMeshName).get<std::string> (),
                  decode_aggregate_target_rid (record),
                  record.at (location_record_fields::targetLifecycleGeneration)
                    .get<std::uint64_t> (),
                  decode_owner (record.at (location_record_fields::targetOwner))};
                capacity_changes_t capacity;
                capacity.change (target,
                                 decode_bundle (record.at (location_record_fields::capacityBundle)),
                                 -1, 0);
                capacity.append_to (abort_request);
            }
            if (!std::holds_alternative<store_write_applied_t> (
                  co_await write_async (std::move (abort_request))))
                co_return aggregate_abort_result_t::stale;
        }
        const auto commit_page_count =
          record.value (location_record_fields::commitPageCount, std::size_t{0});
        if (commit_page_count != 0) {
            for (std::size_t page_index = 0; page_index < commit_page_count; ++page_index) {
                auto page =
                  co_await _store.read (key_aggregate_commit_page (fence.aggregate_id, page_index));
                const auto *stored_page = std::get_if<store_found_t> (&page);
                if (!stored_page)
                    continue;
                const auto entries =
                  decode_aggregate_commit_page (stored_page->value.bytes, page_index);
                if (!entries)
                    co_return aggregate_abort_result_t::stale;
                store_write_request_t rollback;
                for (std::size_t entry_index = 0; entry_index < entries->size (); ++entry_index) {
                    const auto &entry = (*entries)[entry_index];
                    const auto lock_key = key_aggregate_lock (entry.authority_key);
                    const auto lock = co_await _store.read (lock_key);
                    const auto *stored_lock = std::get_if<store_found_t> (&lock);
                    if (!stored_lock)
                        continue;
                    const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                    if (!decoded_lock || decoded_lock->authority_key != entry.authority_key)
                        co_return aggregate_abort_result_t::stale;
                    // Another recovery may already have removed our lock and
                    // allowed the participant to join a new aggregate.
                    if (decoded_lock->aggregate_id.value != fence.aggregate_id.value
                        || decoded_lock->aggregate_generation != fence.aggregate_generation)
                        continue;
                    if (decoded_lock->status != aggregate_status::committing
                        || decoded_lock->page_index != page_index
                        || decoded_lock->entry_index != entry_index)
                        co_return aggregate_abort_result_t::stale;
                    const auto authority =
                      co_await _store.read (key_authority (entry.authority_key));
                    const auto *stored_authority = std::get_if<store_found_t> (&authority);
                    if (!stored_authority
                        || (stored_authority->value.bytes != entry.before
                            && stored_authority->value.bytes != entry.after))
                        co_return aggregate_abort_result_t::stale;
                    rollback.conditions.push_back (
                      version_condition (lock_key, stored_lock->value.version));
                    rollback.mutations.push_back (store_delete_t{lock_key});
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

        const auto inventory_page_count =
          record.value (location_record_fields::inventoryPageCount, std::size_t{0});
        const auto singleton = record.value (location_record_fields::inventoryCount, 0u) == 1;
        for (std::size_t page_index = 0; !singleton && page_index < inventory_page_count;
             ++page_index) {
            auto page =
              co_await _store.read (key_aggregate_inventory (fence.aggregate_id, page_index));
            const auto *stored_page = std::get_if<store_found_t> (&page);
            if (!stored_page) {
                // A preparing aggregate claims its row before the first
                // child page. Missing pages after that point cannot own a
                // lock, so the durable claim can still be marked aborted
                // without inventing participant keys.
                if (status == aggregate_status::preparing || already_aborted)
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
                auto lock = co_await _store.read (lock_key);
                if (const auto *stored_lock = std::get_if<store_found_t> (&lock)) {
                    const auto decoded_lock = decode_aggregate_lock (stored_lock->value.bytes);
                    if (!decoded_lock || decoded_lock->authority_key != participant.key.value)
                        co_return aggregate_abort_result_t::stale;
                    if (decoded_lock->aggregate_id.value != fence.aggregate_id.value
                        || decoded_lock->aggregate_generation != fence.aggregate_generation)
                        continue;
                    if (decoded_lock->status == aggregate_status::committing)
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
        co_return already_aborted ? aggregate_abort_result_t::already_aborted
                                  : aggregate_abort_result_t::aborted;
    }

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (aggregate_fence_t fence,
                                 std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            co_return co_await cancelled<std::optional<std::vector<aggregate_participant_t>>> ();
        try {
            const auto current = co_await _store.read (key_aggregate (fence.aggregate_id));
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
                  co_await _store.read (key_aggregate_inventory (fence.aggregate_id, page_index));
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
                  co_await _store.read (key_aggregate_inventory (fence.aggregate_id, page_index));
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
                const auto index = co_await _store.read (key_aggregate_inventory_index (
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
        return remove_all_by_owner (std::move (owner),
                                    std::chrono::steady_clock::time_point::max ());
    }

    task_t<std::int64_t>
    remove_all_by_owner (location_owner_token_t owner,
                         std::chrono::steady_clock::time_point deadline) override
    {
        const auto check_deadline = [&] {
            if (std::chrono::steady_clock::now () >= deadline)
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "owner cleanup deadline expired");
        };
        std::vector<store_scan_item_t> rows;
        std::optional<store_scan_cursor_t> cursor;
        do {
            check_deadline ();
            auto scan =
              co_await _store.scan ({prefix_authority (), cursor, location_page_item_limit});
            const auto *page = std::get_if<store_scan_page_t> (&scan);
            if (!page)
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Location Store scan cursor expired");
            rows.insert (rows.end (), page->items.begin (), page->items.end ());
            cursor = page->next_cursor;
        } while (cursor);
        std::int64_t removed = 0;
        for (const auto &row : rows) {
            const auto snapshot =
              decode_authority (row.value.bytes, row.value.version, row.value.store_now);
            if (!same_owner (snapshot.owner, owner))
                continue;
            const auto logical = authority_from_store_key (row.key);
            if (!logical)
                continue;
            check_deadline ();
            if (snapshot.allocation.state == placement_allocation_state_t::active) {
                auto result = co_await compare_exchange_authority_core (
                  *logical, snapshot.store_version, authority_delete_t{}, {}, deadline);
                if (std::holds_alternative<authority_deleted_t> (result))
                    ++removed;
            } else if (snapshot.pending_creation) {
                const auto decoded =
                  authority_key_codec_detail::decode_authority_key (logical->value).value ();
                object_abort_request_t request{
                  {snapshot.allocation.object_kind, decoded.object_id},
                  {snapshot.pending_creation->reservation_id, snapshot.store_version,
                   snapshot.object_generation, snapshot.authority_owner_generation,
                   snapshot.allocation.target, snapshot.allocation.capacity_bundle}};
                auto result = co_await abort (std::move (request), {}, nullptr, {}, {}, deadline);
                if (std::holds_alternative<object_aborted_t> (result))
                    ++removed;
                else if (std::holds_alternative<object_abort_conflict_t> (result)) {
                    const auto released = co_await release_ended_reservation (
                      *logical, snapshot.store_version, {}, std::nullopt, owner);
                    if (released)
                        ++removed;
                }
            }
        }
        co_return removed;
    }

  private:
    class store_format_t;

    using json_t = nlohmann::json;
    static constexpr std::string_view prefix = "zlink:v11:";
    static constexpr std::string_view aggregate_prefix = "zlink:v11:aggregate:";
    static constexpr std::uint64_t max_generation = std::numeric_limits<std::int64_t>::max ();
    inline static const store_key_t counter_key{std::string (prefix) + "owner-counter"};
    inline static const store_key_t generation_floor_key{"zlink:v12:generation-floor"};

    enum class generation_kind_t
    {
        object,
        owner
    };

    struct stored_target_t
    {
        store_key_t key;
        std::string provider_version;
        bool owner_present;
        mesh_node_descriptor_t descriptor;
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

    task_t<std::optional<aggregate_lock_t>>
    read_aggregate_lock_async (std::string authority_key,
                               store_condition_t *observed_condition = nullptr)
    {
        auto key = key_aggregate_lock (authority_key);
        auto value = co_await _store.read (observed_condition ? key : std::move (key));
        const auto *found = std::get_if<store_found_t> (&value);
        if (observed_condition)
            *observed_condition =
              found ? version_condition (key, found->value.version) : missing_condition (key);
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
        auto aggregate = co_await _store.read (key_aggregate (lock->aggregate_id));
        if (const auto *aggregate_found = std::get_if<store_found_t> (&aggregate)) {
            const auto aggregate_record = parse_json (aggregate_found->value.bytes);
            if (aggregate_record.value (location_record_fields::status, "")
                == aggregate_status::committed)
                co_return raw;
        }
        if (!lock->page_index || !lock->entry_index)
            co_return raw;
        auto page =
          co_await _store.read (key_aggregate_commit_page (lock->aggregate_id, *lock->page_index));
        const auto *found = std::get_if<store_found_t> (&page);
        if (!found)
            co_return std::nullopt;
        const auto entries = decode_aggregate_commit_page (found->value.bytes);
        if (!entries || *lock->entry_index >= entries->size ())
            co_return std::nullopt;
        const auto &entry = (*entries)[*lock->entry_index];
        if (entry.authority_key != authority_key)
            co_return std::nullopt;
        co_return decode_authority (entry.before, store_version_t{lock->expected_store_version},
                                    store_now);
    }

    task_t<bool> authority_mutation_locked_async (std::string authority_key,
                                                  store_condition_t *observed_condition = nullptr)
    {
        const auto lock = co_await read_aggregate_lock_async (authority_key, observed_condition);
        if (!lock)
            co_return false;
        co_return co_await aggregate_lock_holds_async (*lock);
    }

    task_t<bool> aggregate_lock_holds_async (aggregate_lock_t lock)
    {
        if (lock.status != aggregate_status::prepared
            && lock.status != aggregate_status::committing)
            co_return false;
        const auto aggregate = co_await _store.read (key_aggregate (lock.aggregate_id));
        const auto *found = std::get_if<store_found_t> (&aggregate);
        if (!found)
            co_return true;
        const auto record = parse_json (found->value.bytes);
        if (record.value (location_record_fields::aggregateGeneration, std::uint64_t{0})
            != lock.aggregate_generation)
            co_return true;
        const auto status = record.value (location_record_fields::status, "");
        if (status == aggregate_status::committed)
            co_return false;
        if (status != aggregate_status::aborted)
            co_return true;

        // The terminal CAS returned capacity. Resume its existing rollback
        // and reject this attempt, which may have read the staged authority.
        aggregate_fence_t fence{lock.aggregate_id, lock.aggregate_generation, std::nullopt};
        (void) co_await abort_aggregate (fence);
        co_return true;
    }

    task_t<store_write_result_t>
    write_async (store_write_request_t request,
                 const creation_terminal_record_t *terminal = nullptr,
                 std::chrono::system_clock::time_point terminal_store_now = {},
                 const char *operation = "compare_exchange",
                 std::chrono::steady_clock::time_point deadline =
                   std::chrono::steady_clock::time_point::max ())
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
        if (std::chrono::steady_clock::now () >= deadline)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         "Store cleanup deadline expired");
        auto result = co_await await_result (_store.write_as (request, operation));
        if (!result) {
            if (auto applied = co_await reconcile_write_async (request, deadline))
                co_return store_write_result_t{std::move (*applied)};
            if (result.error () != nullptr
                && detail::is_transient_error (result.error ()->kind ())) {
                if (std::chrono::steady_clock::now () >= deadline)
                    throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                                 "Store cleanup deadline expired");
                result = co_await await_result (_store.write_as (request, operation));
                if (!result)
                    if (auto applied = co_await reconcile_write_async (request, deadline))
                        co_return store_write_result_t{std::move (*applied)};
            }
        }
        const auto &written = result.value ();
        if (std::holds_alternative<store_write_corrupt_t> (written))
            throw framework_exception_t (framework_error_kind_t::data_lost,
                                         "Location Store write reported corrupt data");
        if (_capacity_metrics.enabled ()
            && std::holds_alternative<store_write_applied_t> (written)) {
            std::set<std::string> scopes;
            for (const auto &mutation : request.mutations) {
                const auto *add = std::get_if<store_add_t> (&mutation);
                if (!add)
                    continue;
                const auto scope = capacity_scope_from_key (add->key.value);
                if (scope && _capacity_metrics.contains (*scope))
                    scopes.insert (*scope);
            }
            for (const auto &scope : scopes)
                (void) co_await await_result (confirm_capacity (scope));
        }
        co_return written;
    }

    task_t<std::optional<store_write_applied_t>>
    reconcile_write_async (const store_write_request_t &request,
                           std::chrono::steady_clock::time_point deadline)
    {
        if (std::all_of (
              request.mutations.begin (), request.mutations.end (),
              [] (const auto &mutation) { return std::holds_alternative<store_add_t> (mutation); }))
            co_return std::nullopt;
        store_write_applied_t applied;
        for (const auto &mutation : request.mutations) {
            if (std::holds_alternative<store_add_t> (mutation))
                continue;
            const auto key = std::visit ([] (const auto &value) { return value.key; }, mutation);
            if (std::chrono::steady_clock::now () >= deadline)
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "Store cleanup deadline expired");
            auto observed = co_await await_result (_store.read (key));
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

    static std::uint64_t read_counter (const std::vector<std::byte> &bytes, bool generation)
    {
        const auto value = parse_location_counter (to_string (bytes));
        if (!value || (generation && *value == 0))
            throw framework_exception_t (framework_error_kind_t::data_lost,
                                         "Location Store counter is invalid");
        return static_cast<std::uint64_t> (*value);
    }

    task_t<std::uint64_t> generation_floor ()
    {
        const auto cached = _generation_floor.load (std::memory_order_relaxed);
        if (cached != 0)
            co_return cached;
        const auto row = co_await _store.read (generation_floor_key);
        const auto *found = std::get_if<store_found_t> (&row);
        co_return found ? read_counter (found->value.bytes, true) : std::uint64_t{1};
    }

    task_t<std::optional<std::uint64_t>> issue_generation (const store_key_t &authority,
                                                           generation_kind_t kind,
                                                           store_write_request_t &write_request)
    {
        const auto domain = kind == generation_kind_t::object ? "zlink:v12:object-counter"
                                                              : "zlink:v12:authority-owner-counter";
        // The authority store key already contains the canonical kind and public ID.
        const store_key_t key{domain + authority.value.substr (authority_domain.size ())};
        const auto counter = co_await _store.read (key);
        const auto *found = std::get_if<store_found_t> (&counter);
        std::uint64_t next;
        if (found)
            next = read_counter (found->value.bytes, true);
        else
            next = co_await generation_floor ();
        if (next == max_generation)
            co_return std::nullopt;
        write_request.conditions.push_back (condition_for (key, counter));
        write_request.mutations.push_back (
          store_put_t{key, to_bytes (std::to_string (next + 1)), std::nullopt});
        co_return next;
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

    static std::optional<authority_key_t> authority_from_store_key (const store_key_t &key)
    {
        const auto &value = key.value;
        const auto domain_end = authority_domain.size ();
        if (value.size () <= domain_end + 1 || value.compare (0, domain_end, authority_domain) != 0
            || value[domain_end] != '\0')
            return std::nullopt;
        const auto kind_start = domain_end + 1;
        const auto kind_end = value.find ('\0', kind_start);
        if (kind_end == std::string::npos)
            return std::nullopt;
        const auto kind = value.substr (kind_start, kind_end - kind_start);
        const auto id = value.substr (kind_end + 1);
        if (kind == "actor")
            return actor_authority_key (id);
        if (kind == "spot")
            return spot_authority_key (id);
        return std::nullopt;
    }

    static std::string prefix_authority (std::string_view key_prefix = {})
    {
        if (key_prefix.empty ())
            return preimage ({authority_domain, ""});
        // A kind prefix is the codec prefix followed by one kind byte and ':'.
        const auto &codec_prefix = authority_key_codec_detail::prefix;
        if (key_prefix.size () != codec_prefix.size () + 2 || !key_prefix.starts_with (codec_prefix)
            || key_prefix.back () != ':')
            throw std::invalid_argument ("unsupported authority scan prefix");
        return preimage (
          {authority_domain, authority_kind_name (key_prefix[codec_prefix.size ()]), ""});
    }

    static std::string_view authority_kind_name (char kind)
    {
        if (kind == 'a')
            return "actor";
        if (kind == 's')
            return "spot";
        throw std::invalid_argument ("unsupported authority kind");
    }

    static store_key_t key_authority (std::string_view value)
    {
        const auto decoded = authority_key_codec_detail::decode_authority_key (value);
        if (!decoded)
            throw std::invalid_argument ("authority store key requires a valid identity");
        return {
          preimage ({authority_domain, authority_kind_name (decoded->kind), decoded->object_id})};
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
        return {std::string (aggregate_prefix) + hex (id.value)};
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

    static std::string raw_rid (const zlink::routing_id_t &rid)
    {
        return {reinterpret_cast<const char *> (rid.data ()), rid.size ()};
    }

    static constexpr std::string_view capacity_domain = "zlink:v12:capacity";

    static std::string capacity_prefix (std::string_view mesh_name, std::string_view rid_hex)
    {
        return preimage ({capacity_domain, mesh_name, rid_hex, ""});
    }

    static std::optional<std::string> capacity_scope_from_key (std::string_view key)
    {
        const auto prefix_length = capacity_domain.size () + 1;
        if (!key.starts_with (capacity_domain) || key.size () <= prefix_length
            || key[capacity_domain.size ()] != '\0')
            return std::nullopt;
        const auto mesh_end = key.find ('\0', prefix_length);
        const auto rid_end = key.find ('\0', mesh_end + 1);
        return std::string (key.substr (0, rid_end + 1));
    }

    static std::string capacity_counter_key (std::string_view scope,
                                             std::string_view dimension,
                                             std::string_view state)
    {
        return std::string (scope) + std::string (dimension) + '\0' + std::string (state);
    }

    static std::string capacity_scope (const object_creation_target_t &target)
    {
        return capacity_prefix (target.mesh_name, hex (to_bytes (target.node_rid.value ())));
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
        const auto read_sequence = reclaim_read_sequence ();
        const bool mesh = record.at (location_record_fields::descriptor)
                            .contains (location_record_fields::meshName);
        const auto lease_key = key_owner (owner_id);
        auto lease = co_await _store.read (lease_key);
        const auto *live_lease = std::get_if<store_found_t> (&lease);
        if (!live_lease || owner_generation (live_lease->value.bytes) != lease_generation)
            co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};

        auto current = co_await _store.read (row_key);
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
        location_owner_token_t observed_owner;
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
            observed_owner = {stored_owner, stored_lease};
            const auto previous_owner = co_await _store.read (key_owner (stored_owner));
            const auto *previous_lease = std::get_if<store_found_t> (&previous_owner);
            const auto previous_owner_live =
              previous_lease
              && owner_lease_is_live (decode_owner_lease (*previous_lease),
                                      {stored_owner, stored_lease});
            if (intent == location_write_intent_t::new_claim
                || intent == location_write_intent_t::fixed_rid_new_claim
                || (intent == location_write_intent_t::renew && mesh)) {
                auto previous = stored;
                auto requested = record;
                previous.at (location_record_fields::descriptor)
                  .erase (location_record_fields::updatedAt);
                previous.at (location_record_fields::descriptor)
                  .erase (location_record_fields::updatedAtEpochMs);
                requested.at (location_record_fields::descriptor)
                  .erase (location_record_fields::updatedAt);
                requested.at (location_record_fields::descriptor)
                  .erase (location_record_fields::updatedAtEpochMs);
                if (previous == requested) {
                    retain_reclaim_observation (row_key.value, std::nullopt, read_sequence);
                    co_return location_write_result_t::stored (
                      static_cast<std::int64_t> (provider_generation_known ? provider_generation
                                                                           : 1),
                      found->value.store_now);
                }
            }
            if ((intent == location_write_intent_t::new_claim
                 || intent == location_write_intent_t::fixed_rid_new_claim)
                && previous_owner_live) {
                retain_reclaim_observation (row_key.value, std::nullopt, read_sequence);
                co_return location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}};
            }
            if (intent == location_write_intent_t::fixed_rid_new_claim
                && !reclaim_wait_elapsed (row_key.value, {stored_owner, stored_lease}, false,
                                          read_sequence)) {
                const auto observed = co_await _store.read (row_key);
                if (!matches_store_version (observed, found->value.version))
                    co_return location_write_result_t{
                      location_write_status_t::ignored_stale, 0, {}};
                co_return location_write_result_t{location_write_status_t::waiting, 0, {}};
            }
            if (intent == location_write_intent_t::takeover && previous_owner_live)
                co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
            if (intent == location_write_intent_t::renew) {
                if (stored_owner != owner_id
                    || (mesh ? stored_lease > lease_generation : stored_lease != lease_generation)
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
            if (intent == location_write_intent_t::fixed_rid_new_claim) {
                if (!reclaim_wait_elapsed (row_key.value, {}, false, read_sequence))
                    co_return location_write_result_t{location_write_status_t::waiting, 0, {}};
            } else
                retain_reclaim_observation (row_key.value, std::nullopt, read_sequence);
            if (intent == location_write_intent_t::renew)
                co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
            row_condition = missing_condition (row_key);
        }
        auto encoded = to_bytes (record.dump ());
        store_write_request_t write_request{
          {owner_condition ({owner_id, lease_generation}), std::move (row_condition)},
          {store_put_t{row_key, std::move (encoded), std::nullopt}}};
        auto result = co_await write_async (std::move (write_request));
        if (const auto *applied = std::get_if<store_write_applied_t> (&result)) {
            if (observed_owner.owner_id != owner_id
                || observed_owner.lease_generation != lease_generation)
                remove_reclaim_observation (row_key.value, observed_owner);
            co_return location_write_result_t::stored (static_cast<std::int64_t> (generation),
                                                       applied->store_now);
        }
        co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
    }

    task_t<location_write_status_t>
    remove_descriptor (store_key_t row_key,
                       location_owner_token_t owner,
                       std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::time_point::max ())
    {
        const auto read_sequence = reclaim_read_sequence ();
        auto current = co_await _store.read (row_key);
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found) {
            remove_reclaim_observation (row_key.value, owner, read_sequence);
            co_return location_write_status_t::ignored_stale;
        }
        const auto record =
          parse_canonical_record (found->value.bytes, location_record_fields::descriptor);
        if (record_owner_id (record) != owner.owner_id
            || record_lease_generation (record) != owner.lease_generation)
            co_return location_write_status_t::ignored_stale;
        store_write_request_t result_request{{version_condition (row_key, found->value.version)},
                                             {store_delete_t{row_key}}};
        auto result = co_await write_async (std::move (result_request), nullptr, {},
                                            "compare_exchange", deadline);
        if (std::holds_alternative<store_write_applied_t> (result)) {
            remove_reclaim_observation (row_key.value, owner);
            co_return location_write_status_t::stored;
        }
        co_return location_write_status_t::ignored_stale;
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
        auto result = co_await _store.scan (std::move (request));
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

    static bool owner_lease_is_live (const owner_lease_found_t &lease,
                                     const location_owner_token_t &owner)
    {
        return same_owner (lease.token, owner) && lease.lease_expires_at > lease.store_now;
    }

    task_t<bool> owner_is_live_async (location_owner_token_t owner)
    {
        co_return (co_await read_live_owner_async (std::move (owner))).has_value ();
    }

    task_t<std::optional<store_found_t>> read_live_owner_async (location_owner_token_t owner)
    {
        auto current = co_await _store.read (key_owner (owner.owner_id));
        const auto *found = std::get_if<store_found_t> (&current);
        if (!found)
            co_return std::nullopt;
        const auto lease = decode_owner_lease (*found);
        if (lease.token.owner_id != owner.owner_id)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Location Store owner lease record is invalid");
        if (!owner_lease_is_live (lease, owner))
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
        json_t encoded{
          {location_record_fields::recordVersion, 1},
          {location_record_fields::payload, base64_encode (value.payload)},
          {location_record_fields::objectGeneration, generation_string (value.object_generation)},
          {location_record_fields::authorityOwnerGeneration,
           generation_string (value.authority_owner_generation)},
          {location_record_fields::ownerId, value.owner.owner_id},
          {location_record_fields::ownerLeaseGeneration,
           generation_string (value.owner.lease_generation)},
          {location_record_fields::allocation, encode_allocation (value.allocation)},
          {location_record_fields::pendingCreation, encode_pending (value.pending_creation)}};
        if (value.aggregate)
            encoded["aggregate"] = value.aggregate->encoded;
        return to_bytes (encoded.dump ());
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
                decode_pending (value.at (location_record_fields::pendingCreation)),
                value.contains ("aggregate")
                  ? authority_aggregate_codec::read (value.at ("aggregate"))
                  : nullptr};
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

    task_t<authority_compare_exchange_result_t>
    authority_conflict (store_read_result_t current,
                        std::optional<framework_error_kind_t> membership_failure = std::nullopt)
    {
        if (const auto *found = std::get_if<store_found_t> (&current))
            return completed (authority_compare_exchange_result_t{authority_conflict_t{
              decode_authority (found->value.bytes, found->value.version, found->value.store_now),
              membership_failure}});
        return completed (authority_compare_exchange_result_t{authority_conflict_t{
          authority_missing_t{std::get<store_missing_t> (current).store_now}, membership_failure}});
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
               {location_record_fields::limit, item.spot_limit},
               {location_record_fields::entryAccepted, item.entry_accepted}});
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
                   && lhs.spot_limit == rhs.spot_limit && lhs.entry_accepted == rhs.entry_accepted;
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
        auto row = co_await _store.read (row_key);
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
        const auto owner = co_await _store.read (key_owner (owner_id));
        const auto *live = std::get_if<store_found_t> (&owner);
        if (require_live
            && (!live || owner_generation (live->value.bytes) != descriptor.lease_generation))
            co_return std::nullopt;
        co_return stored_target_t{row_key, found->value.version.value, live != nullptr,
                                  std::move (descriptor)};
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

    static std::string capacity_type_dimension (placement_object_kind_t kind,
                                                std::string_view stable_type)
    {
        return preimage ({"spot-type", object_kind3_name (kind), stable_type});
    }

    class capacity_changes_t
    {
      public:
        void change (const object_creation_target_t &target,
                     const placement_capacity_bundle_t &bundle,
                     std::int64_t pending,
                     std::int64_t active)
        {
            for_each_dimension (bundle, [&] (std::string_view dimension, std::uint32_t slots) {
                add (target, dimension, slots, pending, active, 0);
            });
        }

        bool reserve (const object_creation_target_t &target,
                      const placement_capacity_bundle_t &bundle,
                      const mesh_node_descriptor_t &descriptor)
        {
            std::int32_t spot_type_limit = 0;
            if (bundle.spot_type) {
                const auto &type = *bundle.spot_type;
                const auto &types = descriptor.capacity.spot_types;
                const auto entry =
                  std::find_if (types.begin (), types.end (), [&] (const auto &item) {
                      return item.object_kind == type.object_kind
                             && item.stable_type == type.stable_type;
                  });
                if (entry == types.end ())
                    return false;
                spot_type_limit = entry->usage.limit;
            }
            for_each_dimension (bundle, [&] (std::string_view dimension, std::uint32_t slots) {
                const auto limit = [&] {
                    if (dimension == "actors")
                        return descriptor.capacity.actors.limit;
                    if (dimension == "spots")
                        return descriptor.capacity.spots.limit;
                    return spot_type_limit;
                }();
                add (target, dimension, slots, 1, 0, limit);
            });
            return true;
        }

        bool limited () const { return !limits.empty (); }

        void append_to (store_write_request_t &request) const
        {
            request.conditions.insert (request.conditions.end (), limits.begin (), limits.end ());
            for (const auto &[key, delta] : deltas)
                if (delta != 0)
                    request.mutations.push_back (store_add_t{{key}, delta});
        }

        store_write_request_t recheck () const { return {limits, {}}; }

      private:
        template <typename Visit>
        static void for_each_dimension (const placement_capacity_bundle_t &bundle, Visit visit)
        {
            if (bundle.actor_slots != 0)
                visit ("actors", bundle.actor_slots);
            if (bundle.spot_slots != 0)
                visit ("spots", bundle.spot_slots);
            if (bundle.spot_type && bundle.spot_type->slots != 0)
                visit (capacity_type_dimension (bundle.spot_type->object_kind,
                                                bundle.spot_type->stable_type),
                       bundle.spot_type->slots);
        }

        void add (const object_creation_target_t &target,
                  std::string_view dimension,
                  std::uint32_t slots,
                  std::int64_t pending,
                  std::int64_t active,
                  std::int32_t limit)
        {
            const auto scope = capacity_scope (target);
            const auto active_key = capacity_counter_key (scope, dimension, "active");
            const auto pending_key = capacity_counter_key (scope, dimension, "pending");
            if (limit != 0)
                limits.push_back (
                  store_sum_at_most_condition_t{{{active_key}, {pending_key}}, slots, limit});
            if (active != 0)
                deltas[active_key] += active * slots;
            if (pending != 0)
                deltas[pending_key] += pending * slots;
        }

        std::vector<store_condition_t> limits;
        std::map<std::string, std::int64_t> deltas;
    };

    task_t<bool> capacity_still_available (const capacity_changes_t &changes)
    {
        if (!changes.limited ())
            co_return true;
        const auto result = co_await write_async (changes.recheck ());
        co_return std::holds_alternative<store_write_applied_t> (result);
    }

    task_t<authority_compare_exchange_result_t>
    publish_retarget (const authority_key_t &authority,
                      const store_key_t &authority_key,
                      const store_version_t &authority_version,
                      authority_snapshot_t target,
                      const placement_allocation_t &source,
                      stored_target_t descriptor,
                      store_write_request_t commit,
                      std::stop_token cancellation,
                      std::chrono::steady_clock::time_point deadline)
    {
        const auto &placement = target.allocation.target;
        aggregate_prepare_request_t reservation;
        reservation.aggregate_generation = target.object_generation;
        reservation.participants.push_back ({authority,
                                             authority_version.value,
                                             authority_generation_transition_t::new_owner,
                                             {},
                                             {}});
        reservation.target_descriptor = {placement.mesh_name, descriptor.descriptor.rid};
        reservation.target_descriptor_lifecycle_generation = placement.node_lifecycle_generation;
        reservation.target_owner = placement.owner;
        reservation.capacity_bundle = source.capacity_bundle;
        const auto inventory = aggregate_inventory::build_tree (reservation.participants);
        if (!inventory)
            co_return co_await authority_conflict (co_await _store.read (authority_key));
        reservation.inventory_digest.value = inventory->root;
        // Use the existing aggregate reservation record and inventory encoding.
        // The source version identifies the attempt and fences its final write.
        const auto digest = runtime::sha256 (to_bytes (
          encode_aggregate (reservation, aggregate_status::prepared, *inventory).dump ()));
        std::copy_n (digest.begin (), reservation.aggregate_id.value.size (),
                     reservation.aggregate_id.value.begin ());
        const auto row_key = key_aggregate (reservation.aggregate_id);
        const auto page_key = key_aggregate_inventory (reservation.aggregate_id, 0);
        auto prepared = encode_aggregate (reservation, aggregate_status::prepared, *inventory);
        const auto prepared_bytes = to_bytes (prepared.dump ());
        store_version_t version;
        for (;;) {
            if (cancellation.stop_requested ())
                co_return co_await cancelled<authority_compare_exchange_result_t> ();
            const auto current = co_await _store.read (row_key);
            if (const auto *found = std::get_if<store_found_t> (&current)) {
                if (found->value.bytes != prepared_bytes)
                    co_return co_await authority_conflict (co_await _store.read (authority_key));
                version = found->value.version;
                break;
            }
            store_write_request_t reserve{
              {missing_condition (row_key), missing_condition (page_key),
               version_condition (authority_key, authority_version),
               version_condition (descriptor.key, descriptor.provider_version),
               owner_condition (placement.owner)},
              {store_put_t{row_key, prepared_bytes, std::nullopt},
               store_put_t{page_key, inventory->pages.front ().encoded, std::nullopt}}};
            capacity_changes_t capacity;
            if (!capacity.reserve (placement, source.capacity_bundle, descriptor.descriptor))
                co_return co_await authority_conflict (co_await _store.read (authority_key));
            capacity.append_to (reserve);
            const auto result =
              co_await write_async (std::move (reserve), nullptr, {}, "compare_exchange", deadline);
            if (const auto *applied = std::get_if<store_write_applied_t> (&result)) {
                version = {version_of (*applied, row_key)};
                break;
            }
            if (!matches_store_version (co_await _store.read (row_key), std::nullopt))
                continue;
            const auto current_authority = co_await _store.read (authority_key);
            if (!matches_store_version (current_authority, authority_version))
                co_return co_await authority_conflict (current_authority);
            auto qualified = co_await read_target_descriptor_async (placement);
            if (!qualified
                || !target_accepts (qualified->descriptor, source.object_kind, source.stable_type))
                co_return co_await authority_conflict (current_authority);
            if (qualified->provider_version == descriptor.provider_version) {
                const auto available = co_await capacity_still_available (capacity);
                if (!available)
                    co_return co_await authority_conflict (current_authority);
            }
            descriptor = std::move (*qualified);
        }

        prepared[location_record_fields::status] = aggregate_status::committed;
        commit.conditions.push_back (version_condition (row_key, version));
        commit.mutations.push_back (
          store_put_t{row_key, to_bytes (prepared.dump ()), std::nullopt});
        capacity_changes_t capacity;
        capacity.change (source.target, source.capacity_bundle, 0, -1);
        capacity.change (placement, source.capacity_bundle, -1, 1);
        capacity.append_to (commit);
        for (;;) {
            const auto qualified = co_await read_target_descriptor_async (placement, false);
            if (!qualified || cancellation.stop_requested ()) {
                (void) co_await abort_aggregate ({reservation.aggregate_id,
                                                  reservation.aggregate_generation,
                                                  reservation.inventory_digest});
                co_return co_await authority_conflict (co_await _store.read (authority_key));
            }
            auto attempt = commit;
            attempt.conditions.push_back (
              version_condition (qualified->key, qualified->provider_version));
            const auto written = co_await await_result (
              write_async (std::move (attempt), nullptr, {}, "compare_exchange", deadline));
            if (const auto *applied =
                  written ? std::get_if<store_write_applied_t> (&written.value ()) : nullptr) {
                target.store_now = applied->store_now;
                target.store_version = version_of (*applied, authority_key);
                co_return authority_compare_exchange_result_t{
                  authority_stored_t{std::move (target)}};
            }
            const auto durable = co_await _store.read (row_key);
            const auto current = co_await _store.read (authority_key);
            // A lost reply is resolved by the reservation's committed state
            // and the target authority, independently of the shared counters.
            if (matches_store_version (durable, version)) {
                if (written && matches_store_version (current, authority_version)) {
                    const auto unchanged = co_await retarget_fences_unchanged (commit.conditions);
                    if (unchanged)
                        continue;
                }
                (void) co_await abort_aggregate ({reservation.aggregate_id,
                                                  reservation.aggregate_generation,
                                                  reservation.inventory_digest});
            } else if (const auto *stored = std::get_if<store_found_t> (&current);
                       stored && stored->value.bytes == encode_authority (target)) {
                const auto *record = std::get_if<store_found_t> (&durable);
                if (record
                    && parse_json (record->value.bytes).value (location_record_fields::status, "")
                         == aggregate_status::committed) {
                    target.store_version = stored->value.version.value;
                    target.store_now = stored->value.store_now;
                    co_return authority_compare_exchange_result_t{
                      authority_stored_t{std::move (target)}};
                }
            }
            if (!written)
                co_return detail::result_access_t::failure<authority_compare_exchange_result_t> (
                  written.exception ());
            co_return co_await authority_conflict (current);
        }
    }

    task_t<bool> retarget_fences_unchanged (const std::vector<store_condition_t> &conditions)
    {
        for (const auto &condition : conditions) {
            if (const auto *version = std::get_if<store_version_condition_t> (&condition)) {
                const auto observed = co_await _store.read (version->key);
                if (!matches_store_version (observed, version->expected))
                    co_return false;
            } else if (const auto *missing = std::get_if<store_missing_condition_t> (&condition)) {
                const auto observed = co_await _store.read (missing->key);
                if (!matches_store_version (observed, std::nullopt))
                    co_return false;
            } else if (const auto *value = std::get_if<store_value_condition_t> (&condition)) {
                const auto observed = co_await _store.read (value->key);
                const auto *found = std::get_if<store_found_t> (&observed);
                if (!found || found->value.bytes != value->expected)
                    co_return false;
            }
        }
        co_return true;
    }

    static bool batch_fits (const store_write_request_t &request)
    {
        try {
            validate_location_write (request, [] (std::string_view key) {
                if (key.empty () || key.size () > location_store_limits::key_bytes)
                    throw std::invalid_argument ("Location Store key exceeds batch limits");
            });
            return true;
        }
        catch (const std::invalid_argument &) {
            return false;
        }
    }

    task_t<authority_read_result_t> read_authority_value_async (std::string key)
    {
        auto current = co_await _store.read (key_authority (key));
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

    static node_rid_t decode_aggregate_target_rid (const json_t &record)
    {
        const auto rid = zlink::routing_id_t::from_hex (
          record.at (location_record_fields::targetNodeRid).get<std::string> ());
        return node_rid_t::from_string (raw_rid (rid));
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
                {location_record_fields::targetNodeRid, request.target_descriptor.rid.to_hex ()},
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
               item.at (location_record_fields::limit).get<std::int32_t> (),
               item.value (location_record_fields::entryAccepted, false)});
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

    void record_instance_takeover (const object_reserve_request_t &request,
                                   const char *outcome) const
    {
        runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled () && request.key.kind == placement_object_kind_t::instance_spot)
            metrics.counter ("zlink.instance_spot.takeovers", "{takeover}", 1,
                             {{"mesh_name", request.target.mesh_name},
                              {"instance_spot_type", request.intent.stable_type},
                              {"outcome", outcome}});
    }
    void record_store_error (const char *operation) const
    {
        runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled ())
            metrics.counter ("zlink.location.store.errors", "{error}", 1,
                             {{"operation", operation}});
    }
    using capacity_snapshot_t = std::map<std::string, std::uint64_t>;

    task_t<capacity_snapshot_t> read_capacity (const std::string &scope)
    {
        for (;;) {
            capacity_snapshot_t counters;
            std::optional<store_scan_cursor_t> cursor;
            do {
                store_scan_request_t request{scope, cursor, location_page_item_limit};
                auto result = co_await _store.scan (std::move (request));
                const auto *page = std::get_if<store_scan_page_t> (&result);
                if (!page)
                    break;
                for (const auto &entry : page->items)
                    counters.emplace (entry.key.value, read_counter (entry.value.bytes, false));
                cursor = page->next_cursor;
                if (!cursor)
                    co_return counters;
            } while (cursor);
        }
    }

    static void project_capacity (const std::string &scope,
                                  placement_capacity_t &value,
                                  const capacity_snapshot_t &counters)
    {
        const auto lookup = [&] (const std::string &key) {
            const auto found = counters.find (key);
            return found == counters.end () ? std::uint64_t{0} : found->second;
        };
        const auto project = [&] (capacity_usage_t &usage, std::string_view dimension) {
            usage.active = lookup (capacity_counter_key (scope, dimension, "active"));
            usage.reserved = lookup (capacity_counter_key (scope, dimension, "pending"));
        };
        project (value.actors, "actors");
        project (value.spots, "spots");
        for (auto &type : value.spot_types)
            project (type.usage, capacity_type_dimension (type.object_kind, type.stable_type));
    }

    task_t<void> confirm_capacity (const std::string &scope)
    {
        const auto counters = co_await read_capacity (scope);
        _capacity_metrics.confirm (
          scope, [&] (placement_capacity_t &value) { project_capacity (scope, value, counters); });
    }

    std::atomic<std::uint64_t> _generation_floor{0};
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    capacity_metrics_t _capacity_metrics;
    provider_boundary_t _store;
};

} // namespace zlink::framework::runtime
