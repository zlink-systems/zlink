/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime::protocol
{

inline constexpr std::string_view actor_join_recovery_packet_name =
  "__zlink.actor.routed_join.recovery";
inline constexpr std::string_view actor_join_recovery_content_type =
  "application/x-zlink-actor-routed-join-recovery-v1";
inline constexpr std::string_view actor_join_recreate_content_type =
  "application/vnd.zlink.actor-relocation.recreate";
inline constexpr std::string_view actor_join_snapshot_content_type =
  "application/vnd.zlink.actor-relocation.snapshot";

inline constexpr std::uint64_t actor_join_framework_metadata_reservation_bytes =
  64u * 1024u;
inline constexpr std::uint64_t actor_join_accepted_journal_reservation_bytes =
  16u * 1024u * 1024u;
inline constexpr std::uint64_t actor_join_snapshot_state_reservation_bytes =
  64u * 1024u * 1024u;

struct actor_join_recovery_t
{
    std::string actor_id;
    std::string actor_type;
    std::string handoff_id;
    std::string source_spot_id;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t actor_generation = 0;
    std::uint64_t actor_authority_owner_generation = 0;
    std::uint64_t actor_node_generation = 0;
    std::uint64_t expected_owner_lease_generation = 0;
    relocation_id_t relocation;
    std::string relocation_content_type;
    std::string request_content_type;
    std::vector<std::uint8_t> request;
    std::string reservation_token;
    std::uint64_t reserved_payload_bytes = 0;
    std::string target_spot_id;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t target_spot_generation = 0;
    std::uint64_t target_authority_owner_generation = 0;
    std::uint64_t target_spot_authority_owner_generation = 0;
    relocation_coordinator_fence_t coordinator;
    wire_operation_id_t operation;
    std::string reply_content_type;
    std::vector<std::uint8_t> reply;
};

namespace actor_join_recovery_detail
{

inline std::string hex (std::span<const std::uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve (bytes.size () * 2);
    for (const auto byte : bytes) {
        result.push_back (digits[byte >> 4u]);
        result.push_back (digits[byte & 0x0fu]);
    }
    return result;
}

inline std::string base64 (std::span<const std::uint8_t> bytes)
{
    static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve (((bytes.size () + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size (); offset += 3) {
        const auto remaining = bytes.size () - offset;
        const auto value = (static_cast<std::uint32_t> (bytes[offset]) << 16u)
                           | (remaining > 1
                                ? static_cast<std::uint32_t> (bytes[offset + 1]) << 8u
                                : 0u)
                           | (remaining > 2 ? bytes[offset + 2] : 0u);
        result.push_back (alphabet[(value >> 18u) & 0x3fu]);
        result.push_back (alphabet[(value >> 12u) & 0x3fu]);
        result.push_back (remaining > 1 ? alphabet[(value >> 6u) & 0x3fu] : '=');
        result.push_back (remaining > 2 ? alphabet[value & 0x3fu] : '=');
    }
    return result;
}

inline std::vector<std::uint8_t> decode_base64 (std::string_view text)
{
    if (text.size () % 4 != 0)
        throw service_wire_error_t ("Actor Join recovery base64 is invalid");
    const auto value_of = [] (char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> result;
    result.reserve ((text.size () / 4) * 3);
    for (std::size_t offset = 0; offset < text.size (); offset += 4) {
        const auto a = value_of (text[offset]);
        const auto b = value_of (text[offset + 1]);
        const auto c = text[offset + 2] == '=' ? -2 : value_of (text[offset + 2]);
        const auto d = text[offset + 3] == '=' ? -2 : value_of (text[offset + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1
            || (c == -2 && d != -2)
            || (offset + 4 != text.size () && (c == -2 || d == -2)))
            throw service_wire_error_t ("Actor Join recovery base64 is invalid");
        const auto packed = (static_cast<std::uint32_t> (a) << 18u)
                            | (static_cast<std::uint32_t> (b) << 12u)
                            | (c >= 0 ? static_cast<std::uint32_t> (c) << 6u : 0u)
                            | (d >= 0 ? static_cast<std::uint32_t> (d) : 0u);
        result.push_back (static_cast<std::uint8_t> (packed >> 16u));
        if (c >= 0) result.push_back (static_cast<std::uint8_t> (packed >> 8u));
        if (d >= 0) result.push_back (static_cast<std::uint8_t> (packed));
    }
    if (base64 (result) != text)
        throw service_wire_error_t ("Actor Join recovery base64 is not canonical");
    return result;
}

inline std::uint64_t json_u64 (const nlohmann::json &value,
                               std::string_view name)
{
    try {
        if (value.is_number_unsigned ())
            return value.get<std::uint64_t> ();
        if (value.is_number_integer ()) {
            const auto parsed = value.get<std::int64_t> ();
            if (parsed >= 0) return static_cast<std::uint64_t> (parsed);
        }
        if (value.is_string ()) {
            const auto text = value.get<std::string> ();
            if (!text.empty ()
                && std::all_of (text.begin (), text.end (), [] (unsigned char ch) {
                       return std::isdigit (ch) != 0;
                   })) {
                std::size_t consumed = 0;
                const auto parsed = std::stoull (text, &consumed);
                if (consumed == text.size ()) return parsed;
            }
        }
    }
    catch (...) {
    }
    throw service_wire_error_t (std::string (name) + " is not a u64");
}

inline std::string required_text (const nlohmann::json &value,
                                  std::string_view name)
{
    if (!value.is_string () || value.get_ref<const std::string &> ().empty ())
        throw service_wire_error_t (std::string (name) + " is empty");
    return value.get<std::string> ();
}

inline std::string relocation_text (const relocation_id_t &value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0; index != 8; ++index) {
        bytes[index] = static_cast<std::uint8_t> (
          value.high >> (56u - static_cast<unsigned> (index) * 8u));
        bytes[index + 8] = static_cast<std::uint8_t> (
          value.low >> (56u - static_cast<unsigned> (index) * 8u));
    }
    std::string result;
    result.reserve (36);
    for (std::size_t index = 0; index != bytes.size (); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result.push_back ('-');
        result.push_back (digits[bytes[index] >> 4u]);
        result.push_back (digits[bytes[index] & 0x0fu]);
    }
    return result;
}

inline std::string compact_uuid (std::string_view value)
{
    std::string result;
    result.reserve (32);
    for (const auto ch : value) {
        if (ch == '-') continue;
        if (!std::isxdigit (static_cast<unsigned char> (ch)))
            throw service_wire_error_t ("Actor Join recovery UUID is invalid");
        result.push_back (static_cast<char> (
          std::tolower (static_cast<unsigned char> (ch))));
    }
    if (result.size () != 32
        || std::all_of (result.begin (), result.end (), [] (char ch) { return ch == '0'; }))
        throw service_wire_error_t ("Actor Join recovery UUID is invalid");
    return result;
}

} // namespace actor_join_recovery_detail

inline relocation_id_t actor_join_relocation_id (std::string_view handoff_id)
{
    const auto compact = actor_join_recovery_detail::compact_uuid (handoff_id);
    const auto parse = [&compact] (std::size_t begin) {
        std::uint64_t value = 0;
        for (std::size_t index = begin; index != begin + 16; ++index) {
            const auto ch = compact[index];
            value = (value << 4u)
                    | static_cast<std::uint64_t> (
                        ch >= '0' && ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
        return value;
    };
    return {parse (0), parse (16)};
}

inline std::string actor_join_handoff_id (const relocation_id_t &relocation)
{
    return actor_join_recovery_detail::compact_uuid (
      actor_join_recovery_detail::relocation_text (relocation));
}

inline std::uint64_t actor_join_reserved_payload_bytes (
  std::size_t request_bytes, std::string_view relocation_content_type)
{
    const auto snapshot = relocation_content_type == actor_join_snapshot_content_type;
    if (!snapshot && relocation_content_type != actor_join_recreate_content_type)
        throw service_wire_error_t ("Actor Join relocation content type is invalid");
    const auto base = actor_join_framework_metadata_reservation_bytes
                      + actor_join_accepted_journal_reservation_bytes;
    if (request_bytes > std::numeric_limits<std::uint64_t>::max () - base
        || (snapshot
            && request_bytes + base
                 > std::numeric_limits<std::uint64_t>::max ()
                     - actor_join_snapshot_state_reservation_bytes))
        throw service_wire_error_t ("Actor Join reservation size overflow");
    return base + request_bytes
           + (snapshot ? actor_join_snapshot_state_reservation_bytes : 0);
}

inline frozen_record_t encode_actor_join_recovery_saved_work (
  const actor_join_recovery_t &value)
{
    using namespace actor_join_recovery_detail;
    if (value.actor_id.empty () || value.actor_type.empty ()
        || value.handoff_id.empty () || value.source_spot_id.empty ()
        || value.source_node_routing_id.empty () || value.actor_generation == 0
        || value.actor_authority_owner_generation == 0
        || value.actor_node_generation == 0
        || value.expected_owner_lease_generation == 0
        || value.request_content_type.empty () || value.reservation_token.empty ()
        || value.reserved_payload_bytes == 0 || value.target_spot_id.empty ()
        || value.target_node_routing_id.empty () || value.target_node_generation == 0
        || value.target_spot_generation == 0
        || value.target_authority_owner_generation == 0
        || value.target_spot_authority_owner_generation == 0
        || value.coordinator.owner_id.empty ()
        || value.coordinator.lease_generation == 0
        || value.coordinator.node_routing_id.empty ()
        || value.coordinator.node_generation == 0
        || value.coordinator.expected_authority_store_version.empty ()
        || (value.operation.high == 0 && value.operation.low == 0)
        || value.reply_content_type.empty ())
        throw service_wire_error_t ("Actor Join recovery identity is invalid");
    if (value.request.size () > 1024u * 1024u
        || value.reply.size () > 1024u * 1024u)
        throw service_wire_error_t ("Actor Join recovery message exceeds 1 MiB");

    /* ZLJR is a byte-stable cross-language record.  The Node encoder emits
     * object properties in declaration order, as does System.Text.Json for
     * the .NET record.  Do not use nlohmann::json here: its default object
     * type sorts keys and changes the persisted bytes. */
    nlohmann::ordered_json request = {
      {"ActorId", value.actor_id},
      {"ActorType", value.actor_type},
      {"HandoffId", value.handoff_id},
      {"BoundSessionNodeRid", nullptr},
      {"BoundSessionRid", nullptr},
      {"RelocationContentType", value.relocation_content_type},
      {"RelocationReference", "pending"},
      {"RelocationChecksumCrc32c", 0},
      {"RelocationAggregateId", relocation_text (value.relocation)},
      {"RelocationAggregateGeneration", 1},
      {"RelocationInventoryDigest", base64 (std::array<std::uint8_t, 32>{})},
      {"RequestContentType", value.request_content_type},
      {"Request", ""},
      {"HandoffFrames", nlohmann::json::array ()},
      {"SourceSpotId", value.source_spot_id},
      {"SourceNodeRid", base64 (value.source_node_routing_id)},
      {"ActorGeneration", value.actor_generation},
      {"ActorAuthorityOwnerGeneration", value.actor_authority_owner_generation},
      {"BoundSessionBindingToken", nullptr},
      {"BoundSessionBindingGeneration", 0},
      {"BoundSessionObjectGeneration", 0},
      {"BoundSessionAuthorityOwnerGeneration", 0},
      {"BoundSessionMeshName", nullptr},
      {"BoundSessionTargetNodeGeneration", 0},
      {"BoundSessionOwnerLeaseGeneration", 0},
      {"BoundSessionOwnerNodeGeneration", 0},
      {"BoundSessionAcceptedHighWater", 0},
      {"BoundSessionSessionOwnerId", nullptr},
      {"BoundSessionSessionOwnerLeaseGeneration", 0},
      {"ReservationToken", value.reservation_token},
      {"ReservedPayloadBytes", value.reserved_payload_bytes},
      {"TargetNodeRid", base64 (value.target_node_routing_id)},
      {"TargetNodeGeneration", value.target_node_generation},
      {"TargetSpotGeneration", value.target_spot_generation},
      {"TargetAuthorityOwnerGeneration", value.target_authority_owner_generation},
      {"TargetSpotAuthorityOwnerGeneration",
       value.target_spot_authority_owner_generation},
      {"RelocationCoordinatorOwnerId", value.coordinator.owner_id},
      {"RelocationCoordinatorLeaseGeneration", value.coordinator.lease_generation},
      {"RelocationCoordinatorNodeRid", base64 (value.coordinator.node_routing_id)},
      {"RelocationCoordinatorNodeGeneration", value.coordinator.node_generation},
      {"RelocationCoordinatorExpectedAuthorityStoreVersion",
       value.coordinator.expected_authority_store_version},
      {"ActorNodeGeneration", value.actor_node_generation},
      {"ExpectedOwnerLeaseGeneration", value.expected_owner_lease_generation}};
    nlohmann::ordered_json metadata = {
      {"Request", std::move (request)},
      {"TargetSpotId", value.target_spot_id},
      {"TargetNodeRid", base64 (value.target_node_routing_id)},
      {"TargetNodeGeneration", value.target_node_generation},
      {"TargetSpotGeneration", value.target_spot_generation},
      {"TargetAuthorityOwnerGeneration", value.target_authority_owner_generation},
      {"OperationIdHigh", value.operation.high},
      {"OperationIdLow", value.operation.low},
      {"ReplyContentType", value.reply_content_type},
      {"Reply", ""}};
    const auto metadata_text = metadata.dump ();
    if (metadata_text.size () > 256u * 1024u)
        throw service_wire_error_t ("Actor Join recovery metadata exceeds 256 KiB");

    try {
        const auto source_routing_id = hex (value.source_node_routing_id);
        const auto frozen = encode_zljr_record_v1 ({
          {{source_routing_id.begin (), source_routing_id.end ()},
           value.actor_node_generation,
           value.coordinator.owner_id,
           value.coordinator.lease_generation},
          {0, 0},
          {metadata_text.begin (), metadata_text.end ()},
          value.request,
          value.reply});
        return decode_frozen_record (frozen);
    }
    catch (const std::invalid_argument &error) {
        throw service_wire_error_t (
          std::string ("generated ZLJR codec rejected record: ")
          + error.what ());
    }
}

inline std::optional<actor_join_recovery_t>
decode_actor_join_recovery_saved_work (const frozen_record_t &record)
{
    using namespace actor_join_recovery_detail;
    std::optional<frozen_record_t> decoded_record;
    const frozen_record_t *candidate = &record;
    if (!candidate->application && !candidate->canonical_bytes.empty ()) {
        try {
            decoded_record = decode_frozen_record (
              candidate->canonical_bytes);
            candidate = &*decoded_record;
        }
        catch (const service_wire_error_t &) {
            return std::nullopt;
        }
    }
    if (candidate->kind != frozen_record_kind_t::node_send
        || candidate->source_kind != frozen_source_kind_t::node
        || !candidate->application
        || candidate->application->packet_name
             != actor_join_recovery_packet_name
        || candidate->application->content_type
             != actor_join_recovery_content_type)
        return std::nullopt;
    service_wire_pilot_zljr_record_v1 generated;
    try {
        generated = decode_zljr_record_v1 (
          candidate->canonical_bytes.empty ()
            ? encode_frozen_record (*candidate)
            : candidate->canonical_bytes);
    }
    catch (const std::invalid_argument &error) {
        throw service_wire_error_t (
          std::string ("generated ZLJR codec rejected record: ")
          + error.what ());
    }
    if (generated.operation.high != 0 || generated.operation.low != 0)
        throw service_wire_error_t (
          "Actor Join recovery frozen operation is invalid");
    nlohmann::json metadata;
    try {
        metadata = nlohmann::json::parse (
          generated.metadata.begin (), generated.metadata.end ());
    }
    catch (...) {
        throw service_wire_error_t ("Actor Join recovery metadata is malformed");
    }
    actor_join_recovery_t result;
    const auto &request = metadata.at ("Request");
    result.actor_id = required_text (request.at ("ActorId"), "ActorId");
    result.actor_type = required_text (request.at ("ActorType"), "ActorType");
    result.handoff_id = required_text (request.at ("HandoffId"), "HandoffId");
    result.source_spot_id = required_text (request.at ("SourceSpotId"), "SourceSpotId");
    result.source_node_routing_id = decode_base64 (
      required_text (request.at ("SourceNodeRid"), "SourceNodeRid"));
    result.actor_generation = json_u64 (request.at ("ActorGeneration"), "ActorGeneration");
    result.actor_authority_owner_generation = json_u64 (
      request.at ("ActorAuthorityOwnerGeneration"),
      "ActorAuthorityOwnerGeneration");
    result.actor_node_generation = json_u64 (
      request.at ("ActorNodeGeneration"), "ActorNodeGeneration");
    result.expected_owner_lease_generation = json_u64 (
      request.at ("ExpectedOwnerLeaseGeneration"),
      "ExpectedOwnerLeaseGeneration");
    result.relocation = actor_join_relocation_id (
      required_text (request.at ("RelocationAggregateId"),
                     "RelocationAggregateId"));
    result.relocation_content_type = required_text (
      request.at ("RelocationContentType"), "RelocationContentType");
    result.request_content_type = required_text (
      request.at ("RequestContentType"), "RequestContentType");
    result.reservation_token = required_text (
      request.at ("ReservationToken"), "ReservationToken");
    result.reserved_payload_bytes = json_u64 (
      request.at ("ReservedPayloadBytes"), "ReservedPayloadBytes");
    result.target_spot_id = required_text (
      metadata.at ("TargetSpotId"), "TargetSpotId");
    result.target_node_routing_id = decode_base64 (
      required_text (metadata.at ("TargetNodeRid"), "TargetNodeRid"));
    result.target_node_generation = json_u64 (
      metadata.at ("TargetNodeGeneration"), "TargetNodeGeneration");
    result.target_spot_generation = json_u64 (
      metadata.at ("TargetSpotGeneration"), "TargetSpotGeneration");
    result.target_authority_owner_generation = json_u64 (
      metadata.at ("TargetAuthorityOwnerGeneration"),
      "TargetAuthorityOwnerGeneration");
    result.target_spot_authority_owner_generation = json_u64 (
      request.at ("TargetSpotAuthorityOwnerGeneration"),
      "TargetSpotAuthorityOwnerGeneration");
    result.coordinator.owner_id = required_text (
      request.at ("RelocationCoordinatorOwnerId"),
      "RelocationCoordinatorOwnerId");
    result.coordinator.lease_generation = json_u64 (
      request.at ("RelocationCoordinatorLeaseGeneration"),
      "RelocationCoordinatorLeaseGeneration");
    result.coordinator.node_routing_id = decode_base64 (required_text (
      request.at ("RelocationCoordinatorNodeRid"),
      "RelocationCoordinatorNodeRid"));
    result.coordinator.node_generation = json_u64 (
      request.at ("RelocationCoordinatorNodeGeneration"),
      "RelocationCoordinatorNodeGeneration");
    result.coordinator.expected_authority_store_version = required_text (
      request.at ("RelocationCoordinatorExpectedAuthorityStoreVersion"),
      "RelocationCoordinatorExpectedAuthorityStoreVersion");
    result.operation.high = json_u64 (
      metadata.at ("OperationIdHigh"), "OperationIdHigh");
    result.operation.low = json_u64 (
      metadata.at ("OperationIdLow"), "OperationIdLow");
    result.reply_content_type = required_text (
      metadata.at ("ReplyContentType"), "ReplyContentType");
    result.request = generated.request;
    result.reply = generated.reply;

    const auto request_target = decode_base64 (required_text (
      request.at ("TargetNodeRid"), "Request.TargetNodeRid"));
    const auto expected_source_routing_id = hex (result.source_node_routing_id);
    const std::vector<std::uint8_t> expected_source_storage (
      expected_source_routing_id.begin (), expected_source_routing_id.end ());
    if (result.actor_generation == 0
        || result.actor_authority_owner_generation == 0
        || result.actor_node_generation == 0
        || result.expected_owner_lease_generation == 0
        || result.target_node_generation == 0
        || result.target_spot_generation == 0
        || result.target_authority_owner_generation == 0
        || result.target_spot_authority_owner_generation == 0
        || result.reserved_payload_bytes == 0
        || (result.operation.high == 0 && result.operation.low == 0)
        || request_target != result.target_node_routing_id
        || generated.source.node_rid != expected_source_storage
        || generated.source.node_generation != result.actor_node_generation
        || generated.source.owner_id != result.coordinator.owner_id
        || generated.source.owner_lease_generation
             != result.coordinator.lease_generation)
        throw service_wire_error_t ("Actor Join recovery identity is invalid");
    return result;
}

} // namespace zlink::framework::runtime::protocol
