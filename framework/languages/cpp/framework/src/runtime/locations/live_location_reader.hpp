/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/locations/options.hpp>
#include <runtime/locations/location_repository.hpp>
#include "runtime/execution/task_result.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
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
        if (owner.owner_id.empty () || owner.lease_generation <= 0
            || !owner_is_available (owner.lease_generation, found)
            || found->token.owner_id != owner.owner_id) {
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

    struct mesh_source_t
    {
        mesh_node_descriptor_t descriptor;
        std::chrono::steady_clock::time_point admission_deadline;
        bool available () const noexcept
        {
            return std::chrono::steady_clock::now () < admission_deadline;
        }
    };
    using mesh_sources_t = std::vector<mesh_source_t>;

    // Auto-connect and request acceptance share these descriptor originals and lease deadlines.
    task_t<std::shared_ptr<const mesh_sources_t>> poll_mesh_sources (std::string mesh_name)
    {
        auto sources = std::make_shared<mesh_sources_t> ();
        std::map<std::string,
                 std::pair<owner_lease_read_result_t, std::chrono::steady_clock::time_point>>
          leases;
        location_page_request_t page;
        do {
            auto listed = co_await _store->list_mesh_nodes (mesh_name, page);
            for (auto &descriptor : listed.items) {
                if (!leases.contains (descriptor.owner_id)) {
                    const auto started = std::chrono::steady_clock::now ();
                    auto lease = co_await _store->read_owner_lease (descriptor.owner_id);
                    leases.emplace (descriptor.owner_id,
                                    std::make_pair (std::move (lease), started));
                }
                const auto &observation = leases.at (descriptor.owner_id);
                const auto *lease = std::get_if<owner_lease_found_t> (&observation.first);
                const auto lifetime =
                  admission_lifetime ({descriptor.owner_id, descriptor.lease_generation}, lease);
                const auto deadline = lifetime ? observation.second + *lifetime
                                               : std::chrono::steady_clock::time_point{};
                sources->push_back ({std::move (descriptor), deadline});
            }
            page.continuation_token = std::move (listed.continuation_token);
        } while (page.continuation_token);
        std::sort (sources->begin (), sources->end (), [] (const auto &left, const auto &right) {
            return rid_less (left.descriptor.rid, right.descriptor.rid);
        });
        auto previous = std::atomic_load_explicit (&_mesh_sources, std::memory_order_acquire);
        do {
            auto next = std::make_shared<mesh_source_catalog_t> (*previous);
            next->insert_or_assign (mesh_name, sources);
            std::shared_ptr<const mesh_source_catalog_t> published = std::move (next);
            if (std::atomic_compare_exchange_weak_explicit (&_mesh_sources, &previous, published,
                                                            std::memory_order_release,
                                                            std::memory_order_acquire))
                break;
        } while (true);
        co_return sources;
    }

    std::optional<std::shared_ptr<const mesh_node_descriptor_t>>
    cached_request_source (const std::string &mesh_name,
                           const std::vector<std::uint8_t> &rid,
                           std::uint64_t generation) const
    {
        const auto catalog = std::atomic_load_explicit (&_mesh_sources, std::memory_order_acquire);
        const auto mesh = catalog->find (mesh_name);
        if (mesh != catalog->end ())
            if (const auto *source = find_mesh_source (*mesh->second, rid, generation))
                return source_original (mesh->second, source);
        return std::nullopt;
    }

    task_t<std::shared_ptr<const mesh_node_descriptor_t>> request_source (
      std::string mesh_name, const std::vector<std::uint8_t> &rid, std::uint64_t generation)
    {
        if (auto cached = cached_request_source (mesh_name, rid, generation))
            return task_t<std::shared_ptr<const mesh_node_descriptor_t>> (
              result_t<std::shared_ptr<const mesh_node_descriptor_t>>::success (
                std::move (*cached)));
        return read_request_source (std::move (mesh_name), rid, generation);
    }

  private:
    static const mesh_source_t *find_mesh_source (const mesh_sources_t &sources,
                                                  const std::vector<std::uint8_t> &rid,
                                                  std::uint64_t generation)
    {
        auto source = std::lower_bound (
          sources.begin (), sources.end (), rid, [] (const auto &candidate, const auto &identity) {
              const auto &value = candidate.descriptor.rid;
              return std::lexicographical_compare (value.data (), value.data () + value.size (),
                                                   identity.begin (), identity.end ());
          });
        for (; source != sources.end (); ++source) {
            const auto &identity = source->descriptor.rid;
            if (identity.size () != rid.size ()
                || !std::equal (rid.begin (), rid.end (), identity.data ()))
                break;
            if (source->descriptor.lifecycle_generation == generation)
                return &*source;
        }
        return nullptr;
    }

    static bool rid_less (const zlink::routing_id_t &left, const zlink::routing_id_t &right)
    {
        return std::lexicographical_compare (left.data (), left.data () + left.size (),
                                             right.data (), right.data () + right.size ());
    }

    static std::shared_ptr<const mesh_node_descriptor_t>
    source_original (const std::shared_ptr<const mesh_sources_t> &sources,
                     const mesh_source_t *source)
    {
        if (!source || !source->available ())
            return {};
        return {sources, &source->descriptor};
    }

    task_t<std::shared_ptr<const mesh_node_descriptor_t>> read_request_source (
      std::string mesh_name, std::vector<std::uint8_t> rid, std::uint64_t generation)
    {
        const auto sources = co_await poll_mesh_sources (std::move (mesh_name));
        co_return source_original (sources, find_mesh_source (*sources, rid, generation));
    }

  public:
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

    using mesh_source_catalog_t = std::map<std::string, std::shared_ptr<const mesh_sources_t>>;
    std::shared_ptr<const mesh_source_catalog_t> _mesh_sources{
      std::make_shared<const mesh_source_catalog_t> ()};
    location_repository_t *_store;
    location_options_t _options;
};

} // namespace zlink::framework::runtime
