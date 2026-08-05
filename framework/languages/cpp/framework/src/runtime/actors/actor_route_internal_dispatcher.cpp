/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/actors/actor_route_internal_dispatcher.hpp"

#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"

namespace zlink::framework::detail
{

actor_route_internal_dispatcher_t::actor_route_internal_dispatcher_t (
  actor_gateway_runtime_t runtime, serializer_registry_t &serializers) :
    _runtime (std::move (runtime)), _serializers (&serializers)
{
}

bool actor_route_internal_dispatcher_t::can_handle_send (std::string_view packet_name) const
{
    return packet_name == actor_bound_session_route_request_t::packet_name;
}

bool actor_route_internal_dispatcher_t::can_handle_request (std::string_view packet_name) const
{
    return packet_name == actor_bound_session_route_request_t::packet_name;
}

result_t<void>
actor_route_internal_dispatcher_t::dispatch_send (const route_received_packet_t &received,
                                                  service_provider_t &services) const
{
    (void) services;
    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        return result_t<void>::failure (body.error_kind (), body.error ()
                                                              ? body.error ()->what ()
                                                              : "actor route send body missing");
    }

    try {
        auto request = _serializers->get<actor_bound_session_route_request_t> ().deserialize (
          detail::encoded_payload_from_raw (body.value ()));
        auto actor_ref = actor_ref_from_bound_session_route (request);
        auto runtime = _runtime;
        auto updated = runtime.update_actor_ref (actor_ref);
        if (!updated) {
            return result_t<void>::failure (updated.error_kind (), updated.error ()
                                                                     ? updated.error ()->what ()
                                                                     : "actor ref update failed");
        }
        return runtime.dispatch_bound_session_send (actor_ref, request.packet_name_value,
                                                    request.codec,
                                                    zlink::message_t::from (request.payload));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        std::string ("actor route send decode failed: ")
                                          + error.what ());
    }
}

result_t<zlink::message_t> actor_route_internal_dispatcher_t::dispatch_request (
  const route_received_packet_t &received,
  const runtime::messaging::envelope_header_t &header,
  service_provider_t &services) const
{
    (void) header;
    (void) services;
    auto body = runtime::messaging::envelope_codec_t{}.decode_body (received.parts);
    if (!body) {
        return detail::propagate_failure<zlink::message_t> (body, "actor route request body missing");
    }

    try {
        auto request = _serializers->get<actor_bound_session_route_request_t> ().deserialize (
          detail::encoded_payload_from_raw (body.value ()));
        auto actor_ref = actor_ref_from_bound_session_route (request);
        auto runtime = _runtime;
        auto updated = runtime.update_actor_ref (actor_ref);
        if (!updated) {
            return detail::propagate_failure<zlink::message_t> (updated, "actor ref update failed");
        }
        auto dispatched = runtime.dispatch_bound_session_send (
          actor_ref, request.packet_name_value, request.codec,
          zlink::message_t::from (request.payload));
        if (!dispatched) {
            return result_t<zlink::message_t>::failure (
              dispatched.error_kind (), dispatched.error ()
                                          ? dispatched.error ()->what ()
                                          : "routed actor bound session send failed");
        }
        return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
          _serializers->get<actor_bound_session_route_reply_t> ().serialize (
            actor_bound_session_route_reply_t{.accepted = true})));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (const std::exception &error) {
        return result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error,
          std::string ("actor route request decode failed: ") + error.what ());
    }
}

} // namespace zlink::framework::detail
