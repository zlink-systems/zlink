/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <cstdint>
#include <string>

namespace zlink::framework::runtime::stateful
{
enum class object_kind_t
{
    actor,
    user_spot,
    instance_spot
};

struct object_ref_t
{
    object_kind_t kind = object_kind_t::actor;
    std::string key;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string mesh_name;
    std::string node_id;

    friend bool operator== (const object_ref_t &, const object_ref_t &) = default;
};

} // namespace zlink::framework::runtime::stateful
