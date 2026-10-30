/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_packet_dispatcher.hpp"

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace zlink::framework::detail
{

namespace
{

/// RouteMesh node-direct dispatch has no ChannelName, so the router channel identity is carried in
/// the Mesh name field exactly like the .NET reference invoker does.
framework::route_message_context_t
make_route_message_context (const std::string &router_channel_id,
                            zlink::routing_id_t source_node_rid,
                            runtime::messaging::envelope_header_t &header)
{
    framework::message_context_t base;
    base.mesh_name = router_channel_id;
    base.packet_name = header.message_name;
    base.content_type = header.content_type;
    base.metadata = framework::message_metadata_t (std::move (header.metadata));
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
                       const std::string &router,
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
                                     router,
                                     header.topic,
                                     header.correlation_id,
                                     received.source_node_rid.to_string (),
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt};
          event.flow_id = header.flow_id;
          event.flow_origin = header.flow_origin;
          event.mesh_name = router;
          event.channel_route_kind = "route_mesh";
          return event;
      });
}

result_t<std::optional<route_dispatch_reply_t>> reply_route_error (
  const dispatch_options_t &options,
  const std::string &router,
  const std::function<result_t<void> (const route_dispatch_reply_t &)> &submit_reply,
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
                                                    router,
                                                    header.topic,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    received.source_node_rid.to_string (),
                                                    header.correlation_id,
                                                    std::make_exception_ptr (error)};
        event.flow_id = header.flow_id;
        event.flow_origin = header.flow_origin;
        event.mesh_name = router;
        event.channel_route_kind = "route_mesh";
        return event;
    });
    channel_reply_writer_t writer;
    auto reply = writer.reply_raw_envelope (writer.create_error_header (router, header, error),
                                            zlink::message_t::from (""));
    route_dispatch_reply_t response{received.source_node_rid, received.request_seq,
                                    std::move (reply)};
    if (submit_reply) {
        auto submitted = submit_reply (response);
        if (!submitted)
            return propagate_failure<std::optional<route_dispatch_reply_t>> (
              submitted, "MeshNode error reply submit failed");
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    }
    return result_t<std::optional<route_dispatch_reply_t>>::success (std::move (response));
}

} // namespace

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
  dispatch_options_t dispatch_options,
  const handler_registry_t *filters,
  handler_dispatch_kind_t send_dispatch_kind,
  handler_dispatch_kind_t request_dispatch_kind,
  std::function<void ()> before_application_handler,
  std::shared_ptr<void> registration,
  std::function<result_t<void> (const route_dispatch_reply_t &)> submit_reply) :
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

task_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch (const route_received_packet_t &received,
                                     runtime::messaging::envelope_header_t header) const
{
    auto flow_scope = runtime::flow_context_t::enter (
      header.flow_id, header.flow_origin, message_flow_tracer_t (_dispatch_options).mode (),
      flow_origin_t::inbound, std::nullopt);
    if (_send_dispatch_kind != handler_dispatch_kind_t::channel_send
        && _request_dispatch_kind != handler_dispatch_kind_t::channel_request) {
        trace_route_flow (_dispatch_options, _router_channel_id, message_flow_outcome_t::received,
                          header.kind == runtime::messaging::message_kind_t::request
                            ? dispatch_message_kind_t::request
                            : dispatch_message_kind_t::send,
                          received, header);
    }

    switch (header.kind) {
        case runtime::messaging::message_kind_t::command:
            return dispatch_send (received, std::move (header));
        case runtime::messaging::message_kind_t::request:
            return dispatch_request (received, std::move (header));
        default:
            return task_t<std::optional<route_dispatch_reply_t>> (
              result_t<std::optional<route_dispatch_reply_t>>::failure (
                framework_error_kind_t::protocol_error, "unsupported route message kind"));
    }
}


task_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch_send (const route_received_packet_t &received,
                                          runtime::messaging::envelope_header_t header) const
{
    if (_internal_packets != nullptr && _internal_packets->can_handle_send (header.message_name)) {
        auto dispatched = _internal_packets->dispatch_send (received, header, *_services);
        if (!dispatched) {
            return task_t<std::optional<route_dispatch_reply_t>> (
              detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
                dispatched, "route internal send failed"));
        }
        trace_route_flow (_dispatch_options, _router_channel_id, message_flow_outcome_t::dispatched,
                          dispatch_message_kind_t::send, received, header);
        trace_route_flow (_dispatch_options, _router_channel_id, message_flow_outcome_t::completed,
                          dispatch_message_kind_t::send, received, header);
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt));
    }
    const auto *handler =
      _handlers != nullptr
        ? _handlers->resolve (_router_channel_id, runtime::messaging::message_kind_t::command,
                              header.message_name)
        : nullptr;
    if (handler == nullptr || _services == nullptr || _serializers == nullptr) {
        dispatch_error_reporter_t (_dispatch_options).report_lazy ([&] {
            auto event =
              message_dispatch_error_event_t{dispatch_error_surface_t::route_mesh_channel,
                                             dispatch_message_kind_t::send,
                                             dispatch_error_reason_t::handler_missing,
                                             dispatch_error_action_t::drop,
                                             header.message_name,
                                             _router_channel_id,
                                             header.topic,
                                             std::nullopt,
                                             std::nullopt,
                                             received.source_node_rid.to_string (),
                                             header.correlation_id,
                                             std::exception_ptr{}};
            event.flow_id = header.flow_id;
            event.flow_origin = header.flow_origin;
            event.mesh_name = _router_channel_id;
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
              _router_channel_id,
              header.topic,
              std::nullopt,
              std::nullopt,
              received.source_node_rid.to_string (),
              header.correlation_id,
              body.error () ? std::make_exception_ptr (*body.error ()) : std::exception_ptr{}};
            event.flow_id = header.flow_id;
            event.flow_origin = header.flow_origin;
            event.mesh_name = _router_channel_id;
            event.channel_route_kind = "route_mesh";
            return event;
        });
        return task_t<std::optional<route_dispatch_reply_t>> (
          detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
            body, "route command body missing"));
    }
    auto context =
      make_route_message_context (_router_channel_id, received.source_node_rid, header);
    const auto &filters = _filters != nullptr ? *_filters : empty_handler_filters ();
    auto before = _before_application_handler;
    if (message_flow_tracer_t (_dispatch_options).enabled_for (message_flow_outcome_t::dispatched))
        before = [callback = std::move (before), options = _dispatch_options,
                  router = _router_channel_id, source = received.source_node_rid, header] {
            if (callback)
                callback ();
            trace_route_flow (options, router, message_flow_outcome_t::dispatched,
                              dispatch_message_kind_t::send,
                              route_received_packet_t{source, {}, {}, {}}, header);
        };
    auto complete = [options = _dispatch_options, router = _router_channel_id,
                     registration = _registration, submit_reply = _submit_reply,
                     received = route_received_packet_t{received.source_node_rid,
                                                        received.request_seq,
                                                        {},
                                                        received.source_session_rid},
                     header = std::move (header)] (const result_t<zlink::message_t> &dispatched)
      -> result_t<std::optional<route_dispatch_reply_t>> {
        if (!dispatched) {
            dispatch_error_reporter_t (options).report_lazy ([&] {
                auto event = message_dispatch_error_event_t{
                  dispatch_error_surface_t::route_mesh_channel,
                  dispatch_message_kind_t::send,
                  dispatch_reason_from_error (dispatched.error_kind ()),
                  dispatch_error_action_t::drop,
                  header.message_name,
                  router,
                  header.topic,
                  std::nullopt,
                  std::nullopt,
                  received.source_node_rid.to_string (),
                  header.correlation_id,
                  dispatched.error () ? std::make_exception_ptr (*dispatched.error ())
                                      : std::exception_ptr{}};
                event.flow_id = header.flow_id;
                event.flow_origin = header.flow_origin;
                event.mesh_name = router;
                event.channel_route_kind = "route_mesh";
                return event;
            });
            return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (
              dispatched, "routed send handler failed");
        }
        trace_route_flow (options, router, message_flow_outcome_t::completed,
                          dispatch_message_kind_t::send, received, header);
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    };
    return _invoker.invoke (*handler, filters, _send_dispatch_kind, *_services, *_serializers,
                            std::move (body.value ()), std::move (context), std::move (before),
                            std::move (complete));
}

task_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch_request (const route_received_packet_t &received,
                                             runtime::messaging::envelope_header_t header) const
{
    if (_internal_packets != nullptr
        && _internal_packets->can_handle_request (header.message_name)) {
        auto reply = _internal_packets->dispatch_request (received, header, *_services);
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "route internal request failed");
            return task_t<std::optional<route_dispatch_reply_t>> (reply_route_error (
              _dispatch_options, _router_channel_id, _submit_reply, received, header, error));
        }
        trace_route_flow (_dispatch_options, _router_channel_id, message_flow_outcome_t::replied,
                          dispatch_message_kind_t::response, received, header);
        channel_reply_writer_t writer;
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::success (route_dispatch_reply_t{
            received.source_node_rid, received.request_seq,
            writer.reply_raw_envelope (
              writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                          _router_channel_id, header),
              reply.value ())}));
    }

    const auto *handler =
      _handlers != nullptr
        ? _handlers->resolve (_router_channel_id, runtime::messaging::message_kind_t::request,
                              header.message_name)
        : nullptr;
    if (handler == nullptr || _services == nullptr || _serializers == nullptr) {
        framework_exception_t error (framework_error_kind_t::not_found,
                                     "No routed request handler is registered for '"
                                       + _router_channel_id + ":" + header.message_name + "'.");
        return task_t<std::optional<route_dispatch_reply_t>> (reply_route_error (
          _dispatch_options, _router_channel_id, _submit_reply, received, header, error));
    }

    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        framework_exception_t error (body.error_kind (), body.error ()
                                                           ? body.error ()->what ()
                                                           : "route request body missing");
        return task_t<std::optional<route_dispatch_reply_t>> (reply_route_error (
          _dispatch_options, _router_channel_id, _submit_reply, received, header, error));
    }
    auto context =
      make_route_message_context (_router_channel_id, received.source_node_rid, header);
    const auto &filters = _filters != nullptr ? *_filters : empty_handler_filters ();
    auto before = _before_application_handler;
    if (message_flow_tracer_t (_dispatch_options).enabled_for (message_flow_outcome_t::dispatched))
        before = [callback = std::move (before), options = _dispatch_options,
                  router = _router_channel_id, source = received.source_node_rid, header] {
            if (callback)
                callback ();
            trace_route_flow (options, router, message_flow_outcome_t::dispatched,
                              dispatch_message_kind_t::request,
                              route_received_packet_t{source, {}, {}, {}}, header);
        };
    auto complete = [options = _dispatch_options, router = _router_channel_id,
                     registration = _registration, submit_reply = _submit_reply,
                     received = route_received_packet_t{received.source_node_rid,
                                                        received.request_seq,
                                                        {},
                                                        received.source_session_rid},
                     header = std::move (header)] (const result_t<zlink::message_t> &reply)
      -> result_t<std::optional<route_dispatch_reply_t>> {
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "routed request handler failed");
            return reply_route_error (options, router, submit_reply, received, header, error);
        }
        trace_route_flow (options, router, message_flow_outcome_t::replied,
                          dispatch_message_kind_t::response, received, header);
        channel_reply_writer_t writer;
        auto response =
          route_dispatch_reply_t{received.source_node_rid, received.request_seq,
                                 writer.reply_raw_envelope (
                                   writer.create_reply_header (
                                     runtime::messaging::message_kind_t::response, router, header),
                                   reply.value ())};
        if (submit_reply) {
            auto submitted = submit_reply (response);
            if (!submitted)
                return propagate_failure<std::optional<route_dispatch_reply_t>> (
                  submitted, "MeshNode reply submit failed");
            return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
        }
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::move (response));
    };
    return _invoker.invoke (*handler, filters, _request_dispatch_kind, *_services, *_serializers,
                            std::move (body.value ()), std::move (context), std::move (before),
                            std::move (complete));
}


} // namespace zlink::framework::detail
