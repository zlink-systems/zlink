/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/options.hpp>
#include <runtime/locations/location_repository.hpp>
#include "runtime/execution/task_result.hpp"
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

    task_t<std::optional<std::chrono::steady_clock::duration>>
    owner_admission_lifetime (std::string owner_id)
    {
        const auto lease = co_await _store->read_owner_lease (std::move (owner_id));
        const auto *found = std::get_if<owner_lease_found_t> (&lease);
        if (found == nullptr)
            co_return std::nullopt;
        co_return admission_lifetime (found->token, found);
    }

    task_t<std::optional<std::chrono::steady_clock::duration>>
    owner_admission_lifetime (location_owner_token_t owner)
    {
        const auto lease = co_await _store->read_owner_lease (owner.owner_id);
        const auto *found = std::get_if<owner_lease_found_t> (&lease);
        co_return admission_lifetime (owner, found);
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
              co_await _store->list_mesh_nodes (std::move (mesh_name), std::move (page));
            co_await filter_live (result.items);
            co_return std::move (result);
        }
        catch (const std::invalid_argument &error) {
            co_return result_t<location_page_t<mesh_node_descriptor_t>>::failure (
              framework_error_kind_t::internal_failure, error.what ());
        }
    }

    /* Every stored descriptor, including those whose owner lease is gone.
     * Location runtime §7.4 service summaries classify these by the lease. */
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_stored_mesh_nodes (std::string mesh_name, location_page_request_t page = {})
    {
        return _store->list_mesh_nodes (std::move (mesh_name), std::move (page));
    }

    task_t<authority_read_result_t> read_authority (authority_key_t key)
    {
        return _store->read_authority (std::move (key));
    }

    task_t<bool> owner_available (location_owner_token_t owner)
    {
        auto result = co_await await_result (_store->read_owner_lease (owner.owner_id));
        if (!result.has_value ())
            co_return detail::propagate_failure<bool> (result, "owner lease lookup failed");
        const auto *found = std::get_if<owner_lease_found_t> (&result.value ());
        co_return owner_is_available (owner.lease_generation, found);
    }

    task_t<authority_scan_result_t> list_authorities (std::string prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit)
    {
        return _store->list_authorities (std::move (prefix), std::move (cursor), limit);
    }

  private:
    template <typename T> task_t<void> filter_live (std::vector<T> &rows)
    {
        std::map<std::string, owner_lease_read_result_t> leases;
        for (const auto &row : rows) {
            if (!leases.contains (row.owner_id))
                leases.emplace (row.owner_id, co_await _store->read_owner_lease (row.owner_id));
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
