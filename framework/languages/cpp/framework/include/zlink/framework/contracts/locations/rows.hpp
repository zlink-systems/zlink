/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/values.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace zlink::framework
{

struct object_capability_t
{
    placement_object_kind_t object_kind =
      placement_object_kind_t::actor;
    std::string stable_type;
    maintenance_policy_kind_t policy =
      maintenance_policy_kind_t::disabled;
    bool has_snapshot_adapter = false;
    std::int32_t spot_limit = 0;
};

struct capacity_usage_t
{
    std::uint64_t active = 0;
    std::uint64_t reserved = 0;
    std::int32_t limit = 0;
};

struct spot_type_capacity_t
{
    placement_object_kind_t object_kind =
      placement_object_kind_t::user_spot;
    std::string stable_type;
    capacity_usage_t usage;
};

struct placement_capacity_t
{
    capacity_usage_t actors;
    capacity_usage_t spots;
    std::vector<spot_type_capacity_t> spot_types;
};

struct activation_concurrency_t
{
    std::uint32_t active = 0;
    std::int32_t limit = 128;
};

} // namespace zlink::framework
