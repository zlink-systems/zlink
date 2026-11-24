/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_location_store.hpp"
#include <atomic>
#include <string>
#include <utility>

namespace zlink::framework::test::m6b
{
class counting_location_repository_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    counting_location_repository_t ()
    {
        configure_reclaim_margin (std::chrono::milliseconds::zero ());
    }

    zlink::framework::task_t<zlink::framework::object_reserve_result_t>
    reserve_core (zlink::framework::object_reserve_request_t request,
                  std::stop_token cancellation = {}) override
    {
        reservation_calls.fetch_add (1, std::memory_order_relaxed);
        return in_memory_location_repository_t::reserve_core (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t key,
                         std::stop_token cancellation = {}) override
    {
        authority_reads.fetch_add (1, std::memory_order_relaxed);
        return in_memory_location_repository_t::read_authority_core (std::move (key), cancellation);
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh_name,
                          zlink::framework::location_page_request_t page = {}) override
    {
        mesh_node_reads.fetch_add (1);
        return in_memory_location_repository_t::list_mesh_nodes_core (std::move (mesh_name),
                                                                      std::move (page));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        owner_lease_reads.fetch_add (1);
        if (owner_lease_override)
            return zlink::framework::task_t<zlink::framework::owner_lease_read_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_read_result_t>::success (
                *owner_lease_override));
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }

    zlink::framework::task_t<zlink::framework::authority_compare_exchange_result_t>
    compare_exchange_authority_core (zlink::framework::authority_key_t key,
                                     std::string expected_store_version,
                                     zlink::framework::authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override
    {
        const std::vector<std::byte> *payload = nullptr;
        if (const auto *put = std::get_if<zlink::framework::authority_put_t> (&mutation))
            payload = &put->payload;
        else if (const auto *restore =
                   std::get_if<zlink::framework::authority_restore_t> (&mutation))
            payload = &restore->payload;
        if (payload) {
            const auto decoded =
              zlink::framework::runtime::decode_instance_spot_authority_payload (*payload);
            if (decoded) {
                if (activation_recovery_write_observer)
                    activation_recovery_write_observer (key, *decoded);
                const std::lock_guard lock (activation_recovery_writes_mutex);
                activation_recovery_writes.emplace_back (
                  key.value, decoded->activation_recovery
                               ? std::make_optional (decoded->activation_recovery->replay_cursor)
                               : std::nullopt);
            }
        }
        return in_memory_location_repository_t::compare_exchange_authority_core (
          std::move (key), std::move (expected_store_version), std::move (mutation), cancellation);
    }

    void clear_activation_recovery_writes ()
    {
        const std::lock_guard lock (activation_recovery_writes_mutex);
        activation_recovery_writes.clear ();
    }

    std::vector<std::pair<std::string, std::optional<std::uint64_t>>>
    activation_recovery_write_snapshot ()
    {
        const std::lock_guard lock (activation_recovery_writes_mutex);
        return activation_recovery_writes;
    }

    std::atomic<std::size_t> authority_reads{0};
    std::atomic<std::size_t> mesh_node_reads{0};
    std::atomic<std::size_t> owner_lease_reads{0};
    std::optional<zlink::framework::owner_lease_read_result_t> owner_lease_override;
    std::atomic<std::size_t> reservation_calls{0};
    std::function<void (const zlink::framework::authority_key_t &,
                        const zlink::framework::runtime::instance_spot_authority_payload_t &)>
      activation_recovery_write_observer;

  private:
    std::mutex activation_recovery_writes_mutex;
    std::vector<std::pair<std::string, std::optional<std::uint64_t>>> activation_recovery_writes;
};
}
