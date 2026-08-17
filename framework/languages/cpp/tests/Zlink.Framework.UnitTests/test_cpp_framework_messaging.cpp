/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <service_wire_constants.hpp>

#include <nlohmann/json.hpp>

#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

class sample_call_t : public zlink::framework::detail::call_facade_t<sample_call_t, int>
{
  public:
    explicit sample_call_t (int value) :
        call_facade_t (zlink::framework::result_t<int>::success (value))
    {
    }
};

class work_latch_t
{
  public:
    explicit work_latch_t (std::size_t remaining) : _remaining (remaining) {}

    void arrive ()
    {
        std::lock_guard lock (_mutex);
        if (_remaining != 0 && --_remaining == 0)
            _changed.notify_all ();
    }

    bool wait_for (std::chrono::milliseconds timeout)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout,
                                  [&] { return _remaining == 0; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _remaining;
};

struct envelope_payload_t
{
    int value{};
};

} // namespace

int main ()
{
    {
        zlink::framework::runtime::messaging::envelope_header_t request;
        request.message_name = "ActorRequest";
        const auto moving = zlink::framework::detail::make_origin_exception (
          zlink::framework::framework_error_kind_t::unavailable,
          zlink::framework::detail::failure_origin_t::actor_transfer_in_progress,
          "wording is not part of retry classification");
        const auto header =
          zlink::framework::detail::channel_reply_writer_t{}
            .create_error_header ("actor", request, moving);
        const auto restored =
          zlink::framework::runtime::messaging::restore_failure_origin (
            header,
            zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::unavailable,
              "translated text"));
        if (zlink::framework::detail::failure_origin (restored)
            != zlink::framework::detail::failure_origin_t::
                 actor_transfer_in_progress) {
            return 106;
        }
    }

    {
        zlink::framework::serializer_registry_t serializers;
        const auto packet_serializer =
          serializers.get<zlink::framework::detail::
                            spot_actor_packet_route_request_t> ();
        const auto packet_wire = packet_serializer.serialize (
          zlink::framework::detail::spot_actor_packet_route_request_t{
            .payload = {0, 1, 2, 253, 254, 255}});
        const auto packet_json = nlohmann::json::parse (packet_wire.to_string ());
        if (packet_serializer.content_type () != "application/json"
            || !packet_json.at ("payload").is_string ()
            || packet_json.at ("payload").get<std::string> () != "AAEC/f7/"
            || packet_serializer.deserialize (packet_wire).payload
                 != std::vector<std::uint8_t> ({0, 1, 2, 253, 254, 255})) {
            return 154;
        }
        auto duplicate_field_wire = packet_wire.to_string ();
        duplicate_field_wire.insert (
          duplicate_field_wire.size () - 1,
          R"(,"payload":"AA==")");
        try {
            (void) packet_serializer.deserialize (
              zlink::framework::encoded_payload_t::from_string (
                duplicate_field_wire));
            return 158;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            if (error.kind ()
                != zlink::framework::framework_error_kind_t::protocol_error)
                return 159;
        }
        const auto actor_node = zlink::routing_id_t::from ("actor-node-b");
        const auto fenced_packet = packet_serializer.deserialize (
          packet_serializer.serialize (
            zlink::framework::detail::spot_actor_packet_route_request_t{
              .actor_node_rid = actor_node.to_string (),
              .actor_type = "player",
              .actor_id = "actor-1",
              .actor_generation = 7,
              .actor_node_generation = 11,
              .actor_authority_owner_generation = 13,
              .actor_owner_lease_generation = 17,
              .spot_id = "spot-b",
              .packet_name_value = "ProbeReq",
              .message_follow_hop_count = 1,
              .payload = {1}}));
        if (fenced_packet.actor_node_generation != 11
            || fenced_packet.actor_authority_owner_generation != 13
            || fenced_packet.actor_owner_lease_generation != 17
            || fenced_packet.message_follow_hop_count != 1) {
            return 156;
        }
        auto incomplete_fence = packet_json;
        incomplete_fence["messageFollowHopCount"] = 1;
        try {
            (void) packet_serializer.deserialize (
              zlink::framework::encoded_payload_t::from_string (
                incomplete_fence.dump ()));
            return 157;
        }
        catch (const std::exception &) {
        }
        for (const auto *invalid_payload : {"AQ=", "A===", "AB==", "AQ=A"}) {
            auto invalid_json = packet_json;
            invalid_json["payload"] = invalid_payload;
            try {
                (void) packet_serializer.deserialize (
                  zlink::framework::encoded_payload_t::from_string (
                    invalid_json.dump ()));
                return 155;
            }
            catch (const std::exception &) {
            }
        }
        const auto admission_root =
          serializers
            .get<zlink::framework::detail::
                   spot_actor_admission_route_reply_t> ()
            .deserialize (
              serializers
                .get<zlink::framework::detail::
                       spot_actor_admission_route_reply_t> ()
                .serialize (
                  zlink::framework::detail::
                    spot_actor_admission_route_reply_t{
                      true,
                      {1, 2},
                      "join-root",
                      0x12345678}));
        if (admission_root.completion_root_reference
              != "join-root"
            || admission_root.completion_root_checksum
                 != 0x12345678) {
            return 150;
        }
        const auto serialized_commit =
          serializers
            .get<zlink::framework::detail::
                   spot_actor_commit_route_request_t> ()
            .serialize (
              zlink::framework::detail::
                spot_actor_commit_route_request_t{
                  .transfer_id = "transfer-root",
                  .completion_root_reference = "join-root",
                  .completion_root_checksum = 0x12345678,
                  .source_spot_id = "source-spot",
                  .session_relocation_route = {3, 5, 8}});
        const auto commit_shape =
          nlohmann::json::parse (serialized_commit.to_string ());
        if (commit_shape.contains ("coreReserveMessageCount")
            || commit_shape.contains ("coreReserveByteCount")
            || commit_shape.contains ("deferCompletion")
            || commit_shape.contains ("completionOnly")) {
            return 153;
        }
        const auto commit_root =
          serializers
            .get<zlink::framework::detail::
                   spot_actor_commit_route_request_t> ()
            .deserialize (serialized_commit);
        if (commit_root.completion_root_reference
              != "join-root"
            || commit_root.completion_root_checksum
                 != 0x12345678
            || commit_root.source_spot_id != "source-spot"
            || commit_root.session_relocation_route
                 != std::vector<std::uint8_t> ({3, 5, 8})) {
            return 151;
        }
        //  The handoff backlog has no record-count or stored-size bound, so a
        //  large backlog round-trips instead of being rejected.
        auto large_backlog =
          zlink::framework::detail::spot_actor_commit_route_request_t{};
        large_backlog.handoff_backlog.resize (2048);
        for (auto &packet : large_backlog.handoff_backlog)
            packet.packet_name_value = "handoff";
        const auto round_tripped =
          serializers
            .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
            .deserialize (
              serializers
                .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
                .serialize (large_backlog));
        if (round_tripped.handoff_backlog.size () != 2048) {
            return 152;
        }
    }
    {
        zlink::framework::serializer_registry_t serializers;
        serializers.add<envelope_payload_t> (
          [] (const envelope_payload_t &payload) {
              return zlink::framework::encoded_payload_t::from_string (
                std::to_string (payload.value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return envelope_payload_t{std::stoi (payload.to_string ())};
          });

        zlink::framework::runtime::messaging::client_call_codec_t client_codec;
        const auto header = client_codec.create_envelope (
          zlink::framework::runtime::messaging::message_kind_t::request, "profile",
          "EnvelopePayload", std::chrono::milliseconds (10), std::string ("lookup"),
          std::string ("client"));
        const auto parts =
          client_codec.encode_envelope_parts (header, envelope_payload_t{42}, serializers);
        zlink::framework::runtime::messaging::envelope_codec_t envelope_codec;
        const auto decoded_header = envelope_codec.decode_header (parts);
        if (!decoded_header
            || decoded_header.value ().kind
                 != zlink::framework::runtime::messaging::message_kind_t::request
            || decoded_header.value ().channel_name != "profile"
            || decoded_header.value ().message_name != "EnvelopePayload"
            || decoded_header.value ().content_type != "application/octet-stream"
            || decoded_header.value ().correlation_id.empty () || !decoded_header.value ().deadline
            || decoded_header.value ().topic.value_or ("") != "lookup"
            || decoded_header.value ().source.value_or ("") != "client") {
            return 11;
        }
        const auto decoded_body = envelope_codec.decode_body (parts);
        if (!decoded_body
            || serializers.get<envelope_payload_t> ()
                   .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                     decoded_body.value ()))
                   .value
                 != 42) {
            return 12;
        }

        zlink::framework::runtime::messaging::envelope_header_t reply_header;
        reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
        reply_header.channel_name = "profile";
        reply_header.message_name = "EnvelopePayload";
        reply_header.correlation_id = header.correlation_id;
        const auto reply = envelope_codec.encode_raw_body_parts (
          reply_header, zlink::message_t::from (std::string ("7")));
        const auto reply_result = client_codec.decode_envelope_reply<envelope_payload_t> (
          reply, serializers, "empty reply", "reply failed", "profile request");
        if (!reply_result || reply_result.value ().value != 7) {
            return 13;
        }

        zlink::framework::runtime::messaging::envelope_header_t error_header;
        error_header.kind = zlink::framework::runtime::messaging::message_kind_t::error;
        error_header.channel_name = "profile";
        error_header.message_name = "EnvelopePayload";
        error_header.error_code = "route_not_connected";
        error_header.error_message = "route is down";
        const auto error_reply = envelope_codec.encode_raw_body_parts (
          error_header, zlink::message_t::from (std::string{}));
        const auto error_result = client_codec.decode_envelope_reply<envelope_payload_t> (
          error_reply, serializers, "empty reply", "reply failed", "profile request");
        if (error_result
            || error_result.error_kind ()
                 != zlink::framework::framework_error_kind_t::unavailable
            || error_result.error () == nullptr) {
            return 14;
        }

        zlink::framework::runtime::messaging::envelope_header_t wire_error_header;
        wire_error_header.kind = zlink::framework::runtime::messaging::message_kind_t::error;
        wire_error_header.channel_name = "profile";
        wire_error_header.message_name = "MissingProfileReq";
        wire_error_header.correlation_id = "request-2";
        wire_error_header.error_code = "handler_not_found";
        wire_error_header.error_message = "missing handler";
        const auto wire_error_json = envelope_codec.encode_header (wire_error_header).to_string ();
        if (wire_error_json.find (R"("kind":5)") == std::string::npos
            || wire_error_json.find (R"("errorCode":"handler_not_found")")
                 == std::string::npos
            || wire_error_json.find (R"("errorMessage":"missing handler")")
                 == std::string::npos
            || wire_error_json.find (R"("status")") != std::string::npos) {
            return 53;
        }
        const auto decoded_wire_error = envelope_codec.decode_header (
          zlink::message_t::from (wire_error_json));
        if (!decoded_wire_error
            || decoded_wire_error.value ().kind
                 != zlink::framework::runtime::messaging::message_kind_t::error
            || decoded_wire_error.value ().error_code != "handler_not_found"
            || decoded_wire_error.value ().error_message != "missing handler") {
            return 54;
        }

        zlink::framework::runtime::messaging::envelope_header_t wire_response_header;
        wire_response_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
        wire_response_header.channel_name = "profile";
        wire_response_header.message_name = "ProfileReq";
        wire_response_header.correlation_id = "request-3";
        const auto wire_response_json =
          envelope_codec.encode_header (wire_response_header).to_string ();
        if (wire_response_json.find (R"("kind":2)") == std::string::npos
            || wire_response_json.find (R"("errorCode":null)") == std::string::npos
            || wire_response_json.find (R"("errorMessage":null)") == std::string::npos
            || wire_response_json.find (R"("status")") != std::string::npos) {
            return 55;
        }

        zlink::framework::runtime::messaging::request_failure_mapper_t mapper;
        using zlink::framework::framework_error_kind_t;
        using zlink::framework::runtime::messaging::map_request_result_exception;
        using zlink::framework::runtime::messaging::map_submit_result_exception;
        using zlink::framework::runtime::messaging::map_submit_result_error_kind;
        if (map_submit_result_error_kind (zlink::submit_result_t::backpressured)
                != framework_error_kind_t::deadline_exceeded
            || map_submit_result_error_kind (zlink::submit_result_t::not_connected)
                 != framework_error_kind_t::unavailable
            || map_submit_result_error_kind (zlink::submit_result_t::not_found)
                 != framework_error_kind_t::not_found
            || map_submit_result_error_kind (zlink::submit_result_t::not_admitted)
                 != framework_error_kind_t::rejected
            || map_submit_result_error_kind (zlink::submit_result_t::terminated)
                 != framework_error_kind_t::shutting_down
            || map_submit_result_error_kind (zlink::submit_result_t::invalid_argument)
                 != framework_error_kind_t::invalid_operation
            || map_submit_result_error_kind (zlink::submit_result_t::invalid_state)
                 != framework_error_kind_t::invalid_operation) {
            return 25;
        }
        const auto submit_backpressured = map_submit_result_exception (
          zlink::submit_result_t::backpressured, "native submit");
        const auto submit_disconnected = map_submit_result_exception (
          zlink::submit_result_t::not_connected, "native submit");
        const auto submit_shutdown = map_submit_result_exception (
          zlink::submit_result_t::terminated, "native submit");
        const auto submit_not_found = map_submit_result_exception (
          zlink::submit_result_t::not_found, "native submit");
        if (submit_backpressured.kind () != framework_error_kind_t::deadline_exceeded
            || zlink::framework::detail::boundary_state (submit_backpressured)
                 != zlink::framework::detail::boundary_error_t::timed_out
            || submit_disconnected.kind () != framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (submit_disconnected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || submit_shutdown.kind () != framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (submit_shutdown)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || submit_not_found.kind () != framework_error_kind_t::not_found) {
            return 26;
        }
        const auto native_timeout = map_request_result_exception (
          zlink::request_result_t::timed_out, "native request");
        const auto native_disconnected = map_request_result_exception (
          zlink::request_result_t::not_connected, "native request");
        const auto native_shutdown = map_request_result_exception (
          zlink::request_result_t::terminated, "native request");
        const auto native_rejected = map_request_result_exception (
          zlink::request_result_t::rejected, "native request");
        const auto native_busy = map_request_result_exception (
          zlink::request_result_t::busy, "native request");
        const auto worker_queue_full = mapper.reply_header_exception (
          106, 18, "RouteMesh request");
        const auto spot_moving = mapper.reply_header_exception (
          107, 34, "RouteMesh request");
        const auto actor_location_stale = mapper.reply_header_exception (
          107, 21, "RouteMesh request");
        const auto spot_generation_stale = mapper.reply_header_exception (
          107, 33, "RouteMesh request");
        const auto relocation_data_lost = mapper.reply_header_exception (
          105, 35, "RouteMesh request");
        const auto remote_conflict = mapper.reply_header_exception (
          107, 0, "RouteMesh request");
        const auto remote_busy = mapper.reply_header_exception (
          108, 0, "RouteMesh request");
        const auto already_exists = mapper.reply_header_exception (
          107, 3, "RouteMesh request");
        const auto actor_type_mismatch = mapper.reply_header_exception (
          107, 4, "RouteMesh request");
        const auto spot_type_mismatch = mapper.reply_header_exception (
          107, 7, "RouteMesh request");
        const auto session_not_bound = mapper.reply_header_exception (
          107, 8, "RouteMesh request");
        if (zlink::framework::detail::boundary_state (native_timeout)
                != zlink::framework::detail::boundary_error_t::timed_out
            || zlink::framework::detail::boundary_state (native_disconnected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || worker_queue_full.kind ()
                 != framework_error_kind_t::unavailable
            || remote_conflict.kind () != framework_error_kind_t::unavailable
            || remote_busy.kind () != framework_error_kind_t::unavailable
            || already_exists.kind () != framework_error_kind_t::already_exists
            || actor_type_mismatch.kind ()
                 != framework_error_kind_t::type_mismatch
            || spot_type_mismatch.kind ()
                 != framework_error_kind_t::type_mismatch
            || session_not_bound.kind ()
                 != framework_error_kind_t::invalid_operation
            || spot_moving.kind () != framework_error_kind_t::unavailable
            || actor_location_stale.kind ()
                 != framework_error_kind_t::unavailable
            || spot_generation_stale.kind ()
                 != framework_error_kind_t::invalid_operation
            || relocation_data_lost.kind ()
                 != framework_error_kind_t::data_lost
            || native_disconnected.code ()
                 != std::make_error_code (std::errc::not_connected)
            || zlink::framework::detail::boundary_state (native_shutdown)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || native_shutdown.kind () != framework_error_kind_t::shutting_down
            || native_rejected.kind () != framework_error_kind_t::rejected
            || native_busy.kind () != framework_error_kind_t::capacity_exceeded
            || mapper.reply_header_exception (113, 0, "native request").kind ()
                 != framework_error_kind_t::capacity_exceeded) {
            return 26;
        }
        {
            namespace ust = zlink::framework::runtime::user_spot_terminal;
            using zlink::framework::runtime::protocol::reply_header_t;
            //  Spec 32-framework-error-model:99-108 — user-spot remote reply
            //  ownership: a peer's operation-table/queue saturation
            //  Conflict(107)/Busy(108)+None is the target's resource, so
            //  Unavailable; only a target's placement/admission capacity
            //  Backpressured(113)+None is CapacityExceeded. Fine codes still
            //  refine (spotMoving(34)->Unavailable, spotGenerationStale(33)->
            //  InvalidOperation); Terminated(103)->ShuttingDown.
            if (ust::map_user_spot_wire_failure (reply_header_t{0, 108, 0}, true)
                    != framework_error_kind_t::unavailable
                || ust::map_user_spot_wire_failure (reply_header_t{0, 107, 0}, true)
                     != framework_error_kind_t::unavailable
                || ust::map_user_spot_wire_failure (reply_header_t{0, 108, 0}, false)
                     != framework_error_kind_t::unavailable
                || ust::map_user_spot_wire_failure (reply_header_t{0, 113, 0}, true)
                     != framework_error_kind_t::capacity_exceeded
                || ust::map_user_spot_wire_failure (reply_header_t{0, 107, 34}, true)
                     != framework_error_kind_t::unavailable
                || ust::map_user_spot_wire_failure (reply_header_t{0, 107, 33}, true)
                     != framework_error_kind_t::invalid_operation
                || ust::map_user_spot_wire_failure (reply_header_t{0, 103, 0}, true)
                     != framework_error_kind_t::shutting_down
                //  Spec 32:91-92 — a synthesized protocolError terminal from a
                //  malformed reply decode is ProtocolError, not Unavailable.
                || ust::map_user_spot_wire_failure (reply_header_t{0, 104, 0}, true)
                     != framework_error_kind_t::protocol_error
                || ust::map_user_spot_wire_failure (reply_header_t{0, 104, 0}, false)
                     != framework_error_kind_t::protocol_error) {
                return 27;
            }
        }
        const auto not_connected = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_connected, "profile request");
        if (not_connected.kind () != zlink::framework::framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (not_connected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || not_connected.code ()
                 != std::make_error_code (std::errc::not_connected)) {
            return 16;
        }
        const auto not_found = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_found, "profile request");
        if (not_found.kind () != zlink::framework::framework_error_kind_t::not_found) {
            return 17;
        }
        const auto timed_out = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::timed_out, "profile request");
        if (zlink::framework::detail::boundary_state (timed_out) != zlink::framework::detail::boundary_error_t::timed_out
            || timed_out.kind () != zlink::framework::framework_error_kind_t::deadline_exceeded) {
            return 18;
        }
        const auto busy = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::busy, "profile request");
        if (busy.kind () != zlink::framework::framework_error_kind_t::capacity_exceeded) {
            return 15;
        }
        const auto conflict = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::conflict, "profile request");
        const auto rejected = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::rejected, "profile request");
        const auto protocol = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::protocol_error,
          "profile request");
        const auto invalid_argument = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::invalid_argument,
          "profile request");
        const auto invalid_state = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::invalid_state, "profile request");
        const auto not_supported = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_supported, "profile request");
        const auto terminated = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::terminated, "profile request");
        const auto internal_error = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::internal_error,
          "profile request");
        if (conflict.kind () != zlink::framework::framework_error_kind_t::capacity_exceeded
            || rejected.kind () != zlink::framework::framework_error_kind_t::rejected
            || protocol.kind () != zlink::framework::framework_error_kind_t::protocol_error
            || invalid_argument.kind () != zlink::framework::framework_error_kind_t::invalid_operation
            || invalid_state.kind () != zlink::framework::framework_error_kind_t::invalid_operation
            || not_supported.kind () != zlink::framework::framework_error_kind_t::internal_failure
            || terminated.kind () != zlink::framework::framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (terminated)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || terminated.code ()
                 != std::make_error_code (std::errc::operation_canceled)
            || internal_error.kind () != zlink::framework::framework_error_kind_t::internal_failure) {
            return 22;
        }
        const auto timeout_header =
          mapper.error_header_exception ("timeout", "", "profile request");
        const auto timeout_with_message =
          mapper.error_header_exception ("timeout", "explicit timeout", "profile request");
        const auto route_header =
          mapper.error_header_exception ("route_not_connected", "", "profile request");
        const auto unavailable_header =
          mapper.error_header_exception ("unavailable", "", "profile request");
        const auto shutdown_header =
          mapper.error_header_exception ("shutting_down", "", "profile request");
        const auto deadline_header =
          mapper.error_header_exception ("deadline_exceeded", "", "profile request");
        const auto not_found_header =
          mapper.error_header_exception ("request_target_not_found", "", "profile request");
        const auto unknown_header =
          mapper.error_header_exception ("unknown", "", "profile request");
        const auto unknown_with_message =
          mapper.error_header_exception ("unknown", "explicit", "profile request");
        if (zlink::framework::detail::boundary_state (timeout_header) != zlink::framework::detail::boundary_error_t::timed_out
            || std::string (timeout_with_message.what ()) != "explicit timeout"
            || route_header.kind () != zlink::framework::framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (route_header)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || unavailable_header.code ()
                 != std::make_error_code (std::errc::not_connected)
            || zlink::framework::detail::boundary_state (shutdown_header)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || shutdown_header.kind ()
                 != zlink::framework::framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (deadline_header)
                 != zlink::framework::detail::boundary_error_t::timed_out
            || not_found_header.kind ()
                 != zlink::framework::framework_error_kind_t::not_found
            || unknown_header.kind () != zlink::framework::framework_error_kind_t::internal_failure
            || std::string (unknown_with_message.what ()) != "explicit") {
            return 23;
        }
        const auto rejected_header =
          mapper.error_header_exception ("request_rejected", "", "profile request");
        if (rejected_header.kind () != zlink::framework::framework_error_kind_t::rejected) {
            return 19;
        }
        const auto protocol_header =
          mapper.error_header_exception ("request_protocol_error", "", "profile request");
        if (protocol_header.kind ()
            != zlink::framework::framework_error_kind_t::protocol_error) {
            return 20;
        }
        const auto missing_handler_header =
          mapper.error_header_exception ("handler_not_found", "", "profile request");
        if (missing_handler_header.kind ()
            != zlink::framework::framework_error_kind_t::not_found) {
            return 21;
        }
        const auto decode_header =
          mapper.error_header_exception ("payload_decode_failed", "", "profile request");
        if (decode_header.kind ()
            != zlink::framework::framework_error_kind_t::protocol_error) {
            return 24;
        }
    }

    auto sample_task = sample_call_t (42).submit ();
    if (sample_task.result ().value () != 42) {
        return 1;
    }

    {
        /* Copies refer to the same logical call. Exactly one public
         * terminator may start transport admission. */
        std::atomic_int duplicate_attempts{0};
        zlink::framework::send_call_t duplicate_call (
          "duplicate",
          [&] (const std::string &,
               const zlink::framework::send_call_t::metadata_map_t &) {
              ++duplicate_attempts;
              return zlink::framework::result_t<void>::success ();
          });
        auto duplicate_copy = duplicate_call;
        duplicate_call.submit ().result ().value ();
        bool duplicate_rejected = false;
        try {
            (void) duplicate_copy.submit ().result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            duplicate_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        if (!duplicate_rejected || duplicate_attempts.load () != 1) {
            return 78;
        }

        std::atomic_int multicast_calls{0};
        work_latch_t multicast_work_finished (1);
        zlink::framework::publish_call_t multicast_call (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++multicast_calls;
              multicast_work_finished.arrive ();
              return zlink::framework::result_t<void>::success ();
          });
        auto multicast_copy = multicast_call;
        multicast_call.submit ().result ().value ();
        bool duplicate_multicast_rejected = false;
        try {
            (void) multicast_copy.submit ().result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            duplicate_multicast_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        if (!duplicate_multicast_rejected
            || !multicast_work_finished.wait_for (std::chrono::seconds (2))
            || multicast_calls.load () != 1) {
            return 80;
        }

        zlink::framework::publish_call_t failed_after_completion (
          [] (const zlink::framework::publish_call_t::metadata_map_t &) {
              return zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::capacity_exceeded,
                "logical multicast observation probe");
          });
        // Dequeue is already terminal for the caller. Application-bound paths
        // report the later failure through their structured observer.
        failed_after_completion.submit ().result ().value ();

        /* Logical Multicast uses one direct handoff after its worker slots.
         * Test-owned work latches prove that every slot is occupied before the
         * overflow assertion, without exposing executor state from runtime. */
        const auto worker_count = std::max<std::size_t> (
          2, std::thread::hardware_concurrency ());
        std::mutex multicast_gate_mutex;
        std::condition_variable multicast_gate_changed;
        bool release_multicast_workers = false;
        work_latch_t occupied_workers_started (worker_count);
        work_latch_t occupied_workers_finished (worker_count);
        std::vector<zlink::framework::task_t<void>> occupied_workers;
        occupied_workers.reserve (worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            zlink::framework::publish_call_t occupied (
              [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
                  occupied_workers_started.arrive ();
                  std::unique_lock lock (multicast_gate_mutex);
                  multicast_gate_changed.wait (
                    lock, [&] { return release_multicast_workers; });
                  occupied_workers_finished.arrive ();
                  return zlink::framework::result_t<void>::success ();
              });
            occupied_workers.push_back (occupied.submit ());
        }
        if (!occupied_workers_started.wait_for (std::chrono::seconds (2))) {
            {
                std::lock_guard lock (multicast_gate_mutex);
                release_multicast_workers = true;
            }
            multicast_gate_changed.notify_all ();
            return 85;
        }

        std::atomic_int handoff_calls{0};
        work_latch_t handoff_finished (1);
        zlink::framework::publish_call_t handoff (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++handoff_calls;
              handoff_finished.arrive ();
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::seconds (1));
        auto handoff_task = handoff.submit ();
        std::atomic_int overflow_calls{0};
        zlink::framework::publish_call_t overflow (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++overflow_calls;
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::milliseconds (25));
        bool overflow_timed_out = false;
        auto overflow_task = overflow.submit ();
        const bool overflow_completed_immediately = overflow_task.await_ready ();
        try {
            overflow_task.result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            overflow_timed_out =
              error.kind ()
              == zlink::framework::framework_error_kind_t::deadline_exceeded;
        }
        if (!overflow_completed_immediately || !overflow_timed_out
            || overflow_calls.load () != 0) {
            {
                std::lock_guard lock (multicast_gate_mutex);
                release_multicast_workers = true;
            }
            multicast_gate_changed.notify_all ();
            return 82;
        }
        {
            std::lock_guard lock (multicast_gate_mutex);
            release_multicast_workers = true;
        }
        multicast_gate_changed.notify_all ();
        for (auto &task : occupied_workers)
            task.result ().value ();
        handoff_task.result ().value ();
        if (!occupied_workers_finished.wait_for (std::chrono::seconds (2))
            || !handoff_finished.wait_for (std::chrono::seconds (2))
            || handoff_calls.load () != 1) {
            return 83;
        }

        bool release_deadline_workers = false;
        work_latch_t deadline_workers_started (worker_count);
        work_latch_t deadline_workers_finished (worker_count);
        std::vector<zlink::framework::task_t<void>> deadline_workers;
        deadline_workers.reserve (worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            zlink::framework::publish_call_t occupied (
              [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
                  deadline_workers_started.arrive ();
                  std::unique_lock lock (multicast_gate_mutex);
                  multicast_gate_changed.wait (
                    lock, [&] { return release_deadline_workers; });
                  deadline_workers_finished.arrive ();
                  return zlink::framework::result_t<void>::success ();
              });
            deadline_workers.push_back (occupied.submit ());
        }
        if (!deadline_workers_started.wait_for (std::chrono::seconds (2))) {
            {
                std::lock_guard lock (multicast_gate_mutex);
                release_deadline_workers = true;
            }
            multicast_gate_changed.notify_all ();
            return 86;
        }

        std::atomic_int expired_handoff_calls{0};
        zlink::framework::publish_call_t expired_handoff (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++expired_handoff_calls;
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::milliseconds (25));
        auto expired_handoff_task = expired_handoff.submit ();
        std::this_thread::sleep_for (std::chrono::milliseconds (35));
        {
            std::lock_guard lock (multicast_gate_mutex);
            release_deadline_workers = true;
        }
        multicast_gate_changed.notify_all ();
        bool handoff_timed_out = false;
        try {
            expired_handoff_task.result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            handoff_timed_out =
              error.kind ()
              == zlink::framework::framework_error_kind_t::deadline_exceeded;
        }
        for (auto &task : deadline_workers)
            task.result ().value ();
        if (!deadline_workers_finished.wait_for (std::chrono::seconds (2))
            || !handoff_timed_out || expired_handoff_calls.load () != 0) {
            return 84;
        }

        work_latch_t recovered_finished (1);
        zlink::framework::publish_call_t recovered (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              recovered_finished.arrive ();
              return zlink::framework::result_t<void>::success ();
          });
        recovered.submit ().result ().value ();
        if (!recovered_finished.wait_for (std::chrono::seconds (2)))
            return 87;
    }

    /* flow-correlation: envelope marker + optional flow pair round-trip,
     * ambient stamping, create-if-absent gate and off-mode propagation. */
    {
        namespace msg = zlink::framework::runtime::messaging;
        namespace rt = zlink::framework::runtime;
        msg::envelope_codec_t codec;
        msg::envelope_header_t header;
        header.kind = msg::message_kind_t::command;
        header.channel_name = "flows";
        header.message_name = "flow.msg";

        const auto plain = codec.decode_header (codec.encode_header (header));
        if (!plain || plain.value ().flow_id || plain.value ().flow_origin) {
            return 40;
        }
        if (codec.encode_header (header).to_string ().find ("\"formatMarker\":242")
            == std::string::npos) {
            return 41;
        }

        const auto created = rt::flow_id_t::create ();
        if (!rt::flow_id_t::is_valid (created) || created.size () != 36 || created[14] != '7') {
            return 42;
        }
        if (rt::flow_id_t::is_valid ("01890a5d-ac96-474b-bcce-b302099a8057")   // v4
            || rt::flow_id_t::is_valid ("01890A5D-AC96-774B-BCCE-B302099A8057") // uppercase
            || rt::flow_id_t::is_valid ("short")) {
            return 43;
        }

        {
            auto scope = rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::inbound,
                                                    zlink::framework::message_flow_log_mode_t::normal,
                                                    zlink::framework::flow_origin_t::inbound);
            const auto stamped = codec.decode_header (codec.encode_header (header));
            if (!stamped || stamped.value ().flow_id != created
                || stamped.value ().flow_origin != zlink::framework::flow_origin_t::inbound) {
                return 44;
            }
        }
        if (rt::flow_context_t::current ()) {
            return 45;
        }

        /* create-if-absent: no inbound id + capture on → new id. Off skips
         * inbound propagation and does not install even an empty context. */
        {
            auto scope =
              rt::flow_context_t::enter (
                std::nullopt, std::nullopt,
                zlink::framework::message_flow_log_mode_t::normal,
                                         zlink::framework::flow_origin_t::inbound);
            if (!rt::flow_context_t::current ()
                || !rt::flow_id_t::is_valid (rt::flow_context_t::current ()->flow_id)) {
                return 46;
            }
        }
        {
            auto scope = rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::timer,
                                                    zlink::framework::message_flow_log_mode_t::off,
                                                    zlink::framework::flow_origin_t::inbound);
            if (rt::flow_context_t::current ()) {
                return 47;
            }
        }
        {
            auto scope =
              rt::flow_context_t::enter (
                std::nullopt, std::nullopt,
                zlink::framework::message_flow_log_mode_t::off,
                                         zlink::framework::flow_origin_t::inbound);
            if (rt::flow_context_t::current ()) {
                return 48;
            }
        }
        {
            const std::optional<std::string> malformed_flow{"not-a-uuid"};
            auto scope = rt::flow_context_t::enter (
              malformed_flow, std::nullopt,
              zlink::framework::message_flow_log_mode_t::off,
              zlink::framework::flow_origin_t::inbound);
            if (rt::flow_context_t::current ()) {
                return 160;
            }
        }
        /* spec 27 §4: with capture_flow=false the envelope decoder neither
         * validates nor retains the observation-only flow pair, so malformed
         * flow data from a tracing-on sender cannot fail a frame at Off. */
        {
            auto stamped_message = [&] {
                auto scope = rt::flow_context_t::enter (
                  created, zlink::framework::flow_origin_t::inbound,
                  zlink::framework::message_flow_log_mode_t::normal,
                  zlink::framework::flow_origin_t::inbound);
                return codec.encode_header (header);
            } ();
            const auto off_decoded = codec.decode_header (stamped_message, false);
            if (!off_decoded || off_decoded.value ().flow_id
                || off_decoded.value ().flow_origin) {
                return 161;
            }
            auto corrupted = stamped_message.to_string ();
            const auto position = corrupted.find (created);
            if (position == std::string::npos) {
                return 162;
            }
            corrupted.replace (position, 4, "ZZZZ");
            const auto corrupted_message = zlink::message_t::from (corrupted);
            if (!codec.decode_header (corrupted_message, false)) {
                return 163;
            }
            if (codec.decode_header (corrupted_message)) {
                return 164;
            }
        }
        {
            auto outer = rt::flow_context_t::enter (
              created, zlink::framework::flow_origin_t::application,
              zlink::framework::message_flow_log_mode_t::normal,
              zlink::framework::flow_origin_t::application);
            {
                auto off = rt::flow_context_t::enter_current_or_create (
                  zlink::framework::flow_origin_t::application,
                  zlink::framework::message_flow_log_mode_t::off);
                if (rt::flow_context_t::current ()) {
                    return 161;
                }
            }
            if (!rt::flow_context_t::current ()
                || rt::flow_context_t::current ()->flow_id != created) {
                return 162;
            }
        }

        /* Marker/pair validation is fail-closed. */
        auto missing_marker = codec.decode_header (
          zlink::message_t::from ("{\"kind\":3,\"channelName\":\"c\",\"messageName\":\"m\"}"));
        if (missing_marker
            || missing_marker.error_kind ()
                 != zlink::framework::framework_error_kind_t::protocol_error) {
            return 49;
        }
        auto lonely_flow = codec.decode_header (zlink::message_t::from (
          "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"c\",\"messageName\":\"m\",\"flowId\":\""
          + created + "\"}"));
        if (lonely_flow
            || lonely_flow.error_kind ()
                 != zlink::framework::framework_error_kind_t::protocol_error) {
            return 50;
        }

        /* MFLOW-EXT-014: an async continuation re-enters the flow captured at
         * registration even when the task completes from a flow-less thread,
         * and nothing leaks into the completing thread afterwards. */
        {
            zlink::framework::detail::task_completion_source_t<int> source;
            std::string observed_in_callback;
            bool leaked_on_completer = false;
            {
                auto scope =
                  rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::inbound,
                                             zlink::framework::message_flow_log_mode_t::normal,
                                             zlink::framework::flow_origin_t::inbound);
                auto task = source.task ();
                zlink::framework::detail::observe_task_completion (
                  task, [&observed_in_callback] (const zlink::framework::result_t<int> &) {
                      if (const auto &flow = rt::flow_context_t::current ()) {
                          observed_in_callback = flow->flow_id;
                      }
                  });
            }
            std::thread completer ([&source, &leaked_on_completer] {
                source.complete (zlink::framework::result_t<int>::success (1));
                leaked_on_completer = rt::flow_context_t::current ().has_value ();
            });
            completer.join ();
            if (observed_in_callback != created) {
                return 51;
            }
            if (leaked_on_completer || rt::flow_context_t::current ()) {
                return 52;
            }
        }
    }

    return 0;
}
