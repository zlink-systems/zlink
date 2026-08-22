/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

enum class actor_authority_state_t : std::uint8_t
{
    creating = 0,
    ready = 1
};

enum class actor_authority_spot_kind_t : std::uint8_t
{
    entry = 1,
    user = 2
};

struct actor_authority_payload_t
{
    actor_authority_state_t state = actor_authority_state_t::ready;
    std::string stable_type;
    std::string actor_id;
    std::string current_spot_id;
    std::uint64_t current_spot_generation = 0;
    actor_authority_spot_kind_t current_spot_kind = actor_authority_spot_kind_t::user;
    std::string owner_id;
    std::uint64_t owner_lease_generation = 0;
    std::string mesh_name;
    node_rid_t node_rid;
    std::uint64_t node_generation = 0;
};

struct actor_authority_projection_t
{
    actor_ref_t actor;
    spot_id_t spot_id;
    std::uint64_t spot_generation = 0;
    actor_authority_state_t state = actor_authority_state_t::ready;
    actor_authority_spot_kind_t spot_kind = actor_authority_spot_kind_t::user;
    std::string owner_id;
    std::uint64_t owner_lease_generation = 0;
    std::uint64_t node_generation = 0;
};

enum class user_spot_authority_state_t : std::uint8_t
{
    creating = 0,
    ready = 1,
    closing = 2
};

struct user_spot_authority_payload_t
{
    user_spot_authority_state_t state = user_spot_authority_state_t::ready;
    std::string stable_type;
    std::string spot_id;
    std::string owner_id;
    std::uint64_t owner_lease_generation = 0;
    std::string mesh_name;
    node_rid_t node_rid;
    std::uint64_t node_generation = 0;
};

namespace actor_authority_detail
{

inline constexpr std::size_t actor_authority_maximum_bytes = 1024 * 1024;

inline bool valid_utf8 (std::string_view value) noexcept
{
    for (std::size_t index = 0; index < value.size ();) {
        const auto first = static_cast<std::uint8_t> (value[index]);
        if (first < 0x80u) {
            ++index;
            continue;
        }
        const auto continuation = [&] (std::size_t count) {
            if (index + count >= value.size ())
                return false;
            for (std::size_t offset = 1; offset <= count; ++offset)
                if ((static_cast<std::uint8_t> (value[index + offset]) & 0xc0u)
                    != 0x80u)
                    return false;
            return true;
        };
        if (first >= 0xc2u && first <= 0xdfu && continuation (1))
            index += 2;
        else if (first == 0xe0u && continuation (2)
                 && static_cast<std::uint8_t> (value[index + 1]) >= 0xa0u)
            index += 3;
        else if (first >= 0xe1u && first <= 0xecu && continuation (2))
            index += 3;
        else if (first == 0xedu && continuation (2)
                 && static_cast<std::uint8_t> (value[index + 1]) <= 0x9fu)
            index += 3;
        else if (first >= 0xeeu && first <= 0xefu && continuation (2))
            index += 3;
        else if (first == 0xf0u && continuation (3)
                 && static_cast<std::uint8_t> (value[index + 1]) >= 0x90u)
            index += 4;
        else if (first >= 0xf1u && first <= 0xf3u && continuation (3))
            index += 4;
        else if (first == 0xf4u && continuation (3)
                 && static_cast<std::uint8_t> (value[index + 1]) <= 0x8fu)
            index += 4;
        else
            return false;
    }
    return true;
}

inline bool valid_text8 (std::string_view value) noexcept
{
    return !value.empty () && value.size () <= std::numeric_limits<std::uint8_t>::max ()
           && value.find ('\0') == std::string_view::npos && valid_utf8 (value);
}

inline std::uint32_t crc32c (std::span<const std::byte> bytes) noexcept
{
    std::uint32_t value = 0xffffffffu;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t> (byte);
        for (int bit = 0; bit != 8; ++bit)
            value = (value >> 1) ^ ((value & 1u) ? 0x82f63b78u : 0u);
    }
    return ~value;
}

inline void append_u8 (std::vector<std::byte> &bytes, std::uint8_t value)
{
    bytes.push_back (static_cast<std::byte> (value));
}

inline void append_u16be (std::vector<std::byte> &bytes, std::uint16_t value)
{
    append_u8 (bytes, static_cast<std::uint8_t> (value >> 8));
    append_u8 (bytes, static_cast<std::uint8_t> (value));
}

inline void append_u32be (std::vector<std::byte> &bytes, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        append_u8 (bytes, static_cast<std::uint8_t> (value >> shift));
}

inline void append_u64be (std::vector<std::byte> &bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        append_u8 (bytes, static_cast<std::uint8_t> (value >> shift));
}

inline void append_u32le (std::vector<std::byte> &bytes, std::uint32_t value)
{
    for (int shift = 0; shift <= 24; shift += 8)
        append_u8 (bytes, static_cast<std::uint8_t> (value >> shift));
}

inline void append_bytes (std::vector<std::byte> &bytes, std::span<const std::byte> value)
{
    bytes.insert (bytes.end (), value.begin (), value.end ());
}

inline void append_text8 (std::vector<std::byte> &bytes, std::string_view value)
{
    if (!valid_text8 (value))
        throw std::invalid_argument ("actor authority text8 is invalid");
    append_u8 (bytes, static_cast<std::uint8_t> (value.size ()));
    for (const auto character : value)
        append_u8 (bytes, static_cast<std::uint8_t> (static_cast<unsigned char> (character)));
}

class reader_t
{
  public:
    explicit reader_t (std::span<const std::byte> bytes) : _bytes (bytes) {}

    bool done () const noexcept { return _offset == _bytes.size (); }
    std::size_t offset () const noexcept { return _offset; }
    std::span<const std::byte> remaining () const noexcept { return _bytes.subspan (_offset); }

    std::uint8_t u8 ()
    {
        const auto value = take (1);
        return std::to_integer<std::uint8_t> (value[0]);
    }

    std::uint16_t u16be ()
    {
        const auto value = take (2);
        return static_cast<std::uint16_t> (
          (std::to_integer<std::uint8_t> (value[0]) << 8)
          | std::to_integer<std::uint8_t> (value[1]));
    }

    std::uint16_t u16le ()
    {
        const auto value = take (2);
        return static_cast<std::uint16_t> (
          std::to_integer<std::uint8_t> (value[0])
          | (std::to_integer<std::uint8_t> (value[1]) << 8));
    }

    std::uint32_t u32be () { return u32 (true); }
    std::uint32_t u32le () { return u32 (false); }

    std::uint64_t u64be ()
    {
        const auto value = take (8);
        std::uint64_t result = 0;
        for (const auto byte : value)
            result = (result << 8) | std::to_integer<std::uint8_t> (byte);
        return result;
    }

    std::uint64_t u64le ()
    {
        const auto value = take (8);
        std::uint64_t result = 0;
        for (int index = 7; index >= 0; --index)
            result = (result << 8) | std::to_integer<std::uint8_t> (value[index]);
        return result;
    }

    std::span<const std::byte> take (std::size_t count)
    {
        if (count > _bytes.size () - _offset)
            throw std::invalid_argument ("actor authority payload is truncated");
        const auto value = _bytes.subspan (_offset, count);
        _offset += count;
        return value;
    }

    std::string text8 ()
    {
        const auto value = take (u8 ());
        std::string text;
        text.reserve (value.size ());
        for (const auto byte : value)
            text.push_back (static_cast<char> (std::to_integer<std::uint8_t> (byte)));
        if (!valid_text8 (text))
            throw std::invalid_argument ("actor authority text8 is invalid");
        return text;
    }

    std::string text16le ()
    {
        const auto value = take (u16le ());
        std::string text;
        text.reserve (value.size ());
        for (const auto byte : value)
            text.push_back (static_cast<char> (std::to_integer<std::uint8_t> (byte)));
        if (text.empty () || text.find ('\0') != std::string::npos || !valid_utf8 (text))
            throw std::invalid_argument ("actor relocation text is invalid");
        return text;
    }

  private:
    std::uint32_t u32 (bool big_endian)
    {
        const auto value = take (4);
        std::uint32_t result = 0;
        if (big_endian) {
            for (const auto byte : value)
                result = (result << 8) | std::to_integer<std::uint8_t> (byte);
        }
        else {
            for (int index = 3; index >= 0; --index)
                result = (result << 8) | std::to_integer<std::uint8_t> (value[index]);
        }
        return result;
    }

    std::span<const std::byte> _bytes;
    std::size_t _offset = 0;
};

inline bool read_optional_text8 (reader_t &reader, bool *present = nullptr)
{
    const auto bytes = reader.take (reader.u8 ());
    if (present)
        *present = !bytes.empty ();
    if (bytes.empty ())
        return true;
    std::string value;
    value.reserve (bytes.size ());
    for (const auto byte : bytes)
        value.push_back (static_cast<char> (std::to_integer<std::uint8_t> (byte)));
    return valid_text8 (value);
}

/* The durable authority's relocation slot is independent of the steady
 * application authority.  Read and validate it before projecting the latter;
 * in particular, a source-only Preparing/Captured slot has no target fence. */
inline bool read_actor_authority_relocation_state (reader_t &body_reader)
{
    const auto has_relocation = body_reader.u8 ();
    if (has_relocation > 1)
        return false;
    const auto state = body_reader.take (body_reader.u32be ());
    if (has_relocation == 0)
        return state.empty ();

    reader_t relocation_reader (state);
    const auto relocation_high = relocation_reader.u64be ();
    const auto relocation_low = relocation_reader.u64be ();
    const auto target_attempt_generation = relocation_reader.u64be ();
    if (relocation_high == 0 && relocation_low == 0)
        return false;
    if (relocation_reader.take (relocation_reader.u8 ()).empty ())
        return false;
    if (relocation_reader.u64be () == 0)
        return false;
    (void) relocation_reader.text8 ();
    if (relocation_reader.u64be () == 0)
        return false;
    const auto target_node_rid = relocation_reader.take (relocation_reader.u8 ());
    const auto target_node_generation = relocation_reader.u64be ();
    bool target_owner_present = false;
    if (!read_optional_text8 (relocation_reader, &target_owner_present))
        return false;
    const auto target_owner_lease_generation = relocation_reader.u64be ();
    (void) relocation_reader.text8 ();
    if (relocation_reader.u64be () == 0
        || relocation_reader.take (relocation_reader.u8 ()).empty ()
        || relocation_reader.u64be () == 0)
        return false;
    const auto phase = relocation_reader.u8 ();
    if (phase == 0 || phase > 9
        || (relocation_reader.u64be () & (std::uint64_t{1} << 63)) != 0
        || relocation_reader.u8 () > 2)
        return false;

    /* .NET's canonical writer appends its versioned, runtime-local progress
     * extension after the schema-owned source-cleanup byte. */
    if (!relocation_reader.done ()) {
        if (relocation_reader.u8 () != 1)
            return false;
        (void) relocation_reader.u64be ();
        if (!relocation_reader.done () && !read_optional_text8 (relocation_reader))
            return false;
        if (!relocation_reader.done ()) {
            const auto has_pointer = relocation_reader.u8 ();
            if (has_pointer > 1)
                return false;
            if (has_pointer == 1) {
                (void) relocation_reader.text16le ();
                (void) relocation_reader.u32be ();
            }
        }
        if (!relocation_reader.done ())
            return false;
    }

    const auto source_only = phase == 1 || phase == 2;
    const auto has_target = target_attempt_generation != 0 && !target_node_rid.empty ()
                            && target_node_generation != 0
                            && target_owner_present && target_owner_lease_generation != 0;
    if (source_only)
        return !has_target && target_attempt_generation == 0 && target_node_rid.empty ()
               && target_node_generation == 0 && !target_owner_present
               && target_owner_lease_generation == 0;
    return phase == 9 || has_target;
}

} // namespace actor_authority_detail

struct canonical_authority_payload_t
{
    std::vector<std::byte> body;
};

inline node_rid_t actor_authority_node_rid (const zlink::routing_id_t &rid)
{
    const auto bytes = rid.to_bytes ();
    return node_rid_t::from_string (std::string (
      reinterpret_cast<const char *> (bytes.data ()), bytes.size ()));
}

inline std::vector<std::byte> encode_canonical_authority_payload (
  const canonical_authority_payload_t &value)
{
    if (value.body.size () > actor_authority_detail::actor_authority_maximum_bytes - 15)
        throw std::invalid_argument ("canonical authority payload is too large");
    std::vector<std::byte> result;
    result.reserve (11 + value.body.size () + 4);
    for (const char byte : std::string_view{"ZLAU"})
        actor_authority_detail::append_u8 (result, static_cast<std::uint8_t> (byte));
    actor_authority_detail::append_u8 (result, 1);
    actor_authority_detail::append_u16be (result, 0);
    actor_authority_detail::append_u32be (result, static_cast<std::uint32_t> (value.body.size ()));
    actor_authority_detail::append_bytes (result, value.body);
    actor_authority_detail::append_u32be (result, actor_authority_detail::crc32c (result));
    return result;
}

inline std::optional<canonical_authority_payload_t>
decode_canonical_authority_payload (std::span<const std::byte> encoded)
{
    try {
        if (encoded.size () < 15 || encoded.size () > actor_authority_detail::actor_authority_maximum_bytes)
            return std::nullopt;
        actor_authority_detail::reader_t reader (encoded);
        constexpr std::array<std::byte, 4> magic{
          static_cast<std::byte> ('Z'), static_cast<std::byte> ('L'),
          static_cast<std::byte> ('A'), static_cast<std::byte> ('U')};
        const auto actual_magic = reader.take (4);
        if (!std::equal (actual_magic.begin (), actual_magic.end (), magic.begin ())
            || reader.u8 () != 1 || reader.u16be () != 0)
            return std::nullopt;
        const auto body = reader.take (reader.u32be ());
        const auto checksum_offset = reader.offset ();
        if (reader.u32be () != actor_authority_detail::crc32c (encoded.first (checksum_offset)) || !reader.done ())
            return std::nullopt;
        return canonical_authority_payload_t{
          std::vector<std::byte> (body.begin (), body.end ())};
    }
    catch (...) {
        return std::nullopt;
    }
}

inline std::vector<std::byte> encode_user_spot_authority_payload (
  const user_spot_authority_payload_t &value)
{
    if (value.state != user_spot_authority_state_t::creating
        && value.state != user_spot_authority_state_t::ready
        && value.state != user_spot_authority_state_t::closing)
        throw std::invalid_argument ("user Spot authority state is invalid");
    if (value.owner_lease_generation == 0 || value.node_generation == 0)
        throw std::invalid_argument ("user Spot authority generation is zero");
    const auto node_rid = zlink::routing_id_t::from (
      std::string (value.node_rid.value ())).to_bytes ();
    if (node_rid.empty () || node_rid.size () > std::numeric_limits<std::uint8_t>::max ())
        throw std::invalid_argument ("user Spot authority node RID is invalid");

    std::vector<std::byte> spot;
    actor_authority_detail::append_text8 (spot, value.spot_id);
    actor_authority_detail::append_text8 (spot, value.stable_type);
    actor_authority_detail::append_u8 (
      spot, static_cast<std::uint8_t> (value.state));
    if (spot.size () > std::numeric_limits<std::uint16_t>::max ())
        throw std::invalid_argument ("user Spot authority Spot slice is too large");

    std::vector<std::byte> object;
    actor_authority_detail::append_u8 (object, 2); // user Spot
    actor_authority_detail::append_u16be (
      object, static_cast<std::uint16_t> (spot.size ()));
    actor_authority_detail::append_bytes (object, spot);
    if (object.size () > std::numeric_limits<std::uint16_t>::max ())
        throw std::invalid_argument ("user Spot authority object slice is too large");

    std::vector<std::byte> body;
    actor_authority_detail::append_u8 (
      body, value.state == user_spot_authority_state_t::creating ? 1
            : value.state == user_spot_authority_state_t::closing ? 3
                                                                   : 0);
    actor_authority_detail::append_u8 (body, 2); // Spot
    actor_authority_detail::append_u16be (
      body, static_cast<std::uint16_t> (object.size ()));
    actor_authority_detail::append_bytes (body, object);
    actor_authority_detail::append_text8 (body, value.owner_id);
    actor_authority_detail::append_u64be (body, value.owner_lease_generation);
    actor_authority_detail::append_text8 (body, value.mesh_name);
    actor_authority_detail::append_u8 (body, static_cast<std::uint8_t> (node_rid.size ()));
    for (const auto byte : node_rid)
        actor_authority_detail::append_u8 (body, byte);
    actor_authority_detail::append_u64be (body, value.node_generation);
    actor_authority_detail::append_u8 (body, 0); // relocation absent
    actor_authority_detail::append_u32be (body, 0);
    actor_authority_detail::append_u8 (body, 0); // activation recovery absent
    actor_authority_detail::append_u32be (body, 0);

    return encode_canonical_authority_payload ({std::move (body)});
}

inline std::optional<user_spot_authority_payload_t>
decode_direct_user_spot_authority_payload (std::span<const std::byte> encoded)
{
    try {
        const auto canonical = decode_canonical_authority_payload (encoded);
        if (!canonical)
            return std::nullopt;
        actor_authority_detail::reader_t body_reader (canonical->body);
        const auto operation = body_reader.u8 ();
        if (body_reader.u8 () != 2)
            return std::nullopt;
        actor_authority_detail::reader_t object_reader (
          body_reader.take (body_reader.u16be ()));
        if (object_reader.u8 () != 2)
            return std::nullopt;
        actor_authority_detail::reader_t spot_reader (
          object_reader.take (object_reader.u16be ()));
        if (!object_reader.done ())
            return std::nullopt;
        const auto spot_id = spot_reader.text8 ();
        const auto stable_type = spot_reader.text8 ();
        const auto state = static_cast<user_spot_authority_state_t> (spot_reader.u8 ());
        if (!spot_reader.done ()
            || (state != user_spot_authority_state_t::creating
                && state != user_spot_authority_state_t::ready
                && state != user_spot_authority_state_t::closing)
            || (state == user_spot_authority_state_t::creating && operation != 1)
            || (state == user_spot_authority_state_t::ready && operation != 0)
            || (state == user_spot_authority_state_t::closing && operation != 3))
            return std::nullopt;
        const auto owner_id = body_reader.text8 ();
        const auto owner_lease_generation = body_reader.u64be ();
        const auto mesh_name = body_reader.text8 ();
        const auto node_rid_size = body_reader.u8 ();
        if (node_rid_size == 0)
            return std::nullopt;
        const auto node_rid_bytes = body_reader.take (node_rid_size);
        const auto node_generation = body_reader.u64be ();
        if (owner_lease_generation == 0 || node_generation == 0
            || body_reader.u8 () != 0 || body_reader.u32be () != 0
            || body_reader.u8 () != 0 || body_reader.u32be () != 0 || !body_reader.done ())
            return std::nullopt;
        std::string node_rid;
        node_rid.reserve (node_rid_bytes.size ());
        for (const auto byte : node_rid_bytes)
            node_rid.push_back (static_cast<char> (std::to_integer<std::uint8_t> (byte)));
        return user_spot_authority_payload_t{
          state, stable_type, spot_id, owner_id, owner_lease_generation,
          mesh_name, node_rid_t::from_string (std::move (node_rid)), node_generation};
    }
    catch (...) {
        return std::nullopt;
    }
}

inline std::optional<user_spot_authority_payload_t>
decode_ready_user_spot_authority_payload (std::span<const std::byte> encoded)
{
    const auto value = decode_direct_user_spot_authority_payload (encoded);
    if (!value || value->state != user_spot_authority_state_t::ready)
        return std::nullopt;
    return value;
}

inline std::vector<std::byte> encode_actor_authority_payload (
  const actor_authority_payload_t &value)
{
    if (value.state != actor_authority_state_t::creating
        && value.state != actor_authority_state_t::ready)
        throw std::invalid_argument ("actor authority state is invalid");
    if (value.current_spot_kind != actor_authority_spot_kind_t::entry
        && value.current_spot_kind != actor_authority_spot_kind_t::user)
        throw std::invalid_argument ("actor authority Spot kind is invalid");
    if (value.current_spot_generation == 0 || value.owner_lease_generation == 0
        || value.node_generation == 0)
        throw std::invalid_argument ("actor authority generation is zero");
    const auto node_rid = zlink::routing_id_t::from (
      std::string (value.node_rid.value ())).to_bytes ();
    if (node_rid.empty () || node_rid.size () > std::numeric_limits<std::uint8_t>::max ())
        throw std::invalid_argument ("actor authority node RID is invalid");

    std::vector<std::byte> actor;
    actor_authority_detail::append_text8 (actor, value.stable_type);
    actor_authority_detail::append_text8 (actor, value.actor_id);
    actor_authority_detail::append_u8 (actor, static_cast<std::uint8_t> (value.state));
    actor_authority_detail::append_text8 (actor, value.current_spot_id);
    actor_authority_detail::append_u64be (actor, value.current_spot_generation);
    actor_authority_detail::append_u8 (actor, static_cast<std::uint8_t> (value.current_spot_kind));
    if (actor.size () > std::numeric_limits<std::uint16_t>::max ())
        throw std::invalid_argument ("actor authority actor slice is too large");

    std::vector<std::byte> body;
    actor_authority_detail::append_u8 (body, value.state == actor_authority_state_t::creating ? 1 : 0);
    actor_authority_detail::append_u8 (body, 1); // actor
    actor_authority_detail::append_u16be (body, static_cast<std::uint16_t> (actor.size ()));
    actor_authority_detail::append_bytes (body, actor);
    actor_authority_detail::append_text8 (body, value.owner_id);
    actor_authority_detail::append_u64be (body, value.owner_lease_generation);
    actor_authority_detail::append_text8 (body, value.mesh_name);
    actor_authority_detail::append_u8 (body, static_cast<std::uint8_t> (node_rid.size ()));
    for (const auto byte : node_rid)
        actor_authority_detail::append_u8 (body, byte);
    actor_authority_detail::append_u64be (body, value.node_generation);
    actor_authority_detail::append_u8 (body, 0); // relocation absent
    actor_authority_detail::append_u32be (body, 0);
    actor_authority_detail::append_u8 (body, 0); // activation recovery absent
    actor_authority_detail::append_u32be (body, 0);

    return encode_canonical_authority_payload ({std::move (body)});
}

/* Compatibility entry point for the existing C++ authority publication call
 * sites.  The Location Store owns the object generation, so it is deliberately
 * absent from the durable ZLAU body. */
inline std::vector<std::byte> encode_actor_authority_payload (
  const actor_ref_t &actor,
  std::string_view spot_id,
  std::uint64_t spot_generation)
{
    const auto node = actor.node_rid ().value ();
    return encode_actor_authority_payload (actor_authority_payload_t{
      .state = actor_authority_state_t::ready,
      .stable_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_id = std::string (actor.actor_id ().value ()),
      .current_spot_id = std::string (spot_id),
      .current_spot_generation = spot_generation,
      .current_spot_kind = actor_authority_spot_kind_t::user,
      .owner_id = std::string (node),
      .owner_lease_generation = 1,
      .mesh_name = actor.mesh_name ().empty () ? "default" : std::string (actor.mesh_name ()),
      .node_rid = actor.node_rid (),
      .node_generation = spot_generation});
}

inline std::optional<actor_authority_payload_t>
decode_direct_actor_authority_payload (std::span<const std::byte> encoded)
{
    try {
        const auto canonical = decode_canonical_authority_payload (encoded);
        if (!canonical)
            return std::nullopt;
        actor_authority_detail::reader_t body_reader (canonical->body);
        const auto operation = body_reader.u8 ();
        if (body_reader.u8 () != 1)
            return std::nullopt;
        actor_authority_detail::reader_t actor_reader (body_reader.take (body_reader.u16be ()));
        const auto stable_type = actor_reader.text8 ();
        const auto actor_id = actor_reader.text8 ();
        const auto state = static_cast<actor_authority_state_t> (actor_reader.u8 ());
        const auto spot_id = actor_reader.text8 ();
        const auto spot_generation = actor_reader.u64be ();
        const auto spot_kind = static_cast<actor_authority_spot_kind_t> (actor_reader.u8 ());
        if (!actor_reader.done () || spot_generation == 0
            || (state != actor_authority_state_t::creating && state != actor_authority_state_t::ready)
            || (spot_kind != actor_authority_spot_kind_t::entry
                && spot_kind != actor_authority_spot_kind_t::user)
            || (state == actor_authority_state_t::creating && operation != 1)
            || (state == actor_authority_state_t::ready && operation != 0))
            return std::nullopt;
        const auto owner_id = body_reader.text8 ();
        const auto owner_lease_generation = body_reader.u64be ();
        const auto mesh_name = body_reader.text8 ();
        const auto node_rid_size = body_reader.u8 ();
        if (node_rid_size == 0)
            return std::nullopt;
        const auto node_rid_bytes = body_reader.take (node_rid_size);
        const auto node_generation = body_reader.u64be ();
        if (owner_lease_generation == 0 || node_generation == 0
            || !actor_authority_detail::read_actor_authority_relocation_state (body_reader)
            || body_reader.u8 () != 0 || body_reader.u32be () != 0 || !body_reader.done ())
            return std::nullopt;
        std::string node_rid;
        node_rid.reserve (node_rid_bytes.size ());
        for (const auto byte : node_rid_bytes)
            node_rid.push_back (static_cast<char> (std::to_integer<std::uint8_t> (byte)));
        return actor_authority_payload_t{
          state, stable_type, actor_id, spot_id, spot_generation, spot_kind,
          owner_id, owner_lease_generation, mesh_name,
          node_rid_t::from_string (std::move (node_rid)), node_generation};
    }
    catch (...) {
        return std::nullopt;
    }
}

inline std::optional<actor_authority_projection_t>
decode_actor_authority_payload (const std::vector<std::byte> &bytes,
                                std::uint64_t object_generation)
{
    if (object_generation == 0)
        return std::nullopt;
    const auto payload = decode_direct_actor_authority_payload (bytes);
    if (!payload)
        return std::nullopt;
    return actor_authority_projection_t{
      ::zlink::framework::detail::actor_ref_access_t::make (
        payload->node_rid, payload->stable_type, payload->actor_id, object_generation,
        payload->mesh_name),
      spot_id_t{payload->current_spot_id}, payload->current_spot_generation,
      payload->state, payload->current_spot_kind, payload->owner_id,
      payload->owner_lease_generation, payload->node_generation};
}

} // namespace zlink::framework::runtime
