/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_repository.hpp"

#include <zlink/framework/detail/binary_text_codec.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::framework
{

struct authority_relocation_fence_t
{
    std::string node_rid_hex;
    std::uint64_t node_generation;
    std::string owner_id;
    std::uint64_t owner_lease_generation;

    bool operator== (const authority_relocation_fence_t &) const = default;
};

struct authority_relocation_record_t
{
    std::string relocation_id;
    std::string phase;
    authority_relocation_fence_t source;
    authority_relocation_fence_t coordinator;
    std::string expected_store_version;
};

struct authority_aggregate_t
{
    nlohmann::ordered_json encoded;
    std::optional<authority_relocation_record_t> relocation;
};

namespace runtime::authority_aggregate_codec
{
using json_t = nlohmann::ordered_json;

[[noreturn]] inline void invalid_record ()
{
    throw std::invalid_argument ("The Location Store authority aggregate relocation is invalid");
}

inline const std::string &text (const json_t &value, std::string_view field)
{
    const auto found = value.find (field);
    if (found == value.end () || !found->is_string ())
        invalid_record ();
    const auto &text = found->get_ref<const std::string &> ();
    if (text.empty ())
        invalid_record ();
    return text;
}

inline std::uint64_t number (const json_t &value, std::string_view field)
{
    const auto &encoded = text (value, field);
    std::uint64_t number = 0;
    const auto end = encoded.data () + encoded.size ();
    const auto result = std::from_chars (encoded.data (), end, number);
    if (result.ec != std::errc{} || result.ptr != end
        || (encoded.size () > 1 && encoded.front () == '0'))
        invalid_record ();
    return number;
}

inline bool lower_hex (std::string_view value)
{
    return std::all_of (value.begin (), value.end (),
                        [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

inline void validate_sha256 (const json_t &value, std::string_view field)
{
    const auto &digest = text (value, field);
    if (digest.size () != 64 || !lower_hex (digest))
        invalid_record ();
}

inline authority_relocation_fence_t read_fence (const json_t &value)
{
    const auto &rid = text (value, "nodeRid");
    const auto generation = number (value, "nodeGeneration");
    const auto lease = number (value, "ownerLeaseGeneration");
    if (rid.size () % 2 != 0 || rid.size () > 510 || !lower_hex (rid) || generation == 0
        || lease == 0)
        invalid_record ();
    return {rid, generation, text (value, "ownerId"), lease};
}

inline void validate_completion (const json_t &value)
{
    const auto accepted = number (value, "acceptedRequests");
    const auto completed = number (value, "completedResults");
    const auto pending = number (value, "pendingDeliveries");
    if (pending > completed || completed > accepted || !value.contains ("reference")
        || !value.contains ("sha256"))
        invalid_record ();
    if (completed == 0) {
        if (!value.at ("reference").is_null () || !value.at ("sha256").is_null ())
            invalid_record ();
    } else {
        (void) text (value, "reference");
        validate_sha256 (value, "sha256");
    }
}

inline std::shared_ptr<const authority_aggregate_t> read (json_t value)
{
    if (!value.is_object ())
        invalid_record ();
    static constexpr std::array<std::string_view, 8> fields{"aggregateId",
                                                            "aggregateGeneration",
                                                            "index",
                                                            "expectedStoreVersion",
                                                            "ownerTransition",
                                                            "targetAuthorityOwnerGeneration",
                                                            "authorityPayloadSha256",
                                                            "membershipMutationSha256"};
    const auto count = std::count_if (fields.begin (), fields.end (),
                                      [&] (auto field) { return value.contains (field); });
    const auto relocation = value.find ("relocation");
    if ((count != 0 && count != static_cast<std::ptrdiff_t> (fields.size ()))
        || (count == 0 && relocation == value.end ()))
        invalid_record ();
    if (relocation == value.end ())
        return std::make_shared<authority_aggregate_t> (
          authority_aggregate_t{std::move (value), {}});
    const auto &state = *relocation;
    const auto &id = text (state, "relocationId");
    const auto &phase = text (state, "phase");
    static constexpr std::array<std::string_view, 9> phases{"preparing", "captured",   "prepared",
                                                            "committed", "activating", "activated",
                                                            "cleaning",  "completed",  "aborted"};
    if (!detail::is_lowercase_uuid_text (id)
        || std::find (phases.begin (), phases.end (), phase) == phases.end ()
        || !state.contains ("source") || !state.contains ("coordinator")
        || !state.contains ("target"))
        invalid_record ();
    const auto &target = state.at ("target");
    if ((phase == "preparing" || phase == "captured") && !target.is_null ())
        invalid_record ();
    if (target.is_null ()) {
        if (phase != "preparing" && phase != "captured" && phase != "aborted")
            invalid_record ();
    } else {
        (void) read_fence (target);
        if (number (target, "attemptGeneration") == 0)
            invalid_record ();
    }
    validate_sha256 (state, "sourceInventorySha256");
    (void) number (state, "applicationVersion");
    if (const auto completion = state.find ("completion"); completion != state.end ())
        validate_completion (*completion);
    authority_relocation_record_t record{id, phase, read_fence (state.at ("source")),
                                         read_fence (state.at ("coordinator")),
                                         text (state.at ("coordinator"), "expectedStoreVersion")};
    return std::make_shared<authority_aggregate_t> (
      authority_aggregate_t{std::move (value), std::move (record)});
}
} // namespace runtime::authority_aggregate_codec
} // namespace zlink::framework

namespace zlink::framework::runtime::authority_aggregate_codec
{
inline std::string wire_uuid (std::uint64_t high, std::uint64_t low)
{
    std::array<std::uint8_t, detail::uuid_byte_length> bytes{};
    for (std::size_t index = 0; index != 8; ++index) {
        bytes[index] = static_cast<std::uint8_t> (high >> ((7 - index) * 8));
        bytes[index + 8] = static_cast<std::uint8_t> (low >> ((7 - index) * 8));
    }
    return detail::encode_uuid (bytes);
}

inline std::shared_ptr<const authority_aggregate_t>
replace (const std::shared_ptr<const authority_aggregate_t> &current, json_t relocation)
{
    auto value = current ? current->encoded : json_t::object ();
    if (relocation.is_null ())
        value.erase ("relocation");
    else
        value["relocation"] = std::move (relocation);
    return value.empty () ? nullptr : read (std::move (value));
}
inline std::shared_ptr<const authority_aggregate_t>
without_capture (const std::shared_ptr<const authority_aggregate_t> &current)
{
    return current && current->relocation && current->relocation->phase == "captured"
             ? replace (current, nullptr)
             : current;
}

} // namespace zlink::framework::runtime::authority_aggregate_codec

namespace zlink::framework::runtime
{
struct finishing_relocation_t
{
    std::string id;
    std::uint64_t attempt_generation;
};

inline std::optional<finishing_relocation_t>
finishing_relocation (const authority_snapshot_t &snapshot)
{
    using namespace authority_aggregate_codec;
    if (snapshot.aggregate && snapshot.aggregate->relocation) {
        const auto &record = *snapshot.aggregate->relocation;
        if (record.phase != "committed" && record.phase != "activating"
            && record.phase != "activated" && record.phase != "cleaning"
            && record.phase != "completed")
            return std::nullopt;
        const auto &target = snapshot.aggregate->encoded.at ("relocation").at ("target");
        const auto fence = read_fence (target);
        const auto rid = snapshot.allocation.target.node_rid.value ();
        if (fence.owner_id != snapshot.owner.owner_id || snapshot.owner.lease_generation <= 0
            || fence.owner_lease_generation
                 != static_cast<std::uint64_t> (snapshot.owner.lease_generation)
            || fence.node_rid_hex != detail::encode_hex (std::as_bytes (std::span (rid)))
            || fence.node_generation != snapshot.allocation.target.node_lifecycle_generation)
            return std::nullopt;
        return finishing_relocation_t{record.relocation_id, number (target, "attemptGeneration")};
    }
    if (const auto legacy = decode_finishing_relocation (snapshot.payload))
        return finishing_relocation_t{wire_uuid (legacy->high, legacy->low),
                                      legacy->target_attempt_generation};
    return std::nullopt;
}
} // namespace zlink::framework::runtime
