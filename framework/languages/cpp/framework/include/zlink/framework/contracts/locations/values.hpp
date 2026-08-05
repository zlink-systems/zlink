/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <string>

namespace zlink::framework
{

// Matches the core discovery uint16_t service_role values. Value 1 is the
// reserved slot for the removed gateway role and must not be reused.
enum class location_role_t : std::uint16_t
{
    invalid = 0,
    spot = 2,
    router = 3,
    dealer = 4,
    pub = 5,
    sub = 6
};

enum class placement_object_kind_t
{
    actor = 1,
    user_spot = 2,
    instance_spot = 3
};

enum class maintenance_policy_kind_t : std::uint8_t
{
    disabled = 1,
    recreate = 2,
    snapshot = 3
};

enum class object_role_t : std::uint8_t
{
    none = 0,
    client = 1,
    server = 2
};

} // namespace zlink::framework
