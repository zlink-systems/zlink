/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/metadata_codec.hpp"

#include <limits>

namespace zlink::stream_connector::detail
{

namespace
{

void write_u16 (std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
    bytes.push_back (static_cast<std::uint8_t> (value & 0xff));
}

std::uint16_t read_u16 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const auto value = static_cast<std::uint16_t> ((bytes[offset] << 8) | bytes[offset + 1]);
    offset += 2;
    return value;
}

} // namespace

std::size_t metadata_codec_t::encoded_size (const metadata_t &metadata)
{
    if (metadata.values.empty ()) {
        return 0;
    }
    std::size_t size = 1;
    for (const auto &[key, value] : metadata.values) {
        size += 1 + key.size () + 2 + value.size ();
    }
    return size;
}

result_t<std::vector<std::uint8_t>> metadata_codec_t::encode (const metadata_t &metadata)
{
    if (encoded_size (metadata) > max_metadata_size) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::validation_failed, "Metadata payload exceeds 1024 bytes.");
    }
    if (metadata.values.size () > std::numeric_limits<std::uint8_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::validation_failed, "Metadata entry count must not exceed 255.");
    }

    std::vector<std::uint8_t> bytes;
    if (metadata.values.empty ()) {
        return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
    }
    bytes.reserve (encoded_size (metadata));
    bytes.push_back (static_cast<std::uint8_t> (metadata.values.size ()));
    for (const auto &[key, value] : metadata.values) {
        if (key.empty () || key.size () > std::numeric_limits<std::uint8_t>::max ()) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::validation_failed,
                                                                 "Metadata key length is invalid.");
        }
        if (value.size () > std::numeric_limits<std::uint16_t>::max ()) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::validation_failed,
                                                                 "Metadata value is too large.");
        }
        bytes.push_back (static_cast<std::uint8_t> (key.size ()));
        bytes.insert (bytes.end (), key.begin (), key.end ());
        write_u16 (bytes, static_cast<std::uint16_t> (value.size ()));
        bytes.insert (bytes.end (), value.begin (), value.end ());
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<metadata_t> metadata_codec_t::decode (const std::vector<std::uint8_t> &bytes)
{
    if (bytes.size () > max_metadata_size) {
        return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                              "Metadata payload exceeds 1024 bytes.");
    }
    if (bytes.empty ()) {
        return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                              "Metadata payload is empty.");
    }
    std::size_t offset = 0;
    const auto count = bytes[offset++];
    metadata_t metadata;
    for (std::uint8_t i = 0; i < count; ++i) {
        if (bytes.size () - offset < 1) {
            return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                                  "Metadata key length is missing.");
        }
        const auto key_size = bytes[offset++];
        if (key_size == 0 || bytes.size () - offset < key_size) {
            return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                                  "Metadata key is invalid.");
        }
        std::string key (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                         bytes.begin () + static_cast<std::ptrdiff_t> (offset + key_size));
        offset += key_size;
        if (bytes.size () - offset < 2) {
            return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                                  "Metadata value length is missing.");
        }
        const auto value_size = read_u16 (bytes, offset);
        if (bytes.size () - offset < value_size) {
            return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                                  "Metadata value is invalid.");
        }
        std::string value (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                           bytes.begin () + static_cast<std::ptrdiff_t> (offset + value_size));
        offset += value_size;
        if (!metadata.values.emplace (std::move (key), std::move (value)).second) {
            return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                                  "Duplicate metadata key.");
        }
    }
    if (offset != bytes.size ()) {
        return result_t<metadata_t>::failure (error_code_t::frame_decode_failed,
                                              "Metadata contains trailing bytes.");
    }
    return result_t<metadata_t>::success (std::move (metadata));
}

} // namespace zlink::stream_connector::detail
