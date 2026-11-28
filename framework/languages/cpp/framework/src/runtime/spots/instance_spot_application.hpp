/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

namespace zlink::framework::runtime::messaging
{

struct instance_spot_application_t
{
    envelope_header_t header;
    zlink::message_t body;
};

inline protocol::application_payload_t
encode_instance_spot_application (const envelope_header_t &header, zlink::message_t body)
{
    // Command 39 carries flow fields in ChannelHeader inside the v1 multipart envelope.
    return protocol::application_payload_t::from_parts (
      envelope_codec_t{}.encode_raw_body_parts (header, std::move (body)).take_items ());
}

inline result_t<instance_spot_application_t> decode_instance_spot_application (
  const protocol::application_payload_t &payload, message_kind_t kind, bool capture_flow = false)
{
    if (payload.packet_name () != protocol::framework_multipart_packet_name) {
        // Earlier C++ versions persisted inline payloads in activation recovery records.
        envelope_header_t header{.kind = kind,
                                 .message_name = payload.packet_name (),
                                 .content_type = payload.content_type ()};
        if (capture_flow && payload.flow_id) {
            header.flow_id = *payload.flow_id;
            header.flow_origin = payload.flow_origin;
        }
        return result_t<instance_spot_application_t>::success (
          {std::move (header), zlink::message_t::from (payload.payload_bytes ())});
    }

    try {
        auto parts = protocol::decode_application_parts (payload);
        if (parts.size () == 2) {
            auto header = envelope_codec_t{}.decode_header (parts.front (), capture_flow);
            if (header && header.value ().kind == kind)
                return result_t<instance_spot_application_t>::success (
                  {std::move (header.value ()), std::move (parts.back ())});
            if (!header)
                return detail::result_access_t::failure<instance_spot_application_t> (
                  header.exception ());
        }
    }
    catch (const protocol::service_wire_error_t &error) {
        return result_t<instance_spot_application_t>::failure (
          framework_error_kind_t::protocol_error, error.what ());
    }
    return result_t<instance_spot_application_t>::failure (
      framework_error_kind_t::protocol_error, "Invalid Instance Spot application envelope");
}

} // namespace zlink::framework::runtime::messaging
