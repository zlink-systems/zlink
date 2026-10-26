/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/channels/route_packet.hpp"

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace zlink::framework::detail
{

class route_packet_dispatcher_t
{
  public:
    using dispatch_result_t = std::variant<result_t<std::optional<route_dispatch_reply_t>>,
                                           task_t<result_t<std::optional<route_dispatch_reply_t>>>>;

    explicit route_packet_dispatcher_t (std::string router_channel_id);
    route_packet_dispatcher_t (
      std::string router_channel_id,
      service_provider_t &services,
      serializer_registry_t &serializers,
      const route_handler_registry_t &handlers,
      const route_internal_packet_dispatcher_t &internal_packets,
      dispatch_options_t dispatch_options = {},
      const handler_registry_t *filters = nullptr,
      handler_dispatch_kind_t send_dispatch_kind = handler_dispatch_kind_t::node_direct_send,
      handler_dispatch_kind_t request_dispatch_kind = handler_dispatch_kind_t::node_direct_request,
      std::function<void ()> before_application_handler = {});

    dispatch_result_t
    dispatch (route_received_packet_t received) const;

    dispatch_result_t
    dispatch (route_received_packet_t received,
              runtime::messaging::envelope_header_t header) const;

  private:
    dispatch_result_t
    dispatch_send (route_received_packet_t received,
                   runtime::messaging::envelope_header_t header) const;

    dispatch_result_t
    dispatch_request (route_received_packet_t received,
                      runtime::messaging::envelope_header_t header) const;

    result_t<std::optional<route_dispatch_reply_t>>
    complete_send (const route_received_packet_t &received,
                    const runtime::messaging::envelope_header_t &header,
                    const result_t<void> &result) const;
    result_t<std::optional<route_dispatch_reply_t>>
    complete_request (const route_received_packet_t &received,
                       const runtime::messaging::envelope_header_t &header,
                       const result_t<zlink::message_t> &result) const;
    static task_t<result_t<std::optional<route_dispatch_reply_t>>>
    continue_send (route_packet_dispatcher_t self, route_received_packet_t received,
                    runtime::messaging::envelope_header_t header, task_t<void> pending);
    static task_t<result_t<std::optional<route_dispatch_reply_t>>>
    continue_request (route_packet_dispatcher_t self, route_received_packet_t received,
                       runtime::messaging::envelope_header_t header,
                       task_t<zlink::message_t> pending);

    result_t<std::optional<route_dispatch_reply_t>>
    reply_error (const route_received_packet_t &received,
                 const runtime::messaging::envelope_header_t &header,
                 const framework_exception_t &error) const;

    void trace_flow (message_flow_outcome_t outcome,
                     dispatch_message_kind_t kind,
                     const route_received_packet_t &received,
                     const runtime::messaging::envelope_header_t &header) const;

    std::string _router_channel_id;
    service_provider_t *_services = nullptr;
    serializer_registry_t *_serializers = nullptr;
    const route_handler_registry_t *_handlers = nullptr;
    const handler_registry_t *_filters = nullptr;
    const route_internal_packet_dispatcher_t *_internal_packets = nullptr;
    dispatch_options_t _dispatch_options;
    handler_dispatch_kind_t _send_dispatch_kind = handler_dispatch_kind_t::node_direct_send;
    handler_dispatch_kind_t _request_dispatch_kind = handler_dispatch_kind_t::node_direct_request;
    std::function<void ()> _before_application_handler;
};

} // namespace zlink::framework::detail
