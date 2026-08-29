/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/spots/spot_runtime.hpp"

namespace zlink::framework::detail
{

struct spot_actor_commit_route_request_t;

class spot_route_internal_dispatcher_t final : public route_internal_packet_dispatcher_t
{
  public:
    spot_route_internal_dispatcher_t (spot_node_runtime_t runtime,
                                      actor_gateway_runtime_t actor_gateway,
                                      route_client_t route_client,
                                      serializer_registry_t &serializers);

    bool can_handle_send (std::string_view packet_name) const override;
    bool can_handle_request (std::string_view packet_name) const override;
    result_t<void> dispatch_send (const route_received_packet_t &received,
                                  service_provider_t &services) const override;
    result_t<void> dispatch_send (const route_received_packet_t &received,
                                  service_provider_t &services,
                                  std::function<void ()> transfer_owner_reservation,
                                  std::size_t transferred_owner_byte_cost) const;
    result_t<zlink::message_t>
    dispatch_request (const route_received_packet_t &received,
                      const runtime::messaging::envelope_header_t &header,
                      service_provider_t &services) const override;
    bool dispatch_request_async (
      const route_received_packet_t &received,
      const runtime::messaging::envelope_header_t &header,
      service_provider_t &services,
      std::function<void (result_t<zlink::message_t>)> completion,
      runtime::protocol::wire_operation_id_t inbound_operation = {},
      std::uint64_t inbound_reply_route_id = 0) const;

  private:
    result_t<void> bind_actor_session_route (
      actor_gateway_runtime_t &actor_gateway,
      const actor_ref_t &actor_ref,
      std::string route_channel_name,
      zlink::routing_id_t session_node_rid,
      std::optional<zlink::routing_id_t> session_rid,
      bool replace_existing) const;
    result_t<actor_gateway_runtime_t> bind_actor_route (
      const actor_ref_t &actor_ref,
      const runtime::messaging::envelope_header_t &header,
      const route_received_packet_t &received) const;
    void dispatch_actor_commit_request (
      spot_actor_commit_route_request_t request,
      const route_received_packet_t &received,
      const runtime::messaging::envelope_header_t &header,
      service_provider_t &services,
      std::function<void (result_t<zlink::message_t>)> completion) const;

    spot_node_runtime_t _runtime;
    actor_gateway_runtime_t _actor_gateway;
    route_client_t _route_client;
    serializer_registry_t *_serializers;
};

} // namespace zlink::framework::detail
