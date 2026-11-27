/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/detail/binary_text_codec.hpp>

#include "runtime/locations/location_record_fields.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/sha256.hpp"

#include <nlohmann/json.hpp>
#include "../../../../../../runtime/protocol/generated/cpp/service_wire_constants.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

template <typename Entry, typename RawFieldSizes, typename Fits>
std::optional<std::size_t> bounded_page_prefix (std::span<const Entry> entries,
                                                std::size_t item_limit,
                                                std::size_t byte_limit,
                                                RawFieldSizes raw_field_sizes,
                                                Fits fits)
{
    const auto limit = std::min (item_limit, entries.size ());
    std::size_t count = 0;
    std::size_t raw_bytes = 0;
    while (count < limit) {
        const auto fields = raw_field_sizes (entries[count]);
        auto remaining = byte_limit - raw_bytes;
        auto field = fields.begin ();
        for (; field != fields.end (); ++field) {
            if (*field > remaining)
                break;
            remaining -= *field;
        }
        if (field != fields.end ())
            break;
        raw_bytes = byte_limit - remaining;
        ++count;
    }
    if (count == 0)
        return std::nullopt;
    if (fits (count))
        return count;
    std::size_t lower = 0;
    auto upper = count - 1;
    while (lower < upper) {
        const auto candidate = lower + (upper - lower + 1) / 2;
        if (fits (candidate))
            lower = candidate;
        else
            upper = candidate - 1;
    }
    return lower == 0 ? std::nullopt : std::optional{lower};
}

} // namespace zlink::framework::runtime

namespace zlink::framework::runtime::aggregate_inventory
{
using json_t = nlohmann::json;
using bytes_t = std::vector<std::byte>;

inline constexpr std::size_t page_item_limit = protocol::maintenanceAggregateParticipants;
inline constexpr std::size_t page_byte_limit = protocol::maintenanceAggregateBytes;

// #5656 §8.3 assigns one sealed source inventory to each relocation across target attempts.
inline constexpr std::uint64_t sealed_inventory_generation = 1;

inline std::string uuid (const aggregate_id_t &id)
{
    return detail::encode_uuid (std::bit_cast<std::array<std::uint8_t, 16>> (id.value));
}

inline std::string key_suffix (std::string_view id, std::uint64_t generation)
{
    std::string result (id);
    result += ':';
    result += std::to_string (generation);
    return result;
}

inline std::string key_suffix (const aggregate_fence_t &fence)
{
    return key_suffix (uuid (fence.aggregate_id), fence.aggregate_generation);
}

inline std::span<const std::byte> membership_bytes (const bytes_t &mutation)
{
    static constexpr std::array no_changes{std::byte{'['}, std::byte{']'}};
    return mutation.empty () ? std::span<const std::byte> (no_changes) : mutation;
}

inline std::string digest_hex (std::span<const std::byte> value)
{
    return detail::encode_hex (sha256 (value));
}

inline json_t entry (const aggregate_participant_t &participant, std::size_t index)
{
    return {
      {location_record_fields::index, index},
      {location_record_fields::authorityKey, participant.key.value},
      {location_record_fields::expectedStoreVersion, participant.expected_store_version},
      {location_record_fields::ownerTransition,
       participant.owner_transition == authority_generation_transition_t::new_owner ? "newOwner"
                                                                                    : "preserve"},
      {location_record_fields::authorityPayloadSha256, digest_hex (participant.authority_payload)},
      {location_record_fields::membershipMutationSha256,
       digest_hex (membership_bytes (participant.membership_mutation))}};
}

struct page_t
{
    json_t body;
    json_t reference;
};

struct tree_t
{
    std::vector<page_t> pages;
    json_t root;
};

inline tree_t build_tree (const std::vector<aggregate_participant_t> &participants)
{
    tree_t tree;
    std::vector<json_t> entries;
    entries.reserve (participants.size ());
    for (std::size_t index = 0; index != participants.size (); ++index)
        entries.push_back (entry (participants[index], index));
    for (std::size_t level = 0;; ++level) {
        std::vector<json_t> references;
        for (std::size_t offset = 0; offset < entries.size ();) {
            const auto start =
              level == 0
                ? offset
                : entries[offset].at (location_record_fields::startIndex).get<std::size_t> ();
            const auto make_page = [&] (std::size_t size) {
                const auto end = offset + size;
                const auto last_end =
                  level == 0
                    ? end
                    : entries[end - 1].at (location_record_fields::startIndex).get<std::size_t> ()
                        + entries[end - 1]
                            .at (location_record_fields::entryCount)
                            .get<std::size_t> ();
                const auto empty = json_t::array ();
                const auto items =
                  json_t::array_t (entries.begin () + offset, entries.begin () + end);
                return json_t{
                  {location_record_fields::level, level},
                  {location_record_fields::index, references.size ()},
                  {location_record_fields::startIndex, start},
                  {location_record_fields::entryCount, last_end - start},
                  {location_record_fields::entries, level == 0 ? json_t (items) : empty},
                  {location_record_fields::children, level == 0 ? empty : json_t (items)}};
            };
            const auto count = bounded_page_prefix (
              std::span<const json_t> (entries).subspan (offset), page_item_limit, page_byte_limit,
              [] (const json_t &item) { return std::array{item.dump ().size ()}; },
              [&] (std::size_t size) {
                  return make_page (size).dump ().size () <= page_byte_limit;
              });
            if (!count)
                throw std::length_error ("Aggregate inventory entry exceeds the page limit");
            auto page = make_page (*count);
            json_t reference{
              {location_record_fields::level, level},
              {location_record_fields::index, references.size ()},
              {location_record_fields::startIndex, start},
              {location_record_fields::entryCount, page.at (location_record_fields::entryCount)},
              {location_record_fields::sha256, detail::sha256_hex (page.dump ())}};
            references.push_back (reference);
            tree.pages.push_back ({std::move (page), std::move (reference)});
            offset += *count;
        }
        if (references.size () <= 1) {
            tree.root = {{location_record_fields::totalCount, participants.size ()},
                         {location_record_fields::topLevel, level},
                         {location_record_fields::topPages, std::move (references)}};
            const auto digest = detail::sha256_hex (tree.root.dump ());
            tree.root[location_record_fields::digestSha256] = digest;
            return tree;
        }
        entries = std::move (references);
    }
}

} // namespace zlink::framework::runtime::aggregate_inventory
