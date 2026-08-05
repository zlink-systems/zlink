/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/spots/spot_route_internal_dispatcher.hpp"

#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace zlink::framework::detail
{

namespace
{

void trace_actor_transfer_target (std::string_view stage,
                                  std::string_view actor_id,
                                  std::string_view transfer_id = {})
{
    const auto *enabled = std::getenv ("ZLINK_CPP_AUTO_CONNECT_TRACE");
    if (enabled == nullptr || *enabled == '\0') {
        return;
    }
    std::cerr << "zlink actor-transfer stage=" << stage
              << " actor=" << actor_id
              << " transfer=" << transfer_id << '\n';
}

constexpr std::string_view actor_relay_kind_metadata_key = "__zlink.actorRelayKind";
constexpr std::string_view actor_relay_kind_send = "send";
constexpr std::string_view actor_bind_session_route_metadata_key = "__zlink.actorBindSessionRoute";

stream_message_kind_t actor_relay_kind_from_metadata (spot_inbound_message_t &metadata)
{
    auto kind = stream_message_kind_t::request;
    const auto found = metadata.values.find (std::string (actor_relay_kind_metadata_key));
    if (found != metadata.values.end ()) {
        if (found->second == actor_relay_kind_send) {
            kind = stream_message_kind_t::send;
        }
        metadata.values.erase (found);
    }
    return kind;
}

bool should_bind_actor_session_route (spot_inbound_message_t &metadata)
{
    const auto found = metadata.values.find (std::string (actor_bind_session_route_metadata_key));
    if (found == metadata.values.end ()) {
        return true;
    }
    const auto bind = found->second != "false";
    metadata.values.erase (found);
    return bind;
}

} // namespace

spot_route_internal_dispatcher_t::spot_route_internal_dispatcher_t (
  spot_node_runtime_t runtime,
  actor_gateway_runtime_t actor_gateway,
  route_client_t route_client,
  serializer_registry_t &serializers) :
    _runtime (std::move (runtime)),
    _actor_gateway (std::move (actor_gateway)),
    _route_client (std::move (route_client)),
    _serializers (&serializers)
{
}

bool spot_route_internal_dispatcher_t::can_handle_send (std::string_view packet_name) const
{
    return packet_name == actor_bound_session_route_request_t::packet_name
           || packet_name == spot_multicast_route_send_t::packet_name;
}

bool spot_route_internal_dispatcher_t::can_handle_request (std::string_view packet_name) const
{
    return packet_name == actor_bound_session_bind_route_request_t::packet_name
           || packet_name == actor_bound_session_route_request_t::packet_name
           || packet_name == spot_actor_admission_route_request_t::packet_name
           || packet_name == spot_actor_commit_route_request_t::packet_name
           || packet_name == spot_actor_join_route_request_t::packet_name
           || packet_name == spot_actor_packet_route_request_t::packet_name
           || packet_name == spot_actor_disconnect_route_request_t::packet_name;
}

result_t<void>
spot_route_internal_dispatcher_t::dispatch_send (const route_received_packet_t &received,
                                                 service_provider_t &services) const
{
    (void) received;
    (void) services;
    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        return result_t<void>::failure (body.error_kind (), body.error ()
                                                              ? body.error ()->what ()
                                                              : "actor route send body missing");
    }
    try {
        if (body && received.parts.items ().size () > 0) {
            const auto header = runtime::messaging::envelope_codec_t{}
              .decode_header (received.parts);
            if (header
                && header.value ().message_name
                     == spot_multicast_route_send_t::packet_name) {
                auto request = _serializers
                  ->get<spot_multicast_route_send_t> ()
                  .deserialize (detail::encoded_payload_from_raw (body.value ()));
                auto dispatched = _runtime.dispatch_multicast (
                  std::move (request.topic),
                  std::vector<zlink::message_t>{
                    zlink::message_t::from (std::move (request.frame))},
                  services, *_serializers);
                return dispatched
                         ? result_t<void>::success ()
                         : detail::propagate_failure<void> (
                             dispatched, "SPOT multicast route dispatch failed");
            }
        }
        auto request = _serializers->get<actor_bound_session_route_request_t> ().deserialize (
          detail::encoded_payload_from_raw (body.value ()));
        auto actor_ref = actor_ref_from_bound_session_route (request);
        auto actor_gateway = _actor_gateway;
        auto updated = actor_gateway.update_actor_ref (actor_ref);
        if (!updated) {
            return result_t<void>::failure (updated.error_kind (), updated.error ()
                                                                     ? updated.error ()->what ()
                                                                     : "actor ref update failed");
        }
        auto dispatched = actor_gateway.dispatch_bound_session_send (
          actor_ref, request.packet_name_value, request.codec,
          zlink::message_t::from (request.payload));
        return dispatched;
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        std::string ("actor route send decode failed: ")
                                          + error.what ());
    }
}

result_t<void> spot_route_internal_dispatcher_t::bind_actor_session_route (
  actor_gateway_runtime_t &actor_gateway,
  const actor_ref_t &actor_ref,
  std::string route_channel_name,
  zlink::routing_id_t session_node_rid,
  std::optional<zlink::routing_id_t> session_rid,
  bool replace_existing) const
{
    auto bound = actor_gateway.bind_session_route (
      actor_ref, _route_client, std::move (route_channel_name), session_node_rid,
      stream_codec_t::message_pack, replace_existing, std::move (session_rid));
    return bound;
}

result_t<actor_gateway_runtime_t> spot_route_internal_dispatcher_t::bind_actor_route (
  const actor_ref_t &actor_ref,
  const runtime::messaging::envelope_header_t &header,
  const route_received_packet_t &received) const
{
    auto actor_gateway = _actor_gateway;
    auto bound = bind_actor_session_route (
      actor_gateway, actor_ref,
      _runtime.actor_route_transport_name ().value_or (header.channel_name),
      received.source_node_rid, received.source_session_rid, false);
    if (!bound) {
        return detail::propagate_failure<actor_gateway_runtime_t> (
          bound, "actor session route binding failed");
    }
    return result_t<actor_gateway_runtime_t>::success (std::move (actor_gateway));
}

result_t<zlink::message_t> spot_route_internal_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services) const
{
    (void) header;
    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        return detail::propagate_failure<zlink::message_t> (body, "SPOT route request body missing");
    }

    try {
        if (header.message_name == actor_bound_session_bind_route_request_t::packet_name) {
            auto request =
              _serializers->get<actor_bound_session_bind_route_request_t> ().deserialize (
                detail::encoded_payload_from_raw (body.value ()));
            auto actor_ref = actor_ref_from_bound_session_route (request);
            auto actor_gateway = _actor_gateway;
            auto bound = bind_actor_session_route (
              actor_gateway, actor_ref,
              _runtime.actor_route_transport_name ().value_or (header.channel_name),
              zlink::routing_id_t::from (request.session_node_rid), std::nullopt, true);
            if (!bound) {
                return detail::propagate_failure<zlink::message_t> (
                  bound, "actor bound session route binding failed");
            }
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<actor_bound_session_route_reply_t> ().serialize (
                actor_bound_session_route_reply_t{.accepted = true})));
        }
        if (header.message_name == spot_actor_admission_route_request_t::packet_name) {
            auto request = _serializers->get<spot_actor_admission_route_request_t> ().deserialize (
              detail::encoded_payload_from_raw (body.value ()));
            trace_actor_transfer_target ("admission-received", request.actor_id,
                                         request.transfer_id);
            auto runtime = _runtime;
            auto admitted = runtime.admit_remote_actor_to_spot (
              request.transfer_id, actor_ref_from_spot_route (request),
              spot_id_t (request.source_spot_id),
              spot_id_t (request.target_spot_id),
              zlink::message_t::from (request.payload),
              request.completion_operation_id_high,
              request.completion_operation_id_low);
            if (!admitted) {
                return detail::propagate_failure<zlink::message_t> (admitted, "remote actor admission failed");
            }
            const auto completion_root =
              runtime.pending_join_completion_root (
                request.transfer_id);
            const auto reply = spot_actor_admission_route_reply_t{
              .accepted = admitted.value ().accepted,
              .payload =
                admitted.value ().reply
                  ? detail::message_to_raw (*admitted.value ().reply, *_serializers).to_bytes ()
                  : std::vector<std::uint8_t>{},
              .completion_root_reference =
                completion_root
                  ? completion_root->reference
                  : std::string{},
              .completion_root_checksum =
                completion_root
                  ? completion_root->checksum_crc32c
                  : 0};
            trace_actor_transfer_target ("admission-replying", request.actor_id,
                                         request.transfer_id);
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<spot_actor_admission_route_reply_t> ().serialize (reply)));
        }
        if (header.message_name == spot_actor_commit_route_request_t::packet_name) {
            auto request = _serializers->get<spot_actor_commit_route_request_t> ().deserialize (
              detail::encoded_payload_from_raw (body.value ()));
            trace_actor_transfer_target ("commit-received", request.actor_id, request.transfer_id);
            auto actor_ref = actor_ref_from_spot_route (request);
            auto actor_gateway = _actor_gateway;
            if (!request.core_transfer) {
                auto bound = bind_actor_route (actor_ref, header, received);
                if (!bound) {
                    return detail::propagate_failure<zlink::message_t> (
                      bound, "actor session route binding failed");
                }
                actor_gateway = std::move (bound.value ());
            }
            trace_actor_transfer_target ("commit-route-bound", request.actor_id,
                                         request.transfer_id);
            auto runtime = _runtime;
            const auto recovered_completion =
              !request.completion_root_reference.empty ()
              && !runtime.pending_join_completion_root (
                   request.transfer_id);
            if (!request.completion_root_reference.empty ()
                || request.completion_root_checksum != 0) {
                const auto restored = runtime.restore_pending_join_completion (
                  request.transfer_id, actor_ref,
                  spot_id_t (request.target_spot_id),
                  zlink::framework::runtime::stateful::
                    durable_join_completion_root_t{
                    request.completion_root_reference,
                    request.completion_root_checksum});
                if (!restored) {
                    return detail::propagate_failure<zlink::message_t> (
                      restored,
                      "remote Actor completion root validation failed");
                }
            }
            std::vector<handoff_packet_t> handoff_backlog;
            handoff_backlog.reserve (request.handoff_backlog.size ());
            for (auto &packet : request.handoff_backlog) {
                handoff_backlog.push_back (handoff_packet_t{
                  std::move (packet.packet_name_value), std::move (packet.payload),
                  std::move (packet.content_type), std::move (packet.metadata),
                  packet.is_request});
            }
            if (request.finalize && request.core_transfer) {
                auto native = runtime.native_node ();
                if (!native) {
                    return result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure,
                      "target Core MeshNode is unavailable");
                }
                const auto &target_spot = request.target_spot_id;
                runtime::host::actor_transfer_prepare_t transfer_prepare{
                  .role = runtime::host::actor_transfer_role_t::target,
                  .transfer_id = request.transfer_id,
                  .actor = runtime::host::public_host_runtime_t::remote_actor_ref (
                    zlink::routing_id_t::from (request.actor_node_rid),
                    request.actor_id, request.actor_generation),
                  .source_spot_id = target_spot,
                  .target_spot_id = target_spot,
                  .target_node_rid = native->status ().routing_id ()};
                runtime::host::actor_transfer_token_t transfer_token;
                runtime::host::actor_transfer_prepare_result_t transfer_result{
                  transfer_prepare.actor, 0};
                try {
                    if (request.actor_authority_owner_generation != 0) {
                        const auto target_spot_object =
                          native->resolve_spot (target_spot);
                        if (!target_spot_object) {
                            return result_t<zlink::message_t>::failure (
                              framework_error_kind_t::not_found,
                              "target Spot authority is unavailable");
                        }
                        if (request.actor_authority_owner_generation
                            == std::numeric_limits<std::uint64_t>::max ()) {
                            return result_t<zlink::message_t>::failure (
                              framework_error_kind_t::protocol_error,
                              "Actor authority owner generation is exhausted");
                        }
                        (void) native->create_reserved_actor (
                          request.actor_type,
                          runtime::stateful::object_ref_t{
                            runtime::stateful::object_kind_t::actor,
                            request.actor_id,
                            request.actor_generation,
                            request.actor_authority_owner_generation + 1,
                            target_spot_object->mesh_name,
                            native->status ().routing_id ().to_string ()});
                    } else {
                        (void) native->create_actor (
                          request.actor_type,
                          request.actor_id);
                    }
                }
                catch (const std::exception &error) {
                    return result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure,
                      error.what ());
                }
                const auto core_prepared = native->prepare_actor_transfer (
                  transfer_prepare, transfer_token, transfer_result);
                if (!core_prepared) {
                    return result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure,
                      "target Framework Actor relocation prepare failed");
                }
                const auto next_membership_epoch =
                  transfer_result.membership_epoch + 1;
                const auto core_committed = transfer_token.commit (next_membership_epoch);
                const auto core_activated =
                  core_committed && transfer_token.activate ();
                if (!core_activated) {
                    return result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure,
                      "target Framework Actor relocation activation failed");
                }
                runtime.record_core_actor_transfer_activation (
                  request.actor_id, next_membership_epoch);
            }
            if (request.finalize && !request.bound_session_node_rid.empty ()) {
                const auto target_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
                  runtime.node_rid (), std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
                  std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
                const auto actor_ref_updated =
                  actor_gateway.update_actor_ref (target_actor_ref);
                if (!actor_ref_updated) {
                    return result_t<zlink::message_t>::failure (
                      actor_ref_updated.error_kind (),
                      actor_ref_updated.error () ? actor_ref_updated.error ()->what ()
                                                 : "remote actor ref update failed");
                }
                auto bound = bind_actor_session_route (
                  actor_gateway, target_actor_ref,
                  _runtime.actor_route_transport_name ().value_or (header.channel_name),
                  zlink::routing_id_t::from (request.bound_session_node_rid),
                  request.bound_session_rid.empty ()
                        ? std::nullopt
                        : std::make_optional (
                            zlink::routing_id_t::from (request.bound_session_rid)),
                  true);
                if (!bound) {
                    return detail::propagate_failure<zlink::message_t> (
                      bound, "actor bound session route binding failed");
                }
            }
            if (request.finalize && recovered_completion) {
                const auto replacement_prepared =
                  runtime.prepare_remote_actor_to_spot (
                    request.transfer_id,
                    actor_ref,
                    spot_id_t (request.target_spot_id),
                    zlink::message_t::from (
                      request.transfer_state),
                    actor_gateway.actor_context (actor_ref),
                    true);
                if (!replacement_prepared) {
                    return detail::propagate_failure<
                      zlink::message_t> (
                      replacement_prepared,
                      "replacement target Actor prepare failed");
                }
            }
            auto committed = request.finalize
                               ? runtime.finalize_remote_actor_to_spot (
                                   request.transfer_id, actor_ref,
                                   spot_id_t (request.target_spot_id),
                                   std::move (handoff_backlog), services, &actor_gateway)
                             : request.prepare
                               ? runtime.prepare_remote_actor_to_spot (
                                   request.transfer_id, actor_ref,
                                   spot_id_t (request.target_spot_id),
                                   zlink::message_t::from (request.transfer_state),
                                   actor_gateway.actor_context (actor_ref), true)
                               : runtime.commit_remote_actor_to_spot (
                                   request.transfer_id, actor_ref,
                                   spot_id_t (request.target_spot_id),
                                   zlink::message_t::from (request.transfer_state),
                                   actor_gateway.actor_context (actor_ref),
                                   std::move (handoff_backlog), &services);
            trace_actor_transfer_target ("commit-applied", request.actor_id, request.transfer_id);
            if (!committed) {
                return detail::propagate_failure<zlink::message_t> (committed, "remote actor commit failed");
            }
            if (!request.prepare) {
                const auto actor_ref_updated =
                  actor_gateway.update_actor_ref (committed.value ().actor);
                if (!actor_ref_updated) {
                    return result_t<zlink::message_t>::failure (
                      actor_ref_updated.error_kind (),
                      actor_ref_updated.error () ? actor_ref_updated.error ()->what ()
                                                 : "remote actor ref update failed");
                }
                if (!request.bound_session_node_rid.empty ()) {
                    auto bound = bind_actor_session_route (
                      actor_gateway, committed.value ().actor,
                      _runtime.actor_route_transport_name ().value_or (header.channel_name),
                      zlink::routing_id_t::from (request.bound_session_node_rid),
                      request.bound_session_rid.empty ()
                        ? std::nullopt
                        : std::make_optional (
                            zlink::routing_id_t::from (request.bound_session_rid)),
                      true);
                    if (!bound) {
                        return detail::propagate_failure<zlink::message_t> (
                          bound, "actor bound session route binding failed");
                    }
                }
            }
            trace_actor_transfer_target ("commit-replying", request.actor_id, request.transfer_id);
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<spot_actor_join_route_reply_t> ().serialize (
                make_spot_actor_join_route_reply (committed.value ()))));
        }
        if (header.message_name == actor_bound_session_route_request_t::packet_name) {
            auto request = _serializers->get<actor_bound_session_route_request_t> ().deserialize (
              detail::encoded_payload_from_raw (body.value ()));
            auto actor_ref = actor_ref_from_bound_session_route (request);
            auto actor_gateway = _actor_gateway;
            auto updated = actor_gateway.update_actor_ref (actor_ref);
            if (!updated) {
                return detail::propagate_failure<zlink::message_t> (updated, "actor ref update failed");
            }
            auto dispatched = actor_gateway.dispatch_bound_session_send (
              actor_ref, request.packet_name_value, request.codec,
              zlink::message_t::from (request.payload));
            if (!dispatched) {
                return result_t<zlink::message_t>::failure (
                  dispatched.error_kind (), dispatched.error ()
                                              ? dispatched.error ()->what ()
                                              : "routed actor bound session send failed");
            }
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<actor_bound_session_route_reply_t> ().serialize (
                actor_bound_session_route_reply_t{.accepted = true})));
        }
        if (header.message_name == spot_actor_packet_route_request_t::packet_name) {
            auto request = _serializers->get<spot_actor_packet_route_request_t> ().deserialize (
              detail::encoded_payload_from_raw (body.value ()));
            auto runtime = _runtime;
            auto actor_ref = actor_ref_from_spot_route (request);
            spot_inbound_message_t metadata;
            metadata.content_type = request.content_type;
            metadata.values = request.metadata;
            metadata.values["__zlink.messageFollowHopCount"] =
              std::to_string (request.message_follow_hop_count);
            if (!header.correlation_id.empty ())
                metadata.correlation_id = header.correlation_id;
            const auto message_kind = actor_relay_kind_from_metadata (metadata);
            auto actor_gateway = _actor_gateway;
            if (should_bind_actor_session_route (metadata)) {
                auto bound = bind_actor_route (actor_ref, header, received);
                if (!bound) {
                    return detail::propagate_failure<zlink::message_t> (
                      bound, "actor session route binding failed");
                }
                actor_gateway = std::move (bound.value ());
            }
            auto relayed = runtime.manager ().relay_actor_packet (
              actor_ref, actor_gateway.actor_context (actor_ref), message_kind,
              request.packet_name_value, zlink::message_t::from (request.payload), services,
              *_serializers, std::move (metadata));
            if (!relayed) {
                return detail::propagate_failure<zlink::message_t> (relayed, "remote actor packet failed");
            }
            auto current_actor_ref = runtime.current_actor_ref (actor_ref).value_or (actor_ref);
            (void) actor_gateway.update_actor_ref (current_actor_ref);
            auto reply = spot_actor_packet_route_reply_t{
              .actor_ref_present = true,
              .actor_node_rid = std::string (current_actor_ref.node_rid ().value ()),
              .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (current_actor_ref)),
              .actor_id = std::string (current_actor_ref.actor_id ().value ()),
              .actor_generation = current_actor_ref.object_generation (),
              .has_reply = relayed.value ().has_value (),
              .payload =
                relayed.value () ? relayed.value ()->to_bytes () : std::vector<std::uint8_t>{}};
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<spot_actor_packet_route_reply_t> ().serialize (reply)));
        }
        if (header.message_name == spot_actor_disconnect_route_request_t::packet_name) {
            auto request = _serializers->get<spot_actor_disconnect_route_request_t> ().deserialize (
              detail::encoded_payload_from_raw (body.value ()));
            auto disconnected =
              _runtime.notify_actor_disconnected_erased (actor_ref_from_spot_route (request));
            if (!disconnected) {
                return result_t<zlink::message_t>::failure (
                  disconnected.error_kind (), disconnected.error ()
                                                ? disconnected.error ()->what ()
                                                : "remote actor disconnect notify failed");
            }
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<spot_actor_disconnect_route_reply_t> ().serialize (
                spot_actor_disconnect_route_reply_t{})));
        }
        auto request = _serializers->get<spot_actor_join_route_request_t> ().deserialize (
          detail::encoded_payload_from_raw (body.value ()));
        auto runtime = _runtime;
        auto actor_ref = actor_ref_from_spot_route (request);
        auto bound = bind_actor_route (actor_ref, header, received);
        if (!bound) {
            return detail::propagate_failure<zlink::message_t> (
              bound, "actor session route binding failed");
        }
        auto actor_gateway = std::move (bound.value ());
        const auto entry_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          runtime.node_rid (), std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
          std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
        auto joined =
          request.spot_id.empty ()
            ? runtime.join_actor_to_entry_spot_erased (
                entry_actor_ref, runtime.node_rid (), zlink::message_t::from (request.payload),
                request.actor_snapshot_present
                  ? std::make_optional (zlink::message_t::from (request.actor_snapshot))
                  : std::nullopt,
                actor_gateway.actor_context (entry_actor_ref))
            : runtime.join_remote_actor_to_spot_erased (
                actor_ref, spot_id_t (request.spot_id),
                zlink::message_t::from (request.payload), actor_gateway.actor_context (actor_ref));
        if (!joined) {
            return detail::propagate_failure<zlink::message_t> (joined, "remote actor join failed");
        }
        (void) actor_gateway.update_actor_ref (joined.value ().actor);
        auto reply = make_spot_actor_join_route_reply (joined.value ());
        return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
          _serializers->get<spot_actor_join_route_reply_t> ().serialize (reply)));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (const std::exception &error) {
        return result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error,
          std::string ("SPOT route request decode failed: ") + error.what ());
    }
}

} // namespace zlink::framework::detail
