/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_repository_values.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace zlink::framework::runtime
{

struct entry_spot_id_claim_t
{
    std::string descriptor_key;
    std::uint64_t descriptor_lifecycle_generation;
    location_owner_token_t owner;

    bool matches (std::string_view key,
                  std::uint64_t lifecycle,
                  const location_owner_token_t &token) const
    {
        return descriptor_key == key && descriptor_lifecycle_generation == lifecycle
               && owner.owner_id == token.owner_id
               && owner.lease_generation == token.lease_generation;
    }
};

} // namespace zlink::framework::runtime
