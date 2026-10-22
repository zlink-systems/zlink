/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/stateful/object_ref.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace zlink::framework::runtime::stateful
{
using inventory_digest_t = std::array<std::uint8_t, 32>;

struct authority_relocation_reference_t
{
    object_ref_t source;
    object_ref_t target;
    std::string relocation_reference;
    std::uint32_t checksum_crc32c = 0;
    inventory_digest_t inventory_digest{};
    location_owner_token_t target_owner;
    std::vector<std::byte> application_payload;
};

class authority_relocation_payload_codec_t
{
    static void append_u32 (std::vector<std::byte> &output, std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            output.push_back (static_cast<std::byte> ((value >> shift) & 0xffu));
    }

    static void append_u64 (std::vector<std::byte> &output, std::uint64_t value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
            output.push_back (static_cast<std::byte> ((value >> shift) & 0xffu));
    }

    static void append_text (std::vector<std::byte> &output, std::string_view value)
    {
        append_u32 (output, static_cast<std::uint32_t> (value.size ()));
        for (const auto character : value)
            output.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));
    }

    static constexpr std::string_view authority_relocation_reference_prefix = "ZLRA";
    static constexpr std::uint8_t authority_relocation_reference_version = 3;
    static constexpr std::uint8_t authority_relocation_reference_wire_version =
      static_cast<std::uint8_t> ('0' + authority_relocation_reference_version);

  public:
    static std::vector<std::byte> encode (const authority_relocation_reference_t &reference)
    {
        std::vector<std::byte> output;
        for (const auto value : authority_relocation_reference_prefix)
            output.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
        output.push_back (static_cast<std::byte> (authority_relocation_reference_wire_version));
        output.push_back (static_cast<std::byte> (reference.source.kind));
        append_text (output, reference.source.key);
        append_text (output, reference.source.mesh_name);
        append_text (output, reference.source.node_id);
        append_u64 (output, reference.source.object_generation);
        append_u64 (output, reference.source.authority_owner_generation);
        append_text (output, reference.target.mesh_name);
        append_text (output, reference.target.node_id);
        append_text (output, reference.relocation_reference);
        append_u32 (output, reference.checksum_crc32c);
        for (const auto value : reference.inventory_digest)
            output.push_back (static_cast<std::byte> (value));
        append_u32 (output, static_cast<std::uint32_t> (reference.application_payload.size ()));
        output.insert (output.end (), reference.application_payload.begin (),
                       reference.application_payload.end ());
        return output;
    }

  private:
    class reader_t
    {
      public:
        explicit reader_t (const std::vector<std::byte> &input) : _input (input) {}

        std::optional<std::uint8_t> consume_version ()
        {
            constexpr auto prefix = authority_relocation_reference_prefix;
            if (_input.size () < prefix.size () + 1)
                return std::nullopt;
            for (const auto value : prefix) {
                if (read_byte () != static_cast<std::uint8_t> (value))
                    return std::nullopt;
            }
            const auto version = read_byte ();
            return version == authority_relocation_reference_wire_version
                     ? std::optional<std::uint8_t>{authority_relocation_reference_version}
                     : std::nullopt;
        }

        std::optional<std::uint8_t> byte ()
        {
            if (_offset >= _input.size ())
                return std::nullopt;
            return read_byte ();
        }

        std::optional<std::uint32_t> u32 ()
        {
            std::uint32_t value = 0;
            for (int index = 0; index != 4; ++index) {
                const auto next = byte ();
                if (!next)
                    return std::nullopt;
                value = (value << 8) | *next;
            }
            return value;
        }

        std::optional<std::uint64_t> u64 ()
        {
            std::uint64_t value = 0;
            for (int index = 0; index != 8; ++index) {
                const auto next = byte ();
                if (!next)
                    return std::nullopt;
                value = (value << 8) | *next;
            }
            return value;
        }

        std::optional<std::string> text ()
        {
            const auto size = u32 ();
            if (!size || _offset + *size > _input.size ())
                return std::nullopt;
            std::string value;
            value.reserve (*size);
            for (std::uint32_t index = 0; index != *size; ++index)
                value.push_back (static_cast<char> (read_byte ()));
            return value;
        }

        std::size_t remaining () const noexcept { return _input.size () - _offset; }

      private:
        std::uint8_t read_byte () { return std::to_integer<std::uint8_t> (_input[_offset++]); }

        const std::vector<std::byte> &_input;
        std::size_t _offset = 0;
    };

  public:
    // Authority readers share the same relocation reference decoder as publication.
    static std::optional<authority_relocation_reference_t>
    decode (const authority_snapshot_t &snapshot)
    {
        if (const auto projection = runtime::decode_actor_authority_payload (
              snapshot.payload, snapshot.object_generation)) {
            object_ref_t target{object_kind_t::actor,
                                std::string (projection->actor.actor_id ().value ()),
                                snapshot.object_generation,
                                snapshot.authority_owner_generation,
                                std::string (projection->actor.mesh_name ()),
                                std::string (projection->actor.node_rid ().value ())};
            auto source = target;
            if (source.authority_owner_generation != 0)
                --source.authority_owner_generation;
            return authority_relocation_reference_t{
              std::move (source), std::move (target), {}, 0, {}, snapshot.owner, snapshot.payload};
        }
        reader_t reader (snapshot.payload);
        if (!reader.consume_version ())
            return std::nullopt;
        const auto kind = reader.byte ();
        const auto key = reader.text ();
        const auto mesh = reader.text ();
        const auto source_node = reader.text ();
        const auto object_generation = reader.u64 ();
        const auto source_owner_generation = reader.u64 ();
        const auto target_mesh = reader.text ();
        const auto target_node = reader.text ();
        const auto relocation_reference = reader.text ();
        const auto checksum = reader.u32 ();
        if (!kind || !key || !mesh || !source_node || !object_generation || !source_owner_generation
            || !target_mesh || !target_node || !relocation_reference || !checksum)
            return std::nullopt;
        inventory_digest_t digest{};
        for (auto &value : digest) {
            const auto next = reader.byte ();
            if (!next)
                return std::nullopt;
            value = *next;
        }
        const auto application_payload_size = reader.u32 ();
        if (!application_payload_size || *application_payload_size != reader.remaining ())
            return std::nullopt;
        std::vector<std::byte> application_payload;
        application_payload.reserve (*application_payload_size);
        for (std::uint32_t index = 0; index != *application_payload_size; ++index) {
            const auto next = reader.byte ();
            if (!next)
                return std::nullopt;
            application_payload.push_back (static_cast<std::byte> (*next));
        }
        const auto object_kind = static_cast<object_kind_t> (*kind);
        if (object_kind != object_kind_t::actor && object_kind != object_kind_t::user_spot
            && object_kind != object_kind_t::instance_spot)
            return std::nullopt;
        object_ref_t source{object_kind,        std::move (*key),
                            *object_generation, *source_owner_generation,
                            std::move (*mesh),  std::move (*source_node)};
        auto target = source;
        target.mesh_name = std::move (*target_mesh);
        target.node_id = std::move (*target_node);
        target.authority_owner_generation = snapshot.authority_owner_generation;
        return authority_relocation_reference_t{std::move (source),
                                                std::move (target),
                                                std::move (*relocation_reference),
                                                *checksum,
                                                digest,
                                                snapshot.owner,
                                                std::move (application_payload)};
    }

    // Remove relocation metadata before projecting the current Spot authority.
    static void project_application_payload (authority_snapshot_t &snapshot)
    {
        while (auto reference = decode (snapshot)) {
            if (reference->target.kind == object_kind_t::actor)
                return;
            snapshot.payload = std::move (reference->application_payload);
        }
    }

    // Location membership and address projection use the publication codec.
    static std::optional<runtime::user_spot_authority_payload_t>
    decode_ready_user_spot (const authority_snapshot_t &snapshot)
    {
        if (auto ready = runtime::decode_ready_user_spot_authority_payload (snapshot.payload))
            return ready;
        authority_snapshot_t application;
        application.payload = snapshot.payload;
        project_application_payload (application);
        return runtime::decode_ready_user_spot_authority_payload (application.payload);
    }
};
} // namespace zlink::framework::runtime::stateful
