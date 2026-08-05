/* SPDX-License-Identifier: FSL-1.1-ALv2 */

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
  handler_dispatch_kind_t request_dispatch_kind) :
    _router_channel_id (std::move (router_channel_id)),
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (filters),
    _internal_packets (&internal_packets),
    _dispatch_options (std::move (dispatch_options)),
    _send_dispatch_kind (send_dispatch_kind),
    _request_dispatch_kind (request_dispatch_kind)
{
}

result_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch (const route_received_packet_t &received) const
{
    runtime::messaging::envelope_codec_t codec;
    auto header = codec.decode_header (received.parts);
    if (!header) {
        return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (header, "route envelope header decode failed");
    }

    auto flow_scope = runtime::flow_context_t::enter (
      header.value ().flow_id, header.value ().flow_origin,
      message_flow_tracer_t (_dispatch_options).capture_enabled (),
      flow_origin_t::inbound);
    trace_flow (message_flow_outcome_t::received,
                header.value ().kind == runtime::messaging::message_kind_t::request
                  ? dispatch_message_kind_t::request
                  : dispatch_message_kind_t::send,
                received, header.value ());

    switch (header.value ().kind) {
        case runtime::messaging::message_kind_t::command:
            return dispatch_send (received, std::move (header.value ()));
        case runtime::messaging::message_kind_t::request:
            return dispatch_request (received, std::move (header.value ()));
        default:
            return result_t<std::optional<route_dispatch_reply_t>>::failure (
              framework_error_kind_t::protocol_error, "unsupported route message kind");
    }
}

void route_packet_dispatcher_t::trace_flow (
  message_flow_outcome_t outcome,
  dispatch_message_kind_t kind,
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header) const
{
    message_flow_tracer_t (_dispatch_options).trace (outcome, [&] {
        return message_flow_event_t{outcome,
                                    dispatch_error_surface_t::route_mesh_channel,
                                    kind,
                                    header.message_name,
                                    _router_channel_id,
                                    header.topic,
                                    header.correlation_id,
                                    received.source_node_rid.to_string (),
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt};
    });
}

result_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::dispatch_send (const route_received_packet_t &received,
                                          runtime::messaging::envelope_header_t header) const
{
    if (_internal_packets != nullptr && _internal_packets->can_handle_send (header.message_name)) {
        auto dispatched = _internal_packets->dispatch_send (received, *_services);
        if (!dispatched) {
            return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (dispatched, "route internal send failed");
        }
        trace_flow (message_flow_outcome_t::dispatched, dispatch_message_kind_t::send, received,
                    header);
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    }
    if (_handlers == nullptr || _services == nullptr || _serializers == nullptr
        || _handlers->find (_router_channel_id, runtime::messaging::message_kind_t::command,
                            header.message_name)
             == nullptr) {
        dispatch_error_reporter_t (_dispatch_options)
          .report (message_dispatch_error_event_t{
            dispatch_error_surface_t::route_mesh_channel, dispatch_message_kind_t::send,
            dispatch_error_reason_t::handler_missing, dispatch_error_action_t::drop,
            header.message_name, _router_channel_id, header.topic, std::nullopt, std::nullopt,
            received.source_node_rid.to_string (), header.correlation_id, std::exception_ptr{}});
        return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    }

    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        dispatch_error_reporter_t (_dispatch_options)
          .report (message_dispatch_error_event_t{
            dispatch_error_surface_t::route_mesh_channel, dispatch_message_kind_t::send,
            dispatch_error_reason_t::payload_decode_failed, dispatch_error_action_t::drop,
            header.message_name, _router_channel_id, header.topic, std::nullopt, std::nullopt,
            received.source_node_rid.to_string (), header.correlation_id,
            body.error () ? std::make_exception_ptr (*body.error ()) : std::exception_ptr{}});
        return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (body, "route command body missing");
    }
    auto context = make_route_message_context (_router_channel_id, received.source_node_rid, header);
    const auto &filters =
      _filters != nullptr ? *_filters : empty_handler_filters ();
    auto dispatched =
      _invoker
        .invoke_send (*_handlers, filters, _send_dispatch_kind,
                      _router_channel_id, header.message_name, *_services,
                      *_serializers, body.value (), context)
        .result ();
    if (!dispatched) {
        dispatch_error_reporter_t (_dispatch_options)
          .report (message_dispatch_error_event_t{
            dispatch_error_surface_t::route_mesh_channel, dispatch_message_kind_t::send,
            dispatch_reason_from_error (dispatched.error_kind ()), dispatch_error_action_t::drop,
            header.message_name, _router_channel_id, header.topic, std::nullopt, std::nullopt,
            received.source_node_rid.to_string (), header.correlation_id,
            dispatched.error () ? std::make_exception_ptr (*dispatched.error ())
                                : std::exception_ptr{}});
        return detail::propagate_failure<std::optional<route_dispatch_reply_t>> (dispatched, "routed send handler failed");
    }
    trace_flow (message_flow_outcome_t::dispatched, dispatch_message_kind_t::send, received,
                header);
    return result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
}

result_t<std::optional<route_dispatch_reply_t>> route_packet_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  runtime::messaging::envelope_header_t header) const
{
    if (_internal_packets != nullptr
        && _internal_packets->can_handle_request (header.message_name)) {
        auto reply = _internal_packets->dispatch_request (received, header, *_services);
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "route internal request failed");
            return reply_error (received, header, error);
        }
        trace_flow (message_flow_outcome_t::replied, dispatch_message_kind_t::response, received,
                    header);
        channel_reply_writer_t writer;
        return result_t<std::optional<route_dispatch_reply_t>>::success (route_dispatch_reply_t{
          received.source_node_rid, received.request_seq,
          writer.reply_raw_envelope (
            writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                        _router_channel_id, header),
            reply.value ())});
    }

    if (_handlers == nullptr || _services == nullptr || _serializers == nullptr
        || _handlers->find (_router_channel_id, runtime::messaging::message_kind_t::request,
                            header.message_name)
             == nullptr) {
        framework_exception_t error (framework_error_kind_t::not_found,
                                     "No routed request handler is registered for '"
                                       + _router_channel_id + ":" + header.message_name + "'.");
        return reply_error (received, header, error);
    }

    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        framework_exception_t error (body.error_kind (), body.error ()
                                                           ? body.error ()->what ()
                                                           : "route request body missing");
        return reply_error (received, header, error);
    }
    auto context = make_route_message_context (_router_channel_id, received.source_node_rid, header);
    const auto &filters =
      _filters != nullptr ? *_filters : empty_handler_filters ();
    auto reply =
      _invoker
        .invoke_request (*_handlers, filters, _request_dispatch_kind,
                         _router_channel_id, header.message_name, *_services,
                         *_serializers, body.value (), context)
        .result ();
    if (!reply) {
        framework_exception_t error (reply.error_kind (), reply.error ()
                                                            ? reply.error ()->what ()
                                                            : "routed request handler failed");
        return reply_error (received, header, error);
    }
    trace_flow (message_flow_outcome_t::replied, dispatch_message_kind_t::response, received,
                header);
    channel_reply_writer_t writer;
    return result_t<std::optional<route_dispatch_reply_t>>::success (route_dispatch_reply_t{
      received.source_node_rid, received.request_seq,
      writer.reply_raw_envelope (
        writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                    _router_channel_id, header),
        reply.value ())});
}

result_t<std::optional<route_dispatch_reply_t>>
route_packet_dispatcher_t::reply_error (const route_received_packet_t &received,
                                        const runtime::messaging::envelope_header_t &header,
                                        const framework_exception_t &error) const
{
    dispatch_error_reporter_t (_dispatch_options)
      .report (message_dispatch_error_event_t{
        dispatch_error_surface_t::route_mesh_channel, dispatch_message_kind_t::request,
        dispatch_reason_from_error (error.kind ()), dispatch_error_action_t::reply_error,
        header.message_name, _router_channel_id, header.topic, std::nullopt, std::nullopt,
        received.source_node_rid.to_string (), header.correlation_id,
        std::make_exception_ptr (error)});
    channel_reply_writer_t writer;
    auto reply = writer.reply_raw_envelope (
      writer.create_error_header (_router_channel_id, header, error), zlink::message_t::from (""));
    return result_t<std::optional<route_dispatch_reply_t>>::success (
      route_dispatch_reply_t{received.source_node_rid, received.request_seq, std::move (reply)});
}

} // namespace zlink::framework::detail
