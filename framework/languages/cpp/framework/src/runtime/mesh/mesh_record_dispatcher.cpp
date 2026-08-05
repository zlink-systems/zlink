/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_record_dispatcher.hpp"

#include "runtime/channels/route_packet_dispatcher.hpp"

#include <utility>

namespace zlink::framework::detail
{

mesh_record_dispatcher_t::mesh_record_dispatcher_t (
  service_provider_t &services,
  serializer_registry_t &serializers,
  const route_handler_registry_t &handlers,
  const handler_registry_t &filters,
  dispatch_options_t dispatch_options) :
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers),
    _filters (&filters),
    _dispatch_options (std::move (dispatch_options))
{
}

result_t<void>
mesh_record_dispatcher_t::dispatch (
  const runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts) const
{
    using record_kind_t = runtime::host::record_kind_t;
    if (record.kind != record_kind_t::node_send
        && record.kind != record_kind_t::node_request
        && record.kind != record_kind_t::channel_send
        && record.kind != record_kind_t::channel_request) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "record kind is not node or ChannelName messaging");
    }

    runtime::messaging::message_parts_t message_parts (std::move (parts));
    runtime::messaging::envelope_codec_t codec;
    const auto header = codec.decode_header (message_parts);
    if (!header) {
        return detail::propagate_failure<void> (header, "MeshNode envelope header decode failed");
    }
    const std::string dispatch_channel =
      record.channel_name.empty () ? header.value ().channel_name : record.channel_name;
    const auto channel_dispatch =
      record.kind == record_kind_t::channel_send
      || record.kind == record_kind_t::channel_request;
    route_packet_dispatcher_t dispatcher (
      dispatch_channel, *_services, *_serializers, *_handlers,
      _no_internal_packets, _dispatch_options, _filters,
      channel_dispatch ? handler_dispatch_kind_t::channel_send
                       : handler_dispatch_kind_t::node_direct_send,
      channel_dispatch ? handler_dispatch_kind_t::channel_request
                       : handler_dispatch_kind_t::node_direct_request);
    auto dispatched = dispatcher.dispatch (
      route_received_packet_t{record.source_node_rid, {}, std::move (message_parts), {}});
    if (!dispatched) {
        return detail::propagate_failure<void> (dispatched, "MeshNode handler dispatch failed");
    }
    if (!dispatched.value ())
        return result_t<void>::success ();
    if (record.kind != record_kind_t::node_request
        && record.kind != record_kind_t::channel_request) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "send handler produced a reply");
    }
    const auto submit = runtime::host::reply (
      record.reply_token, dispatched.value ()->parts.items ());
    if (submit != zlink::submit_result_t::ok) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "MeshNode reply submit failed");
    }
    return result_t<void>::success ();
}

} // namespace zlink::framework::detail
