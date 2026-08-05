/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <array>
#include <runtime/locations/location_repository.hpp>

#include "runtime/locations/location_key_codec.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"

#include <zlink/framework/contracts/locations/resolvers.hpp>
#include <zlink/framework/contracts/locations/runtime_query.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime
{

class actor_location_observer_t
{
  public:
    bool accepts (const actor_location_t &row)
    {
        std::lock_guard lock (_gate);
        const auto key = location_key_codec_t::encode_actor_key (
          actor_location_key_t{row.mesh_name, row.actor_id});
        const auto version = std::pair{
          row.membership_epoch,
          row.actor_ref ? row.actor_ref->object_generation () : std::uint64_t{0}};
        auto &observed = _generations[key];
        if (version < observed) {
            return false;
        }
        observed = version;
        return row.actor_ref
               && !::zlink::framework::detail::actor_ref_access_t::empty (*row.actor_ref);
    }

  private:
    std::mutex _gate;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> _generations;
};

class store_location_resolvers_t final : public spot_address_resolver_t,
                                         public actor_address_resolver_t,
                                         public location_readiness_t
{
  public:
    explicit store_location_resolvers_t (
      location_repository_t &store,
      location_options_t options = {},
      std::shared_ptr<actor_location_observer_t> actor_locations =
        std::make_shared<actor_location_observer_t> (),
      std::string actor_mesh_name = {}) :
        _owned_reader (std::make_unique<live_location_reader_t> (store, options)),
        _store (_owned_reader.get ()), _options (std::move (options)),
        _actor_locations (std::move (actor_locations)),
        _actor_mesh_name (std::move (actor_mesh_name))
    {
    }

    void set_actor_mesh_name (std::string mesh_name)
    {
        _actor_mesh_name = std::move (mesh_name);
    }

    explicit store_location_resolvers_t (
      live_location_reader_t &store,
      location_options_t options = {},
      std::shared_ptr<actor_location_observer_t> actor_locations =
        std::make_shared<actor_location_observer_t> (),
      std::string actor_mesh_name = {}) :
        _store (&store), _options (std::move (options)),
        _actor_locations (std::move (actor_locations)),
        _actor_mesh_name (std::move (actor_mesh_name))
    {
    }

    task_t<std::vector<mesh_node_descriptor_t>>
    list_live_mesh_nodes (std::string mesh_name)
    {
        std::vector<mesh_node_descriptor_t> descriptors;
        location_page_request_t page;
        do {
            auto current = _store->list_mesh_nodes (mesh_name, page).result ().value ();
            for (auto &descriptor : current.items) {
                if (descriptor.state == framework_runtime_state_t::stopped
                    || descriptor.state == framework_runtime_state_t::error)
                    continue;
                if (_store->owner_admission_lifetime (
                      location_owner_token_t{descriptor.owner_id,
                                             descriptor.lease_generation}))
                    descriptors.push_back (std::move (descriptor));
            }
            page.continuation_token = current.continuation_token;
        } while (page.continuation_token);
        return completed (std::move (descriptors));
    }

    task_t<std::optional<spot_address_t>>
    resolve_spot_address (std::string mesh_name, std::string spot_id) override
    {
        if (auto cached = cached_route (_spot_routes, spot_id)) {
            return completed (std::optional<spot_address_t>{std::move (*cached)});
        }
        if (const auto authority = read_ready_authority (false, spot_id)) {
            const auto projected_id = decode_spot_id (authority->payload);
            if (!projected_id || *projected_id != spot_id)
                return completed (std::optional<spot_address_t>{});
            auto address = spot_address_t{
              authority->allocation.target.mesh_name,
              zlink::routing_id_t::from (
                std::string (authority->allocation.target.node_rid.value ())),
              *projected_id,
              authority->object_generation};
            address.node_generation =
              authority->allocation.target.node_lifecycle_generation;
            apply_authority (address, *authority);
            cache_ready_route (_spot_routes, spot_id, address);
            return completed (std::optional<spot_address_t>{std::move (address)});
        }
        if (!mesh_name.empty ()) {
            const auto descriptors =
              _store->list_mesh_nodes (mesh_name, {}).result ().value ();
            const auto found = std::find_if (
              descriptors.items.begin (), descriptors.items.end (),
              [&] (const mesh_node_descriptor_t &descriptor) {
                  return descriptor.entry_spot_id
                         && *descriptor.entry_spot_id == spot_id
                         && descriptor.state != framework_runtime_state_t::stopped
                         && descriptor.state != framework_runtime_state_t::error;
              });
            if (found != descriptors.items.end ())
                return completed (std::optional<spot_address_t>{spot_address_t{
                  found->mesh_name, found->rid, spot_id,
                  found->lifecycle_generation, {}, 0, 0, {},
                  found->lifecycle_generation}});
        }
        return completed (std::optional<spot_address_t>{});
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        std::lock_guard lock (_route_cache_gate);
        _spot_routes.erase (std::string (spot_id));
    }

    bool invalidate_spot_address_if_matches (
      std::string_view spot_id, const spot_address_t &expected) override
    {
        std::lock_guard lock (_route_cache_gate);
        const auto found = _spot_routes.find (std::string (spot_id));
        if (found == _spot_routes.end ())
            return false;
        const auto &cached = found->second.address;
        if (cached.spot_id != spot_id
            || cached.node_rid != expected.node_rid
            || cached.node_generation != expected.node_generation
            || cached.object_generation != expected.object_generation
            || cached.authority_owner_generation
                 != expected.authority_owner_generation
            || cached.owner.owner_id != expected.owner.owner_id
            || cached.owner.lease_generation
                 != expected.owner.lease_generation)
            return false;
        _spot_routes.erase (found);
        return true;
    }

    void invalidate_all_routes_after_store_recovery () override
    {
        std::lock_guard lock (_route_cache_gate);
        _spot_routes.clear ();
        _actor_routes.clear ();
        ++_store_recovery_generation;
    }

    void observe_spot_authority_version (std::string_view spot_id,
                                         std::string_view store_version,
                                         std::uint64_t object_generation,
                                         std::uint64_t authority_owner_generation)
    {
        invalidate_on_newer_authority (_spot_routes, spot_id, store_version,
                                       object_generation, authority_owner_generation);
    }

    void observe_actor_authority_version (std::string_view actor_id,
                                          std::string_view store_version,
                                          std::uint64_t object_generation,
                                          std::uint64_t authority_owner_generation)
    {
        invalidate_on_newer_authority (_actor_routes, actor_id, store_version,
                                       object_generation, authority_owner_generation);
    }

    task_t<std::optional<spot_address_t>>
    resolve_actor_address (std::string actor_id) override
    {
        if (auto cached = cached_route (_actor_routes, actor_id)) {
            return completed (std::optional<spot_address_t>{std::move (*cached)});
        }
        const auto authority = read_ready_authority (true, actor_id);
        if (!authority) {
            return completed (std::optional<spot_address_t>{});
        }
        const auto projection = decode_actor_authority_payload (authority->payload);
        if (!projection || projection->actor.actor_id ().value () != actor_id)
            return completed (std::optional<spot_address_t>{});
        auto address = spot_address_t{
          authority->allocation.target.mesh_name,
          zlink::routing_id_t::from (
            std::string (authority->allocation.target.node_rid.value ())),
          projection->spot_id,
          projection->spot_generation};
        address.node_generation =
          authority->allocation.target.node_lifecycle_generation;
        apply_authority (address, *authority);
        cache_ready_route (_actor_routes, actor_id, address);
        return completed (std::optional<spot_address_t>{std::move (address)});
    }

    void invalidate_actor_address (std::string_view actor_id) override
    {
        std::lock_guard lock (_route_cache_gate);
        _actor_routes.erase (std::string (actor_id));
    }

    bool invalidate_actor_address_if_matches (
      std::string_view actor_id, const spot_address_t &expected) override
    {
        std::lock_guard lock (_route_cache_gate);
        const auto found = _actor_routes.find (std::string (actor_id));
        if (found == _actor_routes.end ())
            return false;
        const auto &cached = found->second.address;
        if (cached.node_rid != expected.node_rid
            || cached.node_generation != expected.node_generation
            || cached.object_generation != expected.object_generation
            || cached.authority_owner_generation
                 != expected.authority_owner_generation
            || cached.owner.owner_id != expected.owner.owner_id
            || cached.owner.lease_generation
                 != expected.owner.lease_generation)
            return false;
        _actor_routes.erase (found);
        return true;
    }

    task_t<bool> is_peer_ready (std::string mesh_name,
                                location_role_t role,
                                std::optional<zlink::routing_id_t> node_rid = std::nullopt) override
    {
        try {
            auto descriptors = list_live_mesh_nodes (std::move (mesh_name)).result ().value ();
            if (role != location_role_t::router && role != location_role_t::spot)
                return completed (false);
            return completed (std::any_of (
              descriptors.begin (), descriptors.end (), [&] (const auto &descriptor) {
                  return !node_rid || descriptor.rid.to_hex () == node_rid->to_hex ();
              }));
        }
        catch (...) {
            return completed (false);
        }
    }

  private:
    struct cached_address_t
    {
        spot_address_t address;
        std::chrono::steady_clock::time_point expires_at;
        std::uint64_t store_recovery_generation = 0;
    };

    std::optional<spot_address_t>
    cached_route (std::map<std::string, cached_address_t> &routes,
                  std::string_view key)
    {
        if (_options.route_cache_max_age <= std::chrono::milliseconds::zero ()) {
            return std::nullopt;
        }
        std::lock_guard lock (_route_cache_gate);
        const auto found = routes.find (std::string (key));
        if (found == routes.end ()) {
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now () >= found->second.expires_at
            || found->second.store_recovery_generation != _store_recovery_generation) {
            routes.erase (found);
            return std::nullopt;
        }
        return found->second.address;
    }

    void cache_ready_route (std::map<std::string, cached_address_t> &routes,
                            std::string key,
                            const spot_address_t &address)
    {
        const auto max_age = _options.route_cache_max_age;
        if (max_age <= std::chrono::milliseconds::zero ()) {
            return;
        }
        const auto measured_at = std::chrono::steady_clock::now ();
        if (address.store_version.empty () || address.object_generation == 0
            || address.authority_owner_generation == 0
            || address.owner.owner_id.empty () || address.owner.lease_generation <= 0) {
            return;
        }
        const auto lease_lifetime = _store->owner_admission_lifetime (address.owner);
        if (!lease_lifetime) {
            return;
        }
        const auto lifetime = std::min (
          std::chrono::duration_cast<std::chrono::steady_clock::duration> (max_age),
          *lease_lifetime);
        if (lifetime <= std::chrono::steady_clock::duration::zero ()) {
            return;
        }
        std::lock_guard lock (_route_cache_gate);
        routes.insert_or_assign (
          std::move (key), cached_address_t{address, measured_at + lifetime,
                                            _store_recovery_generation});
    }

    void invalidate_on_newer_authority (
      std::map<std::string, cached_address_t> &routes,
      std::string_view key,
      std::string_view store_version,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation)
    {
        std::lock_guard lock (_route_cache_gate);
        const auto found = routes.find (std::string (key));
        if (found == routes.end ()) {
            return;
        }
        const auto &cached = found->second.address;
        const auto newer_fence = object_generation > cached.object_generation
                                 || (object_generation == cached.object_generation
                                     && authority_owner_generation
                                          > cached.authority_owner_generation);
        if (newer_fence
            || (store_version != cached.store_version
                && object_generation >= cached.object_generation
                && authority_owner_generation >= cached.authority_owner_generation)) {
            routes.erase (found);
        }
    }

    struct authority_projection_t
    {
        std::string store_version;
        std::uint64_t object_generation = 0;
        std::uint64_t authority_owner_generation = 0;
        location_owner_token_t owner;
        placement_allocation_t allocation;
        std::vector<std::byte> payload;
    };

    static std::string authority_key (char kind, std::string_view object_id)
    {
        std::string encoded;
        encoded.reserve (object_id.size ());
        constexpr char hex[] = "0123456789ABCDEF";
        for (const auto character : object_id) {
            const auto value = static_cast<unsigned char> (character);
            if (std::isalnum (value) || value == '-' || value == '.' || value == '_'
                || value == '~') {
                encoded.push_back (static_cast<char> (value));
            }
            else {
                encoded.push_back ('%');
                encoded.push_back (hex[value >> 4]);
                encoded.push_back (hex[value & 0x0f]);
            }
        }
        return "zla1:" + std::string (1, kind) + ":"
               + std::to_string (object_id.size ()) + ":" + encoded;
    }

    std::optional<authority_projection_t>
    read_ready_authority (bool actor, std::string_view object_id)
    {
        const std::array<std::string, 3> keys = actor
          ? std::array<std::string, 3>{authority_key ('a', object_id),
                                       "actor:" + std::string (object_id),
                                       "1:" + std::string (object_id)}
          : std::array<std::string, 3>{authority_key ('s', object_id),
                                       "spot:" + std::string (object_id),
                                       "2:" + std::string (object_id)};
        for (const auto &key : keys) {
            const auto read = _store->read_authority (authority_key_t{key}).result ().value ();
            const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
            if (snapshot == nullptr) {
                continue;
            }
            const auto expected_kind = actor ? placement_object_kind_t::actor
                                             : snapshot->allocation.object_kind;
            if (snapshot->allocation.state != placement_allocation_state_t::active
                || (actor && expected_kind != placement_object_kind_t::actor)
                || (!actor && expected_kind != placement_object_kind_t::user_spot
                    && expected_kind != placement_object_kind_t::instance_spot)) {
                return std::nullopt;
            }
            return authority_projection_t{snapshot->store_version,
                                          snapshot->object_generation,
                                          snapshot->authority_owner_generation,
                                          snapshot->owner,
                                          snapshot->allocation,
                                          snapshot->payload};
        }
        return std::nullopt;
    }

    static bool authority_matches (const authority_projection_t &authority,
                                   const std::string &owner_id,
                                   const std::string &mesh_name,
                                   const zlink::routing_id_t &node_rid)
    {
        return authority.owner.owner_id == owner_id
               && authority.allocation.target.mesh_name == mesh_name
               && authority.allocation.target.node_rid.value () == node_rid.to_string ();
    }

    static void apply_authority (spot_address_t &address,
                                 const authority_projection_t &authority)
    {
        address.store_version = authority.store_version;
        address.object_generation = authority.object_generation;
        address.authority_owner_generation = authority.authority_owner_generation;
        address.owner = authority.owner;
    }

    static std::optional<std::string>
    decode_spot_id (const std::vector<std::byte> &payload)
    {
        std::string text;
        text.reserve (payload.size ());
        for (const auto value : payload)
            text.push_back (static_cast<char> (std::to_integer<unsigned char> (value)));
        constexpr std::string_view user_prefix = "zlink:user-spot:ready:v1\n";
        if (text.starts_with (user_prefix)) {
            const auto type_end = text.find ('\n', user_prefix.size ());
            if (type_end == std::string::npos)
                return std::nullopt;
            const auto id_end = text.find ('\n', type_end + 1);
            if (id_end == std::string::npos || id_end == type_end + 1)
                return std::nullopt;
            return text.substr (type_end + 1, id_end - type_end - 1);
        }
        if (payload.size () < 5
            || std::to_integer<unsigned char> (payload[0]) != 'Z'
            || std::to_integer<unsigned char> (payload[1]) != 'L'
            || std::to_integer<unsigned char> (payload[2]) != 'I'
            || std::to_integer<unsigned char> (payload[3]) != 'R'
            || std::to_integer<unsigned char> (payload[4]) != 1)
            return std::nullopt;
        std::size_t offset = 5;
        const auto read_text = [&] () -> std::optional<std::string> {
            if (offset + 4 > payload.size ())
                return std::nullopt;
            std::uint32_t size = 0;
            for (int index = 0; index < 4; ++index)
                size = (size << 8)
                       | std::to_integer<std::uint8_t> (payload[offset++]);
            if (offset + size > payload.size ())
                return std::nullopt;
            std::string value;
            value.reserve (size);
            for (std::uint32_t index = 0; index < size; ++index)
                value.push_back (static_cast<char> (
                  std::to_integer<unsigned char> (payload[offset++])));
            return value;
        };
        if (!read_text ())
            return std::nullopt;
        return read_text ();
    }

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    static std::string router_channel_for (const location_options_t &options,
                                            const std::string &mesh_name)
    {
        const auto found = options.spot_router_channels.find (mesh_name);
        return found == options.spot_router_channels.end () ? mesh_name : found->second;
    }

    std::string router_channel_for (const std::string &mesh_name) const
    {
        return router_channel_for (_options, mesh_name);
    }

    std::unique_ptr<live_location_reader_t> _owned_reader;
    live_location_reader_t *_store;
    location_options_t _options;
    std::shared_ptr<actor_location_observer_t> _actor_locations;
    std::string _actor_mesh_name;
    std::mutex _route_cache_gate;
    std::map<std::string, cached_address_t> _spot_routes;
    std::map<std::string, cached_address_t> _actor_routes;
    std::uint64_t _store_recovery_generation = 0;
};

class store_location_runtime_query_t final : public location_runtime_query_t
{
  public:
    store_location_runtime_query_t (
      location_repository_t &store,
      location_runtime_t &runtime,
      const location_options_t &options,
      std::shared_ptr<actor_location_observer_t> actor_locations =
        std::make_shared<actor_location_observer_t> ()) :
        _owned_reader (std::make_unique<live_location_reader_t> (store, options)),
        _store (_owned_reader.get ()), _runtime (&runtime), _options (options),
        _actor_locations (std::move (actor_locations))
    {
    }

    store_location_runtime_query_t (live_location_reader_t &store,
                                    location_runtime_t &runtime,
                                    const location_options_t &options,
                                    std::shared_ptr<actor_location_observer_t> actor_locations =
                                      std::make_shared<actor_location_observer_t> ()) :
        _store (&store), _runtime (&runtime), _options (options),
        _actor_locations (std::move (actor_locations))
    {
    }

    task_t<location_runtime_status_t> get_status () override
    {
        location_runtime_status_t value;
        value.watch_enabled = false;
        value.polling_interval = _options.polling_interval;
        value.owner_lease_healthy = _runtime->owner_lease_healthy ();
        value.owner_lease_renewed_at = _runtime->owner_lease_renewed_at ();
        value.last_refresh_at = value.owner_lease_renewed_at;
        value.last_error = _runtime->last_error ();
        value.store_healthy = !value.last_error.has_value ();
        return completed (std::move (value));
    }

    task_t<location_page_t<location_topology_entry_t>>
    list_topology (location_topology_filter_t filter, location_page_request_t page = {}) override
    {
        auto rows = _store->list_mesh_nodes (filter.mesh_name.value_or (""), {}).result ().value ();
        std::vector<location_topology_entry_t> entries;
        entries.reserve (rows.items.size ());
        for (const auto &row : rows.items) {
            const auto state = topology_state (row.state);
            location_topology_entry_t entry{
              .mesh_name = row.mesh_name,
              .node_rid = row.rid,
              .endpoint = row.endpoint,
              .draining = row.state == framework_runtime_state_t::draining,
              .state = state,
              .updated_at = row.updated_at};
            if (matches (entry, filter))
                entries.push_back (std::move (entry));
        }
        return completed (page_in_memory (std::move (entries), page));
    }

    task_t<location_page_t<location_service_summary_t>>
    list_service_summaries (location_service_summary_filter_t filter,
                            location_page_request_t page = {}) override
    {
        auto descriptors =
          _store->list_mesh_nodes (filter.mesh_name.value_or (""), {}).result ().value ();
        std::map<std::string, location_service_summary_t> grouped;
        for (const auto &descriptor : descriptors.items) {
            auto &summary = grouped[descriptor.mesh_name];
            summary.mesh_name = descriptor.mesh_name;
            ++summary.total_count;
            switch (topology_state (descriptor.state)) {
                case location_topology_state_t::ready:
                    ++summary.ready_count;
                    break;
                case location_topology_state_t::error:
                case location_topology_state_t::lost:
                    ++summary.error_count;
                    break;
                case location_topology_state_t::stopped:
                    ++summary.stopped_count;
                    break;
                default:
                    break;
            }
            summary.last_updated_at =
              std::max (summary.last_updated_at, descriptor.updated_at);
        }
        std::vector<location_service_summary_t> result;
        result.reserve (grouped.size ());
        for (auto &[_, summary] : grouped) {
            result.push_back (std::move (summary));
        }
        return completed (page_in_memory (std::move (result), page));
    }

  private:
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    std::unique_ptr<live_location_reader_t> _owned_reader;
    live_location_reader_t *_store;
    location_runtime_t *_runtime;
    location_options_t _options;
    std::shared_ptr<actor_location_observer_t> _actor_locations;

    static bool matches (const location_topology_entry_t &entry,
                         const location_topology_filter_t &filter)
    {
        return (!filter.mesh_name || entry.mesh_name == *filter.mesh_name)
               && (!filter.node_rid || entry.node_rid == *filter.node_rid)
               && (!filter.state || entry.state == *filter.state);
    }

    static location_topology_state_t topology_state (framework_runtime_state_t state)
    {
        switch (state) {
            case framework_runtime_state_t::serving:
            case framework_runtime_state_t::draining:
                return location_topology_state_t::ready;
            case framework_runtime_state_t::stopped:
                return location_topology_state_t::stopped;
            case framework_runtime_state_t::error:
                return location_topology_state_t::error;
            default:
                return location_topology_state_t::discovered;
        }
    }

    template <typename T>
    static location_page_t<T>
    page_in_memory (std::vector<T> entries, const location_page_request_t &page)
    {
        location_page_t<T> result;
        const auto offset = page.continuation_token ? parse_offset (*page.continuation_token) : 0;
        const auto page_size =
          page.page_size > 0 ? static_cast<std::size_t> (page.page_size) : entries.size ();
        for (std::size_t i = offset; i < entries.size () && result.items.size () < page_size; ++i) {
            result.items.push_back (std::move (entries[i]));
        }
        const auto next = offset + result.items.size ();
        if (next < entries.size ()) {
            result.continuation_token = std::to_string (next);
        }
        return result;
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
};

} // namespace zlink::framework::runtime
