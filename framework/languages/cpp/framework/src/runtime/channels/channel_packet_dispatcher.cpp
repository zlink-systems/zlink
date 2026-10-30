/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_packet_dispatcher.hpp"

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <exception>
#include <optional>
#include <utility>
#include <functional>
#include <string_view>

namespace zlink::framework::detail
{

namespace
{

/// Builds the inbound context once from the decoded envelope so request, send and publish handlers
/// see the same universal fields.
inbound_message_context_t make_inbound_context (const std::string &channel_name,
                                                runtime::messaging::envelope_header_t &header,
                                                std::function<void ()> before_application_handler)
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
    inbound.before_application_handler = std::move (before_application_handler);
    return inbound;
}

} // namespace

channel_packet_dispatcher_t::channel_packet_dispatcher_t (channel_runtime_t runtime) :
    _runtime (std::move (runtime))
{
}

result_t<runtime::messaging::message_parts_t> channel_packet_dispatcher_t::dispatch_server_message (
  std::string channel_name,
  runtime::messaging::message_parts_t parts,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  std::function<void ()> before_application_handler) const
{
    return dispatch_server_message_in_handler (std::move (channel_name), std::move (parts),
                                               services, serializers, handlers,
                                               std::move (before_application_handler))
      .result ();
}

task_t<runtime::messaging::message_parts_t>
channel_packet_dispatcher_t::dispatch_server_message_in_handler (
  std::string channel_name,
  runtime::messaging::message_parts_t &&incoming_parts,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  std::function<void ()> incoming_before,
  std::optional<service_scope_t> incoming_scope,
  runtime::offload_executor_t *executor,
  std::function<void (const result_t<runtime::messaging::message_parts_t> &,
                      const runtime::messaging::message_parts_t &)> incoming_complete) const
{
    runtime::offload_executor_t::retained_work_t physical_work;
    // Frame/promise allocation has succeeded. Consume the input before any
    // fallible operation or suspension; the caller retains it on construction failure.
    auto parts = std::move (incoming_parts);
    auto scope = std::move (incoming_scope);
    auto before_application_handler = std::move (incoming_before);
    auto complete = std::move (incoming_complete);
    incoming_scope.reset ();
    incoming_before = {};
    incoming_complete = {};
    auto &invocation_services = scope ? scope->provider () : services;
    auto finish_result = [&] (result_t<runtime::messaging::message_parts_t> result) {
        auto finish = std::exchange (complete, {});
        auto finished_scope = std::exchange (scope, std::nullopt);
        if (finish)
            finish (result, parts);
        return result;
    };
    try {
        runtime::messaging::envelope_codec_t codec;
        message_flow_tracer_t flow (_runtime.dispatch_options_ref ());
        auto header = codec.decode_header (parts, flow.flow_tracing ());
        if (!header) {
            co_return finish_result (
              detail::propagate_failure<runtime::messaging::message_parts_t> (
                header, "channel envelope header decode failed"));
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
        auto flow_scope =
          runtime::flow_context_t::enter (header.value ().flow_id, header.value ().flow_origin,
                                          flow.mode (), flow_origin_t::inbound, std::nullopt);
        if (inbound_kind != dispatch_message_kind_t::publish
            && flow.enabled_for (message_flow_outcome_t::dispatched)) {
            before_application_handler = [before = std::move (before_application_handler), &flow,
                                          &header, &channel_name, inbound_kind] {
                if (before)
                    before ();
                flow.trace (message_flow_outcome_t::dispatched,
                            header.value ().flow_id ? std::string_view (*header.value ().flow_id)
                                                    : std::string_view{},
                            [&] {
                                message_flow_event_t event{message_flow_outcome_t::dispatched,
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

                                event.flow_id = header.value ().flow_id;
                                event.flow_origin = header.value ().flow_origin;
                                return event;
                            });
            };
        }

        auto body = codec.decode_body (parts);
        if (!body) {
            dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
                return message_dispatch_error_event_t{
                  dispatch_error_surface_t::channel,
                  inbound_kind,
                  dispatch_error_reason_t::payload_decode_failed,
                  header.value ().kind == runtime::messaging::message_kind_t::request
                    ? dispatch_error_action_t::reply_error
                    : dispatch_error_action_t::drop,
                  header.value ().message_name,
                  channel_name,
                  header.value ().topic,
                  std::nullopt,
                  std::nullopt,
                  std::nullopt,
                  header.value ().correlation_id,
                  body.error () ? std::make_exception_ptr (*body.error ()) : std::exception_ptr{}};
            });
            if (header.value ().kind == runtime::messaging::message_kind_t::request) {
                channel_reply_writer_t writer;
                framework_exception_t error (body.error_kind (), body.error ()
                                                                   ? body.error ()->what ()
                                                                   : "channel body decode failed");
                co_return finish_result (result_t<runtime::messaging::message_parts_t>::success (
                  writer.reply_raw_envelope (
                    writer.create_error_header (std::move (channel_name), header.value (), error),
                    zlink::message_t::from (""))));
            }
            co_return finish_result (
              detail::propagate_failure<runtime::messaging::message_parts_t> (
                body, "channel body decode failed"));
        }

        if (header.value ().kind == runtime::messaging::message_kind_t::request) {
            auto reply = result_t<zlink::message_t>::failure (
              framework_error_kind_t::internal_failure, "channel request failed");
            try {
                auto pending = _runtime.dispatch_request_in_handler (
                  channel_name, header.value ().topic.value_or (""), header.value ().message_name,
                  invocation_services, serializers, handlers, body.value (),
                  make_inbound_context (channel_name, header.value (), before_application_handler));
                if (!pending.await_ready () && executor)
                    physical_work = executor->retain_internal_work ();
                reply = result_t<zlink::message_t>::success (co_await pending);
            }
            catch (const framework_exception_t &error) {
                reply = detail::result_access_t::failure<zlink::message_t> (error);
            }
            channel_reply_writer_t writer;
            if (!reply) {
                framework_exception_t error (reply.error_kind (), reply.error ()
                                                                    ? reply.error ()->what ()
                                                                    : "channel request failed");
                dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
                    return message_dispatch_error_event_t{
                      dispatch_error_surface_t::channel,
                      dispatch_message_kind_t::request,
                      dispatch_reason_from_error (reply.error ()),
                      dispatch_error_action_t::reply_error,
                      header.value ().message_name,
                      channel_name,
                      header.value ().topic,
                      std::nullopt,
                      std::nullopt,
                      std::nullopt,
                      header.value ().correlation_id,
                      reply.error () ? std::make_exception_ptr (*reply.error ())
                                     : std::exception_ptr{}};
                });
                co_return finish_result (result_t<runtime::messaging::message_parts_t>::success (
                  writer.reply_raw_envelope (
                    writer.create_error_header (std::move (channel_name), header.value (), error),
                    zlink::message_t::from (""))));
            }
            flow.trace (message_flow_outcome_t::replied,
                        header.value ().flow_id ? std::string_view (*header.value ().flow_id)
                                                : std::string_view{},
                        [&] {
                            message_flow_event_t event{message_flow_outcome_t::replied,
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
                            event.flow_id = header.value ().flow_id;
                            event.flow_origin = header.value ().flow_origin;
                            return event;
                        });
            co_return finish_result (
              result_t<runtime::messaging::message_parts_t>::success (writer.reply_raw_envelope (
                writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                            std::move (channel_name), header.value ()),
                reply.value ())));
        }

        if (header.value ().kind == runtime::messaging::message_kind_t::command
            || header.value ().kind == runtime::messaging::message_kind_t::publish) {
            auto result = result_t<void>::success ();
            try {
                auto pending = _runtime.dispatch_send_in_handler (
                  channel_name, header.value ().topic.value_or (""), header.value ().message_name,
                  invocation_services, serializers, handlers, body.value (),
                  make_inbound_context (channel_name, header.value (), before_application_handler));
                if (!pending.await_ready () && executor)
                    physical_work = executor->retain_internal_work ();
                static_cast<void> (co_await pending);
            }
            catch (const framework_exception_t &error) {
                result = detail::result_access_t::failure<void> (error);
            }
            if (!result) {
                dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
                    return message_dispatch_error_event_t{
                      dispatch_error_surface_t::channel,
                      inbound_kind,
                      dispatch_reason_from_error (result.error ()),
                      dispatch_error_action_t::drop,
                      header.value ().message_name,
                      channel_name,
                      header.value ().topic,
                      std::nullopt,
                      std::nullopt,
                      std::nullopt,
                      header.value ().correlation_id,
                      result.error () ? std::make_exception_ptr (*result.error ())
                                      : std::exception_ptr{}};
                });
                co_return finish_result (result_t<runtime::messaging::message_parts_t>::success (
                  runtime::messaging::message_parts_t{}));
            }
            if (inbound_kind != dispatch_message_kind_t::publish) {
                flow.trace (message_flow_outcome_t::completed,
                            header.value ().flow_id ? std::string_view (*header.value ().flow_id)
                                                    : std::string_view{},
                            [&] {
                                message_flow_event_t event{message_flow_outcome_t::completed,
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
                                event.flow_id = header.value ().flow_id;
                                event.flow_origin = header.value ().flow_origin;
                                return event;
                            });
            }
            co_return finish_result (result_t<runtime::messaging::message_parts_t>::success (
              runtime::messaging::message_parts_t{}));
        }

        co_return finish_result (result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::protocol_error, "unsupported channel message kind"));
    }
    catch (...) {
        co_return finish_result (current_exception_result<runtime::messaging::message_parts_t> ());
    }
}

} // namespace zlink::framework::detail
