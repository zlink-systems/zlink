/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::messaging
{

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
           || value == "deadline_exceeded"
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
    explicit header_writer_t (std::string &output) : _string_output (&output) {}

    void append (std::string_view text)
    {
        if (_output && !text.empty ())
            std::memcpy (_output + _size, text.data (), text.size ());
        else if (_string_output != nullptr)
            _string_output->append (text);
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

    char *_output = nullptr;
    std::string *_string_output = nullptr;
    std::size_t _size = 0;
};

struct header_plan_t
{
    std::optional<message_kind_t> kind;
    std::string channel_name;
    std::string message_name;
    std::string content_type;
    std::string prefix;
    std::string middle;

    bool matches (const envelope_header_t &header) const noexcept
    {
        return kind && *kind == header.kind && channel_name == header.channel_name
               && message_name == header.message_name && content_type == header.content_type;
    }

    void rebuild (const envelope_header_t &header)
    {
        // Keep the one-entry backing storage on this thread.  `kind` is the
        // validity marker and is published only after every escaped segment
        // and key has been rebuilt successfully.
        kind.reset ();
        channel_name.clear ();
        message_name.clear ();
        content_type.clear ();
        prefix.clear ();
        middle.clear ();

        channel_name = header.channel_name;
        message_name = header.message_name;
        content_type = header.content_type;

        header_writer_t prefix_writer (prefix);
        prefix_writer.append ("{\"channelName\":");
        prefix_writer.string (channel_name);
        prefix_writer.append (",\"contentType\":");
        prefix_writer.string (content_type);
        prefix_writer.append (",\"correlationId\":");

        header_writer_t middle_writer (middle);
        middle_writer.append (",\"formatMarker\":");
        middle_writer.number (static_cast<int> (flow_id_t::format_marker));
        middle_writer.append (",\"kind\":");
        middle_writer.number (static_cast<int> (header.kind));
        middle_writer.append (",\"messageName\":");
        middle_writer.string (message_name);
        middle_writer.append (",\"metadata\":{");
        kind = header.kind;
    }
};

header_plan_t &header_plan ()
{
    thread_local header_plan_t value;
    return value;
}

void write_header (header_writer_t &writer,
                   const envelope_header_t &header,
                   const std::string *flow_id,
                   const std::optional<flow_origin_t> &flow_origin,
                   const header_plan_t &plan)
{
    writer.append (plan.prefix);
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
    writer.append (plan.middle);
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
    auto &plan = header_plan ();
    if (!plan.matches (header))
        plan.rebuild (header);
    header_writer_t measure;
    write_header (measure, header, flow_id, flow_origin, plan);
    auto message = zlink::message_t::allocate (measure.size ());
    if (!message.valid ())
        throw std::bad_alloc ();
    header_writer_t writer (reinterpret_cast<char *> (message.data ()));
    write_header (writer, header, flow_id, flow_origin, plan);
    return message;
}

namespace
{

enum class header_member_t
{
    none,
    kind,
    channel_name,
    message_name,
    content_type,
    correlation_id,
    deadline,
    topic,
    error_code,
    error_message,
    source,
    flow_id,
    flow_origin,
    format_marker,
    metadata
};

enum class header_value_t
{
    missing,
    null,
    string,
    number,
    other
};

struct header_scalar_t
{
    header_value_t type = header_value_t::missing;
    std::string string;
    int number = 0;

    void set_null ()
    {
        type = header_value_t::null;
        string.clear ();
    }

    void set_string (std::string value)
    {
        type = header_value_t::string;
        string = std::move (value);
    }

    void set_number (int value)
    {
        type = header_value_t::number;
        number = value;
        string.clear ();
    }

    void set_other ()
    {
        type = header_value_t::other;
        string.clear ();
    }
};

class header_sax_t final : public nlohmann::json_sax<nlohmann::json>
{
  public:
    explicit header_sax_t (bool capture_flow) : _capture_flow (capture_flow) {}

    bool null () override { return scalar ([] (header_scalar_t &value) { value.set_null (); }); }
    bool boolean (bool value) override
    {
        return scalar ([value] (header_scalar_t &target) { target.set_number (static_cast<int> (value)); });
    }
    bool number_integer (number_integer_t value) override
    {
        return scalar ([value] (header_scalar_t &target) { target.set_number (static_cast<int> (value)); });
    }
    bool number_unsigned (number_unsigned_t value) override
    {
        return scalar ([value] (header_scalar_t &target) { target.set_number (static_cast<int> (value)); });
    }
    bool number_float (number_float_t value, const string_t &) override
    {
        return scalar ([value] (header_scalar_t &target) { target.set_number (static_cast<int> (value)); });
    }
    bool string (string_t &value) override
    {
        return scalar ([&value] (header_scalar_t &target) { target.set_string (std::move (value)); });
    }
    bool binary (binary_t &) override { return scalar ([] (header_scalar_t &value) { value.set_other (); }); }

    bool start_object (std::size_t) override
    {
        if (_depth == 0) {
            _root_object = true;
        } else if (_depth == 1 && _member == header_member_t::metadata) {
            _metadata_object_depth = _depth + 1;
            clear_metadata ();
            _metadata_is_object = true;
            _member = header_member_t::none;
        } else {
            container_value ();
        }
        ++_depth;
        return true;
    }

    bool key (string_t &value) override
    {
        if (_depth == 1) {
            _member = member (value);
        } else if (_depth == _metadata_object_depth) {
            _metadata_key = std::move (value);
        }
        return true;
    }

    bool end_object () override
    {
        if (_depth == _metadata_object_depth) {
            _metadata_object_depth = 0;
        }
        if (_depth == 1)
            _root_closed = true;
        --_depth;
        return true;
    }

    bool start_array (std::size_t) override
    {
        container_value ();
        ++_depth;
        return true;
    }

    bool end_array () override
    {
        --_depth;
        return true;
    }

    bool parse_error (std::size_t,
                      const std::string &,
                      const nlohmann::detail::exception &error) override
    {
        _parse_error = error.what ();
        return false;
    }

    result_t<envelope_header_t> finish ()
    {
        if (!_parse_error.empty ())
            return failure (_parse_error);
        if (!_root_object || !_root_closed || _depth != 0)
            return failure ("ZLink envelope header must be a JSON object");
        if (_kind.type != header_value_t::number || _channel_name.type != header_value_t::string
            || _message_name.type != header_value_t::string)
            return failure ("ZLink envelope header has invalid required fields");
        if (_content_type.type != header_value_t::missing
            && _content_type.type != header_value_t::string)
            return failure ("ZLink envelope header contentType is invalid");
        if (!optional_string (_correlation_id) || !optional_string (_deadline)
            || !optional_string (_topic) || !optional_string (_error_code)
            || !optional_string (_error_message) || !optional_string (_source)
            || !number_or_default (_format_marker))
            return failure ("ZLink envelope header has an invalid field type");
        if (_capture_flow
            && (!optional_string (_flow_id) || !number_or_default (_flow_origin)))
            return failure ("ZLink envelope header flow fields are invalid");
        if (!_invalid_metadata_keys.empty ())
            return failure ("ZLink envelope header metadata value is invalid");

        envelope_header_t header;
        header.kind = static_cast<message_kind_t> (_kind.number);
        header.channel_name = std::move (_channel_name.string);
        header.message_name = std::move (_message_name.string);
        header.content_type = _content_type.type == header_value_t::missing
                                ? envelope_codec_t::default_content_type
                                : std::move (_content_type.string);
        header.correlation_id = std::move (_correlation_id.string);
        header.deadline = take_optional (_deadline);
        header.topic = take_optional (_topic);
        header.error_code = take_optional (_error_code);
        header.error_message = take_optional (_error_message);
        header.source = take_optional (_source);
        if (_metadata_is_object)
            header.metadata = std::move (_metadata);
        if (_capture_flow) {
            header.flow_id = take_optional (_flow_id);
            if (_flow_origin.type == header_value_t::number)
                header.flow_origin = static_cast<flow_origin_t> (_flow_origin.number);
        }
        const auto format_marker = _format_marker.type == header_value_t::number
                                     ? _format_marker.number : 0;
        if (auto valid = validate_protocol_header (header, format_marker,
                                                    header.flow_id ? &*header.flow_id : nullptr,
                                                    header.flow_origin); !valid) {
            return result_t<envelope_header_t>::failure (valid.error_kind (), valid.error ()->what ());
        }
        return result_t<envelope_header_t>::success (std::move (header));
    }

  private:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;
    using binary_t = nlohmann::json::binary_t;

    static header_member_t member (const std::string &name)
    {
        if (name == "kind") return header_member_t::kind;
        if (name == "channelName") return header_member_t::channel_name;
        if (name == "messageName") return header_member_t::message_name;
        if (name == "contentType") return header_member_t::content_type;
        if (name == "correlationId") return header_member_t::correlation_id;
        if (name == "deadline") return header_member_t::deadline;
        if (name == "topic") return header_member_t::topic;
        if (name == "errorCode") return header_member_t::error_code;
        if (name == "errorMessage") return header_member_t::error_message;
        if (name == "source") return header_member_t::source;
        if (name == "flowId") return header_member_t::flow_id;
        if (name == "flowOrigin") return header_member_t::flow_origin;
        if (name == "formatMarker") return header_member_t::format_marker;
        if (name == "metadata") return header_member_t::metadata;
        return header_member_t::none;
    }

    header_scalar_t *current ()
    {
        if (_depth != 1)
            return nullptr;
        switch (_member) {
            case header_member_t::kind: return &_kind;
            case header_member_t::channel_name: return &_channel_name;
            case header_member_t::message_name: return &_message_name;
            case header_member_t::content_type: return &_content_type;
            case header_member_t::correlation_id: return &_correlation_id;
            case header_member_t::deadline: return &_deadline;
            case header_member_t::topic: return &_topic;
            case header_member_t::error_code: return &_error_code;
            case header_member_t::error_message: return &_error_message;
            case header_member_t::source: return &_source;
            case header_member_t::flow_id: return _capture_flow ? &_flow_id : nullptr;
            case header_member_t::flow_origin: return _capture_flow ? &_flow_origin : nullptr;
            case header_member_t::format_marker: return &_format_marker;
            default: return nullptr;
        }
    }

    template <typename Set>
    bool scalar (Set set)
    {
        if (_metadata_object_depth != 0 && _depth == _metadata_object_depth) {
            header_scalar_t value;
            set (value);
            if (value.type == header_value_t::string)
                set_metadata_value (std::move (value.string));
            else
                set_metadata_invalid ();
        } else if (auto *value = current ()) {
            set (*value);
            _member = header_member_t::none;
        } else if (_depth == 1 && _member == header_member_t::metadata) {
            _metadata_is_object = false;
            clear_metadata ();
            _member = header_member_t::none;
        }
        return true;
    }

    void container_value ()
    {
        if (_metadata_object_depth != 0 && _depth == _metadata_object_depth) {
            set_metadata_invalid ();
        } else if (auto *value = current ()) {
            value->set_other ();
            _member = header_member_t::none;
        } else if (_depth == 1 && _member == header_member_t::metadata) {
            _metadata_is_object = false;
            clear_metadata ();
            _member = header_member_t::none;
        }
    }

    void set_metadata_value (std::string value)
    {
        _invalid_metadata_keys.erase (
          std::remove (_invalid_metadata_keys.begin (), _invalid_metadata_keys.end (), _metadata_key),
          _invalid_metadata_keys.end ());
        const auto existing = _metadata.find (_metadata_key);
        if (existing != _metadata.end ())
            existing->second = std::move (value);
        else
            _metadata.emplace (std::move (_metadata_key), std::move (value));
        _metadata_key.clear ();
    }

    void set_metadata_invalid ()
    {
        _metadata.erase (_metadata_key);
        _invalid_metadata_keys.push_back (std::move (_metadata_key));
        _metadata_key.clear ();
    }

    void clear_metadata ()
    {
        _metadata.clear ();
        _invalid_metadata_keys.clear ();
    }

    static bool optional_string (const header_scalar_t &value)
    {
        return value.type == header_value_t::missing || value.type == header_value_t::null
               || value.type == header_value_t::string;
    }

    static bool number_or_default (const header_scalar_t &value)
    {
        return value.type == header_value_t::missing || value.type == header_value_t::null
               || value.type == header_value_t::number;
    }

    static std::optional<std::string> take_optional (header_scalar_t &value)
    {
        if (value.type != header_value_t::string)
            return std::nullopt;
        return std::move (value.string);
    }

    static result_t<envelope_header_t> failure (std::string_view reason)
    {
        return result_t<envelope_header_t>::failure (
          framework_error_kind_t::protocol_error,
          std::string ("invalid ZLink envelope header: ") + std::string (reason));
    }

    bool _capture_flow;
    std::size_t _depth = 0;
    std::size_t _metadata_object_depth = 0;
    bool _root_object = false;
    bool _root_closed = false;
    bool _metadata_is_object = false;
    header_member_t _member = header_member_t::none;
    std::string _metadata_key;
    std::string _parse_error;
    std::map<std::string, std::string> _metadata;
    std::vector<std::string> _invalid_metadata_keys;
    header_scalar_t _kind;
    header_scalar_t _channel_name;
    header_scalar_t _message_name;
    header_scalar_t _content_type;
    header_scalar_t _correlation_id;
    header_scalar_t _deadline;
    header_scalar_t _topic;
    header_scalar_t _error_code;
    header_scalar_t _error_message;
    header_scalar_t _source;
    header_scalar_t _flow_id;
    header_scalar_t _flow_origin;
    header_scalar_t _format_marker;
};

} // namespace

result_t<envelope_header_t> envelope_codec_t::decode_header (const zlink::message_t &message,
                                                             bool capture_flow) const
{
    try {
        const auto bytes = message.bytes ();
        header_sax_t sax (capture_flow);
        (void) nlohmann::json::sax_parse (bytes.begin (), bytes.end (), &sax);
        return sax.finish ();
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
