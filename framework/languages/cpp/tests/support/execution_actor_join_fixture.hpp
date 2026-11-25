/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace zlink::framework::test::execution
{
class wire_actor_join_authority_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    std::optional<zlink::framework::authority_snapshot_t> spot_snapshot;

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t key, std::stop_token) override
    {
        const auto &selected = key.value.starts_with ("zla1:s:") ? spot_snapshot : snapshot;
        if (selected) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{*selected}));
        }
        return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
          zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
            zlink::framework::authority_read_result_t{
              zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}));
    }
};

class wire_join_spot_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        co_return spot_id == address.spot_id ? std::make_optional (address) : std::nullopt;
    }

    void invalidate_spot_address (std::string_view) override {}

    void invalidate_all_routes_after_store_recovery () override {}

    zlink::framework::runtime::spot_address_t address;
};
}
