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

namespace zlink::framework::detail
{

class route_packet_dispatcher_t
{
  public:
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
      std::function<void ()> before_application_handler = {},
      std::shared_ptr<void> registration = {},
      std::function<result_t<void> (const route_dispatch_reply_t &)> submit_reply = {});

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const route_received_packet_t &received) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const route_received_packet_t &received,
              runtime::messaging::envelope_header_t header) const;

  private:
    task_t<std::optional<route_dispatch_reply_t>>
    dispatch_send (const route_received_packet_t &received,
                   runtime::messaging::envelope_header_t header) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch_request (const route_received_packet_t &received,
                      runtime::messaging::envelope_header_t header) const;

    std::string _router_channel_id;
    service_provider_t *_services = nullptr;
    serializer_registry_t *_serializers = nullptr;
    const route_handler_registry_t *_handlers = nullptr;
    const handler_registry_t *_filters = nullptr;
    const route_internal_packet_dispatcher_t *_internal_packets = nullptr;
    route_handler_invoker_t _invoker;
    dispatch_options_t _dispatch_options;
    handler_dispatch_kind_t _send_dispatch_kind = handler_dispatch_kind_t::node_direct_send;
    handler_dispatch_kind_t _request_dispatch_kind = handler_dispatch_kind_t::node_direct_request;
    std::function<void ()> _before_application_handler;
    std::shared_ptr<void> _registration;
    std::function<result_t<void> (const route_dispatch_reply_t &)> _submit_reply;
};

} // namespace zlink::framework::detail
