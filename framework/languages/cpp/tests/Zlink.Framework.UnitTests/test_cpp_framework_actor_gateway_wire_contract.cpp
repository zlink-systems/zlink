/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <nlohmann/json.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int actor_typed_wire_header_contract ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    // STREAM JSON Request with request_seq=1, as emitted by Java/.NET/Node.
    const std::vector<std::uint8_t> request_header{
      0xf2, 2,   1,   1,   0,   0,   0,   0,   0,   0,   0,   1,   20,  'B', 'e', 'g', 'i',
      'n',  'U', 's', 'e', 'r', 'S', 'p', 'o', 't', 'J', 'o', 'i', 'n', 'R', 'e', 'q'};
    for (const auto kind : {host::record_kind_t::actor_request, host::record_kind_t::actor_send}) {
        auto bytes = request_header;
        if (kind == host::record_kind_t::actor_send) {
            bytes[1] = 1;
            bytes[3] = 0;
            bytes.erase (bytes.begin () + 4, bytes.begin () + 12);
        }
        std::vector<zlink::message_t> parts{zlink::message_t::from (bytes),
                                            zlink::message_t::from ("{}")};
        host::receive_record_t record{.kind = kind};
        auto decoded = decode_mesh_record_header (record, parts, false);
        if (!decoded || !decoded.value ()
            || decoded.value ()->message_name != "BeginUserSpotJoinReq" || parts.size () != 2
            || parts[1].to_string () != "{}") {
            std::cerr << "Actor STREAM header was not accepted as BeginUserSpotJoinReq\n";
            return 1;
        }
        auto wrong_kind = request_header;
        if (kind == host::record_kind_t::actor_request) {
            wrong_kind[1] = 1;
            wrong_kind[3] = 0;
            wrong_kind.erase (wrong_kind.begin () + 4, wrong_kind.begin () + 12);
        }
        parts = {zlink::message_t::from (wrong_kind), zlink::message_t::from ("{}")};
        if (decode_mesh_record_header (record, parts, false))
            return 12;
        for (const auto invalid : {std::string ("BeginUserSpotJoinReq"),
                                   std::string ("{\"messageName\":\"BeginUserSpotJoinReq\"}")}) {
            parts = {zlink::message_t::from (invalid), zlink::message_t::from ("{}")};
            if (decode_mesh_record_header (record, parts, false)) {
                std::cerr << "Actor non-STREAM header was accepted\n";
                return 2;
            }
        }
    }
    using namespace zlink::framework::detail;
    messaging::envelope_header_t request;
    request.kind = messaging::message_kind_t::request;
    request.message_name = "BeginUserSpotJoinReq";
    auto outgoing = encode_actor_packet (request, zlink::message_t::from ("{}"));
    if (outgoing[0].to_bytes () != request_header)
        return 3;
    // Independent complete STREAM frames, with seq=1 and no packet name.
    const std::vector<std::uint8_t> response{0, 13, 0, 0, 0, 2, 0xf2, 3, 1,   1,  0,
                                             0, 0,  0, 0, 0, 0, 1,    0, '{', '}'};
    auto decoded_reply = decode_actor_reply (messaging::message_parts_t (
      std::vector<zlink::message_t>{zlink::message_t::from (response)}));
    if (!decoded_reply || !decoded_reply.value () || decoded_reply.value ()->to_string () != "{}")
        return 4;
    messaging::envelope_header_t reply;
    reply.kind = messaging::message_kind_t::response;
    if (encode_actor_reply (reply, zlink::message_t::from ("{}"))[0].to_bytes () != response)
        return 5;
    for (auto bad : {std::vector<zlink::message_t>{},
                     std::vector<zlink::message_t>{zlink::message_t::from ("{}")},
                     std::vector<zlink::message_t>{outgoing[0], outgoing[1]}}) {
        auto invalid = decode_actor_reply (messaging::message_parts_t (std::move (bad)));
        if (invalid || invalid.error_kind () != framework_error_kind_t::protocol_error)
            return 6;
    }
    const std::string foreign_error = "{\"code\":\"not_found\",\"message\":\"missing\"}";
    auto foreign_error_frame = response;
    foreign_error_frame.resize (19);
    foreign_error_frame[5] = static_cast<std::uint8_t> (foreign_error.size ());
    foreign_error_frame[7] = 4;
    foreign_error_frame.insert (foreign_error_frame.end (), foreign_error.begin (),
                                foreign_error.end ());
    auto foreign_failure = decode_actor_reply (messaging::message_parts_t (
      std::vector<zlink::message_t>{zlink::message_t::from (foreign_error_frame)}));
    if (foreign_failure || foreign_failure.error_kind () != framework_error_kind_t::not_found)
        return 10;
    for (const auto offset : {0u, 5u, 7u, 8u, 18u}) {
        auto malformed = response;
        malformed[offset] = 0xff;
        auto rejected = decode_actor_reply (messaging::message_parts_t (
          std::vector<zlink::message_t>{zlink::message_t::from (malformed)}));
        if (rejected || rejected.error_kind () != framework_error_kind_t::protocol_error)
            return 11;
    }
    for (const auto kind :
         {framework_error_kind_t::not_found, framework_error_kind_t::not_configured,
          framework_error_kind_t::rejected, framework_error_kind_t::deadline_exceeded}) {
        const auto error = channel_reply_writer_t{}.create_error_header (
          "actor", request, framework_exception_t (kind, "missing"));
        auto framed = encode_actor_reply (error, zlink::message_t{});
        if (framed.size () != 1)
            return 7;
        auto [header, body] = stream_runtime_t::decode_frame (framed.items (), false);
        if (header.kind () != stream_message_kind_t::error || !header.packet_name ().empty ()
            || header.codec () != stream_codec_t::json)
            return 8;
        auto decoded_error = decode_actor_reply (framed);
        if (decoded_error || decoded_error.error_kind () != kind
            || std::string (decoded_error.error ()->what ()) != "missing")
            return 9;
    }
    {
        const std::string flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
        runtime::flow_context_t::scope_t flow (runtime::flow_value_t{
          flow_id, flow_origin_t::application, message_flow_log_mode_t::normal});
        auto flowed = encode_actor_packet (request, zlink::message_t::from ("{}"));
        auto header = stream_runtime_t::decode_header (flowed[0].to_bytes (), true);
        if (!header || header.value ().flow_id () != flow_id
            || header.value ().flow_origin () != flow_origin_t::application) {
            std::cerr << "Actor STREAM header lost the ambient flow\n";
            return 13;
        }
        auto framed = encode_actor_reply (reply, zlink::message_t::from ("{}"));
        const auto reply_header = stream_runtime_t::decode_frame (framed.items (), true).first;
        if (reply_header.flow_id () != flow_id
            || reply_header.flow_origin () != flow_origin_t::application)
            return 14;
    }
    {
        runtime::flow_context_t::scope_t flow (
          runtime::flow_value_t{"01890a5d-ac96-774b-bcce-b302099a8057", flow_origin_t::application,
                                message_flow_log_mode_t::off});
        auto off = encode_actor_packet (request, zlink::message_t::from ("{}"));
        if (off[0].to_bytes () != request_header)
            return 15;
    }
    return 0;
}

int actor_reply_framing_vectors ()
{
    auto root = std::filesystem::path (__FILE__).parent_path ();
    const auto relative = "framework/runtime/conformance/actor-reply-framing-v1.json";
    while (!root.empty () && !std::filesystem::is_regular_file (root / relative)) {
        const auto parent = root.parent_path ();
        if (parent == root)
            return 1;
        root = parent;
    }
    std::ifstream input (root / relative);
    if (!input)
        return 1;
    const auto fixture = nlohmann::json::parse (input);
    for (const auto &vector : fixture.at ("cases")) {
        std::vector<zlink::message_t> parts;
        for (const auto &part : vector.at ("parts")) {
            const auto hex = part.get<std::string> ();
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < hex.size (); i += 2)
                bytes.push_back (
                  static_cast<std::uint8_t> (std::stoul (hex.substr (i, 2), nullptr, 16)));
            parts.push_back (zlink::message_t::from (bytes));
        }
        const auto reply = zlink::framework::detail::decode_actor_reply (
          zlink::framework::runtime::messaging::message_parts_t (std::move (parts)));
        if (static_cast<bool> (reply) != vector.at ("accepted").get<bool> ())
            return 2;
        if (!reply
            && reply.error_kind () != zlink::framework::framework_error_kind_t::protocol_error)
            return 3;
        if (reply && (!reply.value () || reply.value ()->to_string () != "{\"value\":\"pong\"}"))
            return 4;
    }
    return 0;
}

int actor_error_payload_vectors ()
{
    using namespace zlink::framework;
    auto root = std::filesystem::path (__FILE__).parent_path ();
    const auto relative = "framework/runtime/conformance/actor-error-payload-v1.json";
    while (!root.empty () && !std::filesystem::is_regular_file (root / relative)) {
        const auto parent = root.parent_path ();
        if (parent == root)
            return 1;
        root = parent;
    }
    std::ifstream input (root / relative);
    if (!input)
        return 1;
    const auto fixture = nlohmann::json::parse (input);
    for (const auto &vector : fixture.at ("cases")) {
        std::vector<zlink::message_t> parts;
        for (const auto &part : vector.at ("parts")) {
            const auto hex = part.get<std::string> ();
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < hex.size (); i += 2)
                bytes.push_back (
                  static_cast<std::uint8_t> (std::stoul (hex.substr (i, 2), nullptr, 16)));
            parts.push_back (zlink::message_t::from (bytes));
        }
        const auto expected =
          static_cast<framework_error_kind_t> (vector.at ("expectedKind").get<int> ());
        const auto reply =
          detail::decode_actor_reply (runtime::messaging::message_parts_t (std::move (parts)));
        if (reply || reply.error_kind () != expected) {
            std::cerr << "Actor Error vector failed: " << vector.at ("id") << '\n';
            return 2;
        }
        if (vector.at ("valid").get<bool> ()) {
            const auto message = vector.at ("message").get<std::string> ();
            if (reply.error ()->what () != message)
                return 3;
            if (expected == framework_error_kind_t::unavailable
                && reply.error ()->code () != std::make_error_code (std::errc::not_connected))
                return 5;
            runtime::messaging::envelope_header_t header;
            header.kind = runtime::messaging::message_kind_t::error;
            header.error_code = detail::stream_error_code (expected);
            header.error_message = message;
            auto encoded = detail::encode_actor_reply (header, zlink::message_t::from (""));
            auto [wire, body] = detail::stream_runtime_t::decode_frame (encoded.items (), false);
            const auto payload = nlohmann::json::parse (body.to_string ());
            if (wire.codec () != stream_codec_t::json || payload.at ("code") != vector.at ("code")
                || payload.at ("message") != message)
                return 4;
        }
    }
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
