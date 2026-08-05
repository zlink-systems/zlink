/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/options.hpp>
#include <runtime/locations/location_repository.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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
        return admission_lifetime (found);
    }

    std::optional<std::chrono::steady_clock::duration>
    owner_admission_lifetime (const location_owner_token_t &owner)
    {
        const auto lease = _store->read_owner_lease (owner.owner_id).result ().value ();
        const auto *found = std::get_if<owner_lease_found_t> (&lease);
        if (found == nullptr || found->token.lease_generation != owner.lease_generation) {
            return std::nullopt;
        }
        return admission_lifetime (found);
    }

  private:
    std::optional<std::chrono::steady_clock::duration>
    admission_lifetime (const owner_lease_found_t *found) const
    {
        if (found == nullptr || found->lease_expires_at <= found->store_now) {
            return std::nullopt;
        }
        const auto remaining = found->lease_expires_at - found->store_now
                               - _options.owner_lease_fencing_margin;
        if (remaining <= std::chrono::system_clock::duration::zero ()) {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::steady_clock::duration> (remaining);
    }

  public:

    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, location_page_request_t page = {})
    {
        auto result =
          _store->list_mesh_nodes (std::move (mesh_name), std::move (page))
            .result ()
            .value ();
        filter_live (result.items);
        return completed (std::move (result));
    }

    task_t<authority_read_result_t> read_authority (authority_key_t key)
    {
        return _store->read_authority (std::move (key));
    }

  private:
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    std::set<std::string> live_owners (
      const std::set<std::string> &owner_ids)
    {
        std::set<std::string> owners;
        for (const auto &owner_id : owner_ids) {
            const auto lease =
              _store->read_owner_lease (owner_id)
                .result ()
                .value ();
            const auto *found =
              std::get_if<owner_lease_found_t> (&lease);
            if (found != nullptr
                && found->lease_expires_at
                     > found->store_now)
                owners.insert (owner_id);
        }
        return owners;
    }

    template <typename T> std::optional<T> live (std::optional<T> row)
    {
        if (!row) {
            return std::nullopt;
        }
        const auto owners =
          live_owners ({row->owner_id});
        return owners.contains (row->owner_id) ? std::move (row) : std::nullopt;
    }

    template <typename T> void filter_live (std::vector<T> &rows)
    {
        std::set<std::string> owner_ids;
        for (const auto &row : rows)
            owner_ids.insert (row.owner_id);
        const auto owners = live_owners (owner_ids);
        std::erase_if (rows,
                       [&owners] (const T &row) { return !owners.contains (row.owner_id); });
    }

    location_repository_t *_store;
    location_options_t _options;
};

} // namespace zlink::framework::runtime
