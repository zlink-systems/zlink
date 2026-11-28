/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/spots/instance_spot_application.hpp"
#include "runtime/streams/admission_queue_messages.hpp"
#include "runtime/utils/weak_runtime_owner.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <type_traits>
#include <typeindex>
#include <variant>

namespace zlink::framework::detail
{

namespace
{

struct instance_spot_activation_trace_context_t
{
    std::string packet_name;
    std::string mesh_name;
    zlink::routing_id_t target_node;
    zlink::framework::spot_id_t spot_id;
    std::string instance_spot_type;
    std::string correlation_id;
    std::uint64_t source_node_generation = 0;
};

std::optional<instance_spot_activation_trace_context_t>
make_instance_spot_activation_trace_context (
  bool may_emit,
  std::string_view packet_name,
  std::string_view mesh_name,
  const zlink::routing_id_t &target_node,
  const zlink::framework::spot_id_t &spot_id,
  const zlink::framework::runtime::protocol::instance_spot_activation_header_t &header)
{
    if (!may_emit)
        return std::nullopt;
    return instance_spot_activation_trace_context_t{std::string (packet_name),
                                                    std::string (mesh_name),
                                                    target_node,
                                                    spot_id,
                                                    header.target.stable_type,
                                                    std::to_string (header.operation.high) + ":"
                                                      + std::to_string (header.operation.low),
                                                    header.source_node_generation};
}

void trace_instance_spot_activation (
  const zlink::framework::dispatch_options_t &dispatch,
  const std::optional<zlink::framework::runtime::flow_value_t> &flow,
  zlink::framework::message_flow_outcome_t outcome,
  zlink::framework::dispatch_message_kind_t message_kind,
  const instance_spot_activation_trace_context_t &context,
  std::optional<zlink::framework::message_flow_result_t> result = std::nullopt,
  std::optional<zlink::framework::message_flow_reason_t> reason = std::nullopt,
  std::exception_ptr failure = {})
{
    if (zlink::framework::detail::message_flow_tracer_t (dispatch).enabled_for (outcome, result)) {
        zlink::framework::detail::message_flow_tracer_t (dispatch).trace (outcome, result, [&] {
            zlink::framework::message_flow_event_t event{
              outcome,
              zlink::framework::dispatch_error_surface_t::instance_spot,
              message_kind,
              context.packet_name,
              std::nullopt,
              std::nullopt,
              context.correlation_id,
              std::nullopt,
              std::string (context.spot_id),
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::exception_ptr{},
              std::nullopt,
              std::nullopt};
            if (flow && !flow->flow_id.empty ()) {
                event.flow_id = flow->flow_id;
                event.flow_origin = flow->origin;
            }
            event.mesh_name = context.mesh_name;
            event.target_rid = context.target_node.to_string ();
            event.instance_spot_type = context.instance_spot_type;
            event.activation_state =
              outcome == zlink::framework::message_flow_outcome_t::reply_received
                  && (!result || *result == zlink::framework::message_flow_result_t::succeeded)
                ? "ready"
                : "activating";
            event.source_mesh_generation = context.source_node_generation;
            if (result)
                event.result = *result;
            if (reason)
                event.reason = *reason;
            event.exception = failure;
            return event;
        });
    }
}

} // namespace

void configure_app_instance_spot (
  app_state_t *_state,
  zlink_framework_options_t &options,
  channel_runtime_t &channel_runtime,
  std::vector<std::shared_ptr<mesh_node_builder_state_t>> &mesh_node_registrations,
  std::vector<std::weak_ptr<mesh_node_runtime_t>> &weak_mesh_nodes)
{
    auto provider = _state->services.build_provider ();
    auto &location_resolvers = provider.get_required<runtime::store_location_resolvers_t> ();
    auto operation_sequence = std::make_shared<std::atomic<std::uint64_t>> (1);
    struct selected_instance_target_t
    {
        std::shared_ptr<detail::mesh_node_runtime_t> source;
        mesh_node_descriptor_t target;
        std::string stable_type;
    };
    auto select_instance_target = [weak_mesh_nodes, &location_resolvers] (
                                    spot_id_t spot_id, detail::spot_activation_intent_t intent,
                                    std::optional<runtime::spot_address_t> cached_route = {})
      -> task_t<result_t<selected_instance_target_t>> {
        if (cached_route && cached_route->authority_owner_generation != 0) {
            const auto source = std::find_if (
              weak_mesh_nodes.begin (), weak_mesh_nodes.end (), [&] (const auto &mesh) {
                  return detail::lock_runtime_owner (mesh)->mesh_name () == cached_route->mesh_name;
              });
            if (source == weak_mesh_nodes.end ())
                co_return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Ready Instance Spot source Mesh is not configured");
            mesh_node_descriptor_t target;
            target.rid = cached_route->node_rid;
            target.lifecycle_generation = cached_route->node_generation;
            target.mesh_name = cached_route->mesh_name;
            co_return result_t<selected_instance_target_t>::success (
              {detail::lock_runtime_owner (*source), std::move (target), {}});
        }
        if (cached_route && !cached_route->stable_type.empty ()) {
            if (intent.stable_type && *intent.stable_type != cached_route->stable_type)
                co_return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::type_mismatch, "Instance Spot type does not match");
            intent.stable_type = cached_route->stable_type;
            intent.mesh_name = cached_route->mesh_name;
        }
        std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> sources;
        for (const auto &weak_mesh : weak_mesh_nodes) {
            const auto mesh = detail::lock_runtime_owner (weak_mesh);
            if (!intent.mesh_name || mesh->mesh_name () == *intent.mesh_name)
                sources.push_back (mesh);
        }
        if (sources.empty ())
            co_return result_t<selected_instance_target_t>::failure (
              intent.mesh_name ? framework_error_kind_t::not_found
                               : framework_error_kind_t::not_configured,
              "No Instance Spot source Mesh is configured");
        if (!intent.mesh_name && sources.size () != 1)
            co_return result_t<selected_instance_target_t>::failure (
              framework_error_kind_t::invalid_operation,
              "More than one object Mesh is configured; select one with in_mesh");
        const auto source = sources.front ();
        std::vector<const mesh_node_descriptor_t *> candidates;
        std::vector<mesh_node_descriptor_t> visible_targets;
        auto listed = co_await runtime::await_result (
          location_resolvers.list_live_mesh_nodes (source->mesh_name ()));
        if (!listed.has_value ())
            co_return detail::propagate_failure<selected_instance_target_t> (
              listed, "Instance Spot target lookup failed");
        for (auto &descriptor : listed.value ()) {
            if (descriptor.state != framework_runtime_state_t::serving
                || descriptor.object_role != object_role_t::server)
                continue;
            visible_targets.push_back (std::move (descriptor));
        }
        std::set<std::string> stable_types;
        for (const auto &candidate : visible_targets)
            for (const auto &capability : candidate.object_capabilities)
                if (capability.object_kind == placement_object_kind_t::instance_spot
                    && (!intent.stable_type || capability.stable_type == *intent.stable_type))
                    stable_types.insert (capability.stable_type);
        if (stable_types.empty ())
            co_return result_t<selected_instance_target_t>::failure (
              framework_error_kind_t::not_found, "No Instance Spot target provides the type");
        if (!intent.stable_type && stable_types.size () != 1)
            co_return result_t<selected_instance_target_t>::failure (
              framework_error_kind_t::invalid_operation,
              "Instance Spot stable type is required when the Mesh publishes multiple types");
        const auto stable_type = intent.stable_type ? *intent.stable_type : *stable_types.begin ();
        for (const auto &candidate : visible_targets) {
            if (candidate.placement_weight <= 0)
                continue;
            const auto capable = std::any_of (
              candidate.object_capabilities.begin (), candidate.object_capabilities.end (),
              [&] (const auto &capability) {
                  return capability.object_kind == placement_object_kind_t::instance_spot
                         && capability.stable_type == stable_type;
              });
            const auto typed = std::find_if (
              candidate.capacity.spot_types.begin (), candidate.capacity.spot_types.end (),
              [&] (const auto &capacity) {
                  return capacity.object_kind == placement_object_kind_t::instance_spot
                         && capacity.stable_type == stable_type;
              });
            const auto &spots = candidate.capacity.spots;
            const auto spots_full =
              spots.limit > 0
              && spots.active + spots.reserved >= static_cast<std::uint64_t> (spots.limit);
            const auto type_full = typed != candidate.capacity.spot_types.end ()
                                   && typed->usage.limit > 0
                                   && typed->usage.active + typed->usage.reserved
                                        >= static_cast<std::uint64_t> (typed->usage.limit);
            if (capable && !spots_full && !type_full)
                candidates.push_back (&candidate);
        }
        if (candidates.empty ())
            co_return result_t<selected_instance_target_t>::failure (
              framework_error_kind_t::unavailable, "No eligible Instance Spot target has capacity");
        const auto index = std::hash<std::string>{}(std::string (spot_id)) % candidates.size ();
        co_return result_t<selected_instance_target_t>::success (
          {source, *candidates[index], stable_type});
    };
    for (const auto &registration : mesh_node_registrations) {
        if (!registration->spot_state)
            continue;
        registration->spot_state->select_instance_spot_target =
          [select_instance_target] (runtime::protocol::instance_spot_activation_header_t original)
          -> task_t<result_t<runtime::protocol::instance_spot_activation_header_t>> {
            const detail::spot_activation_intent_t intent{.instance = true,
                                                          .stable_type =
                                                            original.target.stable_type,
                                                          .mesh_name = original.target.mesh_name};
            const auto selected =
              co_await select_instance_target (spot_id_t (original.target.spot_id), intent);
            if (!selected)
                co_return detail::propagate_failure<
                  runtime::protocol::instance_spot_activation_header_t> (
                  selected, "Instance Spot target selection failed");
            auto header = original;
            header.target.target_node_routing_id = selected.value ().target.rid.to_bytes ();
            header.target.target_node_generation = selected.value ().target.lifecycle_generation;
            header.target.mesh_name = selected.value ().target.mesh_name;
            header.target.stable_type = selected.value ().stable_type;
            header.target.descriptor_version =
              std::to_string (selected.value ().target.descriptor_revision);
            co_return result_t<runtime::protocol::instance_spot_activation_header_t>::success (
              std::move (header));
        };
    }
    auto make_activation = [operation_sequence] (
                             const selected_instance_target_t &selected, const spot_id_t &spot_id,
                             bool request, bool has_metadata,
                             std::chrono::system_clock::time_point activation_deadline,
                             const detail::spot_activation_intent_t &intent,
                             const std::optional<runtime::spot_address_t> &cached_route) {
        const auto source_status = selected.source->status ();
        const auto operation = operation_sequence->fetch_add (1, std::memory_order_relaxed);
        auto operation_scope = static_cast<std::uint64_t> (
                                 std::hash<std::string>{}(source_status.routing_id ().to_string ()))
                               ^ source_status.lifecycle_generation ();
        if (operation_scope == 0)
            operation_scope = source_status.lifecycle_generation () != 0
                                ? source_status.lifecycle_generation ()
                                : 1;
        auto header = runtime::protocol::instance_spot_activation_header_t{
          {selected.target.rid.to_bytes (), selected.target.lifecycle_generation,
           std::string (spot_id), selected.target.mesh_name, selected.stable_type,
           std::to_string (selected.target.descriptor_revision),
           static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                         activation_deadline.time_since_epoch ())
                                         .count ())},
          source_status.lifecycle_generation (),
          source_status.routing_id ().to_bytes (),
          std::nullopt,
          request,
          {operation_scope, operation},
          0,
          has_metadata};
        if (cached_route && cached_route->authority_owner_generation != 0) {
            header.target.object_generation = cached_route->object_generation;
            header.target.authority_owner_generation = cached_route->authority_owner_generation;
            header.target.owner_id = cached_route->owner.owner_id;
            header.target.owner_lease_generation = cached_route->owner.lease_generation;
            header.target.store_version = cached_route->store_version;
            header.target.instance_intent = intent.instance;
            header.target.deadline_unix_ms = 0;
        }
        return header;
    };
    struct request_instance_activation_t
    {
        static request_task_t<zlink::message_t>
        run (std::decay_t<decltype (select_instance_target)> select_instance_target,
             std::decay_t<decltype (make_activation)> make_activation,
             std::decay_t<decltype (&_state->serializers)> serializers,
             std::decay_t<decltype (options.dispatch_options ())> dispatch,
             spot_id_t spot_id,
             detail::spot_activation_intent_t intent,
             std::optional<runtime::spot_address_t> cached_route,
             std::string packet_name,

             std::function<serialized_payload_t (serializer_registry_t &)> encode_payload,
             std::chrono::milliseconds timeout,
             std::map<std::string, std::string> metadata)
        {
            auto flow_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, detail::message_flow_tracer_t (dispatch).mode ());
            const auto flow = runtime::flow_context_t::current ();
            auto selected = co_await select_instance_target (spot_id, intent, cached_route);
            if (!selected)
                co_return detail::propagate_failure<zlink::message_t> (
                  selected, "Instance Spot target selection failed");
            auto metadata_frame = detail::mesh_metadata_codec_t::encode (metadata);
            auto header =
              make_activation (selected.value (), spot_id, true, !metadata_frame.empty (),
                               std::chrono::system_clock::now () + timeout, intent, cached_route);
            auto serialized = encode_payload (*serializers);
            auto application_payload = runtime::messaging::encode_instance_spot_application (
              {.kind = runtime::messaging::message_kind_t::request,
               .message_name = packet_name,
               .content_type = serialized.content_type},
              detail::encoded_payload_to_raw (std::move (serialized.payload)));
            const auto trace_context = make_instance_spot_activation_trace_context (
              detail::message_flow_tracer_t (dispatch).enabled_for (
                message_flow_outcome_t::reply_received, message_flow_result_t::failed),
              packet_name, selected.value ().target.mesh_name, selected.value ().target.rid,
              spot_id, header);
            if (trace_context) {
                trace_instance_spot_activation (dispatch, flow, message_flow_outcome_t::sent,
                                                dispatch_message_kind_t::request, *trace_context);
            }
            auto completion = std::make_shared<task_completion_source_t<zlink::message_t>> ();
            auto output = completion->task ();
            // Submission owns source admission, including when a reply arrives first.
            const auto submitted =
              co_await selected.value ().source->activate_instance_spot_remote (
                selected.value ().target.rid, std::move (header),
                metadata_frame.empty () ? std::optional<std::vector<std::uint8_t>>{}
                                        : std::make_optional (std::move (metadata_frame)),
                std::move (application_payload), timeout,
                [completion, dispatch, flow, trace_context,
                 expected_reply = runtime::admission_queue::reply_packet_name (packet_name)] (
                  runtime::foundation::operation_terminal_t terminal,
                  runtime::protocol::reply_header_t reply,
                  std::optional<runtime::protocol::application_payload_t> application_reply) {
                    if (terminal != runtime::foundation::operation_terminal_t::completed) {
                        auto failure = result_t<zlink::message_t>::failure (
                          framework_error_kind_t::unavailable,
                          "Instance Spot activation transport did not complete");
                        const auto error = failure.exception ();
                        if (completion->complete (std::move (failure)) && trace_context) {
                            trace_instance_spot_activation (
                              dispatch, flow, message_flow_outcome_t::reply_received,
                              dispatch_message_kind_t::request, *trace_context,
                              message_flow_result_t::failed, message_flow_reason_t::stale_target,
                              error);
                        }
                        return;
                    }
                    if (reply.terminal_result != 0) {
                        auto failure = detail::result_access_t::failure<zlink::message_t> (
                          runtime::messaging::request_failure_mapper_t{}.reply_header_exception (
                            reply.terminal_result, reply.failure_code,
                            "Instance Spot activation request"));
                        const auto error = failure.exception ();
                        if (completion->complete (std::move (failure)) && trace_context) {
                            trace_instance_spot_activation (
                              dispatch, flow, message_flow_outcome_t::reply_received,
                              dispatch_message_kind_t::request, *trace_context,
                              message_flow_result_t::failed,
                              message_flow_reason_t::activation_rejected, error);
                        }
                        return;
                    }
                    auto decoded =
                      application_reply
                        ? runtime::messaging::decode_instance_spot_application (
                            *application_reply, runtime::messaging::message_kind_t::response)
                        : result_t<runtime::messaging::instance_spot_application_t>::failure (
                            framework_error_kind_t::protocol_error,
                            "Instance Spot activation reply is missing");
                    if (!decoded
                        || (expected_reply
                            && decoded.value ().header.message_name != *expected_reply)) {
                        auto failure = result_t<zlink::message_t>::failure (
                          framework_error_kind_t::protocol_error,
                          "Invalid Instance Spot activation reply envelope");
                        const auto error = failure.exception ();
                        if (completion->complete (std::move (failure)) && trace_context) {
                            trace_instance_spot_activation (
                              dispatch, flow, message_flow_outcome_t::reply_received,
                              dispatch_message_kind_t::request, *trace_context,
                              message_flow_result_t::failed,
                              message_flow_reason_t::activation_rejected, error);
                        }
                        return;
                    }
                    if (trace_context) {
                        trace_instance_spot_activation (
                          dispatch, flow, message_flow_outcome_t::reply_received,
                          dispatch_message_kind_t::response, *trace_context);
                    }
                    completion->complete (
                      result_t<zlink::message_t>::success (std::move (decoded.value ().body)));
                });
            if (!submitted) {
                auto failure = result_t<zlink::message_t>::failure (
                  framework_error_kind_t::unavailable, "Instance Spot activation was not admitted");
                const auto error = failure.exception ();
                if (completion->complete (std::move (failure)) && trace_context) {
                    trace_instance_spot_activation (
                      dispatch, flow, message_flow_outcome_t::reply_received,
                      dispatch_message_kind_t::request, *trace_context,
                      message_flow_result_t::failed, message_flow_reason_t::stale_target, error);
                }
            }
            if (submitted)
                co_await detail::request_admitted_t{};
            co_return co_await output;
        }
    };
    struct send_instance_activation_t
    {
        static task_t<result_t<void>>
        run (std::decay_t<decltype (select_instance_target)> select_instance_target,
             std::decay_t<decltype (make_activation)> make_activation,
             std::decay_t<decltype (&_state->serializers)> serializers,
             std::decay_t<decltype (options.dispatch_options ())> dispatch,
             spot_id_t spot_id,
             detail::spot_activation_intent_t intent,
             std::optional<runtime::spot_address_t> cached_route,
             std::string packet_name,
             std::function<serialized_payload_t (serializer_registry_t &)> encode_payload,
             std::map<std::string, std::string> metadata,
             std::chrono::system_clock::time_point started_at)
        {
            auto flow_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, detail::message_flow_tracer_t (dispatch).mode ());
            const auto flow = runtime::flow_context_t::current ();
            auto selected = co_await select_instance_target (spot_id, intent, cached_route);
            if (!selected)
                co_return detail::propagate_failure<void> (selected,
                                                           "Instance Spot target selection failed");
            auto metadata_frame = detail::mesh_metadata_codec_t::encode (metadata);
            auto header =
              make_activation (selected.value (), spot_id, false, !metadata_frame.empty (),
                               started_at + selected.value ().source->default_request_timeout (),
                               intent, cached_route);
            auto serialized = encode_payload (*serializers);
            auto application_payload = runtime::messaging::encode_instance_spot_application (
              {.kind = runtime::messaging::message_kind_t::command,
               .message_name = packet_name,
               .content_type = serialized.content_type},
              detail::encoded_payload_to_raw (std::move (serialized.payload)));
            const auto trace_context = make_instance_spot_activation_trace_context (
              detail::message_flow_tracer_t (dispatch).enabled_for (
                message_flow_outcome_t::dropped),
              packet_name, selected.value ().target.mesh_name, selected.value ().target.rid,
              spot_id, header);
            if (trace_context) {
                trace_instance_spot_activation (dispatch, flow, message_flow_outcome_t::sent,
                                                dispatch_message_kind_t::send, *trace_context);
            }
            const auto submitted =
              co_await selected.value ().source->send_instance_spot_activation_remote (
                selected.value ().target.rid, std::move (header),
                metadata_frame.empty () ? std::optional<std::vector<std::uint8_t>>{}
                                        : std::make_optional (std::move (metadata_frame)),
                std::move (application_payload));
            if (submitted)
                co_return result_t<void>::success ();
            auto failure = result_t<void>::failure (framework_error_kind_t::unavailable,
                                                    "Instance Spot activation was not admitted");
            if (trace_context) {
                trace_instance_spot_activation (
                  dispatch, flow, message_flow_outcome_t::dropped, dispatch_message_kind_t::send,
                  *trace_context, message_flow_result_t::failed,
                  message_flow_reason_t::stale_target, failure.exception ());
            }
            co_return failure;
        }
    };
    channel_runtime.bind_instance_spot_activator (
      [select_instance_target, make_activation, serializers = &_state->serializers,
       dispatch = options.dispatch_options ()] (
        const spot_id_t &spot_id, const detail::spot_activation_intent_t &intent,
        const std::optional<runtime::spot_address_t> &cached_route, const std::string &packet_name,
        std::type_index,
        std::function<serialized_payload_t (serializer_registry_t &)> encode_payload,
        const std::map<std::string, std::string> &metadata,
        std::chrono::system_clock::time_point started_at) -> task_t<result_t<void>> {
          return send_instance_activation_t::run (
            select_instance_target, make_activation, serializers, dispatch, spot_id, intent,
            cached_route, packet_name, std::move (encode_payload), metadata, started_at);
      },
      [select_instance_target, make_activation, serializers = &_state->serializers,
       dispatch = options.dispatch_options ()] (
        const spot_id_t &spot_id, const detail::spot_activation_intent_t &intent,
        const std::optional<runtime::spot_address_t> &cached_route, std::string packet_name,
        std::type_index,
        std::function<serialized_payload_t (serializer_registry_t &)> encode_payload,
        std::chrono::milliseconds timeout,
        std::map<std::string, std::string> metadata) -> request_task_t<zlink::message_t> {
          return request_instance_activation_t::run (
            select_instance_target, make_activation, serializers, dispatch, spot_id, intent,
            cached_route, std::move (packet_name), std::move (encode_payload), timeout,
            std::move (metadata));
      });
}

} // namespace zlink::framework::detail
