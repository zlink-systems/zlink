/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/detail/binary_text_codec.hpp>

#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_record_fields.hpp"
#include "runtime/locations/sha256.hpp"

#include <nlohmann/json.hpp>
#include "../../../../../../runtime/protocol/generated/cpp/service_wire_constants.hpp"

#include <algorithm>
#include <array>
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

inline constexpr int format_version = 1;
inline constexpr std::string_view flat_tree_root_domain = "zlink:aggregate-inventory-root:v1";
inline constexpr std::string_view indexed_tree_root_domain = "zlink:aggregate-inventory-root:v2";

inline constexpr std::size_t page_item_limit = protocol::maintenanceAggregateParticipants;
inline constexpr std::size_t page_byte_limit = protocol::maintenanceAggregateBytes;

using json_t = nlohmann::json;
using bytes_t = std::vector<std::byte>;
using digest_t = std::array<std::byte, 32>;

struct page_t
{
    std::vector<aggregate_participant_t> participants;
    bytes_t encoded;
    digest_t digest{};
};

struct index_page_t
{
    std::size_t level = 0;
    std::size_t page_index = 0;
    std::size_t child_start = 0;
    std::vector<digest_t> child_digests;
    bytes_t encoded;
    digest_t digest{};
};

struct tree_t
{
    std::vector<page_t> pages;
    std::vector<index_page_t> index_pages;
    std::size_t index_level_count = 0;
    digest_t root{};
    std::size_t participant_count = 0;
};

inline constexpr std::size_t index_item_limit = page_item_limit;

inline std::string hex (const bytes_t &bytes)
{
    return zlink::framework::detail::encode_hex (std::span<const std::byte> (bytes));
}

inline bytes_t unhex (std::string_view value)
{
    if ((value.size () & 1u) != 0)
        throw std::invalid_argument ("aggregate inventory hex value is malformed");
    bytes_t result;
    result.reserve (value.size () / 2);
    const auto digit = [] (char character) -> std::uint8_t {
        if (character >= '0' && character <= '9')
            return static_cast<std::uint8_t> (character - '0');
        if (character >= 'a' && character <= 'f')
            return static_cast<std::uint8_t> (character - 'a' + 10);
        if (character >= 'A' && character <= 'F')
            return static_cast<std::uint8_t> (character - 'A' + 10);
        throw std::invalid_argument ("aggregate inventory hex value is malformed");
    };
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (std::byte ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

inline bytes_t bytes_from_string (std::string_view value)
{
    bytes_t result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (std::byte (static_cast<unsigned char> (character)));
    return result;
}

inline json_t encode_participant (const aggregate_participant_t &participant)
{
    json_t result{
      {location_record_fields::key, participant.key.value},
      {location_record_fields::expectedStoreVersion, participant.expected_store_version},
      {location_record_fields::ownerTransition, static_cast<int> (participant.owner_transition)},
      {location_record_fields::authorityPayload, hex (participant.authority_payload)},
      {location_record_fields::membershipMutation, hex (participant.membership_mutation)}};
    return result;
}

inline aggregate_participant_t decode_participant (const json_t &value)
{
    aggregate_participant_t result{
      {value.at (location_record_fields::key).get<std::string> ()},
      value.at (location_record_fields::expectedStoreVersion).get<std::string> (),
      static_cast<authority_generation_transition_t> (
        value.at (location_record_fields::ownerTransition).get<int> ()),
      unhex (value.at (location_record_fields::authorityPayload).get<std::string> ()),
      unhex (value.at (location_record_fields::membershipMutation).get<std::string> ())};
    return result;
}

inline bytes_t encode_page (std::size_t index,
                            const std::vector<aggregate_participant_t> &participants)
{
    json_t entries = json_t::array ();
    for (const auto &participant : participants)
        entries.push_back (encode_participant (participant));
    const auto encoded = json_t{{location_record_fields::version, format_version},
                                {location_record_fields::pageIndex, index},
                                {location_record_fields::entries, std::move (entries)}}
                           .dump ();
    return bytes_from_string (encoded);
}

inline std::optional<std::vector<aggregate_participant_t>>
decode_page (const bytes_t &encoded, std::optional<std::size_t> expected_page_index = std::nullopt)
{
    if (encoded.empty () || encoded.size () > page_byte_limit)
        return std::nullopt;
    try {
        const auto text =
          std::string (reinterpret_cast<const char *> (encoded.data ()), encoded.size ());
        const auto value = json_t::parse (text);
        if (value.value (location_record_fields::version, 0) != format_version
            || !value.contains (location_record_fields::pageIndex)
            || (expected_page_index
                && value.at (location_record_fields::pageIndex).get<std::size_t> ()
                     != *expected_page_index)
            || !value.at (location_record_fields::entries).is_array ()
            || value.at (location_record_fields::entries).size () > page_item_limit)
            return std::nullopt;
        std::vector<aggregate_participant_t> participants;
        participants.reserve (value.at (location_record_fields::entries).size ());
        for (const auto &entry : value.at (location_record_fields::entries)) {
            const auto participant = decode_participant (entry);
            if (participant.key.value.empty () || participant.expected_store_version.empty ()
                || participant.owner_transition != authority_generation_transition_t::new_owner)
                return std::nullopt;
            participants.push_back (participant);
        }
        return participants;
    }
    catch (...) {
        return std::nullopt;
    }
}

inline bytes_t encode_index_page (std::size_t level,
                                  std::size_t page_index,
                                  std::size_t child_start,
                                  const std::vector<digest_t> &child_digests)
{
    json_t entries = json_t::array ();
    for (const auto &digest : child_digests)
        entries.push_back (hex (bytes_t (digest.begin (), digest.end ())));
    return bytes_from_string (json_t{
      {location_record_fields::version, format_version},
      {location_record_fields::level, level},
      {location_record_fields::pageIndex, page_index},
      {location_record_fields::childStart, child_start},
      {location_record_fields::entries,
       std::move (entries)}}.dump ());
}

inline std::optional<index_page_t>
decode_index_page (const bytes_t &encoded,
                   std::optional<std::size_t> expected_level = std::nullopt,
                   std::optional<std::size_t> expected_page_index = std::nullopt,
                   std::optional<std::size_t> expected_child_start = std::nullopt)
{
    if (encoded.empty () || encoded.size () > page_byte_limit)
        return std::nullopt;
    try {
        const auto text =
          std::string (reinterpret_cast<const char *> (encoded.data ()), encoded.size ());
        const auto value = json_t::parse (text);
        if (value.value (location_record_fields::version, 0) != format_version
            || !value.contains (location_record_fields::level)
            || !value.contains (location_record_fields::pageIndex)
            || !value.contains (location_record_fields::childStart)
            || !value.at (location_record_fields::entries).is_array ()
            || value.at (location_record_fields::entries).empty ()
            || value.at (location_record_fields::entries).size () > index_item_limit)
            return std::nullopt;
        const auto level = value.at (location_record_fields::level).get<std::size_t> ();
        const auto page_index = value.at (location_record_fields::pageIndex).get<std::size_t> ();
        const auto child_start = value.at (location_record_fields::childStart).get<std::size_t> ();
        if ((expected_level && level != *expected_level)
            || (expected_page_index && page_index != *expected_page_index)
            || (expected_child_start && child_start != *expected_child_start))
            return std::nullopt;
        index_page_t result;
        result.level = level;
        result.page_index = page_index;
        result.child_start = child_start;
        result.child_digests.reserve (value.at (location_record_fields::entries).size ());
        for (const auto &entry : value.at (location_record_fields::entries)) {
            const auto digest = unhex (entry.get<std::string> ());
            if (digest.size () != digest_t{}.size ())
                return std::nullopt;
            digest_t value_digest{};
            std::copy (digest.begin (), digest.end (), value_digest.begin ());
            result.child_digests.push_back (value_digest);
        }
        result.encoded = encoded;
        result.digest = sha256 (encoded);
        return result;
    }
    catch (...) {
        return std::nullopt;
    }
}

inline digest_t tree_root (const std::vector<page_t> &pages, std::size_t participant_count)
{
    bytes_t input = bytes_from_string (flat_tree_root_domain);
    const auto append_u64 = [&input] (std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            input.push_back (std::byte ((value >> shift) & 0xffu));
    };
    append_u64 (participant_count);
    append_u64 (pages.size ());
    for (const auto &page : pages)
        input.insert (input.end (), page.digest.begin (), page.digest.end ());
    return sha256 (input);
}

inline digest_t indexed_tree_root (const tree_t &tree)
{
    bytes_t input = bytes_from_string (indexed_tree_root_domain);
    const auto append_u64 = [&input] (std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            input.push_back (std::byte ((value >> shift) & 0xffu));
    };
    append_u64 (tree.participant_count);
    append_u64 (tree.pages.size ());
    append_u64 (tree.index_level_count);
    const auto top_level = tree.index_level_count - 1;
    for (const auto &page : tree.index_pages) {
        if (page.level == top_level)
            input.insert (input.end (), page.digest.begin (), page.digest.end ());
    }
    return sha256 (input);
}

inline std::optional<tree_t> build_tree (const std::vector<aggregate_participant_t> &participants)
{
    if (participants.empty ())
        return std::nullopt;
    tree_t tree;
    std::vector<aggregate_participant_t> current;
    current.reserve (page_item_limit);
    std::size_t current_bytes = encode_page (0, {}).size ();
    const auto finish_page = [&tree, &current, &current_bytes] (std::size_t index) {
        page_t page;
        page.participants = std::move (current);
        page.encoded = encode_page (index, page.participants);
        page.digest = sha256 (page.encoded);
        tree.pages.push_back (std::move (page));
        current.clear ();
        current.reserve (page_item_limit);
        current_bytes = encode_page (tree.pages.size (), {}).size ();
    };
    for (const auto &participant : participants) {
        if (current.size () == page_item_limit)
            finish_page (tree.pages.size ());
        const auto entry_bytes = encode_participant (participant).dump ().size ();
        auto next_bytes = current_bytes + entry_bytes + (current.empty () ? 0 : 1);
        if (next_bytes > page_byte_limit && !current.empty ()) {
            finish_page (tree.pages.size ());
            next_bytes = current_bytes + entry_bytes;
        }
        if (next_bytes > page_byte_limit)
            return std::nullopt;
        current.push_back (participant);
        current_bytes = next_bytes;
    }
    if (!current.empty ())
        finish_page (tree.pages.size ());
    tree.participant_count = participants.size ();
    tree.root = tree_root (tree.pages, tree.participant_count);
    if (tree.pages.size () > index_item_limit) {
        std::vector<digest_t> children;
        children.reserve (tree.pages.size ());
        for (const auto &page : tree.pages)
            children.push_back (page.digest);
        std::size_t level = 0;
        while (children.size () > 1) {
            std::vector<digest_t> next;
            next.reserve ((children.size () + index_item_limit - 1) / index_item_limit);
            std::size_t page_index = 0;
            for (std::size_t child_start = 0; child_start < children.size ();
                 child_start += index_item_limit, ++page_index) {
                const auto end = std::min (child_start + index_item_limit, children.size ());
                std::vector<digest_t> page_children (
                  children.begin () + static_cast<std::ptrdiff_t> (child_start),
                  children.begin () + static_cast<std::ptrdiff_t> (end));
                index_page_t page;
                page.level = level;
                page.page_index = page_index;
                page.child_start = child_start;
                page.child_digests = std::move (page_children);
                page.encoded = encode_index_page (page.level, page.page_index, page.child_start,
                                                  page.child_digests);
                if (page.encoded.size () > page_byte_limit)
                    return std::nullopt;
                page.digest = sha256 (page.encoded);
                next.push_back (page.digest);
                tree.index_pages.push_back (std::move (page));
            }
            children = std::move (next);
            ++level;
        }
        tree.index_level_count = level;
        tree.root = indexed_tree_root (tree);
    }
    return tree;
}

} // namespace zlink::framework::runtime::aggregate_inventory
