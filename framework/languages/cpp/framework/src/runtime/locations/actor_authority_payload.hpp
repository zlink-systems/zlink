/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

struct actor_authority_projection_t
{
    actor_ref_t actor;
    spot_id_t spot_id;
    std::uint64_t spot_generation = 0;
};

inline std::vector<std::byte> encode_actor_authority_payload (
  const actor_ref_t &actor,
  std::string_view spot_id,
  std::uint64_t spot_generation)
{
    std::vector<std::byte> bytes;
    const auto append_u32 = [&] (std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back (static_cast<std::byte> ((value >> shift) & 0xffu));
    };
    const auto append_u64 = [&] (std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back (static_cast<std::byte> ((value >> shift) & 0xffu));
    };
    const auto append_text = [&] (std::string_view value) {
        append_u32 (static_cast<std::uint32_t> (value.size ()));
        for (const auto character : value)
            bytes.push_back (static_cast<std::byte> (
              static_cast<unsigned char> (character)));
    };
    append_text ("zlink:actor-authority:v1");
    append_text (actor.node_rid ().value ());
    append_text (::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
    append_text (actor.actor_id ().value ());
    append_u64 (actor.object_generation ());
    append_text (spot_id);
    append_u64 (spot_generation);
    return bytes;
}

inline std::optional<actor_authority_projection_t>
decode_actor_authority_payload (const std::vector<std::byte> &bytes)
{
    try {
        std::size_t offset = 0;
        const auto read_u32 = [&] () {
            if (offset + 4 > bytes.size ())
                throw std::invalid_argument ("actor authority payload is truncated");
            std::uint32_t value = 0;
            for (int index = 0; index < 4; ++index)
                value = (value << 8) | std::to_integer<std::uint8_t> (bytes[offset++]);
            return value;
        };
        const auto read_u64 = [&] () {
            if (offset + 8 > bytes.size ())
                throw std::invalid_argument ("actor authority payload is truncated");
            std::uint64_t value = 0;
            for (int index = 0; index < 8; ++index)
                value = (value << 8) | std::to_integer<std::uint8_t> (bytes[offset++]);
            return value;
        };
        const auto read_text = [&] () {
            const auto size = read_u32 ();
            if (offset + size > bytes.size ())
                throw std::invalid_argument ("actor authority payload is truncated");
            std::string value;
            value.reserve (size);
            for (std::uint32_t index = 0; index < size; ++index)
                value.push_back (static_cast<char> (
                  std::to_integer<unsigned char> (bytes[offset++])));
            return value;
        };
        if (read_text () != "zlink:actor-authority:v1")
            return std::nullopt;
        const auto node = read_text ();
        const auto type = read_text ();
        const auto id = read_text ();
        const auto actor_generation = read_u64 ();
        const auto spot_id = read_text ();
        const auto spot_generation = read_u64 ();
        if (offset != bytes.size () || node.empty () || type.empty () || id.empty ()
            || actor_generation == 0 || spot_id.empty () || spot_generation == 0)
            return std::nullopt;
        return actor_authority_projection_t{
          ::zlink::framework::detail::actor_ref_access_t::make (
            node_rid_t::from_string (node), type, id, actor_generation),
          spot_id_t{spot_id}, spot_generation};
    }
    catch (...) {
        return std::nullopt;
    }
}

} // namespace zlink::framework::runtime
