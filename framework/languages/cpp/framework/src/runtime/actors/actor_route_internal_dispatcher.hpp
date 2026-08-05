/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"

namespace zlink::framework::detail
{

class actor_route_internal_dispatcher_t final : public route_internal_packet_dispatcher_t
{
  public:
    actor_route_internal_dispatcher_t (actor_gateway_runtime_t runtime,
                                       serializer_registry_t &serializers);

    bool can_handle_send (std::string_view packet_name) const override;
    bool can_handle_request (std::string_view packet_name) const override;
    result_t<void> dispatch_send (const route_received_packet_t &received,
                                  service_provider_t &services) const override;
    result_t<zlink::message_t>
    dispatch_request (const route_received_packet_t &received,
                      const runtime::messaging::envelope_header_t &header,
                      service_provider_t &services) const override;

  private:
    actor_gateway_runtime_t _runtime;
    serializer_registry_t *_serializers;
};

} // namespace zlink::framework::detail
