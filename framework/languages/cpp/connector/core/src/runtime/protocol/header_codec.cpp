/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/header_codec.hpp"

#include "runtime/protocol/metadata_codec.hpp"

#include <limits>

namespace zlink::stream_connector::detail
{

namespace
{

constexpr auto flow_field_flag = static_cast<header_flags_t> (0x10);

constexpr std::uint8_t known_flags =
  static_cast<std::uint8_t> (header_flags_t::has_request_seq)
  | static_cast<std::uint8_t> (header_flags_t::has_metadata)
  | static_cast<std::uint8_t> (header_flags_t::payload_compressed)
  | static_cast<std::uint8_t> (header_flags_t::has_correlation_id)
  | static_cast<std::uint8_t> (flow_field_flag)
  | static_cast<std::uint8_t> (header_flags_t::has_actor_slot);

bool has_flag (header_flags_t flags, header_flags_t flag)
{
    return (static_cast<std::uint8_t> (flags) & static_cast<std::uint8_t> (flag)) != 0;
}

void set_flag (header_flags_t &flags, header_flags_t flag)
{
    flags = flags | flag;
}

void clear_flag (header_flags_t &flags, header_flags_t flag)
{
    flags = static_cast<header_flags_t> (static_cast<std::uint8_t> (flags)
                                         & ~static_cast<std::uint8_t> (flag));
}

void write_u16 (std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
    bytes.push_back (static_cast<std::uint8_t> (value & 0xff));
}

void write_u64 (std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xff));
    }
}

std::uint16_t read_u16 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const auto value = static_cast<std::uint16_t> ((bytes[offset] << 8) | bytes[offset + 1]);
    offset += 2;
    return value;
}

std::uint64_t read_u64 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[offset + static_cast<std::size_t> (i)];
    }
    offset += 8;
    return value;
}

bool is_defined (message_kind_t kind)
{
    switch (kind) {
        case message_kind_t::send:
        case message_kind_t::request:
        case message_kind_t::response:
        case message_kind_t::error:
        case message_kind_t::control:
            return true;
    }
    return false;
}

bool is_defined (codec_t codec)
{
    switch (codec) {
        case codec_t::raw:
        case codec_t::json:
        case codec_t::message_pack:
        case codec_t::protobuf:
            return true;
    }
    return false;
}

result_t<void> validate_header (const stream_header_t &header)
{
    if (!is_defined (header.kind) || !is_defined (header.codec)
        || (static_cast<std::uint8_t> (header.flags) & ~known_flags) != 0) {
        return result_t<void>::failure (error_code_t::frame_decode_failed,
                                        "Unknown stream header enum value.");
    }
    const auto has_request_seq = has_flag (header.flags, header_flags_t::has_request_seq);
    const auto has_metadata = has_flag (header.flags, header_flags_t::has_metadata);
    if (header.kind == message_kind_t::send && has_request_seq) {
        return result_t<void>::failure (error_code_t::frame_decode_failed,
                                        "Send packet must not contain a request sequence.");
    }
    if ((header.kind == message_kind_t::request || header.kind == message_kind_t::response)
        && !has_request_seq) {
        return result_t<void>::failure (
          error_code_t::frame_decode_failed,
          "Request and response packets must contain a request sequence.");
    }
    if (header.kind == message_kind_t::error && header.codec != codec_t::json) {
        return result_t<void>::failure (error_code_t::frame_decode_failed,
                                        "Error packet must use the JSON codec.");
    }
    if (header.kind == message_kind_t::control) {
        if (header.flags != header_flags_t::none || header.codec != codec_t::raw || has_request_seq
            || has_metadata || header.actor_slot) {
            return result_t<void>::failure (error_code_t::frame_decode_failed,
                                            "Control packet must use raw codec and no flags.");
        }
    }
    if (header.actor_slot && *header.actor_slot == 0) {
        return result_t<void>::failure (error_code_t::frame_decode_failed,
                                        "Actor slot must not be zero.");
    }
    const bool is_reply =
      header.kind == message_kind_t::response || header.kind == message_kind_t::error;
    if ((!is_reply && header.name.empty ())
        || header.name.size () > std::numeric_limits<std::uint8_t>::max ()) {
        return result_t<void>::failure (error_code_t::validation_failed,
                                        "Packet name length is invalid.");
    }
    if (!is_reply && header.name.starts_with ("$zlink.")
        && header.kind != message_kind_t::control) {
        return result_t<void>::failure (
          error_code_t::frame_decode_failed,
          "Reserved packet names are only valid for control packets.");
    }
    if (header.request_seq && *header.request_seq == 0) {
        return result_t<void>::failure (error_code_t::validation_failed,
                                        "Request sequence must not be zero.");
    }
    return result_t<void>::success ();
}

} // namespace

result_t<std::vector<std::uint8_t>> header_codec_t::encode (const stream_header_t &source) const
{
    auto header = source;
    if ((header.kind == message_kind_t::response || header.kind == message_kind_t::error)
        && !header.name.empty ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::validation_failed, "Response and error packet names must be empty.");
    }
    if (header.request_seq) {
        set_flag (header.flags, header_flags_t::has_request_seq);
    } else {
        clear_flag (header.flags, header_flags_t::has_request_seq);
    }
    if (!header.metadata.values.empty ()) {
        set_flag (header.flags, header_flags_t::has_metadata);
    } else {
        clear_flag (header.flags, header_flags_t::has_metadata);
    }
    if (!header.correlation_id.empty ()) {
        set_flag (header.flags, header_flags_t::has_correlation_id);
    } else {
        clear_flag (header.flags, header_flags_t::has_correlation_id);
    }
    clear_flag (header.flags, flow_field_flag);
    if (header.actor_slot) {
        set_flag (header.flags, header_flags_t::has_actor_slot);
    } else {
        clear_flag (header.flags, header_flags_t::has_actor_slot);
    }
    if (header.correlation_id.size () > std::numeric_limits<std::uint8_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::validation_failed,
                                                             "Correlation id is too large.");
    }
    if (auto validation = validate_header (header); !validation) {
        return result_t<std::vector<std::uint8_t>>::failure (validation.error ()->code,
                                                             validation.error ()->message);
    }
    auto metadata = metadata_codec_t::encode (header.metadata);
    if (!metadata) {
        return result_t<std::vector<std::uint8_t>>::failure (metadata.error ()->code,
                                                             metadata.error ()->message);
    }
    if (metadata.value ().size () > std::numeric_limits<std::uint16_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::validation_failed, "Metadata payload exceeds fixed limit.");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve (4 + (header.request_seq ? 8 : 0) + 1 + header.name.size ()
                   + (metadata.value ().empty () ? 0 : 2 + metadata.value ().size ()));
    bytes.push_back (flow_id_codec_t::format_marker);
    bytes.push_back (static_cast<std::uint8_t> (header.kind));
    bytes.push_back (static_cast<std::uint8_t> (header.codec));
    bytes.push_back (static_cast<std::uint8_t> (header.flags));
    if (header.request_seq) {
        write_u64 (bytes, *header.request_seq);
    }
    bytes.push_back (static_cast<std::uint8_t> (header.name.size ()));
    bytes.insert (bytes.end (), header.name.begin (), header.name.end ());
    if (!metadata.value ().empty ()) {
        write_u16 (bytes, static_cast<std::uint16_t> (metadata.value ().size ()));
        bytes.insert (bytes.end (), metadata.value ().begin (), metadata.value ().end ());
    }
    if (!header.correlation_id.empty ()) {
        bytes.push_back (static_cast<std::uint8_t> (header.correlation_id.size ()));
        bytes.insert (bytes.end (), header.correlation_id.begin (), header.correlation_id.end ());
    }
    if (header.actor_slot) {
        write_u16 (bytes, *header.actor_slot);
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<stream_header_t> header_codec_t::decode (const std::vector<std::uint8_t> &bytes) const
{
    if (bytes.size () < 5) {
        return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                   "Helper header is too short.");
    }
    std::size_t offset = 0;
    if (bytes[offset++] != flow_id_codec_t::format_marker) {
        return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                   "Stream format marker is invalid.");
    }
    stream_header_t header;
    header.kind = static_cast<message_kind_t> (bytes[offset++]);
    header.codec = static_cast<codec_t> (bytes[offset++]);
    header.flags = static_cast<header_flags_t> (bytes[offset++]);

    if (has_flag (header.flags, header_flags_t::has_request_seq)) {
        if (bytes.size () - offset < 8) {
            return result_t<stream_header_t>::failure (
              error_code_t::frame_decode_failed, "Helper header request sequence is incomplete.");
        }
        header.request_seq = read_u64 (bytes, offset);
    }
    if (bytes.size () - offset < 1) {
        return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                   "Helper header name length is missing.");
    }
    const auto name_size = bytes[offset++];
    if (bytes.size () - offset < name_size) {
        return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                   "Helper header packet name is invalid.");
    }
    header.name = std::string (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                               bytes.begin () + static_cast<std::ptrdiff_t> (offset + name_size));
    offset += name_size;

    if (has_flag (header.flags, header_flags_t::has_metadata)) {
        if (bytes.size () - offset < 2) {
            return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                       "Helper header metadata length is missing.");
        }
        const auto metadata_size = read_u16 (bytes, offset);
        if (bytes.size () - offset < metadata_size) {
            return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                       "Helper header metadata is incomplete.");
        }
        std::vector<std::uint8_t> metadata (
          bytes.begin () + static_cast<std::ptrdiff_t> (offset),
          bytes.begin () + static_cast<std::ptrdiff_t> (offset + metadata_size));
        offset += metadata_size;
        auto decoded = metadata_codec_t::decode (metadata);
        if (!decoded) {
            return result_t<stream_header_t>::failure (decoded.error ()->code,
                                                       decoded.error ()->message);
        }
        header.metadata = decoded.value ();
    }
    if (has_flag (header.flags, header_flags_t::has_correlation_id)) {
        if (bytes.size () - offset < 1) {
            return result_t<stream_header_t>::failure (
              error_code_t::frame_decode_failed, "Helper header correlation id length is missing.");
        }
        const auto correlation_size = bytes[offset++];
        if (correlation_size == 0 || bytes.size () - offset < correlation_size) {
            return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                       "Helper header correlation id is invalid.");
        }
        header.correlation_id =
          std::string (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                       bytes.begin () + static_cast<std::ptrdiff_t> (offset + correlation_size));
        offset += correlation_size;
    }
    if (has_flag (header.flags, flow_field_flag)) {
        if (bytes.size () - offset < flow_id_codec_t::encoded_length + 1) {
            return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                       "Helper header flow fields are incomplete.");
        }
        offset += flow_id_codec_t::encoded_length;
        ++offset;
    }
    if (has_flag (header.flags, header_flags_t::has_actor_slot)) {
        if (bytes.size () - offset < 2) {
            return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                       "Helper header actor slot is incomplete.");
        }
        header.actor_slot = read_u16 (bytes, offset);
    }
    if (offset != bytes.size ()) {
        return result_t<stream_header_t>::failure (error_code_t::frame_decode_failed,
                                                   "Helper header contains trailing bytes.");
    }
    if (auto validation = validate_header (header); !validation) {
        return result_t<stream_header_t>::failure (validation.error ()->code,
                                                   validation.error ()->message);
    }
    return result_t<stream_header_t>::success (std::move (header));
}

result_t<actor_bound_t>
actor_binding_control_codec_t::decode_bound (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () < 4 || payload[0] != 1) {
        return result_t<actor_bound_t>::failure (error_code_t::frame_decode_failed,
                                                 "Actor bound control is invalid.");
    }
    std::size_t offset = 1;
    const auto actor_slot = read_u16 (payload, offset);
    const auto actor_id_size = payload[offset++];
    if (actor_slot == 0 || actor_id_size == 0 || payload.size () - offset != actor_id_size) {
        return result_t<actor_bound_t>::failure (error_code_t::frame_decode_failed,
                                                 "Actor bound control payload is invalid.");
    }
    return result_t<actor_bound_t>::success (actor_bound_t{
      actor_slot,
      std::string (payload.begin () + static_cast<std::ptrdiff_t> (offset), payload.end ())});
}

result_t<std::uint16_t>
actor_binding_control_codec_t::decode_unbound (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () != 3 || payload[0] != 1) {
        return result_t<std::uint16_t>::failure (error_code_t::frame_decode_failed,
                                                 "Actor unbound control is invalid.");
    }
    std::size_t offset = 1;
    const auto actor_slot = read_u16 (payload, offset);
    if (actor_slot == 0) {
        return result_t<std::uint16_t>::failure (error_code_t::frame_decode_failed,
                                                 "Actor unbound slot is invalid.");
    }
    return result_t<std::uint16_t>::success (actor_slot);
}

result_t<std::vector<std::uint8_t>>
session_closing_codec_t::encode (const session_closing_t &closing)
{
    const auto raw_reason = static_cast<std::uint8_t> (closing.reason);
    if (raw_reason < 1 || raw_reason > 6) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::validation_failed,
                                                             "Session-closing reason is invalid.");
    }
    if (closing.diagnostic.size () > max_diagnostic_bytes) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::validation_failed, "Session-closing diagnostic is too large.");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve (4 + closing.diagnostic.size ());
    bytes.push_back (version);
    bytes.push_back (raw_reason);
    bytes.push_back (static_cast<std::uint8_t> ((closing.diagnostic.size () >> 8) & 0xff));
    bytes.push_back (static_cast<std::uint8_t> (closing.diagnostic.size () & 0xff));
    bytes.insert (bytes.end (), closing.diagnostic.begin (), closing.diagnostic.end ());
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<session_closing_t>
session_closing_codec_t::decode (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () < 4) {
        return result_t<session_closing_t>::failure (error_code_t::frame_decode_failed,
                                                     "Session-closing payload is truncated.");
    }
    if (payload[0] != version) {
        return result_t<session_closing_t>::failure (error_code_t::frame_decode_failed,
                                                     "Session-closing version is not supported.");
    }
    if (payload[1] < 1 || payload[1] > 6) {
        return result_t<session_closing_t>::failure (error_code_t::frame_decode_failed,
                                                     "Session-closing reason is not supported.");
    }
    const auto diagnostic_length = static_cast<std::size_t> ((payload[2] << 8) | payload[3]);
    if (diagnostic_length > max_diagnostic_bytes) {
        return result_t<session_closing_t>::failure (error_code_t::frame_decode_failed,
                                                     "Session-closing diagnostic is too large.");
    }
    if (payload.size () != 4 + diagnostic_length) {
        return result_t<session_closing_t>::failure (
          error_code_t::frame_decode_failed,
          "Session-closing diagnostic length does not match the payload.");
    }
    std::string diagnostic (payload.begin () + 4, payload.end ());
    /* Strict UTF-8 validation (invalid sequences close as protocol error). */
    for (std::size_t i = 0; i < diagnostic.size ();) {
        const auto byte = static_cast<unsigned char> (diagnostic[i]);
        std::size_t continuation = 0;
        if (byte <= 0x7F) {
            continuation = 0;
        } else if ((byte & 0xE0) == 0xC0 && byte >= 0xC2) {
            continuation = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            continuation = 2;
        } else if ((byte & 0xF8) == 0xF0 && byte <= 0xF4) {
            continuation = 3;
        } else {
            return result_t<session_closing_t>::failure (
              error_code_t::frame_decode_failed, "Session-closing diagnostic is not valid UTF-8.");
        }
        for (std::size_t j = 1; j <= continuation; ++j) {
            if (i + j >= diagnostic.size ()
                || (static_cast<unsigned char> (diagnostic[i + j]) & 0xC0) != 0x80) {
                return result_t<session_closing_t>::failure (
                  error_code_t::frame_decode_failed,
                  "Session-closing diagnostic is not valid UTF-8.");
            }
        }
        i += continuation + 1;
    }
    return result_t<session_closing_t>::success (
      session_closing_t{static_cast<close_reason_t> (payload[1]), std::move (diagnostic)});
}

} // namespace zlink::stream_connector::detail
