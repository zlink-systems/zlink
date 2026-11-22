/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <channel-header-v1.pb.h>
#include <service_wire_constants.hpp>
#include <google/protobuf/wire_format_lite.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::protocol
{

struct application_envelope_encoder_t::state_t
{
    channel::v1::ChannelHeader header;
    zlink::message_t body;
    std::shared_ptr<const detail::request_type_plan_t> type;
    std::shared_ptr<const std::string> channel_fields;
    std::array<std::string_view, 2> fixed_fields;
};

} // namespace zlink::framework::runtime::protocol

namespace zlink::framework::runtime::messaging
{

message_parts_t::message_parts_t (zlink::message_t header, zlink::message_t body)
{
    auto &parts = std::get<std::vector<zlink::message_t>> (_parts);
    parts.reserve (2);
    parts.push_back (std::move (header));
    parts.push_back (std::move (body));
}

message_parts_t::message_parts_t (std::vector<zlink::message_t> parts) : _parts (std::move (parts))
{
}

message_parts_t
message_parts_t::from_envelope_encoder (protocol::application_envelope_encoder_t encode)
{
    message_parts_t result;
    result._parts = std::move (encode);
    return result;
}

protocol::application_payload_t message_parts_t::take_application_payload () &&
{
    if (auto *encode = std::get_if<protocol::application_envelope_encoder_t> (&_parts))
        return protocol::application_payload_t::from_envelope_encoder (std::move (*encode));
    return protocol::application_payload_t::from_parts (std::move (*this).take_items ());
}

message_parts_t::message_parts_t (const message_parts_t &other)
{
    if (const auto *encode =
          std::get_if<protocol::application_envelope_encoder_t> (&other._parts)) {
        _parts = *encode;
        return;
    }
    if (!std::holds_alternative<std::span<const std::span<const std::byte>>> (other._parts)) {
        _parts = other.items ();
        return;
    }
    auto &parts = std::get<std::vector<zlink::message_t>> (_parts);
    parts.reserve (other.size ());
    for (std::size_t index = 0; index < other.size (); ++index)
        parts.push_back (zlink::message_t::from (other.part_bytes (index)));
}

message_parts_t &message_parts_t::operator= (const message_parts_t &other)
{
    if (this != &other)
        *this = message_parts_t (other);
    return *this;
}

message_parts_t message_parts_t::borrow (const std::vector<zlink::message_t> &parts)
{
    message_parts_t result;
    result._parts = &parts;
    return result;
}

message_parts_t message_parts_t::borrow (std::span<const std::span<const std::byte>> parts)
{
    message_parts_t result;
    result._parts = parts;
    return result;
}

std::size_t message_parts_t::size () const noexcept
{
    return std::visit (
      [] (const auto &parts) {
          if constexpr (std::is_same_v<std::decay_t<decltype (parts)>,
                                       protocol::application_envelope_encoder_t>)
              return std::size_t{2};
          else if constexpr (std::is_pointer_v<std::decay_t<decltype (parts)>>)
              return parts->size ();
          else
              return parts.size ();
      },
      _parts);
}

const std::vector<zlink::message_t> &message_parts_t::items () const
{
    if (auto *encode = std::get_if<protocol::application_envelope_encoder_t> (&_parts))
        _parts = protocol::decode_application_parts ((*encode) ({}, {}), false);
    if (const auto *parts = std::get_if<const std::vector<zlink::message_t> *> (&_parts))
        return **parts;
    return std::get<std::vector<zlink::message_t>> (_parts);
}

std::vector<zlink::message_t> message_parts_t::take_items () &&
{
    if (auto *encode = std::get_if<protocol::application_envelope_encoder_t> (&_parts))
        return protocol::decode_application_parts ((*encode) ({}, {}), false);
    if (auto *parts = std::get_if<std::vector<zlink::message_t>> (&_parts))
        return std::move (*parts);
    return std::move (message_parts_t (*this)).take_items ();
}

std::span<const std::byte> message_parts_t::part_bytes (std::size_t index) const
{
    if (index >= size ())
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope part is missing");
    if (const auto *parts = std::get_if<std::span<const std::span<const std::byte>>> (&_parts))
        return (*parts)[index];
    return items ()[index].bytes ();
}

const zlink::message_t &message_parts_t::operator[] (std::size_t index) const
{
    (void) part_bytes (index);
    return items ()[index];
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
    const auto content_type = serializers.content_type (body_type);
    auto encoded = detail::encoded_payload_to_raw (serializers.serialize (body_type, body));
    return message_parts_t (encode_header (header, content_type), std::move (encoded));
}

const envelope_metadata_t::map_t &envelope_metadata_t::values () const
{
    if (_values)
        return *_values;
    static const map_t empty;
    return empty;
}

namespace
{

bool valid_utf8 (std::string_view text) noexcept
{
    for (std::size_t index = 0; index < text.size ();) {
        const auto lead = static_cast<unsigned char> (text[index++]);
        if (lead < 0x80)
            continue;
        const std::size_t remaining = lead >= 0xc2 && lead <= 0xdf   ? 1
                                      : lead >= 0xe0 && lead <= 0xef ? 2
                                      : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                                     : 0;
        if (remaining == 0 || text.size () - index < remaining)
            return false;
        const auto second = static_cast<unsigned char> (text[index]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)
            || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
            return false;
        for (std::size_t part = 0; part < remaining; ++part) {
            const auto byte = static_cast<unsigned char> (text[index++]);
            if (byte < 0x80 || byte > 0xbf)
                return false;
        }
    }
    return true;
}

bool valid_string (std::string_view value) noexcept
{
    return value.find ('\0') == std::string_view::npos && valid_utf8 (value);
}

// Wire parsing checks UTF-8; rejection context also excludes strings containing NUL.
bool valid_nul_free_string (std::string_view value) noexcept
{
    return value.find ('\0') == std::string_view::npos;
}

bool valid_correlation_id (std::string_view value) noexcept
{
    return !value.empty () && value.size () <= 64
           && std::all_of (value.begin (), value.end (),
                           [] (unsigned char character) { return character <= 0x7f; });
}

using channel_header_proto_t = zlink::framework::channel::v1::ChannelHeader;

struct header_presence_t
{
    bool correlation_id = false;
    bool metadata_duplicate = false;
    std::size_t metadata_encoded_size = 0;
};

result_t<void> validate_protocol_header (const envelope_header_t &header,
                                         bool capture_flow,
                                         std::string_view content_type,
                                         std::optional<std::string_view> flow_id_override = {},
                                         std::optional<flow_origin_t> flow_origin_override = {},
                                         bool *metadata_invalid = nullptr,
                                         const header_presence_t *presence = nullptr)
{
    if (metadata_invalid)
        *metadata_invalid = false;
    const auto valid_text = [presence] (std::string_view value) {
        return presence ? valid_nul_free_string (value) : valid_string (value);
    };
    const auto valid_optional = [&valid_text] (const std::optional<header_text_t> &value) {
        return !value || valid_text (*value);
    };
    if (header.kind < message_kind_t::request || header.kind > message_kind_t::error)
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope header kind is invalid");
    if (!valid_text (header.channel_name) || !valid_text (header.message_name)
        || !valid_text (content_type) || !valid_optional (header.deadline)
        || !valid_optional (header.error_code) || !valid_optional (header.error_message)
        || !valid_optional (header.source) || !valid_optional (header.topic))
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope header string is invalid");
    std::size_t encoded_metadata_size = 0;
    const auto validate_metadata_entry = [&] (std::string_view key, std::string_view value,
                                              std::size_t entry_size) -> result_t<void> {
        if (key.empty () || !valid_text (key) || !valid_text (value)) {
            if (metadata_invalid)
                *metadata_invalid = true;
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope metadata key/value is invalid");
        }
        if (entry_size
            > protocol::metadataBytes
                - std::min<std::uint64_t> (encoded_metadata_size, protocol::metadataBytes)) {
            if (metadata_invalid)
                *metadata_invalid = true;
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope metadata exceeds 1024 bytes");
        }
        encoded_metadata_size += entry_size;
        return result_t<void>::success ();
    };
    if (presence && presence->metadata_duplicate) {
        if (metadata_invalid)
            *metadata_invalid = true;
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope metadata key is duplicated");
    }
    for (const auto &[key, value] : header.metadata) {
        const auto nested_size =
          1 + google::protobuf::internal::WireFormatLite::LengthDelimitedSize (key.size ()) + 1
          + google::protobuf::internal::WireFormatLite::LengthDelimitedSize (value.size ());
        const auto entry_size =
          1 + google::protobuf::internal::WireFormatLite::LengthDelimitedSize (nested_size);
        auto valid = validate_metadata_entry (key, value, presence ? 0 : entry_size);
        if (!valid)
            return valid;
    }
    if (presence && presence->metadata_encoded_size > protocol::metadataBytes) {
        if (metadata_invalid)
            *metadata_invalid = true;
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope metadata exceeds 1024 bytes");
    }
    if (capture_flow) {
        const bool has_flow_id = flow_id_override || header.flow_id;
        const bool has_flow_origin = flow_origin_override || header.flow_origin;
        const auto flow_id = flow_id_override ? std::string_view (*flow_id_override)
                             : header.flow_id ? std::string_view (*header.flow_id)
                                              : std::string_view{};
        const auto flow_origin = static_cast<std::uint8_t> (flow_origin_override.value_or (
          header.flow_origin.value_or (static_cast<flow_origin_t> (0))));
        if (has_flow_id != has_flow_origin)
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "ZLink envelope flow id and origin must be present together");
        if (has_flow_id && !flow_id_t::is_valid (flow_id))
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope flow id must be UUIDv7");
        if (has_flow_origin && (flow_origin < 1 || flow_origin > 4))
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "ZLink envelope flow origin is invalid");
    }
    const bool has_correlation_id =
      presence ? presence->correlation_id : !header.correlation_id.empty ();
    if (has_correlation_id && !valid_correlation_id (header.correlation_id))
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope correlationId must be 1..64 byte ASCII");
    return result_t<void>::success ();
}

using metadata_entry_proto_t = zlink::framework::channel::v1::MetadataEntry;

bool read_wire_string_view (google::protobuf::io::CodedInputStream &input, std::string_view &value)
{
    std::uint32_t length = 0;
    if (!input.ReadVarint32 (&length))
        return false;
    if (length == 0) {
        value = {};
        return true;
    }
    const void *data = nullptr;
    int available = 0;
    if (!input.GetDirectBufferPointer (&data, &available)
        || length > static_cast<std::uint32_t> (available))
        return false;
    value = std::string_view (static_cast<const char *> (data), length);
    return input.Skip (static_cast<int> (length));
}

bool parse_metadata_entry_view (std::string_view bytes, std::string &key, std::string &value)
{
    google::protobuf::io::ArrayInputStream stream (bytes.data (), static_cast<int> (bytes.size ()),
                                                   std::max (1, static_cast<int> (bytes.size ())));
    google::protobuf::io::CodedInputStream input (&stream);
    for (std::uint32_t tag = input.ReadTag (); tag != 0; tag = input.ReadTag ()) {
        const auto field = google::protobuf::internal::WireFormatLite::GetTagFieldNumber (tag);
        const auto wire = google::protobuf::internal::WireFormatLite::GetTagWireType (tag);
        if ((field == metadata_entry_proto_t::kKeyFieldNumber
             || field == metadata_entry_proto_t::kValueFieldNumber)
            && wire == google::protobuf::internal::WireFormatLite::WIRETYPE_LENGTH_DELIMITED) {
            std::string_view text;
            if (!read_wire_string_view (input, text) || !valid_utf8 (text))
                return false;
            auto &destination = field == metadata_entry_proto_t::kKeyFieldNumber ? key : value;
            destination.assign (text.data (), text.size ());
        } else if (!google::protobuf::internal::WireFormatLite::SkipField (&input, tag)) {
            return false;
        }
    }
    return input.ConsumedEntireMessage ();
}

bool parse_channel_header_view (std::span<const std::byte> bytes,
                                bool capture_flow,
                                bool view_text,
                                envelope_header_t &header,
                                header_presence_t &presence)
{
    using wire_t = google::protobuf::internal::WireFormatLite;
    google::protobuf::io::ArrayInputStream stream (bytes.data (), static_cast<int> (bytes.size ()),
                                                   std::max (1, static_cast<int> (bytes.size ())));
    google::protobuf::io::CodedInputStream input (&stream);
    const auto decoded_text = [view_text] (std::string_view text) {
        return view_text ? header_text_t::view (text) : header_text_t (text);
    };
    for (std::uint32_t tag = input.ReadTag (); tag != 0; tag = input.ReadTag ()) {
        const auto field = wire_t::GetTagFieldNumber (tag);
        const auto wire = wire_t::GetTagWireType (tag);
        if (field == channel_header_proto_t::kKindFieldNumber && wire == wire_t::WIRETYPE_VARINT) {
            std::uint32_t value = 0;
            if (!input.ReadVarint32 (&value))
                return false;
            header.kind = static_cast<message_kind_t> (static_cast<std::int32_t> (value));
        } else if (wire == wire_t::WIRETYPE_LENGTH_DELIMITED
                   && ((field >= channel_header_proto_t::kChannelNameFieldNumber
                        && field <= channel_header_proto_t::kFlowIdFieldNumber)
                       || field == channel_header_proto_t::kSourceFieldNumber
                       || field == channel_header_proto_t::kTopicFieldNumber)) {
            std::string_view text;
            if (!read_wire_string_view (input, text) || !valid_utf8 (text))
                return false;
            switch (field) {
                case channel_header_proto_t::kChannelNameFieldNumber:
                    header.channel_name = decoded_text (text);
                    break;
                case channel_header_proto_t::kMessageNameFieldNumber:
                    header.message_name = decoded_text (text);
                    break;
                case channel_header_proto_t::kContentTypeFieldNumber:
                    header.content_type = decoded_text (text);
                    break;
                case channel_header_proto_t::kCorrelationIdFieldNumber:
                    header.correlation_id = decoded_text (text);
                    presence.correlation_id = true;
                    break;
                case channel_header_proto_t::kDeadlineFieldNumber:
                    header.deadline = decoded_text (text);
                    break;
                case channel_header_proto_t::kErrorCodeFieldNumber:
                    header.error_code = decoded_text (text);
                    break;
                case channel_header_proto_t::kErrorMessageFieldNumber:
                    header.error_message = decoded_text (text);
                    break;
                case channel_header_proto_t::kFlowIdFieldNumber:
                    if (capture_flow)
                        header.flow_id = decoded_text (text);
                    break;
                case channel_header_proto_t::kSourceFieldNumber:
                    header.source = decoded_text (text);
                    break;
                case channel_header_proto_t::kTopicFieldNumber:
                    header.topic = decoded_text (text);
                    break;
                default:
                    break;
            }
        } else if (field == channel_header_proto_t::kFlowOriginFieldNumber
                   && wire == wire_t::WIRETYPE_VARINT) {
            std::uint32_t value = 0;
            if (!input.ReadVarint32 (&value))
                return false;
            if (capture_flow)
                header.flow_origin = static_cast<flow_origin_t> (static_cast<std::int32_t> (value));
        } else if (field == channel_header_proto_t::kMetadataFieldNumber
                   && wire == wire_t::WIRETYPE_LENGTH_DELIMITED) {
            std::string_view entry_bytes;
            if (!read_wire_string_view (input, entry_bytes))
                return false;
            presence.metadata_encoded_size += 1 + wire_t::LengthDelimitedSize (entry_bytes.size ());
            std::string key;
            std::string value;
            if (!parse_metadata_entry_view (entry_bytes, key, value))
                return false;
            if (!header.metadata.emplace (std::move (key), std::move (value)).second)
                presence.metadata_duplicate = true;
        } else if (!wire_t::SkipField (&input, tag)) {
            return false;
        }
    }
    return input.ConsumedEntireMessage ();
}

template <typename Setter>
void copy_optional (const std::optional<header_text_t> &source, Setter &&set)
{
    if (source)
        std::forward<Setter> (set) (source->data (), source->size ());
}

channel_header_proto_t &channel_header_encode_scratch ()
{
    thread_local channel_header_proto_t value;
    value.Clear ();
    return value;
}

void set_channel_header_fields (channel_header_proto_t &proto,
                                message_kind_t kind,
                                std::string_view channel_name)
{
    proto.set_kind (static_cast<zlink::framework::channel::v1::ChannelKind> (kind));
    proto.set_channel_name (channel_name.data (), channel_name.size ());
}

void set_type_header_fields (channel_header_proto_t &proto,
                             std::string_view message_name,
                             std::string_view content_type)
{
    proto.set_message_name (message_name.data (), message_name.size ());
    proto.set_content_type (content_type.data (), content_type.size ());
}

using header_fields_t = std::array<std::string_view, 2>;

header_fields_t selected_header_fields (const envelope_header_t &header,
                                        const detail::request_type_plan_t &type,
                                        std::string_view channel_fields)
{
    if (channel_fields.empty () || type.header_fields.empty ()
        || header.kind != message_kind_t::request || header.message_name != type.message_name)
        return {};
    return {channel_fields, type.header_fields};
}

void populate_outgoing_proto (channel_header_proto_t &proto,
                              const envelope_header_t &header,
                              std::string_view content_type,
                              header_fields_t fixed_fields)
{
    const auto &flow = flow_context_t::current ();
    const bool stamp = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto flow_id = stamp ? std::optional<std::string_view> (flow->flow_id)
                               : std::optional<std::string_view> (header.flow_id);
    const auto flow_origin =
      stamp ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    auto valid = validate_protocol_header (header, true, content_type, flow_id, flow_origin);
    if (!valid)
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());

    if (fixed_fields.front ().empty ()) {
        set_channel_header_fields (proto, header.kind, header.channel_name);
        set_type_header_fields (proto, header.message_name, content_type);
    }
    if (!header.correlation_id.empty ())
        proto.set_correlation_id (header.correlation_id.data (), header.correlation_id.size ());
    copy_optional (header.deadline,
                   [&] (auto data, auto size) { proto.set_deadline (data, size); });
    copy_optional (header.error_code,
                   [&] (auto data, auto size) { proto.set_error_code (data, size); });
    copy_optional (header.error_message,
                   [&] (auto data, auto size) { proto.set_error_message (data, size); });
    if (flow_id)
        proto.set_flow_id (flow_id->data (), flow_id->size ());
    if (flow_origin)
        proto.set_flow_origin (
          static_cast<zlink::framework::channel::v1::FlowOrigin> (*flow_origin));
    copy_optional (header.source, [&] (auto data, auto size) { proto.set_source (data, size); });
    copy_optional (header.topic, [&] (auto data, auto size) { proto.set_topic (data, size); });
    for (const auto &[key, value] : header.metadata) {
        auto *entry = proto.add_metadata ();
        entry->set_key (key);
        entry->set_value (value);
    }
}

std::size_t serialized_header_size (const channel_header_proto_t &proto,
                                    header_fields_t fixed_fields)
{
    const auto size = proto.ByteSizeLong () + fixed_fields[0].size () + fixed_fields[1].size ();
    if (size > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope header is too large");
    return size;
}

void serialize_header_into (const channel_header_proto_t &proto,
                            std::span<std::byte> destination,
                            header_fields_t fixed_fields)
{
    const auto tail_size = static_cast<std::size_t> (proto.GetCachedSize ());
    if (destination.size () != tail_size + fixed_fields[0].size () + fixed_fields[1].size ())
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope header cannot be encoded");
    for (const auto fields : fixed_fields) {
        if (!fields.empty ()) {
            std::memcpy (destination.data (), fields.data (), fields.size ());
            destination = destination.subspan (fields.size ());
        }
    }
    if (proto.SerializeWithCachedSizesToArray (
          reinterpret_cast<std::uint8_t *> (destination.data ()))
        != reinterpret_cast<std::uint8_t *> (destination.data ()) + destination.size ())
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope header cannot be encoded");
}

zlink::message_t serialize_header (const channel_header_proto_t &proto,
                                   header_fields_t fixed_fields)
{
    const auto size = serialized_header_size (proto, fixed_fields);
    auto message = zlink::message_t::allocate (size);
    if (!message.valid ())
        throw std::bad_alloc ();
    serialize_header_into (proto, {message.data (), size}, fixed_fields);
    return message;
}

result_t<envelope_header_t>
decode_header_bytes (std::span<const std::byte> bytes,
                     bool capture_flow,
                     std::optional<envelope_header_t> *rejection_context,
                     bool view_text = false)
{
    if (rejection_context)
        rejection_context->reset ();
    if (bytes.size () > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header is too large");
    envelope_header_t header{.content_type = {}};
    header.kind = static_cast<message_kind_t> (0);
    header_presence_t presence;
    if (!parse_channel_header_view (bytes, capture_flow, view_text, header, presence))
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header protobuf is invalid");
    bool metadata_invalid = false;
    auto valid = validate_protocol_header (header, capture_flow, header.content_type, {}, {},
                                           &metadata_invalid, &presence);
    if (!valid) {
        if (rejection_context) {
            if (!valid_nul_free_string (header.channel_name))
                header.channel_name.clear ();
            if (!valid_nul_free_string (header.message_name))
                header.message_name.clear ();
            if (!valid_nul_free_string (header.content_type))
                header.content_type.clear ();
            if (!valid_correlation_id (header.correlation_id))
                header.correlation_id.clear ();
            if (!header.deadline || !valid_nul_free_string (*header.deadline))
                header.deadline.reset ();
            if (header.error_code && !valid_nul_free_string (*header.error_code))
                header.error_code.reset ();
            if (header.error_message && !valid_nul_free_string (*header.error_message))
                header.error_message.reset ();
            if (header.source && !valid_nul_free_string (*header.source))
                header.source.reset ();
            if (header.topic && !valid_nul_free_string (*header.topic))
                header.topic.reset ();
            if (!metadata_invalid) {
                header.flow_id.reset ();
                header.flow_origin.reset ();
            }
            *rejection_context = std::move (header);
        }
        return result_t<envelope_header_t>::failure (valid.error_kind (), valid.error ()->what ());
    }
    return result_t<envelope_header_t>::success (std::move (header));
}

result_t<envelope_header_t>
decode_header_bytes_result (std::span<const std::byte> bytes,
                            bool capture_flow,
                            std::optional<envelope_header_t> *rejection_context,
                            bool view_text)
{
    try {
        return decode_header_bytes (bytes, capture_flow, rejection_context, view_text);
    }
    catch (const framework_exception_t &error) {
        return result_t<envelope_header_t>::failure (error.kind (), error.what ());
    }
    catch (const std::exception &error) {
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     error.what ());
    }
}

} // namespace

std::string encode_request_channel_header_fields (std::string_view channel_name)
{
    if (!valid_string (channel_name))
        return {};
    channel_header_proto_t proto;
    set_channel_header_fields (proto, message_kind_t::request, channel_name);
    return proto.SerializeAsString ();
}

bool envelope_header_t::can_correlate_reply () const noexcept
{
    return valid_correlation_id (correlation_id);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const
{
    return encode_header (header, header.content_type);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header,
                                                  std::string_view content_type,
                                                  header_fields_t fixed_fields) const
{
    auto &proto = channel_header_encode_scratch ();
    populate_outgoing_proto (proto, header, content_type, fixed_fields);
    return serialize_header (proto, fixed_fields);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header,
                                                  const detail::request_type_plan_t &type,
                                                  std::string_view channel_fields) const
{
    return encode_header (header, type.content_type,
                          selected_header_fields (header, type, channel_fields));
}

protocol::application_envelope_encoder_t
envelope_codec_t::prepare_service_payload (envelope_header_t &&header,
                                           zlink::message_t body,
                                           std::shared_ptr<const detail::request_type_plan_t> type,
                                           std::shared_ptr<const std::string> channel_fields) const
{
    const auto fixed_fields =
      selected_header_fields (header, *type, channel_fields ? *channel_fields : std::string_view{});
    channel_header_proto_t proto;
    populate_outgoing_proto (proto, header, type->content_type, fixed_fields);
    using encoder_t = protocol::application_envelope_encoder_t;
    return encoder_t (std::make_shared<const encoder_t::state_t> (
      std::move (proto), std::move (body), std::move (type), std::move (channel_fields),
      fixed_fields));
}

result_t<envelope_header_t>
envelope_codec_t::decode_header (const zlink::message_t &message,
                                 bool capture_flow,
                                 std::optional<envelope_header_t> *rejection_context) const
{
    return decode_header (message.bytes (), capture_flow, rejection_context);
}

result_t<envelope_header_t>
envelope_codec_t::decode_header_view (const zlink::message_t &message,
                                      bool capture_flow,
                                      std::optional<envelope_header_t> *rejection_context) const
{
    return decode_header_bytes_result (message.bytes (), capture_flow, rejection_context, true);
}

result_t<envelope_header_t>
envelope_codec_t::decode_header_view (const message_parts_t &parts,
                                      bool capture_flow,
                                      std::optional<envelope_header_t> *rejection_context) const
{
    if (parts.size () == 0) {
        if (rejection_context)
            rejection_context->reset ();
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header part is missing");
    }
    return decode_header_bytes_result (parts.part_bytes (0), capture_flow, rejection_context, true);
}

result_t<envelope_header_t>
envelope_codec_t::decode_header (std::span<const std::byte> bytes,
                                 bool capture_flow,
                                 std::optional<envelope_header_t> *rejection_context) const
{
    return decode_header_bytes_result (bytes, capture_flow, rejection_context, false);
}

result_t<envelope_header_t>
envelope_codec_t::decode_header (const message_parts_t &parts,
                                 bool capture_flow,
                                 std::optional<envelope_header_t> *rejection_context) const
{
    if (parts.size () == 0) {
        if (rejection_context)
            rejection_context->reset ();
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header part is missing");
    }
    return decode_header (parts.part_bytes (0), capture_flow, rejection_context);
}

result_t<encoded_payload_t> envelope_codec_t::decode_body_view (const message_parts_t &parts) const
{
    if (parts.size () < 2)
        return result_t<encoded_payload_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope body part is missing");
    return result_t<encoded_payload_t>::success (
      detail::encoded_payload_from_raw (parts.part_bytes (1)));
}

result_t<zlink::message_t> envelope_codec_t::decode_body (const message_parts_t &parts) const
{
    const auto view = decode_body_view (parts);
    if (!view)
        return detail::result_access_t::failure<zlink::message_t> (view.exception ());
    if (std::holds_alternative<std::span<const std::span<const std::byte>>> (parts._parts))
        return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (view.value ()));
    return result_t<zlink::message_t>::success (parts[1].copy ());
}

} // namespace zlink::framework::runtime::messaging

namespace zlink::framework::runtime::protocol
{

zlink::message_t
application_envelope_encoder_t::operator() (const std::optional<std::string> &flow_id,
                                            std::optional<flow_origin_t> flow_origin) const
{
    using namespace messaging;
    return encode_application_envelope_message (
      serialized_header_size (_state->header, _state->fixed_fields),
      [plan = _state.get ()] (std::span<std::byte> destination) {
          serialize_header_into (plan->header, destination, plan->fixed_fields);
      },
      _state->body.bytes (), flow_id, flow_origin);
}

} // namespace zlink::framework::runtime::protocol

namespace zlink::framework::detail
{

request_type_plan_t make_request_type_plan (std::string message_name, std::string content_type)
{
    using namespace runtime::messaging;
    std::string fields;
    if (valid_string (message_name) && valid_string (content_type)) {
        channel_header_proto_t proto;
        set_type_header_fields (proto, message_name, content_type);
        fields = proto.SerializeAsString ();
    }
    return {std::move (content_type), std::move (message_name), std::move (fields)};
}

} // namespace zlink::framework::detail
