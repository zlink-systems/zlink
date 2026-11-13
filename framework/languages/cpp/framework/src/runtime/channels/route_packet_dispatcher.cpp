/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/handlers/handler_registry_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/execution/task_result.hpp"

#include <exception>
#include <optional>
#include <utility>
#include <memory>

namespace zlink::framework::detail
{

namespace
{

/// RouteMesh node-direct dispatch has no ChannelName, so the router channel identity is carried in
/// the Mesh name field exactly like the .NET reference invoker does.
framework::route_message_context_t
make_route_message_context (std::string_view router_channel_id,
                            zlink::routing_id_t source_node_rid,
                            runtime::messaging::envelope_header_view_t &incoming_header,
                            std::chrono::system_clock::time_point received_at)
{
    const auto &header = *incoming_header;
    framework::message_context_t base{.received_at = received_at};
    base.mesh_name = std::string (router_channel_id);
    base.packet_name = header.message_name;
    base.content_type = header.content_type;
    base.metadata = framework::message_metadata_t (incoming_header.take_metadata ());
    if (!header.correlation_id.empty ()) {
        base.correlation_id = header.correlation_id;
    }
    return framework::route_message_context_t{std::move (base), source_node_rid};
}

const handler_registry_t &empty_handler_filters ()
{
    static const handler_registry_t filters;
    return filters;
}

void trace_route_flow (const dispatch_options_t &options,
                       std::string_view router,
                       message_flow_outcome_t outcome,
                       dispatch_message_kind_t kind,
                       const route_received_packet_t &received,
                       const runtime::messaging::envelope_header_t &header)
{
    message_flow_tracer_t (options).trace (
      outcome, header.flow_id ? std::string_view (*header.flow_id) : std::string_view{}, [&] {
          message_flow_event_t event{outcome,
                                     dispatch_error_surface_t::route_mesh_channel,
                                     kind,
                                     header.message_name,
                                     std::string (router),
                                     header.topic,
                                     header.correlation_id,
                                     received.source_node_rid.to_string (),
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt};
          event.flow_id = header.flow_id;
          event.flow_origin = header.flow_origin;
          event.mesh_name = std::string (router);
          event.channel_route_kind = "route_mesh";
          return event;
      });
}

result_t<std::optional<route_dispatch_reply_t>>
reply_route_error (const dispatch_options_t &options,
                   std::string_view router,
                   const route_reply_target_t &submit_reply,
                   const route_received_packet_t &received,
                   const runtime::messaging::envelope_header_t &header,
                   const framework_exception_t &error)
{
    dispatch_error_reporter_t (options).report_lazy ([&] {
        auto event = message_dispatch_error_event_t{dispatch_error_surface_t::route_mesh_channel,
                                                    dispatch_message_kind_t::request,
                                                    dispatch_reason_from_error (error.kind ()),
                                                    dispatch_error_action_t::reply_error,
                                                    header.message_name,
                                                    std::string (router),
                                                    header.topic,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    received.source_node_rid.to_string (),
                                                    header.correlation_id,
                                                    std::make_exception_ptr (error)};
        event.flow_id = header.flow_id;
        event.flow_origin = header.flow_origin;
        event.mesh_name = std::string (router);
        event.channel_route_kind = "route_mesh";
        return event;
    });
    channel_reply_writer_t writer;
    auto reply = writer.reply_raw_envelope (writer.create_error_header (router, header, error),
                                            zlink::message_t::from (""));
    route_dispatch_reply_t response{received.source_node_rid, received.request_seq,
                                    std::move (reply)};
    if (submit_reply) {
        auto submitted = submit_reply (std::move (response));
        if (!submitted)
            return propagate_failure<std::optional<route_dispatch_reply_t>> (
              submitted, "MeshNode error reply submit failed");
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    }
    return result_t<std::optional<route_dispatch_reply_t>>::success (std::move (response));
}

} // namespace

route_reply_target_t::operator bool () const noexcept
{
    if (const auto *callback = std::get_if<0> (&target))
        return static_cast<bool> (*callback);
    return true;
}

result_t<void> route_reply_target_t::operator() (route_dispatch_reply_t &&reply) const
{
    if (const auto *callback = std::get_if<0> (&target))
        return (*callback) (std::move (reply));
    if (runtime::host::reply (std::get<1> (target), std::move (reply.parts).take_items ())
        != zlink::submit_result_t::ok)
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "MeshNode reply submit failed");
    return result_t<void>::success ();
}

route_packet_dispatcher_t::route_packet_dispatcher_t (std::string router_channel_id) :
    _router_channel_id (std::move (router_channel_id))
{
}

route_packet_dispatcher_t::route_packet_dispatcher_t (
  std::string router_channel_id,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const route_handler_registry_t &handlers,
  const route_internal_packet_dispatcher_t &internal_packets,
  retained_dispatch_options_t dispatch_options,
  const handler_registry_t *filters,
  handler_dispatch_kind_t send_dispatch_kind,
  handler_dispatch_kind_t request_dispatch_kind,
  std::function<void ()> before_application_handler,
  std::shared_ptr<void> registration,
  std::function<result_t<void> (route_dispatch_reply_t &&)> submit_reply) :
    _router_channel_id (std::move (router_channel_id)),
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (filters),
    _internal_packets (&internal_packets),
    _dispatch_options (std::move (dispatch_options)),
    _send_dispatch_kind (send_dispatch_kind),
    _request_dispatch_kind (request_dispatch_kind),
    _before_application_handler (std::move (before_application_handler)),
    _registration (std::move (registration)),
    _submit_reply (std::move (submit_reply))
{
}

route_packet_dispatcher_t::route_packet_dispatcher_t (service_provider_t &services,
                                                      serializer_registry_t &serializers,
                                                      const route_handler_registry_t &handlers,
                                                      const handler_registry_t &filters,
                                                      retained_dispatch_options_t options,
                                                      std::function<void ()> before,
                                                      std::shared_ptr<void> registration) :
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (&filters),
    _dispatch_options (std::move (options)),
    _before_application_handler (std::move (before)),
    _registration (std::move (registration))
{
}

task_t<std::optional<route_dispatch_reply_t>> route_packet_dispatcher_t::dispatch (
  const runtime::host::receive_record_t &record,
  std::vector<zlink::message_t> parts,
  runtime::messaging::envelope_header_view_t header,
  runtime::application_dispatch_terminal_owner_t terminal,
  runtime::offload_executor_t *executor,
  const route_handler_registry_t::prepared_header_t *prepared) const
{
    if (!is_node_or_channel_record_kind (record.kind))
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::failure (
            framework_error_kind_t::protocol_error,
            "record kind is not node or ChannelName messaging"));
    if (!parts.empty ())
        header.retain (std::move (parts.front ()));
    const bool channel = record.kind == runtime::host::record_kind_t::channel_send
                         || record.kind == runtime::host::record_kind_t::channel_request;
    const binding_t binding{record.channel_name.empty () ? nullptr : &record.channel_name,
                            channel ? handler_dispatch_kind_t::channel_send
                                    : handler_dispatch_kind_t::node_direct_send,
                            channel ? handler_dispatch_kind_t::channel_request
                                    : handler_dispatch_kind_t::node_direct_request,
                            nullptr, route_reply_target_t{record.reply_token}};
    return dispatch (
      route_received_packet_t{record.source_node_rid,
                              {},
                              runtime::messaging::message_parts_t (std::move (parts)),
                              {},
                              record.received_at},
      std::move (header), std::move (terminal), executor, prepared, binding);
}

task_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch (const route_received_packet_t &received) const
{
    runtime::messaging::envelope_codec_t codec;
    auto header = codec.decode_header (received.parts,
                                       message_flow_tracer_t (_dispatch_options).flow_tracing ());
    if (!header) {
        return task_t<std::optional<route_dispatch_reply_t>> (
          detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
            header, "route envelope header decode failed"));
    }

    return dispatch (received, std::move (header.value ()));
}

task_t<std::optional<route_dispatch_reply_t>> route_packet_dispatcher_t::dispatch (
  const route_received_packet_t &received,
  runtime::messaging::envelope_header_view_t incoming_header,
  runtime::application_dispatch_terminal_owner_t terminal,
  runtime::offload_executor_t *executor,
  const route_handler_registry_t::prepared_header_t *prepared) const
{
    const binding_t binding{&_router_channel_id, _send_dispatch_kind, _request_dispatch_kind,
                            _internal_packets, route_reply_target_t{_submit_reply}};
    return dispatch (received, std::move (incoming_header), std::move (terminal), executor,
                     prepared, binding);
}

task_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch (const route_received_packet_t &received,
                                     runtime::messaging::envelope_header_view_t incoming_header,
                                     runtime::application_dispatch_terminal_owner_t terminal,
                                     runtime::offload_executor_t *executor,
                                     const route_handler_registry_t::prepared_header_t *prepared,
                                     const binding_t &binding) const
{
    const auto &header = *incoming_header;
    const auto router = binding.channel (header);
    auto flow_scope = runtime::flow_context_t::enter (
      header.flow_id, header.flow_origin, message_flow_tracer_t (_dispatch_options).mode (),
      flow_origin_t::inbound, std::nullopt);
    if (binding.send_kind != handler_dispatch_kind_t::channel_send
        && binding.request_kind != handler_dispatch_kind_t::channel_request) {
        if (message_flow_tracer_t (_dispatch_options)
              .enabled_for (message_flow_outcome_t::received)) {
            trace_route_flow (_dispatch_options, router, message_flow_outcome_t::received,
                              header.kind == runtime::messaging::message_kind_t::request
                                ? dispatch_message_kind_t::request
                                : dispatch_message_kind_t::send,
                              received, header);
        }
    }

    const auto *handler = prepared ? prepared->handler
                          : _handlers
                            ? _handlers->resolve (router, header.kind, header.message_name)
                            : nullptr;
    switch (header.kind) {
        case runtime::messaging::message_kind_t::command:
            return dispatch_send (received, std::move (incoming_header), std::move (terminal),
                                  executor, handler, binding);
        case runtime::messaging::message_kind_t::request:
            return dispatch_request (received, std::move (incoming_header), std::move (terminal),
                                     executor, handler, binding);
        default:
            return task_t<std::optional<route_dispatch_reply_t>> (
              result_t<std::optional<route_dispatch_reply_t>>::failure (
                framework_error_kind_t::protocol_error, "unsupported route message kind"));
    }
}


task_t<std::optional<route_dispatch_reply_t>> route_packet_dispatcher_t::dispatch_send (
  const route_received_packet_t &received,
  runtime::messaging::envelope_header_view_t incoming_header,
  runtime::application_dispatch_terminal_owner_t terminal,
  runtime::offload_executor_t *executor,
  const route_handler_registry_t::entry_t *handler,
  const binding_t &binding) const
{
    const auto &header = *incoming_header;
    const auto router = binding.channel (header);
    if (binding.internal != nullptr && binding.internal->can_handle_send (header.message_name)) {
        auto dispatched = binding.internal->dispatch_send (received, header, *_services);
        return runtime::continue_task<std::optional<route_dispatch_reply_t>> (
          std::move (dispatched),
          [received =
             route_received_packet_t{received.source_node_rid, {}, {}, {}, received.received_at},
           incoming_header = std::move (incoming_header), options = _dispatch_options,
           channel_id = std::string (router), terminal = std::move (terminal)] (
            result_t<void> dispatched) -> task_t<std::optional<route_dispatch_reply_t>> {
              const auto &header = *incoming_header;
              if (!dispatched)
                  co_return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
                    dispatched, "route internal send failed");
              if (message_flow_tracer_t (options).enabled_for (
                    message_flow_outcome_t::dispatched)) {
                  trace_route_flow (options, channel_id, message_flow_outcome_t::dispatched,
                                    dispatch_message_kind_t::send, received, header);
              }
              if (message_flow_tracer_t (options).enabled_for (message_flow_outcome_t::completed)) {
                  trace_route_flow (options, channel_id, message_flow_outcome_t::completed,
                                    dispatch_message_kind_t::send, received, header);
              }
              co_return std::nullopt;
          });
    }
    if (handler == nullptr || _services == nullptr || _serializers == nullptr) {
        dispatch_error_reporter_t (_dispatch_options).report_lazy ([&] {
            auto event =
              message_dispatch_error_event_t{dispatch_error_surface_t::route_mesh_channel,
                                             dispatch_message_kind_t::send,
                                             dispatch_error_reason_t::handler_missing,
                                             dispatch_error_action_t::drop,
                                             header.message_name,
                                             std::string (router),
                                             header.topic,
                                             std::nullopt,
                                             std::nullopt,
                                             received.source_node_rid.to_string (),
                                             header.correlation_id,
                                             std::exception_ptr{}};
            event.flow_id = header.flow_id;
            event.flow_origin = header.flow_origin;
            event.mesh_name = router;
            event.channel_route_kind = "route_mesh";
            return event;
        });
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt));
    }

    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        dispatch_error_reporter_t (_dispatch_options).report_lazy ([&] {
            auto event = message_dispatch_error_event_t{
              dispatch_error_surface_t::route_mesh_channel,
              dispatch_message_kind_t::send,
              dispatch_error_reason_t::payload_decode_failed,
              dispatch_error_action_t::drop,
              header.message_name,
              std::string (router),
              header.topic,
              std::nullopt,
              std::nullopt,
              received.source_node_rid.to_string (),
              header.correlation_id,
              body.error () ? std::make_exception_ptr (*body.error ()) : std::exception_ptr{}};
            event.flow_id = header.flow_id;
            event.flow_origin = header.flow_origin;
            event.mesh_name = router;
            event.channel_route_kind = "route_mesh";
            return event;
        });
        return task_t<std::optional<route_dispatch_reply_t>> (
          detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
            body, "route command body missing"));
    }
    auto context = make_route_message_context (router, received.source_node_rid, incoming_header,
                                               received.received_at);
    const auto &filters = _filters != nullptr ? *_filters : empty_handler_filters ();
    route_completion_t complete{
      _dispatch_options,
      _registration,
      {},
      route_received_packet_t{
        received.source_node_rid, received.request_seq, {}, received.source_session_rid},
      std::move (incoming_header)};
    return route_handler_invoker_t::invoke (*handler, filters, binding.send_kind, *_services,
                                            *_serializers, std::move (body.value ()),
                                            std::move (context), _before_application_handler,
                                            std::move (complete), std::move (terminal), executor);
}

task_t<std::optional<route_dispatch_reply_t>> route_packet_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  runtime::messaging::envelope_header_view_t incoming_header,
  runtime::application_dispatch_terminal_owner_t terminal,
  runtime::offload_executor_t *executor,
  const route_handler_registry_t::entry_t *handler,
  const binding_t &binding) const
{
    const auto &header = *incoming_header;
    const auto router = binding.channel (header);
    if (binding.internal != nullptr && binding.internal->can_handle_request (header.message_name)) {
        auto reply = binding.internal->dispatch_request (received, header, *_services);
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "route internal request failed");
            return task_t<std::optional<route_dispatch_reply_t>> (reply_route_error (
              _dispatch_options, router, binding.reply, received, header, error));
        }
        if (message_flow_tracer_t (_dispatch_options)
              .enabled_for (message_flow_outcome_t::replied)) {
            trace_route_flow (_dispatch_options, router, message_flow_outcome_t::replied,
                              dispatch_message_kind_t::response, received, header);
        }
        channel_reply_writer_t writer;
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::success (route_dispatch_reply_t{
            received.source_node_rid, received.request_seq,
            writer.reply_raw_envelope (
              writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                          std::string (router), header),
              reply.value ())}));
    }

    if (handler == nullptr || _services == nullptr || _serializers == nullptr) {
        framework_exception_t error (framework_error_kind_t::not_found,
                                     "No routed request handler is registered for '"
                                       + std::string (router) + ":" + header.message_name + "'.");
        return task_t<std::optional<route_dispatch_reply_t>> (
          reply_route_error (_dispatch_options, router, binding.reply, received, header, error));
    }

    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        framework_exception_t error (body.error_kind (), body.error ()
                                                           ? body.error ()->what ()
                                                           : "route request body missing");
        return task_t<std::optional<route_dispatch_reply_t>> (
          reply_route_error (_dispatch_options, router, binding.reply, received, header, error));
    }
    auto context = make_route_message_context (router, received.source_node_rid, incoming_header,
                                               received.received_at);
    const auto &filters = _filters != nullptr ? *_filters : empty_handler_filters ();
    route_completion_t complete{
      _dispatch_options, _registration, binding.reply,
      route_received_packet_t{
        received.source_node_rid, received.request_seq, {}, received.source_session_rid},
      std::move (incoming_header)};
    return route_handler_invoker_t::invoke (*handler, filters, binding.request_kind, *_services,
                                            *_serializers, std::move (body.value ()),
                                            std::move (context), _before_application_handler,
                                            std::move (complete), std::move (terminal), executor);
}

void route_completion_t::entered (const framework::route_message_context_t &context,
                                  handler_dispatch_kind_t kind) const
{
    if (message_flow_tracer_t (options).enabled_for (message_flow_outcome_t::dispatched))
        trace_route_flow (options, *context.mesh_name, message_flow_outcome_t::dispatched,
                          is_request_dispatch (kind) ? dispatch_message_kind_t::request
                                                     : dispatch_message_kind_t::send,
                          received, header);
}

result_t<std::optional<route_dispatch_reply_t>>
route_completion_t::operator() (const result_t<zlink::message_t> &reply,
                                const framework::route_message_context_t &context,
                                handler_dispatch_kind_t kind) const
{
    const auto &router = *context.mesh_name;
    const auto &header = *this->header;
    if (is_request_dispatch (kind)) {
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "routed request handler failed");
            return reply_route_error (options, *context.mesh_name, submit_reply, received, header,
                                      error);
        }
        if (message_flow_tracer_t (options).enabled_for (message_flow_outcome_t::replied)) {
            trace_route_flow (options, *context.mesh_name, message_flow_outcome_t::replied,
                              dispatch_message_kind_t::response, received, header);
        }
        channel_reply_writer_t writer;
        auto response = route_dispatch_reply_t{
          received.source_node_rid, received.request_seq,
          writer.reply_raw_envelope (
            writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                        *context.mesh_name, header),
            reply.value ())};
        if (submit_reply) {
            auto submitted = submit_reply (std::move (response));
            if (!submitted)
                return propagate_failure<std::optional<route_dispatch_reply_t>> (
                  submitted, "MeshNode reply submit failed");
            return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
        }
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::move (response));
    }


    if (!reply) {
        dispatch_error_reporter_t (options).report_lazy ([&] {
            auto event = message_dispatch_error_event_t{
              dispatch_error_surface_t::route_mesh_channel,
              dispatch_message_kind_t::send,
              dispatch_reason_from_error (reply.error_kind ()),
              dispatch_error_action_t::drop,
              header.message_name,
              router,
              header.topic,
              std::nullopt,
              std::nullopt,
              received.source_node_rid.to_string (),
              header.correlation_id,
              reply.error () ? std::make_exception_ptr (*reply.error ()) : std::exception_ptr{}};
            event.flow_id = header.flow_id;
            event.flow_origin = header.flow_origin;
            event.mesh_name = std::string (router);
            event.channel_route_kind = "route_mesh";
            return event;
        });
        return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
          reply, "routed send handler failed");
    }
    if (message_flow_tracer_t (options).enabled_for (message_flow_outcome_t::completed)) {
        trace_route_flow (options, router, message_flow_outcome_t::completed,
                          dispatch_message_kind_t::send, received, header);
    }
    return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
}


} // namespace zlink::framework::detail
