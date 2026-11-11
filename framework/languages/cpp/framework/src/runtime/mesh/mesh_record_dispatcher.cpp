/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/mesh/mesh_record_dispatcher.hpp"

#include "runtime/channels/route_packet_dispatcher.hpp"

#include <utility>
#include <memory>
#include <algorithm>

namespace zlink::framework::detail
{

mesh_record_dispatcher_t::mesh_record_dispatcher_t (
  service_provider_t &services,
  serializer_registry_t &serializers,
  const route_handler_registry_t &handlers,
  const handler_registry_t &filters,
  retained_dispatch_options_t dispatch_options,
  std::function<void ()> before_application_handler,
  std::shared_ptr<void> registration,
  std::vector<std::string> route_channels) :
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (&filters),
    _dispatch_options (std::move (dispatch_options)),
    _before_application_handler (std::move (before_application_handler)),
    _registration (std::move (registration))
{
    for (auto &channel : handlers.router_channel_ids ()) {
        if (std::find (route_channels.begin (), route_channels.end (), channel)
            == route_channels.end ())
            route_channels.push_back (std::move (channel));
    }
    for (auto &channel : route_channels) {
        if (_route_dispatchers.contains (channel))
            continue;
        _route_dispatchers.emplace (
          channel, std::make_pair (
                     route_packet_dispatcher_t (
                       channel, services, serializers, handlers, _no_internal_packets,
                       _dispatch_options, _filters, handler_dispatch_kind_t::node_direct_send,
                       handler_dispatch_kind_t::node_direct_request, _before_application_handler),
                     route_packet_dispatcher_t (
                       channel, services, serializers, handlers, _no_internal_packets,
                       _dispatch_options, _filters, handler_dispatch_kind_t::channel_send,
                       handler_dispatch_kind_t::channel_request, _before_application_handler)));
    }
}

task_t<std::optional<route_dispatch_reply_t>>
mesh_record_dispatcher_t::dispatch (const runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts,
                                    runtime::messaging::envelope_header_t header,
                                    runtime::application_dispatch_terminal_owner_t terminal,
                                    runtime::offload_executor_t *executor) const
{
    using record_kind_t = runtime::host::record_kind_t;
    if (!is_node_or_channel_record_kind (record.kind)) {
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::failure (
            framework_error_kind_t::protocol_error,
            "record kind is not node or ChannelName messaging"));
    }

    if (!parts.empty ())
        header.retained_header.message = std::move (parts.front ());
    runtime::messaging::message_parts_t message_parts (std::move (parts));
    const std::string_view dispatch_channel = record.channel_name.empty ()
                                                ? std::string_view (header.channel_name)
                                                : std::string_view (record.channel_name);
    const auto channel_dispatch =
      record.kind == record_kind_t::channel_send || record.kind == record_kind_t::channel_request;
    const auto route = _route_dispatchers.find (dispatch_channel);
    if (route == _route_dispatchers.end ())
        return task_t<std::optional<route_dispatch_reply_t>> (
          result_t<std::optional<route_dispatch_reply_t>>::failure (
            framework_error_kind_t::protocol_error, "MeshNode route plan was not registered"));
    const auto &dispatcher = channel_dispatch ? route->second.second : route->second.first;
    return dispatcher.dispatch (
      route_received_packet_t{
        record.source_node_rid, {}, std::move (message_parts), {}, record.received_at},
      std::move (header), std::move (terminal), executor, record.reply_token,
      _registration.lock ());
}

} // namespace zlink::framework::detail
