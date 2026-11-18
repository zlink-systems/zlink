/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/messaging/envelope_codec.hpp"

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <channel-header-v1.pb.h>
#include <service_wire_constants.hpp>
#include <google/protobuf/wire_format_lite.h>

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

// The generated protobuf parser validates UTF-8 for every `string` field.
// The Framework validator still checks its additional string constraint here.
bool valid_proto_string (std::string_view value) noexcept
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

result_t<void> validate_protocol_header (const envelope_header_t &header,
                                         bool capture_flow,
                                         const channel_header_proto_t *proto = nullptr,
                                         bool parser_validated_utf8 = false,
                                         bool *metadata_invalid = nullptr)
{
    if (metadata_invalid)
        *metadata_invalid = false;
    const auto channel_name =
      proto ? std::string_view (proto->channel_name ()) : std::string_view (header.channel_name);
    const auto message_name =
      proto ? std::string_view (proto->message_name ()) : std::string_view (header.message_name);
    const auto content_type =
      proto ? std::string_view (proto->content_type ()) : std::string_view (header.content_type);
    const auto correlation_id = proto ? std::string_view (proto->correlation_id ())
                                      : std::string_view (header.correlation_id);
    const auto valid_text = [parser_validated_utf8] (std::string_view value) {
        return parser_validated_utf8 ? valid_proto_string (value) : valid_string (value);
    };
    const auto valid_optional = [&valid_text] (bool present, std::string_view value) {
        return !present || valid_text (value);
    };
    if (header.kind < message_kind_t::request || header.kind > message_kind_t::error)
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope header kind is invalid");
    const auto has_deadline = proto ? proto->has_deadline () : header.deadline.has_value ();
    const auto has_error_code = proto ? proto->has_error_code () : header.error_code.has_value ();
    const auto has_error_message =
      proto ? proto->has_error_message () : header.error_message.has_value ();
    const auto has_source = proto ? proto->has_source () : header.source.has_value ();
    const auto has_topic = proto ? proto->has_topic () : header.topic.has_value ();
    if (!valid_text (channel_name) || !valid_text (message_name) || !valid_text (content_type)
        || !valid_optional (has_deadline, proto             ? std::string_view (proto->deadline ())
                                          : header.deadline ? std::string_view (*header.deadline)
                                                            : std::string_view{})
        || !valid_optional (has_error_code, proto ? std::string_view (proto->error_code ())
                                                  : header.error_code.value_or (""))
        || !valid_optional (has_error_message, proto ? std::string_view (proto->error_message ())
                                                     : header.error_message.value_or (""))
        || !valid_optional (has_source, proto ? std::string_view (proto->source ())
                                              : header.source.value_or (""))
        || !valid_optional (has_topic, proto ? std::string_view (proto->topic ())
                                             : header.topic.value_or ("")))
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope header string is invalid");
    if (proto) {
        std::size_t encoded_size = 0;
        for (int index = 0; index < proto->metadata_size (); ++index) {
            const auto &entry = proto->metadata (index);
            if (entry.key ().empty ()
                || !(parser_validated_utf8 ? valid_proto_string (entry.key ())
                                           : valid_string (entry.key ()))
                || !(parser_validated_utf8 ? valid_proto_string (entry.value ())
                                           : valid_string (entry.value ()))) {
                if (metadata_invalid)
                    *metadata_invalid = true;
                return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                                "ZLink envelope metadata key/value is invalid");
            }
            for (int previous = 0; previous < index; ++previous) {
                if (proto->metadata (previous).key () == entry.key ()) {
                    if (metadata_invalid)
                        *metadata_invalid = true;
                    return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                                    "ZLink envelope metadata key is duplicated");
                }
            }
            const auto entry_size =
              1
              + google::protobuf::internal::WireFormatLite::LengthDelimitedSize (
                entry.ByteSizeLong ());
            if (entry_size > protocol::metadataBytes
                               - std::min<std::uint64_t> (encoded_size, protocol::metadataBytes)) {
                if (metadata_invalid)
                    *metadata_invalid = true;
                return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                                "ZLink envelope metadata exceeds 1024 bytes");
            }
            encoded_size += entry_size;
        }
    } else {
        for (const auto &[key, value] : header.metadata) {
            if (key.empty () || !valid_string (key) || !valid_string (value))
                return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                                "ZLink envelope metadata key/value is invalid");
        }
    }
    if (capture_flow) {
        const bool has_flow_id = proto ? proto->has_flow_id () : header.flow_id.has_value ();
        const bool has_flow_origin =
          proto ? proto->has_flow_origin () : header.flow_origin.has_value ();
        const auto flow_id = proto            ? std::string_view (proto->flow_id ())
                             : header.flow_id ? std::string_view (*header.flow_id)
                                              : std::string_view{};
        const auto flow_origin = proto ? static_cast<std::uint8_t> (proto->flow_origin ())
                                 : header.flow_origin
                                   ? static_cast<std::uint8_t> (*header.flow_origin)
                                   : std::uint8_t{};
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
      proto ? proto->has_correlation_id () : !header.correlation_id.empty ();
    if (has_correlation_id && !valid_correlation_id (correlation_id))
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "ZLink envelope correlationId must be 1..64 byte ASCII");
    return result_t<void>::success ();
}

template <typename Setter>
void copy_optional (const std::optional<std::string> &source, Setter &&set)
{
    if (source)
        std::forward<Setter> (set) (*source);
}

template <typename Setter>
void copy_optional (const std::optional<header_text_t> &source, Setter &&set)
{
    if (source)
        std::forward<Setter> (set) (std::string (*source));
}

channel_header_proto_t &channel_header_encode_scratch ()
{
    thread_local channel_header_proto_t value;
    value.Clear ();
    return value;
}

channel_header_proto_t &channel_header_scratch ()
{
    thread_local channel_header_proto_t value;
    value.Clear ();
    return value;
}

void to_proto (channel_header_proto_t &proto,
               const envelope_header_t &header,
               std::string_view content_type,
               const std::string *flow_id,
               std::optional<flow_origin_t> flow_origin)
{
    proto.set_kind (static_cast<zlink::framework::channel::v1::ChannelKind> (header.kind));
    proto.set_channel_name (header.channel_name.data (), header.channel_name.size ());
    proto.set_message_name (header.message_name.data (), header.message_name.size ());
    proto.set_content_type (content_type.data (), content_type.size ());
    if (!header.correlation_id.empty ())
        proto.set_correlation_id (header.correlation_id.data (), header.correlation_id.size ());
    copy_optional (header.deadline, [&] (const std::string &v) { proto.set_deadline (v); });
    copy_optional (header.error_code, [&] (const std::string &v) { proto.set_error_code (v); });
    copy_optional (header.error_message,
                   [&] (const std::string &v) { proto.set_error_message (v); });
    if (flow_id)
        proto.set_flow_id (*flow_id);
    if (flow_origin)
        proto.set_flow_origin (
          static_cast<zlink::framework::channel::v1::FlowOrigin> (*flow_origin));
    copy_optional (header.source, [&] (const std::string &v) { proto.set_source (v); });
    copy_optional (header.topic, [&] (const std::string &v) { proto.set_topic (v); });
    for (const auto &[key, value] : header.metadata) {
        auto *entry = proto.add_metadata ();
        entry->set_key (key);
        entry->set_value (value);
    }
}

std::size_t serialized_header_size (const channel_header_proto_t &proto)
{
    const auto size = proto.ByteSizeLong ();
    if (size > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope header is too large");
    return size;
}

void serialize_header_into (const channel_header_proto_t &proto,
                            std::span<std::byte> destination,
                            std::size_t size)
{
    if (destination.size () != size || proto.GetCachedSize () != static_cast<int> (size)
        || proto.SerializeWithCachedSizesToArray (
             reinterpret_cast<std::uint8_t *> (destination.data ()))
             != reinterpret_cast<std::uint8_t *> (destination.data ()) + size)
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "ZLink envelope header cannot be encoded");
}

zlink::message_t serialize_header (const channel_header_proto_t &proto)
{
    const auto size = serialized_header_size (proto);
    auto message = zlink::message_t::allocate (size);
    if (!message.valid ())
        throw std::bad_alloc ();
    serialize_header_into (proto, {message.data (), size}, size);
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
    auto &proto = channel_header_scratch ();
    if (!proto.ParseFromArray (bytes.data (), static_cast<int> (bytes.size ())))
        return result_t<envelope_header_t>::failure (framework_error_kind_t::protocol_error,
                                                     "ZLink envelope header protobuf is invalid");

    envelope_header_t header{.content_type = {}};
    header.kind = static_cast<message_kind_t> (proto.kind ());
    header.channel_name = header_text_t::view (proto.channel_name ());
    header.message_name = header_text_t::view (proto.message_name ());
    header.content_type = header_text_t::view (proto.content_type ());
    if (proto.has_correlation_id ())
        header.correlation_id = header_text_t::view (proto.correlation_id ());
    if (proto.has_deadline ())
        header.deadline = header_text_t::view (proto.deadline ());
    if (capture_flow) {
        if (proto.has_flow_origin ())
            header.flow_origin = static_cast<flow_origin_t> (proto.flow_origin ());
    }

    bool metadata_invalid = false;
    auto valid = validate_protocol_header (header, capture_flow, &proto, true, &metadata_invalid);
    if (!valid) {
        if (rejection_context) {
            if (!valid_proto_string (proto.channel_name ()))
                header.channel_name.clear ();
            if (!valid_proto_string (proto.message_name ()))
                header.message_name.clear ();
            if (!valid_proto_string (proto.content_type ()))
                header.content_type.clear ();
            if (!valid_correlation_id (proto.correlation_id ()))
                header.correlation_id.clear ();
            if (proto.has_deadline () && valid_proto_string (proto.deadline ()))
                header.deadline = header_text_t::view (proto.deadline ());
            else
                header.deadline.reset ();
            if (proto.has_error_code () && valid_proto_string (proto.error_code ()))
                header.error_code = proto.error_code ();
            if (proto.has_error_message () && valid_proto_string (proto.error_message ()))
                header.error_message = proto.error_message ();
            if (proto.has_source () && valid_proto_string (proto.source ()))
                header.source = proto.source ();
            if (proto.has_topic () && valid_proto_string (proto.topic ()))
                header.topic = proto.topic ();
            if (!metadata_invalid) {
                header.flow_id.reset ();
                header.flow_origin.reset ();
            } else if (capture_flow && proto.has_flow_id ()) {
                header.flow_id = proto.flow_id ();
            }
            *rejection_context = header;
        }
        return result_t<envelope_header_t>::failure (valid.error_kind (), valid.error ()->what ());
    }
    if (!view_text) {
        // Keep parser capacity for the next frame; the returned header owns the
        // fields that outlive this decode.
        header.channel_name = std::string (proto.channel_name ());
        header.message_name = std::string (proto.message_name ());
        header.content_type = std::string (proto.content_type ());
        if (proto.has_correlation_id ())
            header.correlation_id = std::string (proto.correlation_id ());
        if (proto.has_deadline ())
            header.deadline = header_text_t (std::string (proto.deadline ()));
    }
    if (proto.has_error_code ())
        header.error_code = std::move (*proto.mutable_error_code ());
    if (proto.has_error_message ())
        header.error_message = std::move (*proto.mutable_error_message ());
    if (proto.has_source ())
        header.source = std::move (*proto.mutable_source ());
    if (proto.has_topic ())
        header.topic = std::move (*proto.mutable_topic ());
    if (capture_flow && proto.has_flow_id ())
        header.flow_id = std::move (*proto.mutable_flow_id ());
    if (rejection_context && !view_text)
        *rejection_context = header;
    for (int index = 0; index < proto.metadata_size (); ++index) {
        auto *entry = proto.mutable_metadata (index);
        header.metadata.emplace (std::move (*entry->mutable_key ()),
                                 std::move (*entry->mutable_value ()));
    }
    if (!capture_flow) {
        header.flow_id.reset ();
        header.flow_origin.reset ();
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

std::pair<const std::string *, std::optional<flow_origin_t>>
validate_outgoing_header (const envelope_header_t &header)
{
    const auto &flow = flow_context_t::current ();
    const bool stamp = !header.flow_id && flow && !flow->flow_id.empty ();
    const auto *id = stamp ? &flow->flow_id : header.flow_id ? &*header.flow_id : nullptr;
    const auto origin = stamp ? std::optional<flow_origin_t> (flow->origin) : header.flow_origin;
    return {id, origin};
}

} // namespace

bool envelope_header_t::can_correlate_reply () const noexcept
{
    return valid_correlation_id (correlation_id);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header) const
{
    return encode_header (header, header.content_type);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header,
                                                  std::string_view content_type) const
{
    const auto [flow_id, origin] = validate_outgoing_header (header);
    auto &proto = channel_header_encode_scratch ();
    to_proto (proto, header, content_type, flow_id, origin);
    auto valid = validate_protocol_header (header, true, &proto);
    if (!valid)
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    return serialize_header (proto);
}

zlink::message_t envelope_codec_t::encode_header (const envelope_header_t &header,
                                                  const detail::request_type_plan_t &type) const
{
    return encode_header (header, type.content_type);
}

protocol::application_envelope_encoder_t envelope_codec_t::prepare_service_payload (
  envelope_header_t &&header,
  zlink::message_t body,
  std::shared_ptr<const detail::request_type_plan_t> type) const
{
    header.content_type = type->content_type;
    const auto [flow_id, origin] = validate_outgoing_header (header);
    if (flow_id && !header.flow_id)
        header.flow_id = *flow_id;
    header.flow_origin = origin;
    channel_header_proto_t proto;
    to_proto (proto, header, type->content_type, flow_id, origin);
    auto valid = validate_protocol_header (header, true, &proto);
    if (!valid)
        throw framework_exception_t (valid.error_kind (), valid.error ()->what ());
    auto state = std::make_shared<std::pair<channel_header_proto_t, zlink::message_t>> (
      std::move (proto), std::move (body));
    return [state] (const std::optional<std::string> &flow_id,
                    std::optional<flow_origin_t> flow_origin) {
        const auto header_size = serialized_header_size (state->first);
        return protocol::encode_application_envelope_message (
          header_size,
          [state, header_size] (std::span<std::byte> destination) {
              serialize_header_into (state->first, destination, header_size);
          },
          state->second.bytes (), flow_id, flow_origin);
    };
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
    if (std::holds_alternative<std::span<const std::span<const std::byte>>> (parts._parts))
        return decode_header_bytes_result (parts.part_bytes (0), capture_flow, rejection_context,
                                           true);
    return decode_header_view (parts[0], capture_flow, rejection_context);
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
    if (!std::holds_alternative<std::span<const std::span<const std::byte>>> (parts._parts))
        return decode_header (parts[0], capture_flow, rejection_context);
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
