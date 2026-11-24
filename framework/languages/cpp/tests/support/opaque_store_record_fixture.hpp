/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/location_records.hpp"
#include <gtest/gtest.h>
#include <zlink/framework/contracts/locations/stores.hpp>

namespace zlink::framework::test::opaque_store
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

inline std::vector<std::byte> bytes (std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));
    return result;
}

inline const std::string capacity_domain = std::string ("zlink:v12:capacity") + '\0';

inline store_key_t capacity_key (const mesh_node_descriptor_t &descriptor,
                                 std::string_view dimension = "actors",
                                 std::string_view state = "active")
{
    return {capacity_domain + descriptor.mesh_name + '\0' + descriptor.rid.to_hex () + '\0'
            + std::string (dimension) + '\0' + std::string (state)};
}

inline store_key_t generation_key (std::string_view kind, std::string_view id, bool owner = false)
{
    return {std::string (owner ? "zlink:v12:authority-owner-counter" : "zlink:v12:object-counter")
            + '\0' + std::string (kind) + '\0' + std::string (id)};
}

inline std::uint64_t capacity_count (location_store_t &provider, const store_key_t &key)
{
    const auto row = provider.read (key).result ().value ();
    const auto *found = std::get_if<store_found_t> (&row);
    if (!found)
        return 0;
    const auto value =
      nlohmann::json::parse (found->value.bytes.begin (), found->value.bytes.end ());
    EXPECT_TRUE (value.is_number_unsigned ());
    return value.get<std::uint64_t> ();
}

inline nlohmann::json capacity_record (location_store_t &provider,
                                       const mesh_node_descriptor_t &descriptor)
{
    nlohmann::json counts;
    for (const auto *state : {"active", "pending"})
        for (const auto *dimension : {"actors", "spots"})
            counts[state][dimension] =
              capacity_count (provider, capacity_key (descriptor, dimension, state));
    return counts;
}
}
