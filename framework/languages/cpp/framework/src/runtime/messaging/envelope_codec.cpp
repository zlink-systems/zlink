/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::messaging
{

namespace
{

template <typename T>
std::optional<T> optional_json_value (const nlohmann::json &json, const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<T> ();
}

} // namespace

message_parts_t::message_parts_t (zlink::message_t header, zlink::message_t body)
{
    _parts.push_back (std::move (header));
    _parts.push_back (std::move (body));
}

message_parts_t::message_parts_t (std::vector<zlink::message_t> parts) : _parts (std::move (parts))
{
}

const zlink::message_t &message_parts_t::operator[] (std::size_t index) const
{
    if (index >= _parts.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope part is missing");
    }
    return _parts[index];
}

message_parts_t envelope_codec_t::encode_raw_body_parts (const envelope_header_t &header,
                                                         zlink::message_t body) const
{
    return message_parts_t (encode_header (header), std::move (body));
}

message_parts_t envelope_codec_t::encode_parts (const envelope_header_t &header,
                                                std::type_index body_type,
                                                const void *body,
                                                const serializer_registry_t &serializers) const
{
    if (body == nullptr) {
        return encode_raw_body_parts (header, zlink::message_t::from (""));
    }
    auto typed_header = header;
    typed_header.content_type = serializers.content_type (body_type);
    return encode_raw_body_parts (
      typed_header, detail::encoded_payload_to_raw (serializers.serialize (body_type, body)));
}

namespace
{

result_t<void> validate_protocol_header (const envelope_header_t &header,
                                         int format_marker)
{
    if (format_marker != static_cast<int> (flow_id_t::format_marker)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope format marker is invalid");
    }
    if (header.flow_id.has_value () != header.flow_origin.has_value ()) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "ZLink envelope flow id and origin must be present together");
    }
    if (header.flow_id && !flow_id_t::is_valid (*header.flow_id)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope flow id must be UUIDv7");
    }
    if (header.flow_origin) {
        const auto raw = static_cast<std::uint8_t> (*header.flow_origin);
        if (raw < 1 || raw > 4) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope flow origin is invalid");
        }
    }
    return result_t<void>::success ();
}

} // namespace

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const
{
    auto stamped = header;
    if (!stamped.flow_id) {
        if (const auto &flow = flow_context_t::current ()) {
            stamped.flow_id = flow->flow_id;
            stamped.flow_origin = flow->origin;
        }
    }
    if (auto valid = validate_protocol_header (stamped, flow_id_t::format_marker); !valid) {
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    }
    nlohmann::json json{
      {"formatMarker", static_cast<int> (flow_id_t::format_marker)},
      {"flowId",
       stamped.flow_id ? nlohmann::json (*stamped.flow_id) : nlohmann::json (nullptr)},
      {"flowOrigin", stamped.flow_origin
                       ? nlohmann::json (static_cast<int> (*stamped.flow_origin))
                       : nlohmann::json (nullptr)},
      {"kind", static_cast<int> (stamped.kind)},
      {"channelName", stamped.channel_name},
      {"messageName", stamped.message_name},
      {"contentType", stamped.content_type},
      {"correlationId", stamped.correlation_id.empty () ? nlohmann::json (nullptr)
                                                        : nlohmann::json (stamped.correlation_id)},
      {"deadline",
       stamped.deadline ? nlohmann::json (*stamped.deadline) : nlohmann::json (nullptr)},
      {"topic", stamped.topic ? nlohmann::json (*stamped.topic) : nlohmann::json (nullptr)},
      {"errorCode",
       stamped.error_code ? nlohmann::json (*stamped.error_code) : nlohmann::json (nullptr)},
      {"errorMessage",
       stamped.error_message ? nlohmann::json (*stamped.error_message) : nlohmann::json (nullptr)},
      {"source", stamped.source ? nlohmann::json (*stamped.source) : nlohmann::json (nullptr)},
      {"metadata",
       stamped.metadata.empty () ? nlohmann::json::object () : nlohmann::json (stamped.metadata)}};
    return zlink::message_t::from (json.dump ());
}

result_t<envelope_header_t> envelope_codec_t::decode_header (const zlink::message_t &message) const
{
    try {
        const auto json = nlohmann::json::parse (message.to_string ());
        envelope_header_t header;
        header.kind = static_cast<message_kind_t> (json.at ("kind").get<int> ());
        header.channel_name = json.at ("channelName").get<std::string> ();
        header.message_name = json.at ("messageName").get<std::string> ();
        header.content_type = json.value<std::string> ("contentType", default_content_type);
        header.correlation_id =
          json.contains ("correlationId") && !json.at ("correlationId").is_null ()
            ? json.at ("correlationId").get<std::string> ()
            : std::string{};
        header.deadline = optional_json_value<std::string> (json, "deadline");
        header.topic = optional_json_value<std::string> (json, "topic");
        header.error_code = optional_json_value<std::string> (json, "errorCode");
        header.error_message = optional_json_value<std::string> (json, "errorMessage");
        header.source = optional_json_value<std::string> (json, "source");
        if (json.contains ("metadata") && json.at ("metadata").is_object ()) {
            header.metadata = json.at ("metadata").get<std::map<std::string, std::string>> ();
        }
        header.flow_id = optional_json_value<std::string> (json, "flowId");
        if (const auto origin = optional_json_value<int> (json, "flowOrigin")) {
            header.flow_origin = static_cast<flow_origin_t> (*origin);
        }
        const auto format_marker = json.contains ("formatMarker") && !json.at ("formatMarker").is_null ()
                                     ? json.at ("formatMarker").get<int> ()
                                     : 0;
        if (auto valid = validate_protocol_header (header, format_marker); !valid) {
            return result_t<envelope_header_t>::failure (valid.error_kind (),
                                                         valid.error ()->what ());
        }
        return result_t<envelope_header_t>::success (std::move (header));
    }
    catch (const std::exception &ex) {
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     std::string ("invalid ZLink envelope header: ")
                                                       + ex.what ());
    }
}

result_t<envelope_header_t> envelope_codec_t::decode_header (const message_parts_t &parts) const
{
    if (parts.size () == 0) {
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header part is missing");
    }
    return decode_header (parts[0]);
}

result_t<zlink::message_t> envelope_codec_t::decode_body (const message_parts_t &parts) const
{
    if (parts.size () < 2) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::protocol_error,
                                                    "ZLink envelope body part is missing");
    }
    return result_t<zlink::message_t>::success (parts[1]);
}

} // namespace zlink::framework::runtime::messaging
