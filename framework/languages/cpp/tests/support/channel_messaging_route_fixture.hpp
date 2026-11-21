/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/spot_address_resolvers.hpp"
#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace framework_channel_messaging_fixture
{
class test_spot_address_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    void set (std::string spot_id, zlink::framework::runtime::spot_address_t address)
    {
        _addresses[std::move (spot_id)] = std::move (address);
    }

    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        ++resolve_count;
        const auto found = _addresses.find (spot_id);
        co_return found == _addresses.end ()
          ? std::nullopt
          : std::optional<zlink::framework::runtime::spot_address_t> (found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        ++invalidate_count;
        _addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override { _addresses.clear (); }

    std::atomic_int resolve_count{0};
    std::atomic_int invalidate_count{0};

  private:
    std::map<std::string, zlink::framework::runtime::spot_address_t> _addresses;
};
} // namespace framework_channel_messaging_fixture
