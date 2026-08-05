/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_packet_dispatcher.hpp"

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace zlink::framework::detail
{

namespace
{

/// Builds the inbound context once from the decoded envelope so request, send and publish handlers
/// see the same universal fields.
inbound_message_context_t
make_inbound_context (const std::string &channel_name,
                      runtime::messaging::envelope_header_t &header)
{
    inbound_message_context_t inbound;
    inbound.message.channel_name = channel_name;
    inbound.message.packet_name = header.message_name;
    inbound.message.content_type = header.content_type;
    inbound.message.metadata = message_metadata_t (std::move (header.metadata));
    if (!header.correlation_id.empty ()) {
        inbound.message.correlation_id = header.correlation_id;
    }
    inbound.topic = header.topic.value_or ("");
    inbound.source = header.source;
    return inbound;
}

} // namespace

channel_packet_dispatcher_t::channel_packet_dispatcher_t (channel_runtime_t runtime) :
    _runtime (std::move (runtime))
{
}

result_t<runtime::messaging::message_parts_t> channel_packet_dispatcher_t::dispatch_server_message (
  std::string channel_name,
  const runtime::messaging::message_parts_t &parts,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers) const
{
    runtime::messaging::envelope_codec_t codec;
    auto header = codec.decode_header (parts);
    if (!header) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (header, "channel envelope header decode failed");
    }
    const auto inbound_kind = [&] {
        switch (header.value ().kind) {
            case runtime::messaging::message_kind_t::request:
                return dispatch_message_kind_t::request;
            case runtime::messaging::message_kind_t::publish:
                return dispatch_message_kind_t::publish;
            default:
                return dispatch_message_kind_t::send;
        }
    }();
    message_flow_tracer_t flow (_runtime.dispatch_options_ref ());
    auto flow_scope = runtime::flow_context_t::enter (
      header.value ().flow_id, header.value ().flow_origin, flow.capture_enabled (),
      flow_origin_t::inbound);
    if (inbound_kind != dispatch_message_kind_t::publish) {
        flow.trace (message_flow_outcome_t::received, [&] {
            return message_flow_event_t{message_flow_outcome_t::received,
                                        dispatch_error_surface_t::channel,
                                        inbound_kind,
                                        header.value ().message_name,
                                        channel_name,
                                        header.value ().topic,
                                        header.value ().correlation_id,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
        });
    }

    auto body = codec.decode_body (parts);
    if (!body) {
        dispatch_error_reporter_t (_runtime.dispatch_options ())
          .report (message_dispatch_error_event_t{
            dispatch_error_surface_t::channel, inbound_kind,
            dispatch_error_reason_t::payload_decode_failed,
            header.value ().kind == runtime::messaging::message_kind_t::request
              ? dispatch_error_action_t::reply_error
              : dispatch_error_action_t::drop,
            header.value ().message_name, channel_name, header.value ().topic, std::nullopt,
            std::nullopt, std::nullopt, header.value ().correlation_id,
            body.error () ? std::make_exception_ptr (*body.error ()) : std::exception_ptr{}});
        if (header.value ().kind == runtime::messaging::message_kind_t::request) {
            channel_reply_writer_t writer;
            framework_exception_t error (body.error_kind (), body.error ()
                                                               ? body.error ()->what ()
                                                               : "channel body decode failed");
            return result_t<runtime::messaging::message_parts_t>::success (
              writer.reply_raw_envelope (
                writer.create_error_header (std::move (channel_name), header.value (), error),
                zlink::message_t::from ("")));
        }
        return detail::propagate_failure<runtime::messaging::message_parts_t> (body, "channel body decode failed");
    }

    if (header.value ().kind == runtime::messaging::message_kind_t::request) {
        auto reply = _runtime.dispatch_request (
          channel_name, header.value ().topic.value_or (""), header.value ().message_name, services,
          serializers, handlers, body.value (),
          make_inbound_context (channel_name, header.value ()));
        channel_reply_writer_t writer;
        if (!reply) {
            framework_exception_t error (reply.error_kind (), reply.error ()
                                                                ? reply.error ()->what ()
                                                                : "channel request failed");
            dispatch_error_reporter_t (_runtime.dispatch_options ())
              .report (message_dispatch_error_event_t{
                dispatch_error_surface_t::channel, dispatch_message_kind_t::request,
                dispatch_reason_from_error (reply.error ()),
                dispatch_error_action_t::reply_error, header.value ().message_name, channel_name,
                header.value ().topic, std::nullopt, std::nullopt, std::nullopt,
                header.value ().correlation_id,
                reply.error () ? std::make_exception_ptr (*reply.error ()) : std::exception_ptr{}});
            return result_t<runtime::messaging::message_parts_t>::success (
              writer.reply_raw_envelope (
                writer.create_error_header (std::move (channel_name), header.value (), error),
                zlink::message_t::from ("")));
        }
        flow.trace (message_flow_outcome_t::replied, [&] {
            return message_flow_event_t{message_flow_outcome_t::replied,
                                        dispatch_error_surface_t::channel,
                                        dispatch_message_kind_t::response,
                                        header.value ().message_name,
                                        channel_name,
                                        header.value ().topic,
                                        header.value ().correlation_id,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
        });
        return result_t<runtime::messaging::message_parts_t>::success (writer.reply_raw_envelope (
          writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                      std::move (channel_name), header.value ()),
          reply.value ()));
    }

    if (header.value ().kind == runtime::messaging::message_kind_t::command
        || header.value ().kind == runtime::messaging::message_kind_t::publish) {
        auto result = _runtime.dispatch_send (
          channel_name, header.value ().topic.value_or (""), header.value ().message_name, services,
          serializers, handlers, body.value (),
          make_inbound_context (channel_name, header.value ()));
        if (!result) {
            dispatch_error_reporter_t (_runtime.dispatch_options ())
              .report (message_dispatch_error_event_t{
                dispatch_error_surface_t::channel, inbound_kind,
                dispatch_reason_from_error (result.error ()), dispatch_error_action_t::drop,
                header.value ().message_name, channel_name, header.value ().topic, std::nullopt,
                std::nullopt, std::nullopt, header.value ().correlation_id,
                result.error () ? std::make_exception_ptr (*result.error ())
                                : std::exception_ptr{}});
            return result_t<runtime::messaging::message_parts_t>::success (
              runtime::messaging::message_parts_t{});
        }
        if (inbound_kind == dispatch_message_kind_t::publish) {
        }
        if (inbound_kind != dispatch_message_kind_t::publish) {
            flow.trace (message_flow_outcome_t::dispatched, [&] {
                return message_flow_event_t{message_flow_outcome_t::dispatched,
                                            dispatch_error_surface_t::channel,
                                            inbound_kind,
                                            header.value ().message_name,
                                            channel_name,
                                            header.value ().topic,
                                            header.value ().correlation_id,
                                            std::nullopt,
                                            std::nullopt,
                                            std::nullopt,
                                            std::nullopt};
            });
        }
        return result_t<runtime::messaging::message_parts_t>::success (
          runtime::messaging::message_parts_t{});
    }

    return result_t<runtime::messaging::message_parts_t>::failure (
      framework_error_kind_t::protocol_error, "unsupported channel message kind");
}

} // namespace zlink::framework::detail
