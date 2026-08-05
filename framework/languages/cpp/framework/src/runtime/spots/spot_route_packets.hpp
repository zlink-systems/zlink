/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

struct spot_multicast_route_send_t
{
    static constexpr const char *packet_name = "__zlink.spot.multicast";

    std::string topic;
    std::vector<std::uint8_t> frame;
};

struct spot_actor_admission_route_request_t
{
    static constexpr const char *packet_name = "__zlink.spot.actor.join.admission";

    std::string transfer_id;
    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::uint64_t actor_authority_owner_generation = 0;
    std::uint64_t completion_operation_id_high = 0;
    std::uint64_t completion_operation_id_low = 0;
    std::string source_spot_id;
    std::string target_spot_id;
    std::vector<std::uint8_t> payload;
};

struct spot_actor_admission_route_reply_t
{
    bool accepted = false;
    std::vector<std::uint8_t> payload;
    std::string completion_root_reference;
    std::uint32_t completion_root_checksum = 0;
};

// One preserved in-flight packet carried with the commit (spot-actor §10.2-2).
// The target enqueues these in order before it publishes the committed
// location, so direct packets cannot overtake them (§10.2-3).
struct spot_actor_handoff_packet_t
{
    std::string packet_name_value;
    std::vector<std::uint8_t> payload;
    std::string content_type;
    std::map<std::string, std::string> metadata;
    bool is_request = false;
};

struct spot_actor_commit_route_request_t
{
    static constexpr const char *packet_name = "__zlink.spot.actor.join.commit";

    std::string transfer_id;
    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::uint64_t actor_authority_owner_generation = 0;
    std::string completion_root_reference;
    std::uint32_t completion_root_checksum = 0;
    std::string target_spot_id;
    std::string bound_session_node_rid;
    std::string bound_session_rid;
    std::vector<std::uint8_t> transfer_state;
    std::vector<spot_actor_handoff_packet_t> handoff_backlog;
    bool core_transfer = false;
    std::uint64_t core_transfer_id_high = 0;
    std::uint64_t core_transfer_id_low = 0;
    std::uint64_t core_membership_epoch = 0;
    std::uint64_t core_final_sequence = 0;
    std::uint64_t core_reserve_message_count = 0;
    std::uint64_t core_reserve_byte_count = 0;
    bool prepare = false;
    bool finalize = false;
};

struct spot_actor_join_route_request_t
{
    static constexpr const char *packet_name = "__zlink.spot.actor.join";

    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::string spot_id;
    std::vector<std::uint8_t> payload;
    bool actor_snapshot_present = false;
    std::vector<std::uint8_t> actor_snapshot;
};

struct spot_actor_join_route_reply_t
{
    int result_code = 0;
    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::vector<std::uint8_t> payload;
};

struct spot_actor_packet_route_request_t
{
    static constexpr const char *packet_name = "__zlink.spot.actor.packet";

    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::string spot_id;
    std::string packet_name_value;
    std::string content_type = "application/json";
    std::uint8_t message_follow_hop_count = 0;
    std::map<std::string, std::string> metadata;
    std::vector<std::uint8_t> payload;
};

struct spot_actor_packet_route_reply_t
{
    bool actor_ref_present = false;
    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    bool has_reply = false;
    std::vector<std::uint8_t> payload;
};

struct spot_actor_disconnect_route_request_t
{
    static constexpr const char *packet_name = "__zlink.spot.actor.disconnect";

    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
};

struct spot_actor_disconnect_route_reply_t
{
    bool accepted = true;
};

struct actor_bound_session_route_request_t
{
    static constexpr const char *packet_name = "__zlink.actor.boundSession.send";

    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::string packet_name_value;
    stream_codec_t codec = stream_codec_t::raw;
    std::vector<std::uint8_t> payload;
};

struct actor_bound_session_bind_route_request_t
{
    static constexpr const char *packet_name = "__zlink.actor.boundSession.bind";

    std::string actor_node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t actor_generation = 0;
    std::string session_node_rid;
};

struct actor_bound_session_route_reply_t
{
    bool accepted = true;
};

result_t<zlink::message_t> encode_actor_bound_session_frame (
  stream_codec_t codec,
  std::string packet_name,
  const zlink::message_t &payload);

spot_actor_join_route_request_t make_spot_actor_join_route_request (
  const actor_ref_t &actor_ref,
  spot_id_t spot_id,
  const zlink::message_t &payload,
  const std::optional<zlink::message_t> &actor_snapshot = std::nullopt);

actor_ref_t actor_ref_from_spot_route (const spot_actor_admission_route_request_t &request);
actor_ref_t actor_ref_from_spot_route (const spot_actor_commit_route_request_t &request);

actor_ref_t actor_ref_from_spot_route (const spot_actor_join_route_request_t &request);

spot_actor_join_route_reply_t make_spot_actor_join_route_reply (const actor_join_reply_t &reply);

actor_join_reply_t actor_join_reply_from_spot_route (const spot_actor_join_route_reply_t &reply);

spot_actor_packet_route_request_t
make_spot_actor_packet_route_request (const actor_ref_t &actor_ref,
                                      spot_id_t spot_id,
                                      std::string_view packet_name,
                                      const zlink::message_t &payload,
                                      const spot_inbound_message_t &metadata);

actor_ref_t actor_ref_from_spot_route (const spot_actor_packet_route_request_t &request);

spot_actor_disconnect_route_request_t
make_spot_actor_disconnect_route_request (const actor_ref_t &actor_ref);

actor_ref_t actor_ref_from_spot_route (const spot_actor_disconnect_route_request_t &request);

actor_bound_session_route_request_t make_actor_bound_session_route_request (
  const actor_ref_t &actor_ref,
  std::string_view packet_name,
  stream_codec_t codec,
  const zlink::message_t &payload);

actor_ref_t actor_ref_from_bound_session_route (const actor_bound_session_route_request_t &request);
actor_ref_t
actor_ref_from_bound_session_route (const actor_bound_session_bind_route_request_t &request);

void register_spot_route_packet_serializers (serializer_registry_t &serializers);

} // namespace zlink::framework::detail
