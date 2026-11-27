/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/object_ref.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

class public_memory_authority_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override;

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t key, std::stop_token) override;

    zlink::framework::task_t<zlink::framework::authority_compare_exchange_result_t>
    compare_exchange_authority_core (zlink::framework::authority_key_t key,
                                     std::string expected_store_version,
                                     zlink::framework::authority_mutation_t mutation,
                                     std::stop_token) override;

    zlink::framework::task_t<zlink::framework::authority_scan_result_t>
      list_authorities (std::string,
                        std::optional<zlink::framework::authority_scan_cursor_t>,
                        std::size_t,
                        std::stop_token) override;

    std::optional<zlink::framework::owner_lease_read_result_t> owner_lease_read;
    bool fail_owner_lease_read = false;
    std::map<std::string, zlink::framework::authority_snapshot_t> authority_rows;
    std::optional<zlink::framework::authority_snapshot_t> conflict_snapshot;
    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    std::optional<zlink::framework::location_owner_token_t> observed_target_owner;
    std::optional<zlink::framework::object_creation_target_t> observed_target_placement;
    std::vector<std::string> observed_keys;
    int remaining_authority_conflicts = 0;
    std::uint64_t retarget_authority_generation_advance = 1;

  private:
    template <typename T> static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }
};

zlink::framework::authority_snapshot_t source_relocation_authority (
  const object_ref_t &actor, std::string mesh, std::chrono::system_clock::time_point now);

} // namespace m6c_test
