/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/mesh/mesh_record_dispatcher.hpp"

#include "runtime/channels/route_packet_dispatcher.hpp"

#include <utility>
#include <memory>

namespace zlink::framework::detail
{

mesh_record_dispatcher_t::mesh_record_dispatcher_t (
  service_provider_t &services,
  serializer_registry_t &serializers,
  const route_handler_registry_t &handlers,
  const handler_registry_t &filters,
  dispatch_options_t dispatch_options,
  std::function<void ()> before_application_handler,
  std::shared_ptr<void> registration) :
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (&filters),
    _dispatch_options (std::move (dispatch_options)),
    _before_application_handler (std::move (before_application_handler)),
    _registration (std::move (registration))
{
}

task_t<std::optional<route_dispatch_reply_t>>
mesh_record_dispatcher_t::dispatch (const runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts,
                                    runtime::messaging::envelope_header_t header,
                                    runtime::application_dispatch_terminal_owner_t terminal,
                                    runtime::offload_executor_t *executor) const
{
    using record_kind_t = runtime::host::record_kind_t;
    if (record.kind != record_kind_t::node_send && record.kind != record_kind_t::node_request
        && record.kind != record_kind_t::channel_send
        && record.kind != record_kind_t::channel_request) {
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::failure (
            framework_error_kind_t::protocol_error,
            "record kind is not node or ChannelName messaging"));
    }

    runtime::messaging::message_parts_t message_parts (std::move (parts));
    const std::string dispatch_channel =
      record.channel_name.empty () ? header.channel_name : record.channel_name;
    const auto channel_dispatch =
      record.kind == record_kind_t::channel_send || record.kind == record_kind_t::channel_request;
    route_packet_dispatcher_t dispatcher (
      dispatch_channel, *_services, *_serializers, *_handlers, _no_internal_packets,
      _dispatch_options, _filters,
      channel_dispatch ? handler_dispatch_kind_t::channel_send
                       : handler_dispatch_kind_t::node_direct_send,
      channel_dispatch ? handler_dispatch_kind_t::channel_request
                       : handler_dispatch_kind_t::node_direct_request,
      _before_application_handler, _registration,
      [token = record.reply_token] (const route_dispatch_reply_t &reply) {
          if (runtime::host::reply (token, reply.parts.items ()) != zlink::submit_result_t::ok)
              return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                              "MeshNode reply submit failed");
          return result_t<void>::success ();
      });
    return dispatcher.dispatch (
      route_received_packet_t{record.source_node_rid, {}, std::move (message_parts), {}},
      std::move (header), std::move (terminal), executor);
}

} // namespace zlink::framework::detail
