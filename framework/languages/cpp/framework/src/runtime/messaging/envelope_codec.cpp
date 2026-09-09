/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
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

bool valid_error_code (const std::string &value) noexcept
{
    return value == "not_found" || value == "already_exists"
           || value == "type_mismatch" || value == "not_configured"
           || value == "rejected" || value == "unavailable"
           || value == "capacity_exceeded" || value == "deadline_exceeded"
           || value == "shutting_down" || value == "protocol_error"
           || value == "invalid_operation" || value == "data_lost"
           || value == "internal_failure";
}

result_t<void> validate_protocol_header (const envelope_header_t &header,
                                         int format_marker,
                                         const std::string *flow_id,
                                         const std::optional<flow_origin_t> &flow_origin)
{
    if (format_marker != static_cast<int> (flow_id_t::format_marker)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope format marker is invalid");
    }
    if ((flow_id != nullptr) != flow_origin.has_value ()) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "ZLink envelope flow id and origin must be present together");
    }
    if (flow_id && !flow_id_t::is_valid (*flow_id)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope flow id must be UUIDv7");
    }
    if (flow_origin) {
        const auto raw = static_cast<std::uint8_t> (*flow_origin);
        if (raw < 1 || raw > 4) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope flow origin is invalid");
        }
    }
    if (header.kind == message_kind_t::error
        && (!header.error_code || !valid_error_code (*header.error_code))) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "ZLink error envelope requires a recognized errorCode");
    }
    return result_t<void>::success ();
}

// The envelope has a fixed field order. Count the escaped bytes first, then
// write directly into its native message; there is no header DOM or byte copy.
class header_writer_t
{
  public:
    explicit header_writer_t (char *output = nullptr) : _output (output) {}

    void append (std::string_view text)
    {
        if (_output && !text.empty ())
            std::memcpy (_output + _size, text.data (), text.size ());
        _size += text.size ();
    }

    void number (int value)
    {
        char buffer[std::numeric_limits<int>::digits10 + 3];
        const auto result = std::to_chars (buffer, buffer + sizeof (buffer), value);
        append (std::string_view (buffer, result.ptr - buffer));
    }

    void string (const std::string &value)
    {
        if (!_output && !valid_utf8 (value)) {
            // Keep the JSON library's strict UTF-8 exception and diagnostic.
            // Only malformed input takes this path; valid headers have no DOM.
            (void) nlohmann::json (value).dump ();
        }
        append ("\"");
        std::size_t start = 0;
        for (std::size_t index = 0; index < value.size (); ++index) {
            const auto byte = static_cast<unsigned char> (value[index]);
            if (byte >= 0x20 && byte != '"' && byte != '\\')
                continue;
            append (std::string_view (value).substr (start, index - start));
            switch (byte) {
                case '"': append ("\\\""); break;
                case '\\': append ("\\\\"); break;
                case '\b': append ("\\b"); break;
                case '\f': append ("\\f"); break;
                case '\n': append ("\\n"); break;
                case '\r': append ("\\r"); break;
                case '\t': append ("\\t"); break;
                default: {
                    constexpr char hex[] = "0123456789abcdef";
                    const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
                    append (std::string_view (escaped, sizeof (escaped)));
                    break;
                }
            }
            start = index + 1;
        }
        append (std::string_view (value).substr (start));
        append ("\"");
    }

    void optional_string (const std::optional<std::string> &value)
    {
        if (value)
            string (*value);
        else
            append ("null");
    }

    std::size_t size () const noexcept { return _size; }

  private:
    static bool valid_utf8 (std::string_view value) noexcept
    {
        for (std::size_t index = 0; index < value.size ();) {
            const auto lead = static_cast<unsigned char> (value[index++]);
            if (lead < 0x80)
                continue;
            const auto remaining = lead >= 0xc2 && lead <= 0xdf ? 1
                                   : lead >= 0xe0 && lead <= 0xef ? 2
                                   : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
            if (remaining == 0 || value.size () - index < static_cast<std::size_t> (remaining))
                return false;
            const auto second = static_cast<unsigned char> (value[index]);
            if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)
                || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
                return false;
            for (int part = 0; part < remaining; ++part) {
                const auto byte = static_cast<unsigned char> (value[index++]);
                if (byte < 0x80 || byte > 0xbf)
                    return false;
            }
        }
        return true;
    }

    char *_output;
    std::size_t _size = 0;
};

void write_header (header_writer_t &writer,
                   const envelope_header_t &header,
                   const std::string *flow_id,
                   const std::optional<flow_origin_t> &flow_origin)
{
    writer.append ("{\"channelName\":");
    writer.string (header.channel_name);
    writer.append (",\"contentType\":");
    writer.string (header.content_type);
    writer.append (",\"correlationId\":");
    if (header.correlation_id.empty ())
        writer.append ("null");
    else
        writer.string (header.correlation_id);
    writer.append (",\"deadline\":");
    writer.optional_string (header.deadline);
    writer.append (",\"errorCode\":");
    writer.optional_string (header.error_code);
    writer.append (",\"errorMessage\":");
    writer.optional_string (header.error_message);
    writer.append (",\"flowId\":");
    if (flow_id)
        writer.string (*flow_id);
    else
        writer.append ("null");
    writer.append (",\"flowOrigin\":");
    if (flow_origin)
        writer.number (static_cast<int> (*flow_origin));
    else
        writer.append ("null");
    writer.append (",\"formatMarker\":");
    writer.number (static_cast<int> (flow_id_t::format_marker));
    writer.append (",\"kind\":");
    writer.number (static_cast<int> (header.kind));
    writer.append (",\"messageName\":");
    writer.string (header.message_name);
    writer.append (",\"metadata\":{");
    bool first = true;
    for (const auto &[name, value] : header.metadata) {
        if (!first)
            writer.append (",");
        first = false;
        writer.string (name);
        writer.append (":");
        writer.string (value);
    }
    writer.append ("},\"source\":");
    writer.optional_string (header.source);
    writer.append (",\"topic\":");
    writer.optional_string (header.topic);
    writer.append ("}");
}

} // namespace

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const
{
    const auto &flow = flow_context_t::current ();
    const bool stamp_flow = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *flow_id = stamp_flow ? &flow->flow_id
                                    : header.flow_id ? &*header.flow_id : nullptr;
    const auto flow_origin = stamp_flow ? std::optional<flow_origin_t> (flow->origin)
                                       : header.flow_origin;
    if (auto valid = validate_protocol_header (header, flow_id_t::format_marker,
                                                flow_id, flow_origin); !valid) {
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    }
    header_writer_t measure;
    write_header (measure, header, flow_id, flow_origin);
    auto message = zlink::message_t::allocate (measure.size ());
    if (!message.valid ())
        throw std::bad_alloc ();
    header_writer_t writer (reinterpret_cast<char *> (message.data ()));
    write_header (writer, header, flow_id, flow_origin);
    return message;
}

result_t<envelope_header_t> envelope_codec_t::decode_header (const zlink::message_t &message,
                                                             bool capture_flow) const
{
    try {
        const auto bytes = message.bytes ();
        const auto json = nlohmann::json::parse (bytes.begin (), bytes.end ());
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
        /* flow-correlation §4: the flow pair is observation-only. It is
         * materialized and validated only when the caller can consume it
         * (tracing on at a flow processing point); at Off the fields are
         * ignored entirely so malformed flow data cannot reject the frame. */
        if (capture_flow) {
            header.flow_id = optional_json_value<std::string> (json, "flowId");
            if (const auto origin = optional_json_value<int> (json, "flowOrigin")) {
                header.flow_origin = static_cast<flow_origin_t> (*origin);
            }
        }
        const auto format_marker = json.contains ("formatMarker") && !json.at ("formatMarker").is_null ()
                                     ? json.at ("formatMarker").get<int> ()
                                     : 0;
        if (auto valid = validate_protocol_header (header, format_marker,
                                                    header.flow_id ? &*header.flow_id : nullptr,
                                                    header.flow_origin); !valid) {
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

result_t<envelope_header_t> envelope_codec_t::decode_header (const message_parts_t &parts,
                                                             bool capture_flow) const
{
    if (parts.size () == 0) {
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header part is missing");
    }
    return decode_header (parts[0], capture_flow);
}

result_t<zlink::message_t> envelope_codec_t::decode_body (const message_parts_t &parts) const
{
    if (parts.size () < 2) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::protocol_error,
                                                    "ZLink envelope body part is missing");
    }
    return result_t<zlink::message_t>::success (parts[1].copy ());
}

} // namespace zlink::framework::runtime::messaging
