/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <iostream>
#include <utility>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::reject_application_request;
using mesh_node_host_service_detail::terminal_callback_guard_t;
using mesh_node_host_service_detail::trace_mesh_application;

namespace
{
bool is_application_request (host::record_kind_t kind)
{
    using kind_t = host::record_kind_t;
    return kind == kind_t::node_request || kind == kind_t::channel_request
           || kind == kind_t::spot_request || kind == kind_t::actor_request;
}

dispatch_error_surface_t application_dispatch_surface (host::record_kind_t kind)
{
    using kind_t = host::record_kind_t;
    return kind == kind_t::actor_send || kind == kind_t::actor_request
             ? dispatch_error_surface_t::spot_actor
           : kind == kind_t::spot_send || kind == kind_t::spot_request
             ? dispatch_error_surface_t::spot_route
           : kind == kind_t::node_send || kind == kind_t::node_request
             ? dispatch_error_surface_t::node
             : dispatch_error_surface_t::route_mesh_channel;
}
} // namespace

result_t<std::optional<messaging::envelope_header_t>>
decode_mesh_record_header (const host::receive_record_t &record,
                           std::vector<zlink::message_t> &parts,
                           bool capture_flow,
                           const dispatch_options_t &dispatch_options)
{
    using kind_t = host::record_kind_t;
    using header_result_t = result_t<std::optional<messaging::envelope_header_t>>;
    switch (record.kind) {
        case kind_t::spot_send:
        case kind_t::spot_request:
        case kind_t::node_send:
        case kind_t::node_request:
        case kind_t::channel_send:
        case kind_t::channel_request:
        case kind_t::actor_send:
        case kind_t::actor_request:
            break;
        default:
            return header_result_t::success (std::nullopt);
    }
    messaging::message_parts_t encoded (std::move (parts));
    std::optional<messaging::envelope_header_t> rejected_header;
    const bool actor = record.kind == kind_t::actor_send || record.kind == kind_t::actor_request;
    auto decoded = [&] {
        if (actor)
            return detail::decode_actor_packet_header (encoded,
                                                       record.kind == kind_t::actor_send
                                                         ? detail::stream_message_kind_t::send
                                                         : detail::stream_message_kind_t::request,
                                                       capture_flow);
        return messaging::envelope_codec_t{}.decode_header_view (encoded, capture_flow,
                                                                 &rejected_header);
    }();
    parts = std::move (encoded).take_items ();
    if (decoded)
        return header_result_t::success (std::move (decoded.value ()));

    const bool request = is_application_request (record.kind);
    const bool can_correlate = rejected_header && rejected_header->can_correlate_reply ();
    auto submitted = zlink::submit_result_t::terminated;
    if (record.kind == kind_t::actor_request) {
        detail::channel_reply_writer_t replies;
        auto reply = detail::encode_actor_reply (
          replies.create_error_header (detail::actor_message_channel_name, {}, *decoded.error ()),
          zlink::message_t{});
        submitted = host::reply (record.reply_token, reply.items ());
    } else if (request && can_correlate) {
        detail::channel_reply_writer_t replies;
        auto reply = replies.reply_raw_envelope (
          replies.create_error_header (rejected_header->channel_name, *rejected_header,
                                       *decoded.error ()),
          zlink::message_t::from (""));
        submitted = host::reply (record.reply_token, reply.items ());
    }
    detail::dispatch_error_reporter_t (dispatch_options).report_lazy ([&] {
        message_dispatch_error_event_t event{
          application_dispatch_surface (record.kind),
          request ? dispatch_message_kind_t::request : dispatch_message_kind_t::send,
          dispatch_error_reason_t::invalid_frame,
          submitted == zlink::submit_result_t::ok ? dispatch_error_action_t::reply_error
                                                  : dispatch_error_action_t::drop};
        event.channel_name = record.channel_name;
        event.source_rid = record.source_node_rid.to_string ();
        event.exception = decoded.exception ();
        if (rejected_header) {
            if (!rejected_header->message_name.empty ())
                event.packet_name = rejected_header->message_name;
            if (!rejected_header->channel_name.empty ())
                event.channel_name = rejected_header->channel_name;
            if (can_correlate)
                event.correlation_id = rejected_header->correlation_id;
            event.flow_id = rejected_header->flow_id;
            event.flow_origin = rejected_header->flow_origin;
        }
        return event;
    });
    return detail::propagate_failure<std::optional<messaging::envelope_header_t>> (
      decoded, "MeshNode envelope header decode failed");
}

void mesh_node_host_service_detail::trace_mesh_application (std::string_view stage,
                                                            const host::receive_record_t &record,
                                                            std::size_t parts,
                                                            std::string_view detail)
{
    if (!::zlink::framework::detail::mesh_trace_enabled ())
        return;
    std::cerr << "zlink mesh-host stage=" << stage << " kind=" << static_cast<int> (record.kind)
              << " source=" << record.source_node_rid.to_string () << " parts=" << parts;
    if (!detail.empty ())
        std::cerr << " detail=" << detail;
    std::cerr << '\n';
}

void mesh_node_host_service_detail::reject_application_request (
  const host::receive_record_t &record,
  std::vector<zlink::message_t> parts,
  framework_error_kind_t error_kind,
  std::string message)
{
    if (!is_application_request (record.kind))
        return;
    if (record.kind == host::record_kind_t::actor_request) {
        detail::channel_reply_writer_t replies;
        auto reply = detail::encode_actor_reply (
          replies.create_error_header (detail::actor_message_channel_name, {},
                                       framework_exception_t (error_kind, std::move (message))),
          zlink::message_t{});
        (void) host::reply (record.reply_token, reply.items ());
        return;
    }
    messaging::message_parts_t encoded (std::move (parts));
    messaging::envelope_codec_t codec;
    /* Error-reply construction needs channel/correlation only; a malformed
     * observation-only flow pair must not suppress the error reply. */
    const auto header = codec.decode_header (encoded, false);
    if (!header)
        return;
    detail::channel_reply_writer_t replies;
    auto reply = replies.reply_raw_envelope (
      replies.create_error_header (header.value ().channel_name, header.value (),
                                   framework_exception_t (error_kind, std::move (message))),
      zlink::message_t::from (""));
    (void) host::reply (record.reply_token, reply.items ());
}

void mesh_node_host_service_t::dispatch_application (
  const std::shared_ptr<detail::mesh_node_runtime_t> &node,
  const std::shared_ptr<detail::mesh_node_builder_state_t> &registration,
  const host::ready_record_t &owner,
  const host::receive_record_t &record,
  std::vector<zlink::message_t> parts,
  bool reject_only,
  service_provider_t *services,
  serializer_registry_t *serializers,
  const handler_registry_t *filters,
  const detail::retained_dispatch_options_t &dispatch_options,
  serial_execution_queue_t::async_completion_t application_completed,
  offload_executor_t *executor,
  application_job_queue_t::permit_t application_permit)
{
    const bool admitted = static_cast<bool> (application_permit);
    if (record.retain_mailbox_reservation)
        record.retain_mailbox_reservation ();
    application_dispatch_terminal_owner_t terminal (
      node, record.complete_stateful_dispatch, record.release_mailbox_reservation,
      handler_entry_t (record.before_application_handler, std::move (application_permit)));
    try {
        // A draining runtime admits no new application work, whatever the target
        // authority state is (Spot address messaging §9).
        if (reject_only
            || (!admitted && !record.before_application_handler
                && (!record.application_record || !record.application_record->saved_work ()))) {
            reject_application_request (record, std::move (parts),
                                        framework_error_kind_t::shutting_down,
                                        "MeshNode is draining and rejects new application work");
            if (application_completed)
                application_completed ([] {});
            return;
        }
        detail::spot_node_runtime_t application_spot_runtime (registration->spot_state);
        bool terminal_deferred = false;
        auto decoded_header = decode_mesh_record_header (
          record, parts, detail::message_flow_tracer_t (dispatch_options).flow_tracing (),
          dispatch_options);
        if (!decoded_header) {
            terminal.settle ();
            if (application_completed)
                application_completed ([] {});
            return;
        }
        handler_entry_t before;
        if (owner.owner_kind == host::owner_kind_t::spot
            || owner.owner_kind == host::owner_kind_t::actor)
            before = terminal.take_application_entry ();
        const auto handled = application_spot_runtime.dispatch_mesh_record (
          owner, record, parts, decoded_header.value (), *services, *serializers, {},
          &terminal_deferred, std::move (before), application_completed, &terminal);
        trace_mesh_application ("framework-dispatch", record, parts.size (),
                                handled ? "handled" : "not-handled");
        if (!handled && decoded_header.value ()
            && (owner.owner_kind == host::owner_kind_t::node
                || owner.owner_kind == host::owner_kind_t::channel)) {
            // Callers always pass an empty application_completed for node and channel owners.
            // The native claim still owns settlement. Transfer its existing release
            // callback to the bounded pipeline handoff, leaving physical work retained.
            terminal_callback_guard_t prefix (terminal.take_mailbox_reservation ());
            detail::mesh_record_dispatcher_t dispatcher (*services, *serializers,
                                                         registration->handlers, *filters,
                                                         dispatch_options, {}, registration);
            auto invocation_terminal = std::move (terminal);
            terminal_deferred = true;
            auto pending =
              dispatcher.dispatch (record, std::move (parts), std::move (*decoded_header.value ()),
                                   std::move (invocation_terminal), executor);
            trace_mesh_application ("route-dispatch", record, 0,
                                    pending.await_ready () ? "ready" : "pending");
        }
        if (!terminal_deferred) {
            terminal.settle ();
            if (application_completed)
                application_completed ([] {});
        }
    }
    catch (...) {
        const auto error = std::current_exception ();
        detail::dispatch_error_reporter_t (dispatch_options).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = application_dispatch_surface (record.kind),
              .message_kind = is_application_request (record.kind)
                                ? dispatch_message_kind_t::request
                                : dispatch_message_kind_t::send,
              .reason = dispatch_error_reason_t::handler_exception,
              .action = dispatch_error_action_t::drop,
              .source_rid = record.source_node_rid.to_string (),
              .exception = error};
        });
        terminal.settle ();
        if (application_completed)
            application_completed ([] {});
    }
}

} // namespace zlink::framework::runtime
