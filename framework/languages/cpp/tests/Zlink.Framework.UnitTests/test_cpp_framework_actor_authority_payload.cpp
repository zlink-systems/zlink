/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/actor_authority_payload.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace zlink::framework;
using namespace zlink::framework::runtime;

std::vector<std::byte> from_hex (std::string_view value)
{
    const auto digit = [] (char ch) -> std::uint8_t {
        assert ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
        return static_cast<std::uint8_t> (ch <= '9' ? ch - '0' : ch - 'a' + 10);
    };
    assert (value.size () % 2 == 0);
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (static_cast<std::byte> (
          (digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

std::string hex (std::span<const std::byte> value)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve (value.size () * 2);
    for (const auto byte : value) {
        const auto value8 = std::to_integer<std::uint8_t> (byte);
        result.push_back (digits[value8 >> 4]);
        result.push_back (digits[value8 & 0x0f]);
    }
    return result;
}

template <typename T>
bool equals_text (const T &value, std::string_view expected)
{
    if constexpr (requires { value.value (); })
        return value.value () == expected;
    else
        return value == expected;
}

void append_rid (std::vector<std::byte> &bytes, std::string_view value)
{
    actor_authority_detail::append_u8 (bytes, static_cast<std::uint8_t> (value.size ()));
    for (const auto byte : value)
        actor_authority_detail::append_u8 (bytes, static_cast<std::uint8_t> (byte));
}

std::vector<std::byte> relocation_slot (std::uint8_t phase,
                                        std::uint64_t target_attempt_generation)
{
    std::vector<std::byte> slot;
    actor_authority_detail::append_u64be (slot, 1);
    actor_authority_detail::append_u64be (slot, 2);
    actor_authority_detail::append_u64be (slot, target_attempt_generation);
    append_rid (slot, "source-node");
    actor_authority_detail::append_u64be (slot, 3);
    actor_authority_detail::append_text8 (slot, "source-owner");
    actor_authority_detail::append_u64be (slot, 4);
    if (target_attempt_generation == 0) {
        actor_authority_detail::append_u8 (slot, 0);
        actor_authority_detail::append_u64be (slot, 0);
        actor_authority_detail::append_u8 (slot, 0);
        actor_authority_detail::append_u64be (slot, 0);
    }
    else {
        append_rid (slot, "target-node");
        actor_authority_detail::append_u64be (slot, 5);
        actor_authority_detail::append_text8 (slot, "target-owner");
        actor_authority_detail::append_u64be (slot, 6);
    }
    actor_authority_detail::append_text8 (slot, "coordinator-owner");
    actor_authority_detail::append_u64be (slot, 7);
    append_rid (slot, "coordinator-node");
    actor_authority_detail::append_u64be (slot, 8);
    actor_authority_detail::append_u8 (slot, phase);
    actor_authority_detail::append_u64be (slot, 0); // applicationVersion i64
    actor_authority_detail::append_u8 (slot, 0); // sourceCleanupState pending
    actor_authority_detail::append_u8 (slot, 1); // .NET private extension marker
    actor_authority_detail::append_u64be (slot, 0); // aggregateGeneration
    return slot;
}

std::vector<std::byte> with_relocation_slot (const std::vector<std::byte> &authority,
                                              const std::vector<std::byte> &slot)
{
    const auto canonical = decode_canonical_authority_payload (authority);
    assert (canonical && canonical->body.size () >= 10);
    std::vector<std::byte> body (canonical->body.begin (), canonical->body.end () - 10);
    actor_authority_detail::append_u8 (body, 1);
    actor_authority_detail::append_u32be (body, static_cast<std::uint32_t> (slot.size ()));
    actor_authority_detail::append_bytes (body, slot);
    actor_authority_detail::append_u8 (body, 0); // activation recovery absent
    actor_authority_detail::append_u32be (body, 0);
    return encode_canonical_authority_payload ({std::move (body)});
}

actor_authority_payload_t actor_payload ()
{
    return {.state = actor_authority_state_t::ready,
            .stable_type = "A",
            .actor_id = "B",
            .current_spot_id = "C",
            .current_spot_generation = 2,
            .current_spot_kind = actor_authority_spot_kind_t::entry,
            .owner_id = "D",
            .owner_lease_generation = 3,
            .mesh_name = "E",
            .node_rid = node_rid_t::from_string ("F"),
            .node_generation = 4};
}

} // namespace

int main ()
{
    std::ifstream fixture (ZLINK_DURABLE_AUTHORITY_GOLDEN_PATH);
    assert (fixture.good ());
    nlohmann::json golden;
    fixture >> golden;
    const auto golden_bytes = from_hex (golden.at ("encodedHex").get<std::string> ());
    const auto golden_decoded = decode_canonical_authority_payload (golden_bytes);
    assert (golden_decoded);
    assert (encode_canonical_authority_payload (*golden_decoded) == golden_bytes);

    constexpr std::string_view actor_hex =
      "5a4c4155010000000000340001001001410142010143"
      "0000000000000002010144000000000000000301450146"
      "000000000000000400000000000000000000b2374797";
    const auto actor = encode_actor_authority_payload (actor_payload ());
    assert (hex (actor) == actor_hex);
    const auto decoded = decode_actor_authority_payload (actor, 17);
    assert (decoded && decoded->actor.object_generation () == 17
            && decoded->actor.actor_id ().value () == "B"
            && equals_text (decoded->spot_id, "C")
            && decoded->spot_generation == 2
            && decoded->spot_kind == actor_authority_spot_kind_t::entry
            && decoded->owner_id == "D" && decoded->owner_lease_generation == 3
            && decoded->node_generation == 4);

    // .NET source phase 2 carries an allocated relocation slot before target
    // selection: targetAttemptGeneration and every target fence are zero.
    const auto source_phase_authority =
      with_relocation_slot (actor, relocation_slot (2, 0));
    const auto source_phase = decode_actor_authority_payload (source_phase_authority, 17);
    assert (source_phase && source_phase->actor.actor_id ().value () == "B"
            && equals_text (source_phase->spot_id, "C")
            && source_phase->spot_generation == 2
            && source_phase->owner_id == "D");

    const auto target_phase_authority =
      with_relocation_slot (actor, relocation_slot (3, 9));
    const auto target_phase = decode_actor_authority_payload (target_phase_authority, 17);
    const auto target_phase_canonical =
      decode_canonical_authority_payload (target_phase_authority);
    assert (target_phase && target_phase_canonical
            && encode_canonical_authority_payload (*target_phase_canonical)
                 == target_phase_authority);

    constexpr std::string_view user_spot_hex =
      "5a4c41550100000000002c000200080200050142014101"
      "0144000000000000000301450146000000000000000400"
      "00000000000000000000034b8d34";
    const auto user_spot = encode_user_spot_authority_payload ({
      .state = user_spot_authority_state_t::ready,
      .stable_type = "A",
      .spot_id = "B",
      .owner_id = "D",
      .owner_lease_generation = 3,
      .mesh_name = "E",
      .node_rid = node_rid_t::from_string ("F"),
      .node_generation = 4});
    assert (hex (user_spot) == user_spot_hex);
    const auto decoded_user_spot =
      decode_ready_user_spot_authority_payload (user_spot);
    assert (decoded_user_spot
            && decoded_user_spot->stable_type == "A"
            && decoded_user_spot->spot_id == "B"
            && decoded_user_spot->owner_id == "D"
            && decoded_user_spot->owner_lease_generation == 3
            && decoded_user_spot->mesh_name == "E"
            && decoded_user_spot->node_rid.value () == "F"
            && decoded_user_spot->node_generation == 4);
    const auto closing_user_spot = encode_user_spot_authority_payload ({
      .state = user_spot_authority_state_t::closing,
      .stable_type = "A",
      .spot_id = "B",
      .owner_id = "D",
      .owner_lease_generation = 3,
      .mesh_name = "E",
      .node_rid = node_rid_t::from_string ("F"),
      .node_generation = 4});
    assert (!decode_ready_user_spot_authority_payload (closing_user_spot));
    const std::string_view legacy_user_spot =
      "zlink:user-spot:ready:v1\nA\nB\n1\n1";
    std::vector<std::byte> legacy_user_spot_bytes;
    legacy_user_spot_bytes.reserve (legacy_user_spot.size ());
    for (const auto byte : legacy_user_spot)
        legacy_user_spot_bytes.push_back (
          static_cast<std::byte> (static_cast<unsigned char> (byte)));
    assert (!decode_ready_user_spot_authority_payload (legacy_user_spot_bytes));

}
