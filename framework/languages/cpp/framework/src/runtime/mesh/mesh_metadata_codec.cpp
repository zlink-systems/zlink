/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_metadata_codec.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <limits>
#include <string_view>

namespace zlink::framework::detail
{

namespace
{

constexpr std::size_t max_encoded_size = 1024;
constexpr std::uint8_t version = 1;

bool has_nul (std::string_view value)
{
    return value.find ('\0') != std::string_view::npos;
}

bool valid_utf8 (std::string_view value)
{
    const auto *bytes = reinterpret_cast<const unsigned char *> (value.data ());
    std::size_t index = 0;
    while (index < value.size ()) {
        const auto first = bytes[index];
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            if (index + 1 >= value.size () || bytes[index + 1] < 0x80
                || bytes[index + 1] > 0xbf)
                return false;
            index += 2;
            continue;
        }
        if (first >= 0xe0 && first <= 0xef) {
            if (index + 2 >= value.size ())
                return false;
            const auto second = bytes[index + 1];
            const auto third = bytes[index + 2];
            const bool valid_second =
              first == 0xe0 ? second >= 0xa0 && second <= 0xbf
                            : (first == 0xed ? second >= 0x80 && second <= 0x9f
                                           : second >= 0x80 && second <= 0xbf);
            if (!valid_second || third < 0x80 || third > 0xbf)
                return false;
            index += 3;
            continue;
        }
        if (first >= 0xf0 && first <= 0xf4) {
            if (index + 3 >= value.size ())
                return false;
            const auto second = bytes[index + 1];
            const bool valid_second =
              first == 0xf0 ? second >= 0x90 && second <= 0xbf
                            : (first == 0xf4 ? second >= 0x80 && second <= 0x8f
                                           : second >= 0x80 && second <= 0xbf);
            if (!valid_second || bytes[index + 2] < 0x80 || bytes[index + 2] > 0xbf
                || bytes[index + 3] < 0x80 || bytes[index + 3] > 0xbf)
                return false;
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

[[noreturn]] void invalid_metadata (std::string message)
{
    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 std::move (message));
}

} // namespace

std::vector<std::uint8_t>
mesh_metadata_codec_t::encode (const std::map<std::string, std::string> &metadata)
{
    if (metadata.empty ())
        return {};
    if (metadata.size () > std::numeric_limits<std::uint8_t>::max ())
        invalid_metadata ("application metadata may contain at most 255 entries");

    std::size_t size = 2;
    for (const auto &[key, value] : metadata) {
        if (key.empty () || key.size () > std::numeric_limits<std::uint8_t>::max ()
            || has_nul (key) || !valid_utf8 (key)) {
            invalid_metadata (
              "application metadata keys must contain 1..255 non-NUL UTF-8 bytes");
        }
        if (value.size () > std::numeric_limits<std::uint16_t>::max () || has_nul (value)
            || !valid_utf8 (value)) {
            invalid_metadata (
              "application metadata values must contain at most 65535 non-NUL UTF-8 bytes");
        }
        size += 1 + key.size () + 2 + value.size ();
        if (size > max_encoded_size)
            invalid_metadata ("encoded application metadata exceeds 1024 bytes");
    }

    std::vector<std::uint8_t> encoded;
    encoded.reserve (size);
    encoded.push_back (version);
    encoded.push_back (static_cast<std::uint8_t> (metadata.size ()));
    for (const auto &[key, value] : metadata) {
        encoded.push_back (static_cast<std::uint8_t> (key.size ()));
        encoded.insert (encoded.end (), key.begin (), key.end ());
        encoded.push_back (static_cast<std::uint8_t> ((value.size () >> 8u) & 0xffu));
        encoded.push_back (static_cast<std::uint8_t> (value.size () & 0xffu));
        encoded.insert (encoded.end (), value.begin (), value.end ());
    }
    return encoded;
}

bool mesh_metadata_codec_t::decode (const std::vector<std::uint8_t> &encoded,
                                    std::map<std::string, std::string> &metadata)
{
    metadata.clear ();
    if (encoded.empty ())
        return true;
    if (encoded.size () < 2 || encoded.size () > max_encoded_size || encoded[0] != version)
        return false;

    const std::size_t count = encoded[1];
    std::size_t offset = 2;
    for (std::size_t entry = 0; entry < count; ++entry) {
        if (offset >= encoded.size ())
            return false;
        const std::size_t key_size = encoded[offset++];
        if (key_size == 0 || offset + key_size > encoded.size ())
            return false;
        std::string key (encoded.begin () + static_cast<std::ptrdiff_t> (offset),
                         encoded.begin () + static_cast<std::ptrdiff_t> (offset + key_size));
        offset += key_size;
        if (has_nul (key) || !valid_utf8 (key) || offset + 2 > encoded.size ())
            return false;
        const std::size_t value_size =
          (static_cast<std::size_t> (encoded[offset]) << 8u) | encoded[offset + 1];
        offset += 2;
        if (offset + value_size > encoded.size ())
            return false;
        std::string value (encoded.begin () + static_cast<std::ptrdiff_t> (offset),
                           encoded.begin () + static_cast<std::ptrdiff_t> (offset + value_size));
        offset += value_size;
        if (has_nul (value) || !valid_utf8 (value)
            || !metadata.emplace (std::move (key), std::move (value)).second)
            return false;
    }
    return offset == encoded.size ();
}

} // namespace zlink::framework::detail
