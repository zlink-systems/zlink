/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/spots/spot_route_internal_dispatcher.hpp"

#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace zlink::framework::detail
{

namespace
{

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

task_t<void> submit_source_actor_leave_async (
  spot_node_runtime_t runtime,
  zlink::routing_id_t source_node_rid,
  std::string source_spot_id,
  std::uint64_t source_spot_generation,
  std::string target_spot_id,
  std::optional<runtime::messaging::message_parts_t> leave_parts)
{
    if (!leave_parts) {
        throw framework_exception_t (
          framework_error_kind_t::shutting_down,
          "source Actor leave transport is unavailable");
    }
    co_await runtime.send_spot_mesh_parts_exact (
      spot_id_t (target_spot_id), source_node_rid,
      spot_id_t (source_spot_id), source_spot_generation,
      std::move (*leave_parts));
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
           || packet_name == spot_actor_commit_route_request_t::packet_name
           || packet_name == spot_actor_leave_route_command_t::packet_name
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
        const auto header = runtime::messaging::envelope_codec_t{}
          .decode_header (received.parts);
        if (!header) {
            return detail::propagate_failure<void> (
              header, "SPOT route send header is invalid");
        }
        if (header.value ().message_name
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
        if (header.value ().message_name
            == spot_actor_commit_route_request_t::packet_name) {
            auto request =
              _serializers->get<spot_actor_commit_route_request_t> ().deserialize (
                detail::encoded_payload_from_raw (body.value ()));
            if (!request.finalize || request.prepare) {
                return result_t<void>::failure (
                  framework_error_kind_t::protocol_error,
                  "remote Actor cutover command shape is invalid");
            }
            dispatch_actor_commit_request (
              std::move (request), received, header.value (), services,
              [] (result_t<zlink::message_t>) {
                  // Cutover is one-way. Target lifecycle/completion owns the
                  // terminal Actor Join outcome; there is no source reply leg.
              });
            return result_t<void>::success ();
        }
        if (header.value ().message_name
            == spot_actor_leave_route_command_t::packet_name) {
            const auto command = _serializers
              ->get<spot_actor_leave_route_command_t> ()
              .deserialize (detail::encoded_payload_from_raw (body.value ()));
            if (command.transfer_id.empty ()
                || command.actor_node_rid != _runtime.node_rid ().value ()
                || command.actor_type.empty () || command.actor_id.empty ()
                || command.actor_generation == 0
                || command.source_spot_id.empty ()
                || command.source_spot_generation == 0
                || command.target_spot_id.empty ()
                || command.target_node_rid.empty ()
                || received.source_node_rid.to_string ()
                     != command.target_node_rid
                || command.target_node_generation == 0
                || command.target_authority_owner_generation == 0
                || command.target_owner_lease_generation == 0) {
                return result_t<void>::failure (
                  framework_error_kind_t::protocol_error,
                  "remote Actor leave command shape is invalid");
            }
            const auto source_actor =
              ::zlink::framework::detail::actor_ref_access_t::make (
                node_rid_t::from_string (command.actor_node_rid),
                command.actor_type, command.actor_id,
                command.actor_generation);
            auto runtime = _runtime;
            return runtime.submit_remote_actor_leave (
              command.transfer_id, source_actor,
              spot_id_t (command.source_spot_id),
              command.source_spot_generation,
              spot_id_t (command.target_spot_id),
              runtime::protocol::actor_route_fence_t{
                command.actor_id, command.actor_generation,
                zlink::routing_id_t::from (command.target_node_rid).to_bytes (),
                command.target_node_generation,
                command.target_authority_owner_generation,
                command.target_owner_lease_generation});
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

bool spot_route_internal_dispatcher_t::dispatch_request_async (
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services,
  std::function<void (result_t<zlink::message_t>)> completion) const
{
    if (!completion) {
        return false;
    }

    const auto body =
      runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        return false;
    }
    try {
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
            auto actor_gateway = _actor_gateway;
            if (should_bind_actor_session_route (metadata)) {
                auto bound = bind_actor_route (actor_ref, header, received);
                if (!bound) {
                    completion (detail::propagate_failure<zlink::message_t> (
                      bound, "actor session route binding failed"));
                    return true;
                }
                actor_gateway = std::move (bound.value ());
            }
            const auto follow_target = request.message_follow_hop_count == 0
              ? std::optional<runtime::protocol::actor_route_fence_t>{}
              : std::make_optional (runtime::protocol::actor_route_fence_t{
                  request.actor_id, request.actor_generation,
                  zlink::routing_id_t::from (request.actor_node_rid).to_bytes (),
                  request.actor_node_generation,
                  request.actor_authority_owner_generation,
                  request.actor_owner_lease_generation});
            auto relayed = runtime.manager ().relay_actor_packet (
              actor_ref, actor_gateway.actor_context (actor_ref),
              actor_relay_kind_from_metadata (metadata), request.packet_name_value,
              zlink::message_t::from (request.payload), services, *_serializers,
              std::move (metadata), follow_target ? &*follow_target : nullptr);
            detail::observe_task_completion (
              relayed,
              [runtime, actor_gateway = std::move (actor_gateway), actor_ref,
               serializers = _serializers, completion = std::move (completion)]
              (const result_t<std::optional<zlink::message_t>> &result) mutable {
                  if (!result) {
                      completion (detail::propagate_failure<zlink::message_t> (
                        result, "remote actor packet failed"));
                      return;
                  }
                  const auto current = runtime.current_actor_ref (actor_ref).value_or (actor_ref);
                  (void) actor_gateway.update_actor_ref (current);
                  const auto reply = spot_actor_packet_route_reply_t{
                    .actor_ref_present = true,
                    .actor_node_rid = std::string (current.node_rid ().value ()),
                    .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (current)),
                    .actor_id = std::string (current.actor_id ().value ()),
                    .actor_generation = current.object_generation (),
                    .has_reply = result.value ().has_value (),
                    .payload = result.value () ? result.value ()->to_bytes () : std::vector<std::uint8_t>{}};
                  completion (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (
                      serializers->get<spot_actor_packet_route_reply_t> ().serialize (reply))));
              });
            return true;
        }
        if (header.message_name != spot_actor_commit_route_request_t::packet_name)
            return false;
        auto request =
          _serializers->get<spot_actor_commit_route_request_t> ().deserialize (
            detail::encoded_payload_from_raw (body.value ()));
        if (request.prepare && !request.finalize) {
            return false;
        }
        dispatch_actor_commit_request (
          std::move (request), received, header, services,
          std::move (completion));
        return true;
    }
    catch (...) {
        /* The synchronous compatibility path owns malformed packets because
         * the finalize flag cannot be trusted until deserialization succeeds. */
        return false;
    }
}

void spot_route_internal_dispatcher_t::dispatch_actor_commit_request (
  spot_actor_commit_route_request_t request,
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services,
  std::function<void (result_t<zlink::message_t>)> completion) const
{
    if (!completion)
        return;

    struct completion_state_t
    {
        std::mutex mutex;
        bool settled = false;
        std::function<void (result_t<zlink::message_t>)> callback;
    };
    auto completion_state = std::make_shared<completion_state_t> ();
    completion_state->callback = std::move (completion);
    auto complete =
      [completion_state] (result_t<zlink::message_t> result) mutable {
          std::function<void (result_t<zlink::message_t>)> callback;
          {
              std::lock_guard lock (completion_state->mutex);
              if (completion_state->settled)
                  return;
              completion_state->settled = true;
              callback = std::move (completion_state->callback);
          }
          if (callback) {
              try {
                  callback (std::move (result));
              }
              catch (...) {
                  /* The request already reached its terminal callback. */
              }
          }
      };

    try {
        auto actor_ref = actor_ref_from_spot_route (request);
        auto runtime = _runtime;
        std::optional<runtime::protocol::session_relocation_route_t>
          session_relocation_route;
        if (!request.session_relocation_route.empty ()) {
            session_relocation_route =
              runtime::protocol::decode_session_relocation_route (
                request.session_relocation_route);
            const auto &route = *session_relocation_route;
            if (request.source_spot_id.empty ()
                || route.sender_role
                     != runtime::protocol::relocation_role_t::target
                || route.route.action
                     != runtime::protocol::
                          session_relocation_route_action_t::commit
                || route.actor.actor_id != request.actor_id
                || route.actor.object_generation
                     != request.actor_generation
                || route.route.previous_authority_owner_generation
                     != request.actor_authority_owner_generation
                || route.route.target_authority_owner_generation
                     != request.actor_authority_owner_generation + 1
                || zlink::routing_id_t::from (
                     route.route.target_node_routing_id)
                     .to_string ()
                     != runtime.node_rid ().value ()
                || route.route.target_node_generation
                     != request.target_node_lifecycle_generation
                || !runtime.stage_session_relocation_route (
                     request.transfer_id,
                     request.session_relocation_route,
                     request.actor_type,
                     request.target_owner_lease_generation)) {
                complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "remote Actor Session relocation route is stale"));
                return;
            }
        }
        std::uint64_t committed_authority_owner_generation = 0;
        // A finalize retry after the target completed returns the same Join
        // result and repeats only the idempotent Session route notification.
        if (request.finalize) {
            const auto completed =
              runtime.completed_remote_actor_commit (
                request.transfer_id, actor_ref,
                spot_id_t (request.target_spot_id));
            if (completed) {
                if (session_relocation_route) {
                    auto activation = std::make_shared<task_t<bool>> (
                      runtime.activate_session_relocation_route (request.transfer_id));
                    const auto reply = *completed;
                    auto *serializers = _serializers;
                    ::zlink::framework::detail::observe_task_completion (
                      *activation,
                      [activation, reply, serializers, complete] (
                        const result_t<bool> &activated) mutable {
                          if (!activated || !activated.value ()) {
                              complete (result_t<zlink::message_t>::failure (
                                framework_error_kind_t::unavailable,
                                "Session relocation route was not activated"));
                              return;
                          }
                          complete (result_t<zlink::message_t>::success (
                            detail::encoded_payload_to_raw (
                              serializers->get<spot_actor_join_route_reply_t> ()
                                .serialize (make_spot_actor_join_route_reply (reply)))));
                      });
                    return;
                }
                complete (result_t<zlink::message_t>::success (
                  detail::encoded_payload_to_raw (
                    _serializers
                      ->get<spot_actor_join_route_reply_t> ()
                      .serialize (
                        make_spot_actor_join_route_reply (*completed)))));
                return;
            }
        }
        auto actor_gateway = _actor_gateway;
        if (!request.core_transfer) {
            auto bound = bind_actor_route (actor_ref, header, received);
            if (!bound) {
                complete (detail::propagate_failure<zlink::message_t> (
                  bound, "actor session route binding failed"));
                return;
            }
            actor_gateway = std::move (bound.value ());
        }
        if (!request.completion_root_reference.empty ()
            || request.completion_root_checksum != 0) {
            complete (result_t<zlink::message_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join completion roots are process-local"));
            return;
        }
        std::vector<handoff_packet_t> handoff_backlog;
        handoff_backlog.reserve (request.handoff_backlog.size ());
        for (auto &packet : request.handoff_backlog) {
            handoff_backlog.push_back (handoff_packet_t{
              std::move (packet.packet_name_value), std::move (packet.payload),
              std::move (packet.content_type), std::move (packet.metadata),
              packet.is_request});
        }
        const auto activate_core_transfer =
          [&] (std::uint64_t authority_owner_generation) -> result_t<void> {
              auto native = runtime.native_node ();
              if (!native) {
                  return result_t<void>::failure (
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
                  const auto target_spot_object =
                    native->resolve_spot (target_spot);
                  if (!target_spot_object) {
                      return result_t<void>::failure (
                        framework_error_kind_t::not_found,
                        "target Spot authority is unavailable");
                  }
                  if (authority_owner_generation == 0) {
                      return result_t<void>::failure (
                        framework_error_kind_t::protocol_error,
                        "Actor authority owner generation is invalid");
                  }
                  (void) native->create_reserved_actor (
                    request.actor_type,
                    runtime::stateful::object_ref_t{
                      runtime::stateful::object_kind_t::actor,
                      request.actor_id,
                      request.actor_generation,
                      authority_owner_generation,
                      target_spot_object->mesh_name,
                      native->status ().routing_id ().to_string ()});
              }
              catch (const std::exception &error) {
                  return result_t<void>::failure (
                    framework_error_kind_t::internal_failure, error.what ());
              }
              const auto core_prepared = native->prepare_actor_transfer (
                transfer_prepare, transfer_token, transfer_result);
              if (!core_prepared) {
                  return result_t<void>::failure (
                    framework_error_kind_t::internal_failure,
                    "target Framework Actor relocation prepare failed");
              }
              const auto next_membership_epoch =
                transfer_result.membership_epoch + 1;
              const auto core_committed =
                transfer_token.commit (next_membership_epoch);
              const auto core_activated =
                core_committed && transfer_token.activate ();
              if (!core_activated) {
                  return result_t<void>::failure (
                    framework_error_kind_t::internal_failure,
                    "target Framework Actor relocation activation failed");
              }
              runtime.record_core_actor_transfer_activation (
                request.actor_id, next_membership_epoch);
              return result_t<void>::success ();
          };
        if (request.finalize) {
            if (request.target_owner_lease_generation
                  > static_cast<std::uint64_t> (
                    std::numeric_limits<std::int64_t>::max ())) {
                complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "remote Actor target owner lease generation is invalid"));
                return;
            }
            const auto backlog_staged =
              runtime.stage_remote_actor_commit_backlog (
                request.transfer_id, std::move (handoff_backlog));
            if (!backlog_staged) {
                complete (detail::propagate_failure<zlink::message_t> (
                  backlog_staged,
                  "remote Actor handoff backlog staging failed"));
                return;
            }
            const auto authority_committed =
              runtime.commit_remote_actor_authority (
                request.transfer_id,
                actor_ref,
                spot_id_t (request.target_spot_id),
                request.target_spot_generation,
                request.actor_authority_owner_generation,
                request.source_mesh_name,
                request.target_mesh_name,
                request.target_node_lifecycle_generation,
                location_owner_token_t{
                  request.target_owner_id,
                  static_cast<std::int64_t> (
                    request.target_owner_lease_generation)},
                &committed_authority_owner_generation);
            if (!authority_committed) {
                complete (detail::propagate_failure<zlink::message_t> (
                  authority_committed,
                  "remote Actor authority commit failed"));
                return;
            }
            if (session_relocation_route
                && !runtime.commit_session_relocation_route_authority (
                  request.transfer_id,
                  committed_authority_owner_generation)) {
                complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::unavailable,
                  "committed Session relocation authority was not retained"));
                return;
            }
            if (request.core_transfer) {
                const auto core_activated = activate_core_transfer (
                  committed_authority_owner_generation);
                if (!core_activated) {
                    complete (detail::propagate_failure<zlink::message_t> (
                      core_activated,
                      "target Framework Actor relocation activation failed"));
                    return;
                }
            }
        }

        if (request.finalize && session_relocation_route) {
            const auto &route = *session_relocation_route;
            const auto session_owner_is_relocation_target =
              route.session_owner_node_routing_id
              == route.route.target_node_routing_id;
            if (!session_owner_is_relocation_target) {
                const auto staged_actor =
                  ::zlink::framework::detail::actor_ref_access_t::make (
                    runtime.node_rid (), request.actor_type,
                    request.actor_id, request.actor_generation);
                const auto staged =
                  actor_gateway.record_bound_session_route (
                    staged_actor,
                    zlink::routing_id_t::from (
                      route.session_owner_node_routing_id),
                    zlink::routing_id_t::from (
                      route.session_routing_id),
                    route.session_owner_node_generation,
                    committed_authority_owner_generation,
                    request.target_owner_lease_generation,
                    route.binding_generation, 0,
                    0);
                if (!staged) {
                    complete (detail::propagate_failure<
                      zlink::message_t> (
                      staged,
                      "target bound Session route staging failed"));
                    return;
                }
            }
        }

        const bool prepare = request.prepare;
        const auto transfer_id = request.transfer_id;
        const auto bound_session_node_rid = request.bound_session_node_rid;
        const auto bound_session_rid = request.bound_session_rid;
        const auto target_owner_lease_generation =
          request.target_owner_lease_generation;
        const auto runtime_owner = runtime.weak_state ();
        const auto actor_gateway_owner = actor_gateway.weak_state ();
        auto route_client = _route_client;
        auto *serializers = _serializers;
        std::function<task_t<void> ()> submit_source_leave;
        if (request.finalize) {
            const auto source_node_rid = received.source_node_rid;
            const auto source_spot_id = request.source_spot_id;
            const auto source_spot_generation =
              request.source_spot_generation;
            const auto target_spot_id = request.target_spot_id;
            const auto leave_command = spot_actor_leave_route_command_t{
              .transfer_id = request.transfer_id,
              .actor_node_rid = request.actor_node_rid,
              .actor_type = request.actor_type,
              .actor_id = request.actor_id,
              .actor_generation = request.actor_generation,
              .source_spot_id = request.source_spot_id,
              .source_spot_generation = request.source_spot_generation,
              .target_spot_id = request.target_spot_id,
              .target_node_rid = std::string (runtime.node_rid ().value ()),
              .target_node_generation =
                request.target_node_lifecycle_generation,
              .target_authority_owner_generation =
                committed_authority_owner_generation,
              .target_owner_lease_generation =
                request.target_owner_lease_generation};
            std::optional<runtime::messaging::message_parts_t> leave_parts;
            if (serializers != nullptr) {
                runtime::messaging::envelope_header_t leave_header;
                leave_header.kind =
                  runtime::messaging::message_kind_t::command;
                leave_header.channel_name = "spot";
                leave_header.message_name =
                  spot_actor_leave_route_command_t::packet_name;
                leave_parts.emplace (
                  runtime::messaging::envelope_codec_t{}.encode_parts (
                    leave_header, leave_command, *serializers));
            }
            submit_source_leave =
              [runtime, source_node_rid, source_spot_id,
               source_spot_generation, target_spot_id,
               leave_parts = std::move (leave_parts)] () mutable {
                  return submit_source_actor_leave_async (
                    runtime, source_node_rid, source_spot_id,
                    source_spot_generation, target_spot_id,
                    std::move (leave_parts));
              };
        }
        auto complete_committed =
          [runtime_owner, actor_gateway_owner,
           route_client = std::move (route_client), serializers,
           session_relocation_route,
           committed_authority_owner_generation, prepare,
           transfer_id, bound_session_node_rid, bound_session_rid,
           target_owner_lease_generation, channel_name = header.channel_name,
           complete] (result_t<actor_join_reply_t> committed) mutable {
              try {
                  if (!committed) {
                      complete (detail::propagate_failure<zlink::message_t> (
                        committed, "remote actor commit failed"));
                      return;
                  }
                  auto runtime_state = runtime_owner.lock ();
                  auto gateway_state = actor_gateway_owner.lock ();
                  if (!runtime_state || !gateway_state || serializers == nullptr) {
                      complete (result_t<zlink::message_t>::failure (
                        framework_error_kind_t::shutting_down,
                        "remote Actor commit owner was released before reply publication"));
                      return;
                  }
                  spot_node_runtime_t runtime (std::move (runtime_state));
                  actor_gateway_runtime_t actor_gateway (
                    std::move (gateway_state));
                  spot_route_internal_dispatcher_t self (
                    runtime, actor_gateway, route_client, *serializers);
                  if (!prepare) {
                      const auto session_owner_is_relocation_target =
                        session_relocation_route
                        && session_relocation_route
                             ->session_owner_node_routing_id
                             == session_relocation_route
                                  ->route.target_node_routing_id;
                      if (!session_owner_is_relocation_target) {
                          const auto actor_ref_updated =
                            actor_gateway.update_actor_ref (
                              committed.value ().actor);
                          if (!actor_ref_updated) {
                              complete (result_t<zlink::message_t>::failure (
                                actor_ref_updated.error_kind (),
                                actor_ref_updated.error ()
                                  ? actor_ref_updated.error ()->what ()
                                  : "remote actor ref update failed"));
                              return;
                          }
                          if (!bound_session_node_rid.empty ()) {
                              auto bound = self.bind_actor_session_route (
                                actor_gateway, committed.value ().actor,
                                self._runtime.actor_route_transport_name ()
                                  .value_or (channel_name),
                                zlink::routing_id_t::from (
                                  bound_session_node_rid),
                                bound_session_rid.empty ()
                                  ? std::nullopt
                                  : std::make_optional (
                                      zlink::routing_id_t::from (
                                        bound_session_rid)),
                                true);
                              if (!bound) {
                                  complete (detail::propagate_failure<
                                    zlink::message_t> (
                                    bound,
                                    "actor bound session route binding failed"));
                                  return;
                              }
                          }
                      }
                      if (session_relocation_route
                          && !session_owner_is_relocation_target) {
                          const auto &route = *session_relocation_route;
                          const auto target_authority_owner_generation =
                            committed_authority_owner_generation != 0
                              ? committed_authority_owner_generation
                              : route.route
                                  .target_authority_owner_generation;
                          const auto recorded =
                            actor_gateway.record_bound_session_route (
                              committed.value ().actor,
                              zlink::routing_id_t::from (
                                route.session_owner_node_routing_id),
                              zlink::routing_id_t::from (
                                route.session_routing_id),
                              route.session_owner_node_generation,
                              target_authority_owner_generation,
                              target_owner_lease_generation,
                              route.binding_generation, 0,
                              0);
                          if (!recorded) {
                              complete (detail::propagate_failure<
                                zlink::message_t> (
                                recorded,
                                "target bound Session route publication failed"));
                              return;
                          }
                      }
                  }
                  if (!prepare && session_relocation_route) {
                      auto activation = std::make_shared<task_t<bool>> (
                        runtime.activate_session_relocation_route (transfer_id));
                      const auto reply = committed.value ();
                      ::zlink::framework::detail::observe_task_completion (
                        *activation,
                        [activation, reply, serializers, complete] (
                          const result_t<bool> &activated) mutable {
                            if (!activated || !activated.value ()) {
                                complete (result_t<zlink::message_t>::failure (
                                  framework_error_kind_t::unavailable,
                                  "Session relocation route was not activated"));
                                return;
                            }
                            complete (result_t<zlink::message_t>::success (
                              detail::encoded_payload_to_raw (
                                serializers->get<spot_actor_join_route_reply_t> ()
                                  .serialize (make_spot_actor_join_route_reply (reply)))));
                        });
                      return;
                  }
                  complete (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (
                      self._serializers
                        ->get<spot_actor_join_route_reply_t> ()
                        .serialize (make_spot_actor_join_route_reply (
                          committed.value ())))));
              }
              catch (const framework_exception_t &error) {
                  complete (
                    detail::result_access_t::failure<zlink::message_t> (
                      error));
              }
              catch (const std::exception &error) {
                  complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::protocol_error,
                    std::string ("SPOT route request decode failed: ")
                      + error.what ()));
              }
          };

        if (request.finalize) {
            runtime.finalize_remote_actor_to_spot_async (
              request.transfer_id, actor_ref,
              spot_id_t (request.target_spot_id),
              services, &actor_gateway,
              request.finalize_timeout_ms == 0
                ? std::nullopt
                : std::make_optional (
                    std::chrono::steady_clock::now ()
                    + std::chrono::milliseconds (
                      request.finalize_timeout_ms)),
              std::move (complete_committed),
              std::move (submit_source_leave));
            return;
        }

        if (!request.prepare) {
            auto prepared = runtime.prepare_remote_actor_to_spot (
              request.transfer_id, actor_ref,
              spot_id_t (request.target_spot_id),
              zlink::message_t::from (request.transfer_state),
              actor_gateway.actor_context (actor_ref), true);
            if (!prepared) {
                complete_committed (std::move (prepared));
                return;
            }
            const auto backlog_staged =
              runtime.stage_remote_actor_commit_backlog (
                request.transfer_id, std::move (handoff_backlog));
            if (!backlog_staged) {
                complete_committed (
                  detail::propagate_failure<actor_join_reply_t> (
                    backlog_staged,
                    "remote Actor handoff backlog staging failed"));
                return;
            }
            runtime.finalize_remote_actor_to_spot_async (
              request.transfer_id, actor_ref,
              spot_id_t (request.target_spot_id),
              services, &actor_gateway,
              request.finalize_timeout_ms == 0
                ? std::nullopt
                : std::make_optional (
                    std::chrono::steady_clock::now ()
                    + std::chrono::milliseconds (
                      request.finalize_timeout_ms)),
              std::move (complete_committed));
            return;
        }

        auto committed =
          request.prepare
            ? runtime.prepare_remote_actor_to_spot (
                request.transfer_id, actor_ref,
                spot_id_t (request.target_spot_id),
                zlink::message_t::from (request.transfer_state),
                actor_gateway.actor_context (actor_ref), true)
            : result_t<actor_join_reply_t>::failure (
                framework_error_kind_t::protocol_error,
                "remote Actor commit shape is invalid");
        complete_committed (std::move (committed));
    }
    catch (const framework_exception_t &error) {
        complete (detail::result_access_t::failure<zlink::message_t> (error));
    }
    catch (const std::exception &error) {
        complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error,
          std::string ("SPOT route request decode failed: ") + error.what ()));
    }
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
            auto runtime = _runtime;
            auto admitted = runtime.admit_remote_actor_to_spot (
              request.transfer_id, actor_ref_from_spot_route (request),
              spot_id_t (request.source_spot_id),
              spot_id_t (request.target_spot_id),
              zlink::message_t::from (request.payload),
              request.completion_operation_id_high,
              request.completion_operation_id_low,
              request.actor_authority_owner_generation);
            if (!admitted) {
                return detail::propagate_failure<zlink::message_t> (admitted, "remote actor admission failed");
            }
            const auto reply = spot_actor_admission_route_reply_t{
              .accepted = admitted.value ().accepted,
              .payload =
                admitted.value ().reply
                  ? detail::message_to_raw (*admitted.value ().reply, *_serializers).to_bytes ()
                  : std::vector<std::uint8_t>{},
              .completion_root_reference = {},
              .completion_root_checksum = 0};
            return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
              _serializers->get<spot_actor_admission_route_reply_t> ().serialize (reply)));
        }
        if (header.message_name == spot_actor_commit_route_request_t::packet_name) {
            auto request =
              _serializers->get<spot_actor_commit_route_request_t> ().deserialize (
                detail::encoded_payload_from_raw (body.value ()));
            detail::task_completion_source_t<zlink::message_t> completion;
            auto result = completion.task ();
            dispatch_actor_commit_request (
              std::move (request), received, header, services,
              [completion] (result_t<zlink::message_t> value) mutable {
                  completion.complete (std::move (value));
              });
            return result.result ();
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
            return result_t<zlink::message_t>::failure (
              framework_error_kind_t::unavailable,
              "actor packet route requires asynchronous terminal dispatch");
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
