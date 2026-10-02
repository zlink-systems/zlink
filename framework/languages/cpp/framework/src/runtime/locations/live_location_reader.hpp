/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/options.hpp>
#include <runtime/locations/location_repository.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

/* Joins raw location rows with owner leases for every framework read path.
 * Store implementations stay policy-free and write APIs do not pass through
 * this read-only boundary. */
class live_location_reader_t final
{
  public:
    live_location_reader_t (location_repository_t &store, location_options_t options = {}) :
        _store (&store), _options (std::move (options))
    {
    }

    std::optional<std::chrono::steady_clock::duration>
    owner_admission_lifetime (const std::string &owner_id)
    {
        const auto lease = _store->read_owner_lease (owner_id).result ().value ();
        const auto *found = std::get_if<owner_lease_found_t> (&lease);
        if (found == nullptr)
            return std::nullopt;
        return admission_lifetime (found->token, found);
    }

    std::optional<std::chrono::steady_clock::duration>
    owner_admission_lifetime (const location_owner_token_t &owner)
    {
        const auto lease = _store->read_owner_lease (owner.owner_id).result ().value ();
        const auto *found = std::get_if<owner_lease_found_t> (&lease);
        return admission_lifetime (owner, found);
    }

  private:
    static bool owner_is_available (std::int64_t lease_generation, const owner_lease_found_t *found)
    {
        return found != nullptr && found->token.lease_generation == lease_generation
               && found->lease_expires_at > found->store_now;
    }

    std::optional<std::chrono::steady_clock::duration>
    admission_lifetime (const location_owner_token_t &owner, const owner_lease_found_t *found) const
    {
        if (!owner_is_available (owner.lease_generation, found)) {
            return std::nullopt;
        }
        const auto remaining =
          found->lease_expires_at - found->store_now - _options.owner_lease_fencing_margin;
        if (remaining <= std::chrono::system_clock::duration::zero ()) {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::steady_clock::duration> (remaining);
    }

  public:
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, location_page_request_t page = {})
    {
        try {
            auto result =
              _store->list_mesh_nodes (std::move (mesh_name), std::move (page)).result ().value ();
            filter_live (result.items);
            return completed (std::move (result));
        }
        catch (const std::invalid_argument &error) {
            return task_t<location_page_t<mesh_node_descriptor_t>> (
              result_t<location_page_t<mesh_node_descriptor_t>>::failure (
                framework_error_kind_t::internal_failure, error.what ()));
        }
    }

    task_t<authority_read_result_t> read_authority (authority_key_t key)
    {
        return _store->read_authority (std::move (key));
    }

    task_t<bool> owner_available (const location_owner_token_t &owner)
    {
        auto result = _store->read_owner_lease (owner.owner_id).result ();
        if (!result.has_value ()) {
            return task_t<bool> (
              detail::propagate_failure<bool> (result, "owner lease lookup failed"));
        }
        const auto *found = std::get_if<owner_lease_found_t> (&result.value ());
        return completed (owner_is_available (owner.lease_generation, found));
    }

    task_t<authority_scan_result_t> list_authorities (std::string prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit)
    {
        return _store->list_authorities (std::move (prefix), std::move (cursor), limit);
    }

  private:
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    template <typename T> void filter_live (std::vector<T> &rows)
    {
        std::map<std::string, owner_lease_read_result_t> leases;
        for (const auto &row : rows) {
            if (!leases.contains (row.owner_id))
                leases.emplace (row.owner_id,
                                _store->read_owner_lease (row.owner_id).result ().value ());
        }
        std::erase_if (rows, [&leases] (const T &row) {
            const auto *found = std::get_if<owner_lease_found_t> (&leases.at (row.owner_id));
            return !owner_is_available (row.lease_generation, found);
        });
    }

    location_repository_t *_store;
    location_options_t _options;
};

} // namespace zlink::framework::runtime
