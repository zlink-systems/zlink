/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/framing/frame_codec.hpp"

#include <limits>

namespace zlink::stream_connector::detail
{

namespace
{

void write_prefix (std::vector<std::uint8_t> &frame,
                   std::size_t header_size,
                   std::size_t payload_size)
{
    frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_size >> 24) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_size >> 16) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_size >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (payload_size & 0xff));
}

} // namespace

bool frame_codec_t::validate_frame_size (std::size_t header_size,
                                         std::size_t payload_size,
                                         const connector_options_t &options)
{
    return header_size <= std::numeric_limits<std::uint16_t>::max ()
           && payload_size <= options.max_send_payload_size;
}

bool frame_codec_t::validate_receive_frame_size (std::size_t header_size,
                                                 std::size_t payload_size,
                                                 const connector_options_t &options)
{
    return header_size <= std::numeric_limits<std::uint16_t>::max ()
           && payload_size <= options.max_receive_payload_size;
}

result_t<std::vector<std::uint8_t>> frame_codec_t::encode_prefix (std::size_t header_size,
                                                                  std::size_t payload_size)
{
    if (header_size > std::numeric_limits<std::uint16_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::frame_too_large,
                                                             "Header exceeds u16 header_size.");
    }
    if (payload_size > std::numeric_limits<std::uint32_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::frame_too_large,
                                                             "Payload exceeds u32 payload_size.");
    }
    std::vector<std::uint8_t> prefix;
    prefix.reserve (6);
    write_prefix (prefix, header_size, payload_size);
    return result_t<std::vector<std::uint8_t>>::success (std::move (prefix));
}

result_t<std::vector<std::uint8_t>> frame_codec_t::encode (const std::vector<std::uint8_t> &header,
                                                           const std::vector<std::uint8_t> &payload,
                                                           const connector_options_t &options)
{
    if (!validate_frame_size (header.size (), payload.size (), options)) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::frame_too_large, "Stream frame exceeds configured limits.");
    }
    if (header.size () > std::numeric_limits<std::uint16_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::frame_too_large,
                                                             "Header exceeds u16 header_size.");
    }
    if (payload.size () > std::numeric_limits<std::uint32_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::frame_too_large,
                                                             "Payload exceeds u32 payload_size.");
    }
    std::vector<std::uint8_t> frame;
    frame.reserve (6 + header.size () + payload.size ());
    write_prefix (frame, header.size (), payload.size ());
    frame.insert (frame.end (), header.begin (), header.end ());
    frame.insert (frame.end (), payload.begin (), payload.end ());
    return result_t<std::vector<std::uint8_t>>::success (std::move (frame));
}

} // namespace zlink::stream_connector::detail
