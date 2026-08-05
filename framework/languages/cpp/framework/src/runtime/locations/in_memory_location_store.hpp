/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/location_key_codec.hpp"
#include "runtime/locations/aggregate_inventory.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace zlink::framework::runtime
{

struct owner_lease_row_t
{
    std::string owner_id;
    zlink::routing_id_t node_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::chrono::system_clock::time_point lease_expires_at{};
    std::chrono::system_clock::time_point updated_at{};
};

class in_memory_location_repository_t : public location_repository_t
{
  public:
    in_memory_location_repository_t () = default;

    explicit in_memory_location_repository_t (
      std::uint64_t initial_store_revision) :
        _store_revision (initial_store_revision)
    {
    }

    task_t<location_write_result_t> update_mesh_node (
      mesh_node_descriptor_t descriptor,
      location_write_intent_t intent) override
    {
        if (!valid_mesh_node_descriptor (descriptor))
            throw std::invalid_argument (
              "mesh node descriptor is incomplete");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto key = mesh_node_key (
          descriptor.mesh_name, descriptor.rid);
        const auto found = _mesh_nodes.find (key);
        const auto token = location_owner_token_t{
          descriptor.owner_id, descriptor.lease_generation};
        if (!owner_token_is_live (token, now))
            return completed (location_write_result_t{
              location_write_status_t::ignored_stale, 0, {}});

        if (intent == location_write_intent_t::new_claim) {
            if (found != _mesh_nodes.end ())
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}});
        } else if (intent == location_write_intent_t::renew) {
            if (found == _mesh_nodes.end ()
                || found->second.owner_id != descriptor.owner_id
                || found->second.lease_generation
                     != descriptor.lease_generation
                || descriptor.descriptor_revision
                     < found->second.descriptor_revision)
                return completed (location_write_result_t{
                  location_write_status_t::ignored_stale, 0, {}});
            if (!same_mesh_node_identity (
                  found->second, descriptor))
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict,
                  0,
                  {}});
            if (descriptor.descriptor_revision
                  == found->second.descriptor_revision) {
                if (!same_mesh_node_descriptor (
                      found->second, descriptor))
                    return completed (location_write_result_t{
                      location_write_status_t::rejected_conflict,
                      0,
                      {}});
                return completed (
                  location_write_result_t::stored (
                    static_cast<std::int64_t> (
                      descriptor.descriptor_revision),
                    found->second.updated_at));
            }
        } else {
            return completed (location_write_result_t{
              location_write_status_t::rejected_conflict, 0, {}});
        }
        if (!can_publish_entry_spot_id (
              descriptor, key, now))
            return completed (location_write_result_t{
              location_write_status_t::rejected_conflict, 0, {}});

        descriptor.updated_at = now;
        const auto previous =
          found == _mesh_nodes.end ()
            ? std::optional<mesh_node_descriptor_t>{}
            : std::make_optional (found->second);
        _mesh_nodes[key] = descriptor;
        publish_entry_spot_id (
          previous, descriptor, key);
        return completed (
          location_write_result_t::stored (
            static_cast<std::int64_t> (
              descriptor.descriptor_revision),
            now));
    }

    task_t<location_write_status_t> remove_mesh_node (
      mesh_node_descriptor_key_t key,
      location_owner_token_t owner) override
    {
        std::lock_guard lock (_gate);
        const auto found = _mesh_nodes.find (
          mesh_node_key (key.mesh_name, key.rid));
        if (found == _mesh_nodes.end ()
            || found->second.owner_id != owner.owner_id
            || found->second.lease_generation
                 != owner.lease_generation)
            return completed (
              location_write_status_t::ignored_stale);
        remove_entry_spot_id_claim (
          found->second,
          mesh_node_key (key.mesh_name, key.rid));
        _mesh_nodes.erase (found);
        return completed (location_write_status_t::stored);
    }

    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name,
                     location_page_request_t page = {}) override
    {
        std::lock_guard lock (_gate);
        std::vector<mesh_node_descriptor_t> matched;
        for (const auto &[_, descriptor] : _mesh_nodes) {
            if (descriptor.mesh_name == mesh_name) {
                auto projected = descriptor;
                apply_capacity_projection (projected);
                matched.push_back (std::move (projected));
            }
        }
        const auto offset =
          page.continuation_token
            ? parse_offset (*page.continuation_token)
            : 0;
        const auto page_size =
          page.page_size > 0
            ? static_cast<std::size_t> (page.page_size)
            : matched.size ();
        location_page_t<mesh_node_descriptor_t> result;
        for (std::size_t index = offset;
             index < matched.size ()
             && result.items.size () < page_size;
             ++index)
            result.items.push_back (matched[index]);
        const auto next = offset + result.items.size ();
        if (next < matched.size ())
            result.continuation_token = std::to_string (next);
        return completed (std::move (result));
    }

    task_t<location_write_result_t> update_client_server (
      client_server_server_descriptor_t descriptor,
      location_write_intent_t intent) override
    {
        if (!valid_client_server_descriptor (descriptor))
            throw std::invalid_argument (
              "ClientServer descriptor is incomplete");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto key = client_server_key (
          descriptor.channel_name, descriptor.server_rid);
        const auto found = _client_servers.find (key);
        const auto token = location_owner_token_t{
          descriptor.owner_id, descriptor.lease_generation};
        if (!owner_token_is_live (token, now))
            return completed (location_write_result_t{
              location_write_status_t::ignored_stale, 0, {}});

        if (intent == location_write_intent_t::new_claim) {
            if (found != _client_servers.end ()
                && owner_token_is_live (
                  {found->second.owner_id,
                   found->second.lease_generation},
                  now))
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}});
        } else if (intent == location_write_intent_t::renew) {
            if (found == _client_servers.end ()
                || found->second.owner_id != descriptor.owner_id
                || found->second.lease_generation
                     != descriptor.lease_generation
                || descriptor.descriptor_revision
                     < found->second.descriptor_revision)
                return completed (location_write_result_t{
                  location_write_status_t::ignored_stale, 0, {}});
            if (!same_client_server_identity (
                  found->second, descriptor))
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}});
            if (descriptor.descriptor_revision
                  == found->second.descriptor_revision) {
                if (!same_client_server_descriptor (
                      found->second, descriptor))
                    return completed (location_write_result_t{
                      location_write_status_t::rejected_conflict, 0, {}});
                return completed (
                  location_write_result_t::stored (
                    static_cast<std::int64_t> (
                      descriptor.descriptor_revision),
                    found->second.updated_at));
            }
        } else {
            return completed (location_write_result_t{
              location_write_status_t::rejected_conflict, 0, {}});
        }

        descriptor.updated_at = now;
        _client_servers[key] = descriptor;
        return completed (
          location_write_result_t::stored (
            static_cast<std::int64_t> (
              descriptor.descriptor_revision),
            now));
    }

    task_t<location_write_status_t> remove_client_server (
      client_server_server_descriptor_key_t key,
      location_owner_token_t owner) override
    {
        std::lock_guard lock (_gate);
        const auto found = _client_servers.find (
          client_server_key (key.channel_name, key.server_rid));
        if (found == _client_servers.end ()
            || found->second.owner_id != owner.owner_id
            || found->second.lease_generation
                 != owner.lease_generation)
            return completed (
              location_write_status_t::ignored_stale);
        _client_servers.erase (found);
        return completed (location_write_status_t::stored);
    }

    task_t<location_page_t<client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name,
                         location_page_request_t page = {}) override
    {
        if (channel_name.empty () || page.page_size < 1
            || page.page_size > 1000)
            throw std::invalid_argument (
              "ClientServer list arguments are invalid");
        std::lock_guard lock (_gate);
        std::vector<client_server_server_descriptor_t> matched;
        for (const auto &[_, descriptor] : _client_servers) {
            if (descriptor.channel_name == channel_name)
                matched.push_back (descriptor);
        }
        const auto offset =
          page.continuation_token
            ? parse_offset (*page.continuation_token)
            : 0;
        location_page_t<client_server_server_descriptor_t> result;
        for (std::size_t index = offset;
             index < matched.size ()
             && result.items.size ()
                  < static_cast<std::size_t> (page.page_size);
             ++index)
            result.items.push_back (matched[index]);
        const auto next = offset + result.items.size ();
        if (next < matched.size ())
            result.continuation_token = std::to_string (next);
        return completed (std::move (result));
    }

    task_t<location_write_result_t> update_fanout_publisher (
      fanout_publisher_descriptor_t descriptor,
      location_write_intent_t intent) override
    {
        if (!valid_fanout_descriptor (descriptor))
            throw std::invalid_argument (
              "fanout publisher descriptor is incomplete");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto key = fanout_key (
          descriptor.channel_name, descriptor.publisher_rid);
        const auto found = _fanout_publishers.find (key);
        const auto token = location_owner_token_t{
          descriptor.owner_id, descriptor.lease_generation};
        if (!owner_token_is_live (token, now))
            return completed (location_write_result_t{
              location_write_status_t::ignored_stale, 0, {}});

        if (intent == location_write_intent_t::new_claim
            || intent == location_write_intent_t::takeover) {
            if (found != _fanout_publishers.end ()
                && owner_token_is_live (
                  {found->second.owner_id,
                   found->second.lease_generation},
                  now)
                && (found->second.owner_id
                      != descriptor.owner_id
                    || found->second.lease_generation
                         != descriptor.lease_generation))
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}});
        } else if (intent != location_write_intent_t::renew) {
            return completed (location_write_result_t{
              location_write_status_t::rejected_conflict, 0, {}});
        }

        if (found != _fanout_publishers.end ()
            && found->second.owner_id == descriptor.owner_id
            && found->second.lease_generation
                 == descriptor.lease_generation) {
            if (!same_fanout_identity (
                  found->second, descriptor))
                return completed (location_write_result_t{
                  location_write_status_t::rejected_conflict, 0, {}});
            if (descriptor.descriptor_revision
                < found->second.descriptor_revision)
                return completed (location_write_result_t{
                  location_write_status_t::ignored_stale, 0, {}});
            if (descriptor.descriptor_revision
                  == found->second.descriptor_revision) {
                if (!same_fanout_descriptor (
                      found->second, descriptor))
                    return completed (location_write_result_t{
                      location_write_status_t::rejected_conflict, 0, {}});
                return completed (
                  location_write_result_t::stored (
                    static_cast<std::int64_t> (
                      descriptor.descriptor_revision),
                    found->second.updated_at));
            }
        } else if (intent == location_write_intent_t::renew) {
            return completed (location_write_result_t{
              location_write_status_t::ignored_stale, 0, {}});
        }

        descriptor.updated_at = now;
        _fanout_publishers[key] = descriptor;
        return completed (
          location_write_result_t::stored (
            static_cast<std::int64_t> (
              descriptor.descriptor_revision),
            now));
    }

    task_t<location_write_status_t> remove_fanout_publisher (
      fanout_publisher_descriptor_key_t key,
      location_owner_token_t owner) override
    {
        std::lock_guard lock (_gate);
        const auto found = _fanout_publishers.find (
          fanout_key (key.channel_name, key.publisher_rid));
        if (found == _fanout_publishers.end ()
            || found->second.owner_id != owner.owner_id
            || found->second.lease_generation
                 != owner.lease_generation)
            return completed (
              location_write_status_t::ignored_stale);
        _fanout_publishers.erase (found);
        return completed (location_write_status_t::stored);
    }

    task_t<location_page_t<fanout_publisher_descriptor_t>>
    list_fanout_publishers (
      std::string channel_name,
      location_page_request_t page = {}) override
    {
        if (channel_name.empty () || page.page_size < 1
            || page.page_size > 1000)
            throw std::invalid_argument (
              "fanout publisher list arguments are invalid");
        std::lock_guard lock (_gate);
        const auto offset =
          page.continuation_token
            ? parse_offset (*page.continuation_token)
            : 0;
        location_page_t<fanout_publisher_descriptor_t> result;
        std::size_t matched = 0;
        std::size_t encoded_bytes = 0;
        for (const auto &[_, descriptor] :
             _fanout_publishers) {
            if (descriptor.channel_name != channel_name)
                continue;
            if (matched++ < offset)
                continue;
            const auto row_bytes =
              fanout_descriptor_encoded_size_upper_bound (
                descriptor);
            if (result.items.size ()
                  == static_cast<std::size_t> (
                    page.page_size)
                || (!result.items.empty ()
                    && encoded_bytes + row_bytes
                         > 4u * 1024u * 1024u)) {
                result.continuation_token =
                  std::to_string (
                    offset + result.items.size ());
                break;
            }
            encoded_bytes += row_bytes;
            result.items.push_back (descriptor);
        }
        if (matched < offset)
            throw std::invalid_argument (
              "fanout publisher continuation token is invalid");
        return completed (std::move (result));
    }

    task_t<owner_lease_claim_result_t> claim_owner_lease (
      std::string owner_id,
      std::chrono::milliseconds lease_ttl) override
    {
        if (owner_id.empty () || lease_ttl.count () <= 0)
            throw std::invalid_argument (
              "owner lease claim is incomplete");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto existing = _leases.find (owner_id);
        if (existing != _leases.end ()
            && existing->second.lease_expires_at > now)
            return completed (
              owner_lease_claim_result_t{
                owner_lease_conflict_t{}});
        if (_lease_generation >= max_generation)
            return completed (
              owner_lease_claim_result_t{
                owner_lease_generation_exhausted_t{}});
        if (existing != _leases.end ())
            _leases.erase (existing);
        ++_lease_generation;
        const auto generation =
          static_cast<std::int64_t> (_lease_generation);
        const auto expires_at = now + lease_ttl;
        _active_lease_generations[owner_id] = generation;
        _leases[owner_id] = owner_lease_row_t{
          owner_id,
          zlink::routing_id_t::from (std::uint32_t{0}),
          expires_at,
          now};
        return completed (
          owner_lease_claim_result_t{
            owner_lease_claimed_t{
              {std::move (owner_id), generation},
              expires_at,
              now}});
    }

    task_t<owner_lease_read_result_t> read_owner_lease (
      std::string owner_id) override
    {
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto lease = _leases.find (owner_id);
        const auto generation =
          _active_lease_generations.find (owner_id);
        if (lease == _leases.end ()
            || generation == _active_lease_generations.end ()
            || lease->second.lease_expires_at <= now) {
            if (lease != _leases.end ())
                _leases.erase (lease);
            _active_lease_generations.erase (owner_id);
            return completed (
              owner_lease_read_result_t{
                owner_lease_missing_t{}});
        }
        return completed (
          owner_lease_read_result_t{
            owner_lease_found_t{
              {std::move (owner_id), generation->second},
              lease->second.lease_expires_at,
              now}});
    }

    task_t<owner_lease_renew_result_t> renew_owner_lease (
      location_owner_token_t token,
      std::chrono::milliseconds lease_ttl) override
    {
        if (lease_ttl.count () <= 0)
            throw std::invalid_argument (
              "owner lease TTL must be positive");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto lease = _leases.find (token.owner_id);
        if (!owner_token_is_live (token, now))
            return completed (
              owner_lease_renew_result_t{
                owner_lease_stale_t{}});
        const auto expires_at = now + lease_ttl;
        lease->second.lease_expires_at = expires_at;
        lease->second.updated_at = now;
        return completed (
          owner_lease_renew_result_t{
            owner_lease_renewed_t{expires_at, now}});
    }

    task_t<owner_lease_release_result_t> release_owner_lease (
      location_owner_token_t token) override
    {
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        if (!owner_token_is_live (token, now))
            return completed (
              owner_lease_release_result_t{
                owner_lease_stale_t{}});
        _leases.erase (token.owner_id);
        _active_lease_generations.erase (token.owner_id);
        return completed (
          owner_lease_release_result_t{
            owner_lease_released_t{}});
    }

    task_t<authority_read_result_t> read_authority (
      authority_key_t key,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_read_result_t> ();
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto found = _authorities.find (key.value);
        if (found == _authorities.end ())
            return completed (
              authority_read_result_t{authority_missing_t{now}});
        auto snapshot = found->second;
        snapshot.store_now = now;
        return completed (
          authority_read_result_t{std::move (snapshot)});
    }

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority (
      authority_key_t key,
      std::string expected_store_version,
      authority_mutation_t mutation,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_compare_exchange_result_t> ();
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        auto found = _authorities.find (key.value);
        if (found == _authorities.end ()
            || found->second.store_version != expected_store_version) {
            authority_read_result_t current =
              found == _authorities.end ()
                ? authority_read_result_t{authority_missing_t{now}}
                : authority_read_result_t{found->second};
            return completed (
              authority_compare_exchange_result_t{
                authority_conflict_t{std::move (current)}});
        }

        if (std::holds_alternative<authority_delete_t> (mutation)) {
            if (found == _authorities.end ())
                return completed (
                  authority_compare_exchange_result_t{
                    authority_conflict_t{
                      authority_missing_t{now}}});
            if (found->second.allocation.state
                  != placement_allocation_state_t::active
                || !owner_token_is_live (found->second.owner, now)
                || !capacity_bundle_present (
                  _active_by_placement,
                  found->second.allocation.target,
                  found->second.allocation.capacity_bundle))
                return completed (
                  authority_compare_exchange_result_t{
                    authority_conflict_t{found->second}});
            if (!store_revisions_available ())
                return completed (
                  authority_compare_exchange_result_t{
                    authority_generation_exhausted_t{}});
            const auto store_version = next_store_version ();
            apply_capacity_bundle (
              _active_by_placement,
              found->second.allocation.target,
              found->second.allocation.capacity_bundle,
              false);
            _authorities.erase (found);
            return completed (
              authority_compare_exchange_result_t{
                authority_deleted_t{store_version, now}});
        }

        if (auto *restore =
              std::get_if<authority_restore_t> (&mutation)) {
            if (found->second.allocation.state
                  != placement_allocation_state_t::active
                || !same_owner (
                  found->second.owner,
                  restore->expected_owner))
                return completed (
                  authority_compare_exchange_result_t{
                    authority_conflict_t{found->second}});
            if (!store_revisions_available ())
                return completed (
                  authority_compare_exchange_result_t{
                    authority_generation_exhausted_t{}});
            auto snapshot = found->second;
            snapshot.store_version = next_store_version ();
            snapshot.payload = std::move (restore->payload);
            snapshot.store_now = now;
            _authorities[key.value] = snapshot;
            return completed (
              authority_compare_exchange_result_t{
                authority_stored_t{std::move (snapshot)}});
        }

        auto put = std::get<authority_put_t> (std::move (mutation));
        const auto transition = put.generation_transition;
        if ((transition == authority_generation_transition_t::preserve
             && (put.target_owner
                 || put.relocation_capacity_fence))
            || (transition == authority_generation_transition_t::new_owner
                && (!put.target_owner
                    || !put.relocation_capacity_fence)))
            throw std::invalid_argument (
              "authority owner or relocation capacity fence does not match generation transition");

        if (found == _authorities.end ()
            || found->second.allocation.state
                 != placement_allocation_state_t::active)
            return completed (
              authority_compare_exchange_result_t{
                authority_conflict_t{
                  found == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{now}}
                    : authority_read_result_t{found->second}}});
        auto owner = found->second.owner;
        const auto object_generation =
          found->second.object_generation;
        auto owner_generation =
          found->second.authority_owner_generation;
        auto allocation = found->second.allocation;
        if (transition
            == authority_generation_transition_t::new_owner) {
                const auto capacity =
                  _relocation_capacity_reservations.find (
                    put.relocation_capacity_fence->value);
                if (capacity
                      == _relocation_capacity_reservations.end ()
                    || capacity->second.status
                         != relocation_reservation_status_t::reserved
                    || capacity->second.request.key.value != key.value
                    || capacity->second.request.expected_store_version
                         != found->second.store_version
                    || !same_owner (
                      capacity->second.request.source.owner,
                      found->second.owner)
                    || !same_owner (
                      capacity->second.request.target.owner,
                      *put.target_owner)
                    || !allocation_matches_source (
                      found->second.allocation,
                      capacity->second.request)
                    || !live_target_descriptor (
                      capacity->second.request.target,
                      capacity->second.request.object_kind,
                      capacity->second.request.stable_type,
                      now)
                    || !relocation_capacity_counters_available (
                      capacity->second))
                    return completed (
                      authority_compare_exchange_result_t{
                        authority_conflict_t{found->second}});
                if (!store_revisions_available ())
                    return completed (
                      authority_compare_exchange_result_t{
                        authority_generation_exhausted_t{}});
                if (!next_generation (_authority_owner_generation))
                    return completed (
                      authority_compare_exchange_result_t{
                        authority_generation_exhausted_t{}});
                owner_generation = _authority_owner_generation;
                owner = *put.target_owner;
                allocation = allocation_from_relocation (
                  capacity->second.request);
                consume_relocation_capacity (
                  capacity->second);
        } else {
                if (!owner_token_is_live (found->second.owner, now))
                    return completed (
                      authority_compare_exchange_result_t{
                        authority_conflict_t{found->second}});
                if (!store_revisions_available ())
                    return completed (
                      authority_compare_exchange_result_t{
                        authority_generation_exhausted_t{}});
        }

        authority_snapshot_t snapshot{
          next_store_version (),
          std::move (put.payload),
          object_generation,
          owner_generation,
          std::move (owner),
          now,
          std::move (allocation)};
        _authorities[key.value] = snapshot;
        return completed (
          authority_compare_exchange_result_t{
            authority_stored_t{std::move (snapshot)}});
    }

    task_t<authority_scan_result_t> list_authorities (
      std::string prefix,
      std::optional<authority_scan_cursor_t> cursor,
      std::size_t limit,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<authority_scan_result_t> ();
        if (limit == 0 || limit > 1000)
            throw std::invalid_argument (
              "authority scan limit must be between 1 and 1000");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        cleanup_scans (now);

        std::string scan_id;
        std::size_t offset = 0;
        if (cursor) {
            const auto encoded = std::string (cursor->encoded ());
            const auto separator = encoded.find (':');
            if (separator == std::string::npos)
                return completed (
                  authority_scan_result_t{
                    authority_scan_expired_t{}});
            scan_id = encoded.substr (0, separator);
            try {
                offset = static_cast<std::size_t> (
                  std::stoull (encoded.substr (separator + 1)));
            }
            catch (...) {
                return completed (
                  authority_scan_result_t{
                    authority_scan_expired_t{}});
            }
        } else {
            scan_id = std::to_string (++_next_scan_id);
            authority_scan_state_t state;
            state.created_at = now;
            for (const auto &[authority_key, snapshot] : _authorities) {
                if (authority_key.starts_with (prefix))
                    state.entries.push_back (
                      {{authority_key}, snapshot});
            }
            _authority_scans.emplace (scan_id, std::move (state));
        }

        const auto scan = _authority_scans.find (scan_id);
        if (scan == _authority_scans.end ()
            || offset > scan->second.entries.size ())
            return completed (
              authority_scan_result_t{
                authority_scan_expired_t{}});

        authority_page_t page;
        std::size_t encoded_size = 0;
        while (offset < scan->second.entries.size ()
               && page.items.size () < limit) {
            const auto &entry = scan->second.entries[offset];
            const auto item_size =
              entry.key.value.size () + entry.snapshot.payload.size ();
            if (!page.items.empty ()
                && encoded_size + item_size > 4u * 1024u * 1024u)
                break;
            page.items.push_back (entry);
            encoded_size += item_size;
            ++offset;
        }
        if (offset < scan->second.entries.size ()) {
            page.next_cursor = authority_scan_cursor_t{
              scan_id + ":" + std::to_string (offset)};
        } else {
            _authority_scans.erase (scan);
        }
        return completed (
          authority_scan_result_t{std::move (page)});
    }

    task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (
      creation_operation_identity_t operation,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<std::optional<creation_terminal_record_t>> ();
        std::lock_guard lock (_gate);
        const auto key = creation_operation_key (operation);
        const auto found = _creation_terminals.find (key);
        if (found == _creation_terminals.end ()
            || found->second.expires_at <= clock_t::now ()) {
            if (found != _creation_terminals.end ())
                _creation_terminals.erase (found);
            return completed (
              std::optional<creation_terminal_record_t>{});
        }
        return completed (
          std::optional<creation_terminal_record_t>{found->second});
    }

    task_t<object_complete_creation_result_t>
    complete_creation (
      object_complete_creation_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_complete_creation_result_t> ();
        const auto publication = std::visit (
          [] (const auto &value)
            -> creation_terminal_publication_t {
              return value.terminal;
          },
          request.completion);
        if (publication.terminal_envelope.size () > 1024u * 1024u
            || sha256 (publication.terminal_envelope)
                 != publication.sha256)
            throw std::invalid_argument (
              "creation terminal envelope or SHA-256 is invalid");
        const auto expires_at =
          publication.operation_deadline + std::chrono::minutes (5);
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        if (expires_at <= now)
            throw std::invalid_argument (
              "creation terminal expiry is not in the future");
        const auto terminal_key =
          creation_operation_key (publication.operation);
        if (const auto existing = _creation_terminals.find (
              terminal_key);
            existing != _creation_terminals.end ()
            && existing->second.expires_at > now)
            return completed (
              object_complete_creation_result_t{
                object_creation_already_completed_result_t{
                  existing->second}});

        const auto key = object_key (request.key);
        const auto reservation = _reservations.find (key);
        if (reservation == _reservations.end ()
            || !same_fence (
              reservation->second.fence, request.fence))
            return completed (
              object_complete_creation_result_t{
                object_creation_completion_stale_t{}});
        auto authority = _authorities.find (key);
        if (authority == _authorities.end ()
            || authority->second.store_version
                 != request.fence.expected_store_version)
            return completed (
              object_complete_creation_result_t{
                object_creation_completion_conflict_t{
                  authority == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{now}}
                    : authority_read_result_t{
                        authority->second}}});

        creation_terminal_record_t terminal{
          publication.operation,
          request.key,
          request.fence,
          std::holds_alternative<object_creation_completed_t> (
            request.completion)
            ? creation_terminal_state_t::created
            : (std::holds_alternative<object_creation_rejected_t> (
                 request.completion)
                 ? creation_terminal_state_t::rejected
                 : creation_terminal_state_t::failed),
          publication.terminal_envelope,
          publication.sha256,
          expires_at};
        std::optional<authority_snapshot_t> ready;
        if (const auto *created =
              std::get_if<object_creation_completed_t> (
                &request.completion)) {
            const auto descriptor = live_target_descriptor (
              request.fence.target,
              reservation->second.request.key.kind,
              reservation->second.request.intent.stable_type,
              now);
            if (!descriptor
                || !capacity_bundle_present (
                  _pending_by_placement,
                  request.fence.target,
                  request.fence.capacity_bundle))
                return completed (
                  object_complete_creation_result_t{
                    object_creation_completion_conflict_t{
                      authority->second}});
            if (!store_revisions_available ())
                return completed (
                  object_complete_creation_result_t{
                    authority_generation_exhausted_t{}});
            authority->second.store_version =
              next_store_version ();
            authority->second.payload =
              created->ready_payload;
            authority->second.store_now = now;
            authority->second.allocation.state =
              placement_allocation_state_t::active;
            reservation->second.snapshot = authority->second;
            reservation->second.status =
              reservation_status_t::committed;
            release_pending (reservation->second);
            apply_capacity_bundle (
              _active_by_placement,
              request.fence.target,
              request.fence.capacity_bundle,
              true);
            ready = authority->second;
        } else {
            _authorities.erase (authority);
            _object_types.erase (key);
            release_pending (reservation->second);
            reservation->second.status =
              reservation_status_t::aborted;
        }
        _creation_terminals.insert_or_assign (
          terminal_key, terminal);
        return completed (
          object_complete_creation_result_t{
            object_creation_completed_result_t{
              std::move (terminal), std::move (ready)}});
    }

    task_t<object_reserve_result_t> reserve (
      object_reserve_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_reserve_result_t> ();
        if (request.creating_payload.size () > 1024u * 1024u
            || request.intent.request_encoded_size > 1024u * 1024u)
            throw std::invalid_argument (
              "object reservation payload exceeds 1 MiB");
        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto key = object_key (request.key);
        if (request.key.kind != placement_object_kind_t::actor) {
            const auto claim =
              _entry_spot_id_claims.find (
                request.key.global_id);
            if (claim != _entry_spot_id_claims.end ()
                && owner_token_is_live (
                  claim->second.owner, now))
                return completed (
                  object_reserve_result_t{
                    object_reserve_conflict_t{
                      authority_missing_t{now}}});
        }
        const auto authority = _authorities.find (key);
        if (authority != _authorities.end ()) {
            const auto type = _object_types.find (key);
            if (type != _object_types.end ()
                && type->second != request.intent.stable_type)
                return completed (
                  object_reserve_result_t{
                    object_type_mismatch_t{authority->second}});
            if (authority->second.allocation.state
                == placement_allocation_state_t::reserved)
                return completed (
                  object_reserve_result_t{
                    object_reserve_conflict_t{
                      authority->second}});
            return completed (
              object_reserve_result_t{
                object_already_exists_t{authority->second}});
        }
        const auto target_descriptor =
          live_target_descriptor (
            request.target,
            request.key.kind,
            request.intent.stable_type,
            now);
        if (!target_descriptor)
            return completed (
              object_reserve_result_t{
                object_reserve_conflict_t{
                  authority_missing_t{now}}});
        if (!bundle_matches_object (
              request.capacity_bundle,
              request.key.kind,
              request.intent.stable_type))
            throw std::invalid_argument (
              "object reservation capacity bundle does not match the object");
        if (!capacity_available (
              *target_descriptor,
              request.target,
              request.capacity_bundle))
            return completed (
              object_reserve_result_t{
                object_placement_capacity_exhausted_t{}});
        if (!store_revisions_available ()
            || _object_generation >= max_generation
            || _authority_owner_generation >= max_generation)
            return completed (
              object_reserve_result_t{
                authority_generation_exhausted_t{}});
        ++_object_generation;
        ++_authority_owner_generation;

        const auto store_version = next_store_version ();
        object_reservation_fence_t fence{
          "reservation-" + store_version,
          store_version,
          _object_generation,
          _authority_owner_generation,
          request.target,
          request.capacity_bundle};
        authority_snapshot_t creating{
          store_version,
          request.creating_payload,
          _object_generation,
          _authority_owner_generation,
          request.target.owner,
          now,
          {placement_allocation_state_t::reserved,
           request.key.kind,
           request.intent.stable_type,
           request.target,
           request.capacity_bundle},
          pending_object_creation_t{
            fence.reservation_id,
            request.intent.request_content_reference,
            request.intent.request_sha256,
            static_cast<std::uint32_t> (
              request.intent.request_encoded_size)}};
        reservation_state_t reservation{
          request, fence, creating, reservation_status_t::prepared};
        _authorities[key] = creating;
        _object_types[key] = request.intent.stable_type;
        _reservations[key] = std::move (reservation);
        apply_capacity_bundle (
          _pending_by_placement,
          request.target,
          request.capacity_bundle,
          true);
        return completed (
          object_reserve_result_t{
            object_reserved_t{std::move (fence),
                              std::move (creating)}});
    }

    task_t<object_commit_result_t> commit (
      object_commit_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_commit_result_t> ();
        if (request.ready_payload.size () > 1024u * 1024u)
            throw std::invalid_argument (
              "object commit payload exceeds 1 MiB");
        std::lock_guard lock (_gate);
        const auto key = object_key (request.key);
        const auto reservation = _reservations.find (key);
        if (reservation == _reservations.end ())
            return completed (
              object_commit_result_t{object_commit_stale_t{}});
        if (!same_fence (reservation->second.fence, request.fence))
            return completed (
              object_commit_result_t{object_commit_stale_t{}});
        if (reservation->second.status
            == reservation_status_t::committed)
            return completed (
              object_commit_result_t{
                object_already_committed_t{
                  reservation->second.snapshot}});
        if (reservation->second.status
            == reservation_status_t::aborted)
            return completed (
              object_commit_result_t{object_commit_stale_t{}});

        auto authority = _authorities.find (key);
        if (authority == _authorities.end ()
            || authority->second.store_version
                 != request.fence.expected_store_version)
            return completed (
              object_commit_result_t{
                object_commit_conflict_t{
                  authority == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{clock_t::now ()}}
                    : authority_read_result_t{authority->second}}});
        const auto now = clock_t::now ();
        const auto descriptor = live_target_descriptor (
              request.fence.target,
              reservation->second.request.key.kind,
              reservation->second.request.intent.stable_type,
              now);
        if (!descriptor
            || !capacity_bundle_present (
              _pending_by_placement,
              request.fence.target,
              request.fence.capacity_bundle))
            return completed (
              object_commit_result_t{
                  object_commit_conflict_t{authority->second}});
        if (!store_revisions_available ())
            return completed (
              object_commit_result_t{
                authority_generation_exhausted_t{}});

        authority->second.store_version = next_store_version ();
        authority->second.payload = std::move (request.ready_payload);
        authority->second.store_now = now;
        authority->second.allocation.state =
          placement_allocation_state_t::active;
        reservation->second.snapshot = authority->second;
        reservation->second.status = reservation_status_t::committed;
        release_pending (reservation->second);
        apply_capacity_bundle (
          _active_by_placement,
          request.fence.target,
          request.fence.capacity_bundle,
          true);
        return completed (
          object_commit_result_t{
            object_committed_t{authority->second}});
    }

    task_t<object_abort_result_t> abort (
      object_abort_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<object_abort_result_t> ();
        std::lock_guard lock (_gate);
        const auto key = object_key (request.key);
        const auto reservation = _reservations.find (key);
        if (reservation == _reservations.end ())
            return completed (
              object_abort_result_t{object_abort_stale_t{}});
        if (!same_fence (reservation->second.fence, request.fence))
            return completed (
              object_abort_result_t{object_abort_stale_t{}});
        if (reservation->second.status
            == reservation_status_t::aborted)
            return completed (
              object_abort_result_t{object_already_aborted_t{}});
        if (reservation->second.status
            == reservation_status_t::committed)
            return completed (
              object_abort_result_t{object_abort_stale_t{}});

        const auto authority = _authorities.find (key);
        if (authority == _authorities.end ()
            || authority->second.store_version
                 != request.fence.expected_store_version)
            return completed (
              object_abort_result_t{
                object_abort_conflict_t{
                  authority == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{clock_t::now ()}}
                    : authority_read_result_t{authority->second}}});
        _authorities.erase (authority);
        _object_types.erase (key);
        release_pending (reservation->second);
        reservation->second.status = reservation_status_t::aborted;
        return completed (
          object_abort_result_t{object_aborted_t{}});
    }

    task_t<relocation_capacity_reserve_result_t>
    reserve_relocation_capacity (
      relocation_capacity_reserve_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_capacity_reserve_result_t> ();
        if (all_zero (request.reservation_id)
            || request.key.value.empty ()
            || request.expected_store_version.empty ()
            || request.stable_type.empty ()
            || !bundle_matches_object (
              request.capacity_bundle,
              request.object_kind,
              request.stable_type))
            throw std::invalid_argument (
              "relocation capacity reservation is incomplete");

        std::lock_guard lock (_gate);
        const auto now = clock_t::now ();
        const auto reservation_key =
          reservation_id_key (request.reservation_id);
        const auto existing_id =
          _relocation_capacity_by_id.find (reservation_key);
        if (existing_id != _relocation_capacity_by_id.end ()) {
            const auto existing =
              _relocation_capacity_reservations.find (
                existing_id->second);
            if (existing
                  != _relocation_capacity_reservations.end ()
                && same_relocation_capacity_request (
                  existing->second.request, request)) {
                return completed (
                  relocation_capacity_reserve_result_t{
                    relocation_capacity_already_reserved_t{
                      existing->second.fence}});
            }
            const auto authority =
              _authorities.find (request.key.value);
            return completed (
              relocation_capacity_reserve_result_t{
                relocation_capacity_conflict_t{
                  authority == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{now}}
                    : authority_read_result_t{
                        authority->second}}});
        }

        const auto authority =
          _authorities.find (request.key.value);
        if (authority == _authorities.end ()
            || authority->second.store_version
                 != request.expected_store_version
            || !same_owner (
              authority->second.owner, request.source.owner)
            || !allocation_matches_source (
              authority->second.allocation, request)) {
            return completed (
              relocation_capacity_reserve_result_t{
                relocation_capacity_conflict_t{
                  authority == _authorities.end ()
                    ? authority_read_result_t{
                        authority_missing_t{now}}
                    : authority_read_result_t{
                        authority->second}}});
        }
        const auto target_descriptor =
          live_target_descriptor (
            request.target,
            request.object_kind,
            request.stable_type,
            now);
        if (!target_descriptor)
            return completed (
              relocation_capacity_reserve_result_t{
                relocation_capacity_target_unavailable_t{}});
        if (!capacity_available (
              *target_descriptor,
              request.target,
              request.capacity_bundle))
            return completed (
              relocation_capacity_reserve_result_t{
                relocation_capacity_exhausted_t{}});

        relocation_capacity_fence_t fence{
          "relocation-" + reservation_key};
        relocation_capacity_state_t state{
          request, fence, relocation_reservation_status_t::reserved};
        apply_capacity_bundle (
          _pending_by_placement,
          request.target,
          request.capacity_bundle,
          true);
        _relocation_capacity_by_id.emplace (
          reservation_key, fence.value);
        _relocation_capacity_reservations.emplace (
          fence.value, std::move (state));
        return completed (
          relocation_capacity_reserve_result_t{
            relocation_capacity_reserved_t{
              std::move (fence)}});
    }

    task_t<relocation_capacity_abort_result_t>
    abort_relocation_capacity (
      relocation_capacity_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<relocation_capacity_abort_result_t> ();
        std::lock_guard lock (_gate);
        const auto reservation =
          _relocation_capacity_reservations.find (fence.value);
        if (reservation
            == _relocation_capacity_reservations.end ())
            return completed (
              relocation_capacity_abort_result_t::stale);
        if (reservation->second.status
            == relocation_reservation_status_t::committed)
            return completed (
              relocation_capacity_abort_result_t::
                already_committed);
        if (reservation->second.status
            == relocation_reservation_status_t::aborted)
            return completed (
              relocation_capacity_abort_result_t::
                already_aborted);
        if (reservation->second.status
            != relocation_reservation_status_t::reserved)
            return completed (
              relocation_capacity_abort_result_t::stale);
        release_relocation_pending (reservation->second);
        reservation->second.status =
          relocation_reservation_status_t::aborted;
        return completed (
          relocation_capacity_abort_result_t::aborted);
    }

    task_t<aggregate_prepare_result_t> prepare_aggregate (
      aggregate_prepare_request_t request,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_prepare_result_t> ();
        const auto inventory_tree = aggregate_inventory::build_tree (request.participants);
        std::lock_guard lock (_gate);
        if (request.aggregate_generation == 0
            || request.aggregate_generation > max_generation
            || request.participants.empty ()
            || all_zero (request.aggregate_id.value)
            || request.target_owner.owner_id.empty ()
            || request.target_owner.lease_generation <= 0
            || !valid_capacity_bundle (
              request.capacity_bundle)
            || request.capacity_bundle.spot_slots != 1
            || !request.capacity_bundle.spot_type
            || request.capacity_bundle.spot_type->object_kind
                 != placement_object_kind_t::user_spot
            || std::any_of (
                 request.participants.begin (), request.participants.end (),
                 [] (const aggregate_participant_t &participant) {
                     return !participant.membership_mutation.empty ();
                 })
            || !inventory_tree)
            return completed (
              aggregate_prepare_result_t{
                aggregate_prepare_conflict_t{}});

        const auto aggregate_key =
          aggregate_id_key (request.aggregate_id);
        const auto existing = _aggregates.find (aggregate_key);
        if (existing != _aggregates.end ()) {
            if (same_aggregate_request (
                  existing->second.request, request))
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_already_prepared_t{
                      {request.aggregate_id,
                       request.aggregate_generation,
                       request.inventory_digest}}});
            return completed (
              aggregate_prepare_result_t{
                aggregate_prepare_stale_t{}});
        }

        if (!request.capacity_fences.empty ()
            && request.capacity_fences.size () != request.participants.size ())
            return completed (
              aggregate_prepare_result_t{
                aggregate_prepare_conflict_t{}});
        for (std::size_t index = 0;
             index < request.participants.size ();
             ++index) {
            const auto &participant = request.participants[index];
            if (request.capacity_fences.empty ()) {
                if (participant.capacity_fence)
                    return completed (
                      aggregate_prepare_result_t{
                        aggregate_prepare_conflict_t{}});
                continue;
            }
            const auto &fence = request.capacity_fences[index];
            if (fence.value.empty ()
                || !participant.capacity_fence
                || participant.capacity_fence->value != fence.value)
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_prepare_conflict_t{}});
            const auto reservation =
              _relocation_capacity_reservations.find (fence.value);
            if (reservation == _relocation_capacity_reservations.end ()
                || reservation->second.status
                     != relocation_reservation_status_t::reserved
                || reservation->second.request.key.value
                     != participant.key.value
                || reservation->second.request.expected_store_version
                     != participant.expected_store_version
                || !same_owner (
                  reservation->second.request.target.owner,
                  request.target_owner))
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_prepare_conflict_t{}});
        }

        std::string previous;
        for (const auto &participant : request.participants) {
            if (!previous.empty ()
                && participant.key.value <= previous)
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_prepare_conflict_t{}});
            previous = participant.key.value;
            const auto authority =
              _authorities.find (participant.key.value);
            if (authority == _authorities.end ()
                || authority->second.store_version
                     != participant.expected_store_version)
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_prepare_conflict_t{}});
        }
        placement_capacity_bundle_t inventory;
        std::optional<spot_type_capacity_delta_t> inventory_spot_type;
        for (const auto &participant : request.participants) {
            const auto authority =
              _authorities.find (participant.key.value);
            if (authority == _authorities.end ()
                || authority->second.allocation.state
                     != placement_allocation_state_t::active
                || participant.owner_transition
                     != authority_generation_transition_t::new_owner
                || !capacity_bundle_present (
                  _active_by_placement,
                  authority->second.allocation.target,
                  authority->second.allocation.capacity_bundle))
                return completed (
                  aggregate_prepare_result_t{
                    aggregate_prepare_conflict_t{}});
            inventory.actor_slots +=
              authority->second.allocation.capacity_bundle.actor_slots;
            inventory.spot_slots +=
              authority->second.allocation.capacity_bundle.spot_slots;
            if (authority->second.allocation.capacity_bundle.spot_type) {
                const auto &spot_type =
                  *authority->second.allocation.capacity_bundle.spot_type;
                if (inventory_spot_type
                    && (inventory_spot_type->object_kind
                          != spot_type.object_kind
                        || inventory_spot_type->stable_type
                             != spot_type.stable_type))
                    return completed (
                      aggregate_prepare_result_t{
                        aggregate_prepare_conflict_t{}});
                inventory_spot_type = spot_type;
            }
        }
        inventory.spot_type = inventory_spot_type;
        if (!same_capacity_bundle (
              inventory, request.capacity_bundle))
            return completed (
              aggregate_prepare_result_t{
                aggregate_prepare_conflict_t{}});
        const object_creation_target_t target{
          request.target_descriptor.mesh_name,
          node_rid_t::from_string (
            request.target_descriptor.rid.to_string ()),
          request.target_descriptor_lifecycle_generation,
          request.target_owner};
        const auto target_descriptor = live_target_descriptor (
          target,
          placement_object_kind_t::user_spot,
          request.capacity_bundle.spot_type->stable_type,
          clock_t::now ());
        if (!target_descriptor
            || (request.capacity_fences.empty ()
                && !capacity_available (
                  *target_descriptor,
                  target,
                  request.capacity_bundle))
            || (!request.capacity_fences.empty ()
                && !capacity_bundle_present (
                  _pending_by_placement,
                  target,
                  request.capacity_bundle)))
            return completed (
              aggregate_prepare_result_t{
                aggregate_prepare_conflict_t{}});

        aggregate_state_t state;
        state.request = std::move (request);
        state.inventory = *inventory_tree;
        _aggregates.emplace (aggregate_key, std::move (state));
        const auto &stored = _aggregates.at (aggregate_key).request;
        if (stored.capacity_fences.empty ())
            apply_capacity_bundle (
              _pending_by_placement,
              target,
              stored.capacity_bundle,
              true);
        else {
            for (const auto &capacity : stored.capacity_fences) {
                auto &reservation =
                  _relocation_capacity_reservations.at (capacity.value);
                reservation.status =
                  relocation_reservation_status_t::prepared;
                reservation.aggregate_id = stored.aggregate_id;
                reservation.aggregate_generation =
                  stored.aggregate_generation;
            }
        }
        return completed (
          aggregate_prepare_result_t{
            aggregate_prepared_t{
              {stored.aggregate_id,
               stored.aggregate_generation,
               stored.inventory_digest}}});
    }

    task_t<aggregate_commit_result_t> commit_aggregate (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_commit_result_t> ();
        std::lock_guard lock (_gate);
        const auto aggregate =
          _aggregates.find (aggregate_id_key (fence.aggregate_id));
        if (aggregate == _aggregates.end ()
            || aggregate->second.request.aggregate_generation
                 != fence.aggregate_generation)
            return completed (
              aggregate_commit_result_t::stale);
        if (aggregate->second.status
            == aggregate_status_t::committed)
            return completed (
              aggregate_commit_result_t::already_committed);
        if (aggregate->second.status
            == aggregate_status_t::aborted)
            return completed (aggregate_commit_result_t::stale);
        if (fence.inventory_digest
            && fence.inventory_digest->value
                 != aggregate->second.request.inventory_digest.value)
            return completed (aggregate_commit_result_t::stale);

        const auto participant_count =
          static_cast<std::size_t> (std::count_if (
            aggregate->second.request.participants.begin (),
            aggregate->second.request.participants.end (),
            [] (const aggregate_participant_t &participant) {
                return participant.owner_transition
                       == authority_generation_transition_t::
                            new_owner;
            }));
        if (!store_revisions_available (
              aggregate->second.request.participants.size ())
            || _authority_owner_generation
            > max_generation - participant_count)
            return completed (
              aggregate_commit_result_t::generation_exhausted);
        const auto now = clock_t::now ();
        for (const auto &participant :
             aggregate->second.request.participants) {
            const auto authority =
              _authorities.find (participant.key.value);
            if (authority == _authorities.end ()
                || authority->second.store_version
                     != participant.expected_store_version)
                return completed (
                  aggregate_commit_result_t::stale);
        }
        const object_creation_target_t target{
          aggregate->second.request.target_descriptor.mesh_name,
          node_rid_t::from_string (
            aggregate->second.request.target_descriptor.rid.to_string ()),
          aggregate->second.request
            .target_descriptor_lifecycle_generation,
          aggregate->second.request.target_owner};
        const auto target_descriptor = live_target_descriptor (
          target,
          placement_object_kind_t::user_spot,
          aggregate->second.request.capacity_bundle.spot_type
            ? aggregate->second.request.capacity_bundle
                .spot_type->stable_type
            : std::string{},
          now);
        if (!target_descriptor
            || (aggregate->second.request.capacity_fences.empty ()
                && !capacity_bundle_present (
                  _pending_by_placement,
                  target,
                  aggregate->second.request.capacity_bundle)))
            return completed (
              aggregate_commit_result_t::stale);
        for (const auto &participant :
             aggregate->second.request.participants) {
            const auto authority =
              _authorities.find (participant.key.value);
            if (authority == _authorities.end ()
                || authority->second.allocation.state
                     != placement_allocation_state_t::active
                || !capacity_bundle_present (
                  _active_by_placement,
                  authority->second.allocation.target,
                  authority->second.allocation.capacity_bundle))
                return completed (
                  aggregate_commit_result_t::stale);
        }
        if (!aggregate->second.request.capacity_fences.empty ()) {
            for (std::size_t index = 0;
                 index < aggregate->second.request.capacity_fences.size ();
                 ++index) {
                const auto &participant =
                  aggregate->second.request.participants[index];
                const auto reservation =
                  _relocation_capacity_reservations.find (
                    aggregate->second.request.capacity_fences[index].value);
                const auto authority =
                  _authorities.find (participant.key.value);
                if (reservation == _relocation_capacity_reservations.end ()
                    || authority == _authorities.end ()
                    || reservation->second.status
                         != relocation_reservation_status_t::prepared
                    || !reservation->second.aggregate_id
                    || reservation->second.aggregate_id->value
                         != fence.aggregate_id.value
                    || reservation->second.aggregate_generation
                         != fence.aggregate_generation
                    || reservation->second.request.key.value
                         != participant.key.value
                    || reservation->second.request.expected_store_version
                         != participant.expected_store_version
                    || !same_owner (
                      reservation->second.request.target.owner,
                      aggregate->second.request.target_owner)
                    || !allocation_matches_source (
                      authority->second.allocation,
                      reservation->second.request)
                    || !relocation_capacity_counters_available (
                      reservation->second))
                    return completed (
                      aggregate_commit_result_t::stale);
            }
        }

        for (std::size_t index = 0;
             index < aggregate->second.request.participants.size ();
             ++index) {
            const auto &participant =
              aggregate->second.request.participants[index];
            auto &snapshot = _authorities.at (
              participant.key.value);
            snapshot.store_version = next_store_version ();
            snapshot.payload = participant.authority_payload;
            snapshot.store_now = now;
            if (participant.owner_transition
                == authority_generation_transition_t::new_owner) {
                ++_authority_owner_generation;
                snapshot.authority_owner_generation =
                  _authority_owner_generation;
                snapshot.owner =
                  aggregate->second.request.target_owner;
                const auto source_allocation =
                  snapshot.allocation;
                snapshot.allocation.target = target;
                if (aggregate->second.request.capacity_fences.empty ()) {
                    apply_capacity_bundle (
                      _active_by_placement,
                      source_allocation.target,
                      source_allocation.capacity_bundle,
                      false);
                    apply_capacity_bundle (
                      _active_by_placement,
                      target,
                      snapshot.allocation.capacity_bundle,
                      true);
                } else {
                    auto &reservation =
                      _relocation_capacity_reservations.at (
                        aggregate->second.request.capacity_fences[index].value);
                    consume_relocation_capacity (reservation);
                }
            }
        }
        if (aggregate->second.request.capacity_fences.empty ())
            apply_capacity_bundle (
              _pending_by_placement,
              target,
              aggregate->second.request.capacity_bundle,
              false);
        aggregate->second.status = aggregate_status_t::committed;
        return completed (
          aggregate_commit_result_t::committed);
    }

    task_t<aggregate_abort_result_t> abort_aggregate (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<aggregate_abort_result_t> ();
        std::lock_guard lock (_gate);
        const auto aggregate =
          _aggregates.find (aggregate_id_key (fence.aggregate_id));
        if (aggregate == _aggregates.end ()
            || aggregate->second.request.aggregate_generation
                 != fence.aggregate_generation)
            return completed (aggregate_abort_result_t::stale);
        if (aggregate->second.status
            == aggregate_status_t::aborted)
            return completed (
              aggregate_abort_result_t::already_aborted);
        if (aggregate->second.status
            == aggregate_status_t::committed)
            return completed (aggregate_abort_result_t::stale);
        const object_creation_target_t target{
          aggregate->second.request.target_descriptor.mesh_name,
          node_rid_t::from_string (
            aggregate->second.request.target_descriptor.rid.to_string ()),
          aggregate->second.request
            .target_descriptor_lifecycle_generation,
          aggregate->second.request.target_owner};
        if (aggregate->second.request.capacity_fences.empty ())
            apply_capacity_bundle (
              _pending_by_placement,
              target,
              aggregate->second.request.capacity_bundle,
              false);
        else {
            for (const auto &capacity :
                 aggregate->second.request.capacity_fences) {
                const auto reservation =
                  _relocation_capacity_reservations.find (capacity.value);
                if (reservation == _relocation_capacity_reservations.end ()
                    || reservation->second.status
                         != relocation_reservation_status_t::prepared
                    || !reservation->second.aggregate_id
                    || reservation->second.aggregate_id->value
                         != fence.aggregate_id.value
                    || reservation->second.aggregate_generation
                         != fence.aggregate_generation)
                    return completed (aggregate_abort_result_t::stale);
                release_relocation_pending (reservation->second);
                reservation->second.status =
                  relocation_reservation_status_t::aborted;
            }
        }
        aggregate->second.status = aggregate_status_t::aborted;
        return completed (aggregate_abort_result_t::aborted);
    }

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        if (cancellation.stop_requested ())
            return cancelled<std::optional<std::vector<aggregate_participant_t>>> ();
        std::lock_guard lock (_gate);
        const auto aggregate =
          _aggregates.find (aggregate_id_key (fence.aggregate_id));
        if (aggregate == _aggregates.end ()
            || aggregate->second.request.aggregate_generation
                 != fence.aggregate_generation
            || (fence.inventory_digest
                && fence.inventory_digest->value
                     != aggregate->second.request.inventory_digest.value))
            return completed (
              std::optional<std::vector<aggregate_participant_t>>{});
        return completed (
          std::optional<std::vector<aggregate_participant_t>>{
            aggregate->second.request.participants});
    }

    task_t<std::int64_t> remove_all_by_owner (
      location_owner_token_t owner) override
    {
        std::lock_guard lock (_gate);
        if (!owner_token_is_live (owner, clock_t::now ()))
            return completed (std::int64_t{0});
        std::int64_t removed = 0;
        std::vector<std::string> mesh_node_keys;
        for (const auto &[key, descriptor] : _mesh_nodes) {
            if (descriptor.owner_id == owner.owner_id
                && descriptor.lease_generation == owner.lease_generation)
                mesh_node_keys.push_back (key);
        }
        for (const auto &key : mesh_node_keys)
        {
            const auto descriptor = _mesh_nodes.find (key);
            if (descriptor != _mesh_nodes.end ())
                remove_entry_spot_id_claim (
                  descriptor->second, key);
            _mesh_nodes.erase (key);
        }
        removed += static_cast<std::int64_t> (mesh_node_keys.size ());
        std::vector<std::string> client_server_keys;
        for (const auto &[key, descriptor] : _client_servers) {
            if (descriptor.owner_id == owner.owner_id
                && descriptor.lease_generation
                     == owner.lease_generation)
                client_server_keys.push_back (key);
        }
        for (const auto &key : client_server_keys)
            _client_servers.erase (key);
        removed += static_cast<std::int64_t> (
          client_server_keys.size ());
        std::vector<std::string> fanout_keys;
        for (const auto &[key, descriptor] :
             _fanout_publishers) {
            if (descriptor.owner_id == owner.owner_id
                && descriptor.lease_generation
                     == owner.lease_generation)
                fanout_keys.push_back (key);
        }
        for (const auto &key : fanout_keys)
            _fanout_publishers.erase (key);
        removed += static_cast<std::int64_t> (
          fanout_keys.size ());
        return completed (removed);
    }

  private:
    using clock_t = std::chrono::system_clock;

    enum class reservation_status_t
    {
        prepared,
        committed,
        aborted
    };

    struct reservation_state_t
    {
        object_reserve_request_t request;
        object_reservation_fence_t fence;
        authority_snapshot_t snapshot;
        reservation_status_t status = reservation_status_t::prepared;
    };

    enum class relocation_reservation_status_t
    {
        reserved,
        prepared,
        committed,
        aborted
    };

    struct relocation_capacity_state_t
    {
        relocation_capacity_reserve_request_t request;
        relocation_capacity_fence_t fence;
        relocation_reservation_status_t status =
          relocation_reservation_status_t::reserved;
        std::optional<aggregate_id_t> aggregate_id;
        std::uint64_t aggregate_generation = 0;
    };

    enum class aggregate_status_t
    {
        prepared,
        committed,
        aborted
    };

    struct aggregate_state_t
    {
        aggregate_prepare_request_t request;
        aggregate_inventory::tree_t inventory;
        aggregate_status_t status = aggregate_status_t::prepared;
    };


    struct authority_scan_state_t
    {
        std::vector<authority_entry_t> entries;
        clock_t::time_point created_at;
    };

    static constexpr std::uint64_t max_generation =
      static_cast<std::uint64_t> (
        std::numeric_limits<std::int64_t>::max ());
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    template <typename T> static task_t<T> cancelled ()
    {
        return task_t<T> (
          detail::boundary_failure<T> (
            detail::boundary_error_t::cancelled,
            "location store operation was cancelled"));
    }

    bool owner_token_is_live (
      const location_owner_token_t &token,
      clock_t::time_point now) const
    {
        const auto lease = _leases.find (token.owner_id);
        const auto generation =
          _active_lease_generations.find (token.owner_id);
        return lease != _leases.end ()
               && generation != _active_lease_generations.end ()
               && lease->second.lease_expires_at > now
               && generation->second == token.lease_generation;
    }

    const mesh_node_descriptor_t *live_target_descriptor (
      const object_creation_target_t &target,
      placement_object_kind_t kind,
      const std::string &stable_type,
      clock_t::time_point now) const
    {
        const auto found = _mesh_nodes.find (
          mesh_node_key (
            target.mesh_name,
            std::string (target.node_rid.value ())));
        if (found == _mesh_nodes.end ())
            return nullptr;
        const auto &descriptor = found->second;
        if (descriptor.lifecycle_generation
              != target.node_lifecycle_generation
            || descriptor.owner_id != target.owner.owner_id
            || descriptor.lease_generation
                 != target.owner.lease_generation
            || descriptor.state
                 != framework_runtime_state_t::serving
            || descriptor.object_role != object_role_t::server
            || descriptor.placement_weight == 0
            || !owner_token_is_live (target.owner, now))
            return nullptr;
        const auto capability = find_capability (
          descriptor, kind, stable_type);
        if (!capability)
            return nullptr;
        return &descriptor;
    }

    static const object_capability_t *find_capability (
      const mesh_node_descriptor_t &descriptor,
      placement_object_kind_t kind,
      const std::string &stable_type)
    {
        const auto found = std::find_if (
          descriptor.object_capabilities.begin (),
          descriptor.object_capabilities.end (),
          [&] (const object_capability_t &capability) {
              return capability.object_kind == kind
                     && capability.stable_type == stable_type;
          });
        return found == descriptor.object_capabilities.end ()
                 ? nullptr
                 : &*found;
    }

    static bool valid_mesh_node_descriptor (
      const mesh_node_descriptor_t &descriptor)
    {
        if (descriptor.mesh_name.empty ()
            || descriptor.rid.size () == 0
            || descriptor.lifecycle_generation == 0
            || descriptor.descriptor_revision == 0
            || descriptor.descriptor_revision > max_generation
            || descriptor.endpoint.empty ()
            || descriptor.application_version < 0
            || descriptor.placement_weight < 0
            || descriptor.placement_weight > 10000
            || descriptor.activation_concurrency.limit <= 0
            || descriptor.activation_concurrency.active
                 > static_cast<std::uint32_t> (
                     descriptor.activation_concurrency.limit)
            || descriptor.security_identity.empty ()
            || descriptor.owner_id.empty ()
            || descriptor.lease_generation <= 0
            || descriptor.object_capabilities.size () > 1024
            || descriptor.capacity.actors.limit < 0
            || descriptor.capacity.spots.limit < 0
            || (descriptor.capacity.actors.limit > 0
                && descriptor.capacity.actors.active
                     + descriptor.capacity.actors.reserved
                   > static_cast<std::uint64_t> (
                       descriptor.capacity.actors.limit))
            || (descriptor.capacity.spots.limit > 0
                && descriptor.capacity.spots.active
                     + descriptor.capacity.spots.reserved
                   > static_cast<std::uint64_t> (
                       descriptor.capacity.spots.limit))
            || descriptor.capacity.spot_types.size ()
                 > 1024
            || (descriptor.object_role != object_role_t::server
                && !descriptor.object_capabilities.empty ()))
            return false;
        for (const auto &[name, weight] :
             descriptor.channel_weights) {
            if (name.empty () || weight < 0
                || weight > 10000)
                return false;
        }
        std::pair<int, std::string> previous;
        bool first = true;
        for (const auto &capability :
             descriptor.object_capabilities) {
            if (capability.stable_type.empty ()
                || ((capability.policy
                       == maintenance_policy_kind_t::snapshot)
                    != capability.has_snapshot_adapter)
                || capability.spot_limit < 0
                || (capability.object_kind
                      == placement_object_kind_t::actor
                    && capability.spot_limit != 0))
                return false;
            const auto key = std::make_pair (
              static_cast<int> (capability.object_kind),
              capability.stable_type);
            if (!first && previous >= key)
                return false;
            previous = key;
            first = false;
        }
        std::pair<int, std::string> previous_capacity;
        first = true;
        for (const auto &typed :
             descriptor.capacity.spot_types) {
            if (typed.stable_type.empty ()
                || typed.object_kind
                     == placement_object_kind_t::actor
                || typed.usage.limit < 0
                || (typed.usage.limit > 0
                    && typed.usage.active
                         + typed.usage.reserved
                       > static_cast<std::uint64_t> (
                           typed.usage.limit)))
                return false;
            const auto key = std::make_pair (
              static_cast<int> (typed.object_kind),
              typed.stable_type);
            if (!first && previous_capacity >= key)
                return false;
            previous_capacity = key;
            first = false;
        }
        return true;
    }

    static bool same_capability (
      const object_capability_t &left,
      const object_capability_t &right)
    {
        return left.object_kind == right.object_kind
               && left.stable_type == right.stable_type
               && left.policy == right.policy
               && left.has_snapshot_adapter
                    == right.has_snapshot_adapter
               && left.spot_limit == right.spot_limit;
    }

    static bool same_capabilities (
      const std::vector<object_capability_t> &left,
      const std::vector<object_capability_t> &right)
    {
        return left.size () == right.size ()
               && std::equal (
                 left.begin (), left.end (), right.begin (),
                 same_capability);
    }

    static bool same_mesh_node_identity (
      const mesh_node_descriptor_t &left,
      const mesh_node_descriptor_t &right)
    {
        return left.mesh_name == right.mesh_name
               && left.rid == right.rid
               && left.lifecycle_generation
                    == right.lifecycle_generation
               && left.endpoint == right.endpoint
               && left.entry_spot_id == right.entry_spot_id
               && left.application_version
                    == right.application_version
               && same_channel_names (
                 left.channel_weights, right.channel_weights)
               && same_capabilities (
                 left.object_capabilities,
                 right.object_capabilities)
               && left.object_role == right.object_role
               && left.capacity.actors.limit
                    == right.capacity.actors.limit
               && left.capacity.spots.limit
                    == right.capacity.spots.limit
               && std::equal (
                 left.capacity.spot_types.begin (),
                 left.capacity.spot_types.end (),
                 right.capacity.spot_types.begin (),
                 right.capacity.spot_types.end (),
                 [] (const auto &lhs, const auto &rhs) {
                     return lhs.object_kind == rhs.object_kind
                            && lhs.stable_type == rhs.stable_type
                            && lhs.usage.limit == rhs.usage.limit;
                 })
               && left.activation_concurrency.limit
                    == right.activation_concurrency.limit
               && left.security_identity
                    == right.security_identity;
    }

    static bool same_mesh_node_descriptor (
      const mesh_node_descriptor_t &left,
      const mesh_node_descriptor_t &right)
    {
        return same_mesh_node_identity (left, right)
               && left.descriptor_revision
                    == right.descriptor_revision
               && left.channel_weights == right.channel_weights
               && left.placement_weight == right.placement_weight
               && left.capacity.actors.active
                    == right.capacity.actors.active
               && left.capacity.actors.reserved
                    == right.capacity.actors.reserved
               && left.capacity.spots.active
                    == right.capacity.spots.active
               && left.capacity.spots.reserved
                    == right.capacity.spots.reserved
               && std::equal (
                 left.capacity.spot_types.begin (),
                 left.capacity.spot_types.end (),
                 right.capacity.spot_types.begin (),
                 right.capacity.spot_types.end (),
                 [] (const auto &lhs, const auto &rhs) {
                     return lhs.object_kind == rhs.object_kind
                            && lhs.stable_type == rhs.stable_type
                            && lhs.usage.active == rhs.usage.active
                            && lhs.usage.reserved == rhs.usage.reserved
                            && lhs.usage.limit == rhs.usage.limit;
                 })
               && left.activation_concurrency.active
                    == right.activation_concurrency.active
               && left.maintenance_wave
                    == right.maintenance_wave
               && left.state == right.state
               && left.owner_id == right.owner_id
               && left.lease_generation
                    == right.lease_generation;
    }

    static bool valid_client_server_descriptor (
      const client_server_server_descriptor_t &descriptor)
    {
        return !descriptor.channel_name.empty ()
               && descriptor.server_rid.size () != 0
               && descriptor.lifecycle_generation != 0
               && descriptor.descriptor_revision != 0
               && descriptor.descriptor_revision <= max_generation
               && !descriptor.endpoint.empty ()
               && descriptor.weight >= 0
               && descriptor.weight <= 10000
               && !descriptor.security_identity.empty ()
               && !descriptor.owner_id.empty ()
               && descriptor.lease_generation > 0;
    }

    static bool same_client_server_identity (
      const client_server_server_descriptor_t &left,
      const client_server_server_descriptor_t &right)
    {
        return left.channel_name == right.channel_name
               && left.server_rid == right.server_rid
               && left.lifecycle_generation
                    == right.lifecycle_generation
               && left.endpoint == right.endpoint
               && left.security_identity
                    == right.security_identity;
    }

    static bool same_client_server_descriptor (
      const client_server_server_descriptor_t &left,
      const client_server_server_descriptor_t &right)
    {
        return same_client_server_identity (left, right)
               && left.descriptor_revision
                    == right.descriptor_revision
               && left.weight == right.weight
               && left.state == right.state
               && left.owner_id == right.owner_id
               && left.lease_generation
                    == right.lease_generation;
    }

    static bool valid_fanout_descriptor (
      const fanout_publisher_descriptor_t &descriptor)
    {
        return valid_fanout_descriptor_text (
                 descriptor.channel_name)
               && descriptor.publisher_rid.size () != 0
               && descriptor.lifecycle_generation != 0
               && descriptor.lifecycle_generation
                    <= max_generation
               && descriptor.descriptor_revision != 0
               && descriptor.descriptor_revision <= max_generation
               && valid_fanout_descriptor_text (
                 descriptor.endpoint)
               && valid_fanout_descriptor_text (
                 descriptor.security_identity)
               && valid_fanout_descriptor_text (
                 descriptor.owner_id)
               && descriptor.lease_generation > 0
               && static_cast<unsigned int> (descriptor.state)
                    <= static_cast<unsigned int> (
                      framework_runtime_state_t::error);
    }

    static bool valid_fanout_descriptor_text (
      std::string_view value) noexcept
    {
        return !value.empty () && value.size () <= 255
               && value.find ('\0')
                    == std::string_view::npos;
    }

    static std::size_t
    fanout_descriptor_encoded_size_upper_bound (
      const fanout_publisher_descriptor_t &descriptor)
      noexcept
    {
        const auto escaped_text_bytes =
          descriptor.channel_name.size ()
          + descriptor.endpoint.size ()
          + descriptor.security_identity.size ()
          + descriptor.owner_id.size ();
        return 512u + escaped_text_bytes * 6u
               + descriptor.publisher_rid.size () * 2u;
    }

    static bool same_fanout_identity (
      const fanout_publisher_descriptor_t &left,
      const fanout_publisher_descriptor_t &right)
    {
        return left.channel_name == right.channel_name
               && left.publisher_rid == right.publisher_rid
               && left.lifecycle_generation
                    == right.lifecycle_generation
               && left.endpoint == right.endpoint
               && left.security_identity
                    == right.security_identity;
    }

    static bool same_fanout_descriptor (
      const fanout_publisher_descriptor_t &left,
      const fanout_publisher_descriptor_t &right)
    {
        return same_fanout_identity (left, right)
               && left.descriptor_revision
                    == right.descriptor_revision
               && left.state == right.state
               && left.owner_id == right.owner_id
               && left.lease_generation
                    == right.lease_generation;
    }

    static bool same_channel_names (
      const std::map<std::string, int> &left,
      const std::map<std::string, int> &right)
    {
        if (left.size () != right.size ())
            return false;
        return std::equal (
          left.begin (), left.end (), right.begin (),
          [] (const auto &l, const auto &r) {
              return l.first == r.first;
          });
    }

    static std::string mesh_node_key (
      const std::string &mesh_name,
      const zlink::routing_id_t &rid)
    {
        return mesh_node_key (mesh_name, rid.to_string ());
    }

    static std::string mesh_node_key (
      const std::string &mesh_name,
      const std::string &rid)
    {
        return mesh_name + "\x1f" + rid;
    }

    static std::string client_server_key (
      const std::string &channel_name,
      const zlink::routing_id_t &rid)
    {
        return channel_name + "\x1f" + rid.to_hex ();
    }

    static std::string fanout_key (
      const std::string &channel_name,
      const zlink::routing_id_t &rid)
    {
        return channel_name + "\x1f" + rid.to_hex ();
    }

    static bool next_generation (std::uint64_t &counter)
    {
        if (counter >= max_generation)
            return false;
        ++counter;
        return true;
    }

    bool store_revisions_available (
      std::size_t count = 1) const
    {
        return count <= max_generation
               && _store_revision
                    <= max_generation
                         - static_cast<std::uint64_t> (count);
    }

    std::string next_store_version ()
    {
        ++_store_revision;
        return std::to_string (_store_revision);
    }

    static std::string object_key (
      const object_creation_key_t &key)
    {
        return std::to_string (static_cast<int> (key.kind))
               + ":" + key.global_id;
    }

    struct entry_spot_id_claim_t
    {
        std::string descriptor_key;
        std::uint64_t descriptor_lifecycle_generation = 0;
        location_owner_token_t owner;
    };

    bool can_publish_entry_spot_id (
      const mesh_node_descriptor_t &descriptor,
      const std::string &descriptor_key,
      clock_t::time_point now) const
    {
        if (!descriptor.entry_spot_id)
            return true;
        const auto claim =
          _entry_spot_id_claims.find (
            *descriptor.entry_spot_id);
        if (claim != _entry_spot_id_claims.end ()
            && owner_token_is_live (
              claim->second.owner, now)
            && (claim->second.descriptor_key
                  != descriptor_key
                || claim->second
                     .descriptor_lifecycle_generation
                     != descriptor.lifecycle_generation
                || !same_owner (
                  claim->second.owner,
                  {descriptor.owner_id,
                   descriptor.lease_generation})))
            return false;
        return !_authorities.contains (
          object_key (
            {placement_object_kind_t::user_spot,
             *descriptor.entry_spot_id}))
          && !_authorities.contains (
            object_key (
              {placement_object_kind_t::instance_spot,
               *descriptor.entry_spot_id}));
    }

    void publish_entry_spot_id (
      const std::optional<mesh_node_descriptor_t> &previous,
      const mesh_node_descriptor_t &descriptor,
      const std::string &descriptor_key)
    {
        if (previous
            && previous->entry_spot_id
            != descriptor.entry_spot_id)
            remove_entry_spot_id_claim (
              *previous, descriptor_key);
        if (descriptor.entry_spot_id)
            _entry_spot_id_claims.insert_or_assign (
              *descriptor.entry_spot_id,
              entry_spot_id_claim_t{
                descriptor_key,
                descriptor.lifecycle_generation,
                {descriptor.owner_id,
                 descriptor.lease_generation}});
    }

    void remove_entry_spot_id_claim (
      const mesh_node_descriptor_t &descriptor,
      const std::string &descriptor_key)
    {
        if (!descriptor.entry_spot_id)
            return;
        const auto claim =
          _entry_spot_id_claims.find (
            *descriptor.entry_spot_id);
        if (claim == _entry_spot_id_claims.end ()
            || claim->second.descriptor_key
                 != descriptor_key
            || claim->second
                 .descriptor_lifecycle_generation
                 != descriptor.lifecycle_generation
            || !same_owner (
              claim->second.owner,
              {descriptor.owner_id,
               descriptor.lease_generation}))
            return;
        _entry_spot_id_claims.erase (claim);
    }

    static std::string creation_operation_key (
      const creation_operation_identity_t &identity)
    {
        return std::string (identity.source_node_rid.value ())
               + ":" + std::to_string (
                 identity.source_node_generation)
               + ":" + std::to_string (
                 identity.operation_id.high)
               + ":" + std::to_string (
                 identity.operation_id.low);
    }


    static bool same_owner (
      const location_owner_token_t &left,
      const location_owner_token_t &right)
    {
        return left.owner_id == right.owner_id
               && left.lease_generation == right.lease_generation;
    }

    static bool same_target (
      const object_creation_target_t &left,
      const object_creation_target_t &right)
    {
        return left.mesh_name == right.mesh_name
               && left.node_rid.value () == right.node_rid.value ()
               && left.node_lifecycle_generation
                    == right.node_lifecycle_generation
               && same_owner (left.owner, right.owner);
    }

    static bool allocation_matches_source (
      const placement_allocation_t &allocation,
      const relocation_capacity_reserve_request_t &request)
    {
        return allocation.state
                 == placement_allocation_state_t::active
               && allocation.object_kind == request.object_kind
               && allocation.stable_type == request.stable_type
               && allocation.target.mesh_name == request.source.mesh_name
               && allocation.target.node_rid.value ()
                    == request.source.node_rid.value ()
               && allocation.target.node_lifecycle_generation
                    == request.source.node_lifecycle_generation
               && same_capacity_bundle (
                 allocation.capacity_bundle,
                 request.capacity_bundle);
    }

    static placement_allocation_t allocation_from_relocation (
      const relocation_capacity_reserve_request_t &request)
    {
        return {
          placement_allocation_state_t::active,
          request.object_kind,
          request.stable_type,
          request.target,
          request.capacity_bundle};
    }

    static bool same_fence (
      const object_reservation_fence_t &left,
      const object_reservation_fence_t &right)
    {
        return left.reservation_id == right.reservation_id
               && left.expected_store_version
                    == right.expected_store_version
               && left.object_generation == right.object_generation
               && left.authority_owner_generation
                    == right.authority_owner_generation
               && same_capacity_bundle (
                 left.capacity_bundle,
                 right.capacity_bundle)
               && same_target (left.target, right.target);
    }

    static bool same_relocation_capacity_request (
      const relocation_capacity_reserve_request_t &left,
      const relocation_capacity_reserve_request_t &right)
    {
        return left.reservation_id == right.reservation_id
               && left.key.value == right.key.value
               && left.expected_store_version
                    == right.expected_store_version
               && left.object_kind == right.object_kind
               && left.stable_type == right.stable_type
               && same_target (left.source, right.source)
               && same_target (left.target, right.target)
               && same_capacity_bundle (
                 left.capacity_bundle,
                 right.capacity_bundle);
    }

    static bool same_aggregate_request (
      const aggregate_prepare_request_t &left,
      const aggregate_prepare_request_t &right)
    {
        if (left.aggregate_id.value != right.aggregate_id.value
            || left.aggregate_generation
                 != right.aggregate_generation
            || left.inventory_digest.value
                 != right.inventory_digest.value
            || !same_owner (left.target_owner, right.target_owner)
            || left.participants.size () != right.participants.size ()
            || left.target_descriptor.mesh_name
                 != right.target_descriptor.mesh_name
            || left.target_descriptor.rid
                 != right.target_descriptor.rid
            || left.target_descriptor_lifecycle_generation
                 != right.target_descriptor_lifecycle_generation
            || left.capacity_fences.size ()
                 != right.capacity_fences.size ()
            || !same_capacity_bundle (
              left.capacity_bundle,
              right.capacity_bundle))
            return false;
        for (std::size_t index = 0;
             index < left.participants.size (); ++index) {
            const auto &l = left.participants[index];
            const auto &r = right.participants[index];
            if (l.key.value != r.key.value
                || l.expected_store_version
                     != r.expected_store_version
                || l.authority_payload != r.authority_payload
                || l.membership_mutation
                     != r.membership_mutation
                || l.owner_transition != r.owner_transition
                || l.capacity_fence.has_value ()
                     != r.capacity_fence.has_value ()
                || (l.capacity_fence
                    && l.capacity_fence->value
                         != r.capacity_fence->value)
                || (index < left.capacity_fences.size ()
                    && left.capacity_fences[index].value
                         != right.capacity_fences[index].value))
                return false;
        }
        return true;
    }

    static std::string capacity_node_key (
      const object_creation_target_t &target)
    {
        return target.mesh_name + "\x1f"
               + std::string (target.node_rid.value ()) + "\x1f"
               + std::to_string (
                 target.node_lifecycle_generation);
    }

    static std::string actor_capacity_key (
      const object_creation_target_t &target)
    {
        return capacity_node_key (target) + "\x1f" + "actor";
    }

    static std::string spot_capacity_key (
      const object_creation_target_t &target)
    {
        return capacity_node_key (target) + "\x1f" + "spot";
    }

    static std::string spot_type_capacity_key (
      const object_creation_target_t &target,
      const spot_type_capacity_delta_t &spot_type)
    {
        return spot_capacity_key (target) + "\x1f"
               + std::to_string (
                 static_cast<int> (spot_type.object_kind))
               + "\x1f" + spot_type.stable_type;
    }

    static bool valid_capacity_bundle (
      const placement_capacity_bundle_t &bundle)
    {
        if (bundle.actor_slots
              > static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ())
            || bundle.spot_slots > 1
            || (bundle.actor_slots == 0
                && bundle.spot_slots == 0)
            || (bundle.spot_slots == 0
                && bundle.spot_type)
            || (bundle.spot_slots == 1
                && (!bundle.spot_type
                    || bundle.spot_type->slots != 1
                    || bundle.spot_type->stable_type.empty ()
                    || bundle.spot_type->object_kind
                         == placement_object_kind_t::actor)))
            return false;
        return true;
    }

    static bool bundle_matches_object (
      const placement_capacity_bundle_t &bundle,
      placement_object_kind_t kind,
      const std::string &stable_type)
    {
        if (!valid_capacity_bundle (bundle))
            return false;
        if (kind == placement_object_kind_t::actor)
            return bundle.actor_slots == 1
                   && bundle.spot_slots == 0;
        return bundle.actor_slots == 0
               && bundle.spot_slots == 1
               && bundle.spot_type
               && bundle.spot_type->object_kind == kind
               && bundle.spot_type->stable_type == stable_type;
    }

    static bool same_capacity_bundle (
      const placement_capacity_bundle_t &left,
      const placement_capacity_bundle_t &right)
    {
        if (left.actor_slots != right.actor_slots
            || left.spot_slots != right.spot_slots
            || left.spot_type.has_value ()
                 != right.spot_type.has_value ())
            return false;
        return !left.spot_type
               || (left.spot_type->object_kind
                     == right.spot_type->object_kind
                   && left.spot_type->stable_type
                        == right.spot_type->stable_type
                   && left.spot_type->slots
                        == right.spot_type->slots);
    }

    bool capacity_available (
      const mesh_node_descriptor_t &descriptor,
      const object_creation_target_t &target,
      const placement_capacity_bundle_t &bundle) const
    {
        const auto available = [&] (
          const std::string &key, std::uint32_t delta,
          std::int32_t limit) {
            if (delta == 0 || limit == 0)
                return true;
            const auto active = _active_by_placement.find (key);
            const auto reserved = _pending_by_placement.find (key);
            const auto current =
              (active == _active_by_placement.end ()
                 ? 0u
                 : active->second)
              + (reserved == _pending_by_placement.end ()
                   ? 0u
                   : reserved->second);
            return current <= static_cast<std::uint64_t> (limit)
                     - std::min<std::uint64_t> (
                       delta, static_cast<std::uint64_t> (limit))
                   && delta <= static_cast<std::uint32_t> (limit);
        };
        if (!available (
              actor_capacity_key (target), bundle.actor_slots,
              descriptor.capacity.actors.limit)
            || !available (
              spot_capacity_key (target), bundle.spot_slots,
              descriptor.capacity.spots.limit))
            return false;
        if (bundle.spot_type) {
            const auto typed = std::find_if (
              descriptor.capacity.spot_types.begin (),
              descriptor.capacity.spot_types.end (),
              [&] (const spot_type_capacity_t &candidate) {
                  return candidate.object_kind
                           == bundle.spot_type->object_kind
                         && candidate.stable_type
                              == bundle.spot_type->stable_type;
              });
            const auto limit =
              typed == descriptor.capacity.spot_types.end ()
                ? 0
                : typed->usage.limit;
            if (!available (
                  spot_type_capacity_key (
                    target, *bundle.spot_type),
                  bundle.spot_type->slots, limit))
                return false;
        }
        return true;
    }

    static void apply_capacity_bundle (
      std::map<std::string, std::uint64_t> &counters,
      const object_creation_target_t &target,
      const placement_capacity_bundle_t &bundle,
      bool add)
    {
        const auto apply = [&] (
          const std::string &key, std::uint32_t delta) {
            if (delta == 0)
                return;
            auto &current = counters[key];
            if (add)
                current += delta;
            else
                current = current >= delta
                            ? current - delta
                            : 0;
        };
        apply (actor_capacity_key (target), bundle.actor_slots);
        apply (spot_capacity_key (target), bundle.spot_slots);
        if (bundle.spot_type)
            apply (
              spot_type_capacity_key (
                target, *bundle.spot_type),
              bundle.spot_type->slots);
    }

    static bool capacity_bundle_present (
      const std::map<std::string, std::uint64_t> &counters,
      const object_creation_target_t &target,
      const placement_capacity_bundle_t &bundle)
    {
        const auto present = [&] (
          const std::string &key, std::uint32_t delta) {
            if (delta == 0)
                return true;
            const auto found = counters.find (key);
            return found != counters.end ()
                   && found->second >= delta;
        };
        return present (
                 actor_capacity_key (target),
                 bundle.actor_slots)
               && present (
                 spot_capacity_key (target),
                 bundle.spot_slots)
               && (!bundle.spot_type
                   || present (
                     spot_type_capacity_key (
                       target, *bundle.spot_type),
                     bundle.spot_type->slots));
    }

    void apply_capacity_projection (
      mesh_node_descriptor_t &descriptor) const
    {
        descriptor.capacity.actors.active = 0;
        descriptor.capacity.actors.reserved = 0;
        descriptor.capacity.spots.active = 0;
        descriptor.capacity.spots.reserved = 0;
        const object_creation_target_t target{
          descriptor.mesh_name,
          node_rid_t::from_string (descriptor.rid.to_string ()),
          descriptor.lifecycle_generation,
          {descriptor.owner_id, descriptor.lease_generation}};
        const auto count = [] (
          const std::map<std::string, std::uint64_t> &values,
          const std::string &key) {
            const auto found = values.find (key);
            return found == values.end () ? 0u : found->second;
        };
        descriptor.capacity.actors.active =
          count (_active_by_placement, actor_capacity_key (target));
        descriptor.capacity.actors.reserved =
          count (_pending_by_placement, actor_capacity_key (target));
        descriptor.capacity.spots.active =
          count (_active_by_placement, spot_capacity_key (target));
        descriptor.capacity.spots.reserved =
          count (_pending_by_placement, spot_capacity_key (target));
        for (auto &typed : descriptor.capacity.spot_types) {
            const spot_type_capacity_delta_t delta{
              typed.object_kind, typed.stable_type, 1};
            typed.usage.active =
              count (
                _active_by_placement,
                spot_type_capacity_key (target, delta));
            typed.usage.reserved =
              count (
                _pending_by_placement,
                spot_type_capacity_key (target, delta));
        }
    }

    void release_pending (const reservation_state_t &reservation)
    {
        apply_capacity_bundle (
          _pending_by_placement,
          reservation.fence.target,
          reservation.fence.capacity_bundle,
          false);
    }

    void release_relocation_pending (
      const relocation_capacity_state_t &reservation)
    {
        apply_capacity_bundle (
          _pending_by_placement,
          reservation.request.target,
          reservation.request.capacity_bundle,
          false);
    }

    bool relocation_capacity_counters_available (
      const relocation_capacity_state_t &reservation) const
    {
        return capacity_bundle_present (
                 _active_by_placement,
                 reservation.request.source,
                 reservation.request.capacity_bundle)
               && capacity_bundle_present (
                 _pending_by_placement,
                 reservation.request.target,
                 reservation.request.capacity_bundle);
    }

    void consume_relocation_capacity (
      relocation_capacity_state_t &reservation)
    {
        release_relocation_pending (reservation);
        apply_capacity_bundle (
          _active_by_placement,
          reservation.request.source,
          reservation.request.capacity_bundle,
          false);
        apply_capacity_bundle (
          _active_by_placement,
          reservation.request.target,
          reservation.request.capacity_bundle,
          true);
        reservation.status =
          relocation_reservation_status_t::committed;
    }

    static bool all_zero (
      const std::array<std::byte, 16> &value)
    {
        return std::all_of (
          value.begin (), value.end (),
          [] (std::byte item) { return item == std::byte{0}; });
    }

    static std::string aggregate_id_key (
      const aggregate_id_t &id)
    {
        static constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve (32);
        for (const auto value : id.value) {
            const auto byte = std::to_integer<unsigned char> (value);
            result.push_back (hex[byte >> 4]);
            result.push_back (hex[byte & 0x0f]);
        }
        return result;
    }

    static std::string reservation_id_key (
      const std::array<std::byte, 16> &id)
    {
        return aggregate_id_key (aggregate_id_t{id});
    }

    void cleanup_scans (clock_t::time_point now)
    {
        for (auto scan = _authority_scans.begin ();
             scan != _authority_scans.end ();) {
            if (now - scan->second.created_at > std::chrono::minutes (1))
                scan = _authority_scans.erase (scan);
            else
                ++scan;
        }
    }

    static std::size_t parse_offset (const std::string &value)
    {
        try {
            return static_cast<std::size_t> (std::stoull (value));
        }
        catch (...) {
            return 0;
        }
    }

    mutable std::mutex _gate;
    std::map<std::string, mesh_node_descriptor_t> _mesh_nodes;
    std::map<std::string, entry_spot_id_claim_t>
      _entry_spot_id_claims;
    std::map<std::string, client_server_server_descriptor_t>
      _client_servers;
    std::map<std::string, fanout_publisher_descriptor_t>
      _fanout_publishers;
    std::map<std::string, owner_lease_row_t> _leases;
    std::map<std::string, std::int64_t> _active_lease_generations;
    std::uint64_t _lease_generation = 0;
    std::map<std::string, authority_snapshot_t> _authorities;
    std::map<std::string, std::string> _object_types;
    std::map<std::string, reservation_state_t> _reservations;
    std::map<std::string, creation_terminal_record_t>
      _creation_terminals;
    std::map<std::string, relocation_capacity_state_t>
      _relocation_capacity_reservations;
    std::map<std::string, std::string>
      _relocation_capacity_by_id;
    std::map<std::string, aggregate_state_t> _aggregates;
    std::map<std::string, std::uint64_t> _pending_by_placement;
    std::map<std::string, std::uint64_t> _active_by_placement;
    std::uint64_t _object_generation = 0;
    std::uint64_t _authority_owner_generation = 0;
    std::uint64_t _store_revision = 0;
    std::map<std::string, authority_scan_state_t> _authority_scans;
    std::uint64_t _next_scan_id = 0;
};

} // namespace zlink::framework::runtime
