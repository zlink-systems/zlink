/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/json_profile.hpp>

#include "runtime/spots/spot_route_packets.hpp"

#include "runtime/streams/stream_runtime.hpp"

#include <nlohmann/json.hpp>
#include <service_wire_constants.hpp>
#include <zlink/framework/detail/binary_text_codec.hpp>
#include <zlink/framework/detail/base64.hpp>

#include <stdexcept>
#include <string_view>
#include <typeindex>
#include <utility>

namespace zlink::framework::detail
{

namespace
{

namespace packet_field
{
inline constexpr char accepted[] = "accepted";
inline constexpr char actorAuthorityOwnerGeneration[] = "actorAuthorityOwnerGeneration";
inline constexpr char actorGeneration[] = "actorGeneration";
inline constexpr char actorId[] = "actorId";
inline constexpr char actorNodeGeneration[] = "actorNodeGeneration";
inline constexpr char actorNodeRid[] = "actorNodeRid";
inline constexpr char actorOwnerLeaseGeneration[] = "actorOwnerLeaseGeneration";
inline constexpr char actorRefPresent[] = "actorRefPresent";
inline constexpr char actorType[] = "actorType";
inline constexpr char boundSessionNodeRid[] = "boundSessionNodeRid";
inline constexpr char boundSessionRid[] = "boundSessionRid";
inline constexpr char codec[] = "codec";
inline constexpr char completionOperationIdHigh[] = "completionOperationIdHigh";
inline constexpr char completionOperationIdLow[] = "completionOperationIdLow";
inline constexpr char completionRootChecksum[] = "completionRootChecksum";
inline constexpr char completionRootReference[] = "completionRootReference";
inline constexpr char contentType[] = "contentType";
inline constexpr char coreFinalSequence[] = "coreFinalSequence";
inline constexpr char coreMembershipEpoch[] = "coreMembershipEpoch";
inline constexpr char coreTransfer[] = "coreTransfer";
inline constexpr char coreTransferIdHigh[] = "coreTransferIdHigh";
inline constexpr char coreTransferIdLow[] = "coreTransferIdLow";
inline constexpr char expectedActorStoreVersion[] = "expectedActorStoreVersion";
inline constexpr char expectedOwnerLeaseGeneration[] = "expectedOwnerLeaseGeneration";
inline constexpr char finalize[] = "finalize";
inline constexpr char finalizeTimeoutMs[] = "finalizeTimeoutMs";
inline constexpr char frame[] = "frame";
inline constexpr char handoffBacklog[] = "handoffBacklog";
inline constexpr char hasReply[] = "hasReply";
inline constexpr char isRequest[] = "isRequest";
inline constexpr char messageFollowHopCount[] = "messageFollowHopCount";
inline constexpr char metadata[] = "metadata";
inline constexpr char packetName[] = "packetName";
inline constexpr char payload[] = "payload";
inline constexpr char prepare[] = "prepare";
inline constexpr char receiveChunkLimitBytes[] = "receiveChunkLimitBytes";
inline constexpr char resultCode[] = "resultCode";
inline constexpr char sessionNodeRid[] = "sessionNodeRid";
inline constexpr char sessionRelocationRoute[] = "sessionRelocationRoute";
inline constexpr char sourceMeshName[] = "sourceMeshName";
inline constexpr char sourceSpotGeneration[] = "sourceSpotGeneration";
inline constexpr char sourceSpotId[] = "sourceSpotId";
inline constexpr char spotId[] = "spotId";
inline constexpr char targetAuthorityOwnerGeneration[] = "targetAuthorityOwnerGeneration";
inline constexpr char targetMeshName[] = "targetMeshName";
inline constexpr char targetNodeGeneration[] = "targetNodeGeneration";
inline constexpr char targetNodeLifecycleGeneration[] = "targetNodeLifecycleGeneration";
inline constexpr char targetNodeRid[] = "targetNodeRid";
inline constexpr char targetOwnerId[] = "targetOwnerId";
inline constexpr char targetOwnerLeaseGeneration[] = "targetOwnerLeaseGeneration";
inline constexpr char targetSpotGeneration[] = "targetSpotGeneration";
inline constexpr char targetSpotId[] = "targetSpotId";
inline constexpr char topic[] = "topic";
inline constexpr char transferId[] = "transferId";
} // namespace packet_field

std::uint8_t decode_base64_symbol (char symbol)
{
    const auto position = zlink::framework::detail::base64_alphabet.find (symbol);
    if (position == std::string_view::npos)
        throw std::invalid_argument ("Byte sequence is not valid RFC 4648 base64");
    return static_cast<std::uint8_t> (position);
}

std::vector<std::uint8_t> decode_base64 (std::string_view encoded)
{
    if (encoded.size () % 4 != 0)
        throw std::invalid_argument ("Byte sequence is not valid RFC 4648 base64");

    std::vector<std::uint8_t> bytes;
    bytes.reserve ((encoded.size () / 4) * 3);
    for (std::size_t offset = 0; offset < encoded.size (); offset += 4) {
        const bool final_group = offset + 4 == encoded.size ();
        const bool pad_two = encoded[offset + 2] == '=';
        const bool pad_one = encoded[offset + 3] == '=';
        if (encoded[offset] == '=' || encoded[offset + 1] == '='
            || (pad_two && (!pad_one || !final_group)) || (pad_one && !final_group)) {
            throw std::invalid_argument ("Byte sequence is not valid RFC 4648 base64");
        }

        const auto first = decode_base64_symbol (encoded[offset]);
        const auto second = decode_base64_symbol (encoded[offset + 1]);
        const auto third = pad_two ? std::uint8_t{0} : decode_base64_symbol (encoded[offset + 2]);
        const auto fourth = pad_one ? std::uint8_t{0} : decode_base64_symbol (encoded[offset + 3]);
        if ((pad_two && (second & 0x0fU) != 0) || (pad_one && !pad_two && (third & 0x03U) != 0)) {
            throw std::invalid_argument ("Byte sequence is not canonical RFC 4648 base64");
        }

        bytes.push_back (static_cast<std::uint8_t> ((first << 2) | (second >> 4)));
        if (!pad_two)
            bytes.push_back (static_cast<std::uint8_t> ((second << 4) | (third >> 2)));
        if (!pad_one)
            bytes.push_back (static_cast<std::uint8_t> ((third << 6) | fourth));
    }
    return bytes;
}

std::vector<std::uint8_t> decode_base64_field (const nlohmann::json &json, const char *field)
{
    const auto &encoded = json.at (field);
    if (!encoded.is_string ())
        throw std::invalid_argument (std::string (field) + " must be a base64 string");
    return decode_base64 (encoded.get_ref<const std::string &> ());
}

void validate_handoff_backlog_json (const nlohmann::json &backlog)
{
    if (!backlog.is_array ()) {
        throw std::invalid_argument ("Actor handoff backlog must be an array");
    }

    for (const auto &item : backlog) {
        if (!item.is_object ()) {
            throw std::invalid_argument ("Actor handoff backlog item must be an object");
        }
        const auto packet_name = item.find (packet_field::packetName);
        if (packet_name == item.end () || !packet_name->is_string ()) {
            throw std::invalid_argument ("Actor handoff backlog packet name is required");
        }
        const auto content_type = item.find (packet_field::contentType);
        if (content_type != item.end ()) {
            if (!content_type->is_string ()) {
                throw std::invalid_argument ("Actor handoff backlog content type must be text");
            }
        }

        const auto payload = item.find (packet_field::payload);
        if (payload == item.end () || !payload->is_string ()) {
            throw std::invalid_argument ("Actor handoff backlog payload is required");
        }
        (void) decode_base64 (payload->get_ref<const std::string &> ());

        const auto metadata = item.find (packet_field::metadata);
        if (metadata == item.end ())
            continue;
        if (!metadata->is_object () || metadata->size () > runtime::protocol::metadataBytes) {
            throw std::invalid_argument ("Actor handoff backlog metadata is invalid");
        }
        for (const auto &[key, value] : metadata->items ()) {
            if (!value.is_string ()) {
                throw std::invalid_argument ("Actor handoff backlog metadata value must be text");
            }
            (void) key;
        }
    }
}

} // namespace

void to_json (nlohmann::json &json, const spot_multicast_route_send_t &value)
{
    json =
      nlohmann::json{{packet_field::topic, value.topic},
                     {packet_field::frame, zlink::framework::detail::base64_encode (std::as_bytes (
                                             std::span<const std::uint8_t> (value.frame)))}};
}

void from_json (const nlohmann::json &json, spot_multicast_route_send_t &value)
{
    value.topic = json.at (packet_field::topic).get<std::string> ();
    value.frame = decode_base64_field (json, packet_field::frame);
}

result_t<zlink::message_t> encode_actor_bound_session_frame (stream_codec_t codec,
                                                             std::string packet_name,
                                                             const zlink::message_t &payload)
{
    stream_runtime_t stream_runtime (std::make_shared<stream_runtime_state_t> ());
    const stream_header_t header (stream_message_kind_t::send, codec, stream_header_flags_t::none,
                                  std::nullopt, std::move (packet_name));
    auto encoded_frame = stream_runtime.encode_frame (header, payload);
    if (!encoded_frame) {
        return result_t<zlink::message_t>::failure (
          encoded_frame.error_kind (),
          encoded_frame.error () ? encoded_frame.error ()->what () : "STREAM frame encode failed");
    }
    return result_t<zlink::message_t>::success (
      zlink::message_t::from (std::move (encoded_frame.value ())));
}

void to_json (nlohmann::json &json, const spot_actor_admission_route_request_t &value)
{
    json = nlohmann::json{
      {packet_field::transferId, value.transfer_id},
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::actorNodeGeneration, value.actor_node_generation},
      {packet_field::actorAuthorityOwnerGeneration, value.actor_authority_owner_generation},
      {packet_field::expectedOwnerLeaseGeneration, value.expected_owner_lease_generation},
      {packet_field::completionOperationIdHigh, value.completion_operation_id_high},
      {packet_field::completionOperationIdLow, value.completion_operation_id_low},
      {packet_field::sourceSpotId, value.source_spot_id},
      {packet_field::targetSpotId, value.target_spot_id},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))}};
}

void from_json (const nlohmann::json &json, spot_actor_admission_route_request_t &value)
{
    value.transfer_id = json.at (packet_field::transferId).get<std::string> ();
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.actor_node_generation = json.value (packet_field::actorNodeGeneration, std::uint64_t{0});
    value.actor_authority_owner_generation =
      json.value (packet_field::actorAuthorityOwnerGeneration, std::uint64_t{0});
    value.expected_owner_lease_generation =
      json.value (packet_field::expectedOwnerLeaseGeneration, std::uint64_t{0});
    value.completion_operation_id_high =
      json.value (packet_field::completionOperationIdHigh, std::uint64_t{0});
    value.completion_operation_id_low =
      json.value (packet_field::completionOperationIdLow, std::uint64_t{0});
    value.source_spot_id = json.at (packet_field::sourceSpotId).get<std::string> ();
    value.target_spot_id = json.at (packet_field::targetSpotId).get<std::string> ();
    value.payload = decode_base64_field (json, packet_field::payload);
}

void to_json (nlohmann::json &json, const spot_actor_admission_route_reply_t &value)
{
    json = nlohmann::json{
      {packet_field::accepted, value.accepted},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))},
      {packet_field::completionRootReference, value.completion_root_reference},
      {packet_field::completionRootChecksum, value.completion_root_checksum}};
}

void from_json (const nlohmann::json &json, spot_actor_admission_route_reply_t &value)
{
    value.accepted = json.at (packet_field::accepted).get<bool> ();
    value.payload = decode_base64_field (json, packet_field::payload);
    value.completion_root_reference = json.value (packet_field::completionRootReference, "");
    value.completion_root_checksum =
      json.value (packet_field::completionRootChecksum, std::uint32_t{0});
}

void to_json (nlohmann::json &json, const spot_actor_handoff_packet_t &value)
{
    json = nlohmann::json{
      {packet_field::packetName, value.packet_name_value},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))},
      {packet_field::contentType, value.content_type},
      {packet_field::metadata, value.metadata},
      {packet_field::isRequest, value.is_request}};
}

void from_json (const nlohmann::json &json, spot_actor_handoff_packet_t &value)
{
    value.packet_name_value = json.at (packet_field::packetName).get<std::string> ();
    value.payload = decode_base64_field (json, packet_field::payload);
    value.content_type = json.value (packet_field::contentType, "");
    value.metadata = json.value (packet_field::metadata, std::map<std::string, std::string>{});
    value.is_request = json.value (packet_field::isRequest, false);
}

void to_json (nlohmann::json &json, const spot_actor_commit_route_request_t &value)
{
    json = nlohmann::json{
      {packet_field::transferId, value.transfer_id},
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::actorAuthorityOwnerGeneration, value.actor_authority_owner_generation},
      {packet_field::expectedActorStoreVersion, value.expected_actor_store_version},
      {packet_field::completionRootReference, value.completion_root_reference},
      {packet_field::completionRootChecksum, value.completion_root_checksum},
      {packet_field::targetSpotId, value.target_spot_id},
      {packet_field::targetSpotGeneration, value.target_spot_generation},
      {packet_field::sourceMeshName, value.source_mesh_name},
      {packet_field::targetMeshName, value.target_mesh_name},
      {packet_field::targetNodeLifecycleGeneration, value.target_node_lifecycle_generation},
      {packet_field::targetOwnerId, value.target_owner_id},
      {packet_field::targetOwnerLeaseGeneration, value.target_owner_lease_generation},
      {packet_field::sourceSpotId, value.source_spot_id},
      {packet_field::sourceSpotGeneration, value.source_spot_generation},
      {packet_field::boundSessionNodeRid, value.bound_session_node_rid},
      {packet_field::boundSessionRid, value.bound_session_rid},
      {packet_field::sessionRelocationRoute,
       zlink::framework::detail::base64_encode (
         std::as_bytes (std::span<const std::uint8_t> (value.session_relocation_route)))},
      {packet_field::handoffBacklog, value.handoff_backlog},
      {packet_field::coreTransfer, value.core_transfer},
      {packet_field::coreTransferIdHigh, value.core_transfer_id_high},
      {packet_field::coreTransferIdLow, value.core_transfer_id_low},
      {packet_field::coreMembershipEpoch, value.core_membership_epoch},
      {packet_field::coreFinalSequence, value.core_final_sequence},
      {packet_field::finalizeTimeoutMs, value.finalize_timeout_ms},
      {packet_field::prepare, value.prepare},
      {packet_field::finalize, value.finalize}};
}

void from_json (const nlohmann::json &json, spot_actor_commit_route_request_t &value)
{
    value.transfer_id = json.at (packet_field::transferId).get<std::string> ();
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.actor_authority_owner_generation =
      json.value (packet_field::actorAuthorityOwnerGeneration, std::uint64_t{0});
    value.expected_actor_store_version = json.value (packet_field::expectedActorStoreVersion, "");
    value.completion_root_reference = json.value (packet_field::completionRootReference, "");
    value.completion_root_checksum =
      json.value (packet_field::completionRootChecksum, std::uint32_t{0});
    value.target_spot_id = json.at (packet_field::targetSpotId).get<std::string> ();
    value.target_spot_generation =
      json.value (packet_field::targetSpotGeneration, std::uint64_t{0});
    value.source_mesh_name = json.value (packet_field::sourceMeshName, "");
    value.target_mesh_name = json.value (packet_field::targetMeshName, "");
    value.target_node_lifecycle_generation =
      json.value (packet_field::targetNodeLifecycleGeneration, std::uint64_t{0});
    value.target_owner_id = json.value (packet_field::targetOwnerId, "");
    value.target_owner_lease_generation =
      json.value (packet_field::targetOwnerLeaseGeneration, std::uint64_t{0});
    value.source_spot_id = json.value (packet_field::sourceSpotId, "");
    value.source_spot_generation =
      json.value (packet_field::sourceSpotGeneration, std::uint64_t{0});
    value.bound_session_node_rid = json.value (packet_field::boundSessionNodeRid, "");
    value.bound_session_rid = json.value (packet_field::boundSessionRid, "");
    value.session_relocation_route =
      json.contains (packet_field::sessionRelocationRoute)
        ? decode_base64_field (json, packet_field::sessionRelocationRoute)
        : std::vector<std::uint8_t>{};
    const auto handoff_backlog = json.find (packet_field::handoffBacklog);
    if (handoff_backlog != json.end ()) {
        validate_handoff_backlog_json (*handoff_backlog);
        value.handoff_backlog = handoff_backlog->get<std::vector<spot_actor_handoff_packet_t>> ();
    } else {
        value.handoff_backlog.clear ();
    }
    value.core_transfer = json.value (packet_field::coreTransfer, false);
    value.core_transfer_id_high = json.value (packet_field::coreTransferIdHigh, std::uint64_t{0});
    value.core_transfer_id_low = json.value (packet_field::coreTransferIdLow, std::uint64_t{0});
    value.core_membership_epoch = json.value (packet_field::coreMembershipEpoch, std::uint64_t{0});
    value.core_final_sequence = json.value (packet_field::coreFinalSequence, std::uint64_t{0});
    value.finalize_timeout_ms = json.value (packet_field::finalizeTimeoutMs, std::uint64_t{0});
    value.prepare = json.value (packet_field::prepare, false);
    value.finalize = json.value (packet_field::finalize, false);
}

void to_json (nlohmann::json &json, const spot_actor_leave_route_command_t &value)
{
    json = nlohmann::json{
      {packet_field::transferId, value.transfer_id},
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::sourceSpotId, value.source_spot_id},
      {packet_field::sourceSpotGeneration, value.source_spot_generation},
      {packet_field::targetSpotId, value.target_spot_id},
      {packet_field::targetNodeRid, value.target_node_rid},
      {packet_field::targetNodeGeneration, value.target_node_generation},
      {packet_field::targetAuthorityOwnerGeneration, value.target_authority_owner_generation},
      {packet_field::targetOwnerLeaseGeneration, value.target_owner_lease_generation}};
}

void from_json (const nlohmann::json &json, spot_actor_leave_route_command_t &value)
{
    value.transfer_id = json.at (packet_field::transferId).get<std::string> ();
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.source_spot_id = json.at (packet_field::sourceSpotId).get<std::string> ();
    value.source_spot_generation =
      json.at (packet_field::sourceSpotGeneration).get<std::uint64_t> ();
    value.target_spot_id = json.at (packet_field::targetSpotId).get<std::string> ();
    value.target_node_rid = json.at (packet_field::targetNodeRid).get<std::string> ();
    value.target_node_generation =
      json.at (packet_field::targetNodeGeneration).get<std::uint64_t> ();
    value.target_authority_owner_generation =
      json.at (packet_field::targetAuthorityOwnerGeneration).get<std::uint64_t> ();
    value.target_owner_lease_generation =
      json.at (packet_field::targetOwnerLeaseGeneration).get<std::uint64_t> ();
}

void to_json (nlohmann::json &json, const spot_actor_join_route_reply_t &value)
{
    json = nlohmann::json{
      {packet_field::resultCode, value.result_code},
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))},
      {packet_field::receiveChunkLimitBytes, value.receive_chunk_limit_bytes}};
}

void from_json (const nlohmann::json &json, spot_actor_join_route_reply_t &value)
{
    value.result_code = json.at (packet_field::resultCode).get<int> ();
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.payload = decode_base64_field (json, packet_field::payload);
    value.receive_chunk_limit_bytes =
      json.value (packet_field::receiveChunkLimitBytes, std::uint64_t{0});
}

void to_json (nlohmann::json &json, const spot_actor_packet_route_request_t &value)
{
    json = nlohmann::json{
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::actorNodeGeneration, value.actor_node_generation},
      {packet_field::actorAuthorityOwnerGeneration, value.actor_authority_owner_generation},
      {packet_field::actorOwnerLeaseGeneration, value.actor_owner_lease_generation},
      {packet_field::spotId, value.spot_id},
      {packet_field::packetName, value.packet_name_value},
      {packet_field::contentType, value.content_type},
      {packet_field::messageFollowHopCount, value.message_follow_hop_count},
      {packet_field::metadata, value.metadata},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))}};
}

void from_json (const nlohmann::json &json, spot_actor_packet_route_request_t &value)
{
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.actor_node_generation = json.value (packet_field::actorNodeGeneration, std::uint64_t{0});
    value.actor_authority_owner_generation =
      json.value (packet_field::actorAuthorityOwnerGeneration, std::uint64_t{0});
    value.actor_owner_lease_generation =
      json.value (packet_field::actorOwnerLeaseGeneration, std::uint64_t{0});
    value.spot_id = json.at (packet_field::spotId).get<std::string> ();
    value.packet_name_value = json.at (packet_field::packetName).get<std::string> ();
    value.content_type =
      json.value (packet_field::contentType, zlink::detail::json_profile::content_type);
    value.message_follow_hop_count =
      json.value (packet_field::messageFollowHopCount, std::uint8_t{0});
    if (value.message_follow_hop_count > runtime::protocol::messageFollowHopCount)
        throw std::invalid_argument ("Actor packet Message Follow hop count exceeds 8");
    const bool has_any_target_fence = value.actor_node_generation != 0
                                      || value.actor_authority_owner_generation != 0
                                      || value.actor_owner_lease_generation != 0;
    const bool has_complete_target_fence = value.actor_node_generation != 0
                                           && value.actor_authority_owner_generation != 0
                                           && value.actor_owner_lease_generation != 0;
    if ((value.message_follow_hop_count == 0 && has_any_target_fence)
        || (value.message_follow_hop_count != 0 && !has_complete_target_fence)) {
        throw std::invalid_argument ("Actor packet Message Follow target fence is incomplete");
    }
    value.metadata = json.value (packet_field::metadata, std::map<std::string, std::string>{});
    value.payload = decode_base64_field (json, packet_field::payload);
}

void to_json (nlohmann::json &json, const spot_actor_packet_route_reply_t &value)
{
    json = nlohmann::json{
      {packet_field::actorRefPresent, value.actor_ref_present},
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::hasReply, value.has_reply},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))}};
}

void from_json (const nlohmann::json &json, spot_actor_packet_route_reply_t &value)
{
    value.actor_ref_present = json.value (packet_field::actorRefPresent, false);
    value.actor_node_rid = json.value (packet_field::actorNodeRid, "");
    value.actor_type = json.value (packet_field::actorType, "");
    value.actor_id = json.value (packet_field::actorId, "");
    value.actor_generation = json.value (packet_field::actorGeneration, std::uint64_t{0});
    value.has_reply = json.at (packet_field::hasReply).get<bool> ();
    value.payload = decode_base64_field (json, packet_field::payload);
}

void to_json (nlohmann::json &json, const spot_actor_disconnect_route_request_t &value)
{
    json = nlohmann::json{{packet_field::actorNodeRid, value.actor_node_rid},
                          {packet_field::actorType, value.actor_type},
                          {packet_field::actorId, value.actor_id},
                          {packet_field::actorGeneration, value.actor_generation}};
}

void from_json (const nlohmann::json &json, spot_actor_disconnect_route_request_t &value)
{
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
}

void to_json (nlohmann::json &json, const spot_actor_disconnect_route_reply_t &value)
{
    json = nlohmann::json{{packet_field::accepted, value.accepted}};
}

void from_json (const nlohmann::json &json, spot_actor_disconnect_route_reply_t &value)
{
    value.accepted = json.value (packet_field::accepted, true);
}

void to_json (nlohmann::json &json, const actor_bound_session_route_request_t &value)
{
    json = nlohmann::json{
      {packet_field::actorNodeRid, value.actor_node_rid},
      {packet_field::actorType, value.actor_type},
      {packet_field::actorId, value.actor_id},
      {packet_field::actorGeneration, value.actor_generation},
      {packet_field::packetName, value.packet_name_value},
      {packet_field::codec, static_cast<std::uint8_t> (value.codec)},
      {packet_field::payload, zlink::framework::detail::base64_encode (
                                std::as_bytes (std::span<const std::uint8_t> (value.payload)))}};
}

void from_json (const nlohmann::json &json, actor_bound_session_route_request_t &value)
{
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.packet_name_value = json.at (packet_field::packetName).get<std::string> ();
    const auto codec =
      json.value (packet_field::codec, static_cast<std::uint8_t> (stream_codec_t::raw));
    if (codec > static_cast<std::uint8_t> (stream_codec_t::protobuf)) {
        throw std::invalid_argument ("actor bound session route codec is invalid");
    }
    value.codec = static_cast<stream_codec_t> (codec);
    value.payload = decode_base64_field (json, packet_field::payload);
}

void to_json (nlohmann::json &json, const actor_bound_session_bind_route_request_t &value)
{
    json = nlohmann::json{{packet_field::actorNodeRid, value.actor_node_rid},
                          {packet_field::actorType, value.actor_type},
                          {packet_field::actorId, value.actor_id},
                          {packet_field::actorGeneration, value.actor_generation},
                          {packet_field::sessionNodeRid, value.session_node_rid}};
}

void from_json (const nlohmann::json &json, actor_bound_session_bind_route_request_t &value)
{
    value.actor_node_rid = json.at (packet_field::actorNodeRid).get<std::string> ();
    value.actor_type = json.at (packet_field::actorType).get<std::string> ();
    value.actor_id = json.at (packet_field::actorId).get<std::string> ();
    value.actor_generation = json.at (packet_field::actorGeneration).get<std::uint64_t> ();
    value.session_node_rid = json.at (packet_field::sessionNodeRid).get<std::string> ();
}

void to_json (nlohmann::json &json, const actor_bound_session_route_reply_t &value)
{
    json = nlohmann::json{{packet_field::accepted, value.accepted}};
}

void from_json (const nlohmann::json &json, actor_bound_session_route_reply_t &value)
{
    value.accepted = json.value (packet_field::accepted, true);
}

zlink::message_t message_from_bytes (const std::vector<std::uint8_t> &bytes)
{
    return zlink::message_t::from (bytes);
}

actor_ref_t actor_ref_from_spot_route (const spot_actor_admission_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

actor_ref_t actor_ref_from_spot_route (const spot_actor_commit_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

spot_actor_join_route_reply_t make_spot_actor_join_route_reply (const actor_join_reply_t &reply)
{
    return spot_actor_join_route_reply_t{
      .result_code = reply.result_code,
      .actor_node_rid = std::string (reply.actor.node_rid ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (reply.actor)),
      .actor_id = std::string (reply.actor.actor_id ().value ()),
      .actor_generation = reply.actor.object_generation (),
      .payload = reply.reply.to_bytes (),
      /* This node's advertised inbound cap for direct-transfer relocation
       * payload chunks bound for actors admitted here. */
      .receive_chunk_limit_bytes = spot_actor_join_advertised_receive_chunk_limit_bytes};
}

actor_join_reply_t actor_join_reply_from_spot_route (const spot_actor_join_route_reply_t &reply)
{
    return actor_join_reply_t{reply.result_code,
                              ::zlink::framework::detail::actor_ref_access_t::make (
                                node_rid_t::from_string (reply.actor_node_rid), reply.actor_type,
                                reply.actor_id, reply.actor_generation),
                              message_from_bytes (reply.payload)};
}

spot_actor_packet_route_request_t make_spot_actor_packet_route_request (
  const actor_ref_t &actor_ref,
  spot_id_t spot_id,
  std::string_view packet_name,
  const zlink::message_t &payload,
  const spot_inbound_message_t &metadata,
  std::optional<runtime::protocol::actor_route_fence_t> target_fence)
{
    std::uint8_t message_follow_hop_count = 0;
    if (const auto hop = metadata.find (runtime::protocol::message_follow_hop_count_metadata_key)) {
        const auto parsed = std::stoul (std::string (*hop));
        if (parsed > runtime::protocol::messageFollowHopCount)
            throw std::invalid_argument ("Actor packet Message Follow hop count exceeds 8");
        message_follow_hop_count = static_cast<std::uint8_t> (parsed);
    }
    if (target_fence
        && (target_fence->actor_id != actor_ref.actor_id ().value ()
            || target_fence->object_generation != actor_ref.object_generation ()
            || target_fence->target_node_routing_id
                 != zlink::routing_id_t::from (std::string (actor_ref.node_rid ().value ()))
                      .to_bytes ()
            || target_fence->target_node_generation == 0
            || target_fence->authority_owner_generation == 0
            || target_fence->owner_lease_generation == 0)) {
        throw std::invalid_argument ("Actor packet target fence does not match the target Actor");
    }
    return spot_actor_packet_route_request_t{
      .actor_node_rid = std::string (actor_ref.node_rid ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      .actor_id = std::string (actor_ref.actor_id ().value ()),
      .actor_generation = actor_ref.object_generation (),
      .actor_node_generation = target_fence ? target_fence->target_node_generation : 0,
      .actor_authority_owner_generation =
        target_fence ? target_fence->authority_owner_generation : 0,
      .actor_owner_lease_generation = target_fence ? target_fence->owner_lease_generation : 0,
      .spot_id = std::string (spot_id),
      .packet_name_value = std::string (packet_name),
      .content_type = metadata.content_type,
      .message_follow_hop_count = message_follow_hop_count,
      .metadata = metadata.values,
      .payload = payload.to_bytes ()};
}

actor_ref_t actor_ref_from_spot_route (const spot_actor_packet_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

spot_actor_disconnect_route_request_t
make_spot_actor_disconnect_route_request (const actor_ref_t &actor_ref)
{
    return spot_actor_disconnect_route_request_t{
      .actor_node_rid = std::string (actor_ref.node_rid ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      .actor_id = std::string (actor_ref.actor_id ().value ()),
      .actor_generation = actor_ref.object_generation ()};
}

actor_ref_t actor_ref_from_spot_route (const spot_actor_disconnect_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

actor_bound_session_route_request_t
make_actor_bound_session_route_request (const actor_ref_t &actor_ref,
                                        std::string_view packet_name,
                                        stream_codec_t codec,
                                        const zlink::message_t &payload)
{
    return actor_bound_session_route_request_t{
      .actor_node_rid = std::string (actor_ref.node_rid ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      .actor_id = std::string (actor_ref.actor_id ().value ()),
      .actor_generation = actor_ref.object_generation (),
      .packet_name_value = std::string (packet_name),
      .codec = codec,
      .payload = payload.to_bytes ()};
}

actor_ref_t actor_ref_from_bound_session_route (const actor_bound_session_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

actor_ref_t
actor_ref_from_bound_session_route (const actor_bound_session_bind_route_request_t &request)
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (request.actor_node_rid), request.actor_type, request.actor_id,
      request.actor_generation);
}

} // namespace zlink::framework::detail
