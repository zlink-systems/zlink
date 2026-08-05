/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_internal_packet_dispatcher.hpp"

#include "runtime/messaging/envelope_codec.hpp"

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

void composite_route_internal_packet_dispatcher_t::add (
  const route_internal_packet_dispatcher_t &dispatcher)
{
    _dispatchers.push_back (&dispatcher);
}

void composite_route_internal_packet_dispatcher_t::add (
  std::shared_ptr<route_internal_packet_dispatcher_t> dispatcher)
{
    if (!dispatcher) {
        return;
    }
    _dispatchers.push_back (dispatcher.get ());
    _owned_dispatchers.push_back (std::move (dispatcher));
}

bool composite_route_internal_packet_dispatcher_t::can_handle_send (
  std::string_view packet_name) const
{
    return resolve_send (packet_name) != nullptr;
}

bool composite_route_internal_packet_dispatcher_t::can_handle_request (
  std::string_view packet_name) const
{
    return resolve_request (packet_name) != nullptr;
}

result_t<void> composite_route_internal_packet_dispatcher_t::dispatch_send (
  const route_received_packet_t &received, service_provider_t &services) const
{
    auto header = runtime::messaging::envelope_codec_t{}.decode_header (received.parts);
    if (!header) {
        return detail::propagate_failure<void> (header, "route internal send header decode failed");
    }
    const auto *dispatcher = resolve_send (header.value ().message_name);
    if (dispatcher == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "routed internal send packet is not supported");
    }
    return dispatcher->dispatch_send (received, services);
}

result_t<zlink::message_t> composite_route_internal_packet_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services) const
{
    const auto *dispatcher = resolve_request (header.message_name);
    if (dispatcher == nullptr) {
        return result_t<zlink::message_t>::failure (
          framework_error_kind_t::not_found,
          "routed internal request packet is not supported");
    }
    return dispatcher->dispatch_request (received, header, services);
}

const route_internal_packet_dispatcher_t *
composite_route_internal_packet_dispatcher_t::resolve_send (std::string_view packet_name) const
{
    for (const auto *dispatcher : _dispatchers) {
        if (dispatcher->can_handle_send (packet_name)) {
            return dispatcher;
        }
    }
    return nullptr;
}

const route_internal_packet_dispatcher_t *
composite_route_internal_packet_dispatcher_t::resolve_request (std::string_view packet_name) const
{
    for (const auto *dispatcher : _dispatchers) {
        if (dispatcher->can_handle_request (packet_name)) {
            return dispatcher;
        }
    }
    return nullptr;
}

} // namespace zlink::framework::detail
