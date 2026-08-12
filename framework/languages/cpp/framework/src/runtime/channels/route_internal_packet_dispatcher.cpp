/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_internal_packet_dispatcher.hpp"

namespace zlink::framework::detail
{

bool no_route_internal_packet_dispatcher_t::can_handle_send (std::string_view packet_name) const
{
    (void) packet_name;
    return false;
}

bool no_route_internal_packet_dispatcher_t::can_handle_request (std::string_view packet_name) const
{
    (void) packet_name;
    return false;
}

result_t<void>
no_route_internal_packet_dispatcher_t::dispatch_send (const route_received_packet_t &received,
                                                      service_provider_t &services) const
{
    (void) received;
    (void) services;
    return result_t<void>::failure (framework_error_kind_t::not_found,
                                    "no routed internal send dispatcher is configured");
}

result_t<zlink::message_t> no_route_internal_packet_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services) const
{
    (void) received;
    (void) header;
    (void) services;
    return result_t<zlink::message_t>::failure (
      framework_error_kind_t::not_found,
      "no routed internal request dispatcher is configured");
}

} // namespace zlink::framework::detail
