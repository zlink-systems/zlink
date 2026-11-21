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

inline store_key_t capacity_key (const mesh_node_descriptor_t &descriptor)
{
    // These fixture Mesh names and RIDs contain URI-safe ASCII only.
    return {"zlink:v11:capacity:" + descriptor.mesh_name + ":" + descriptor.rid.to_string ()};
}

inline nlohmann::json capacity_record (location_store_t &provider,
                                       const mesh_node_descriptor_t &descriptor)
{
    const auto row = provider.read (capacity_key (descriptor)).result ().value ();
    const auto *found = std::get_if<store_found_t> (&row);
    EXPECT_NE (found, nullptr);
    return found
             ? nlohmann::json::parse (reinterpret_cast<const char *> (found->value.bytes.data ()),
                                      reinterpret_cast<const char *> (found->value.bytes.data ())
                                        + found->value.bytes.size ())
             : nlohmann::json{};
}
}
