/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_metadata_codec.hpp"
#include <service_wire_constants.hpp>
#include <zlink/framework/detail/utf8.hpp>

#include <zlink/framework/contracts/errors/error.hpp>

#include <limits>
#include <string_view>

namespace zlink::framework::detail
{

namespace
{

constexpr std::uint8_t version = 1;

[[noreturn]] void invalid_metadata (std::string message)
{
    throw framework_exception_t (framework_error_kind_t::protocol_error, std::move (message));
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
            || !zlink::framework::detail::is_valid_non_nul_utf8 (key)) {
            invalid_metadata ("application metadata keys must contain 1..255 non-NUL UTF-8 bytes");
        }
        if (value.size () > std::numeric_limits<std::uint16_t>::max ()
            || !zlink::framework::detail::is_valid_non_nul_utf8 (value)) {
            invalid_metadata (
              "application metadata values must contain at most 65535 non-NUL UTF-8 bytes");
        }
        size += 1 + key.size () + 2 + value.size ();
        if (size > runtime::protocol::metadataBytes)
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
    if (encoded.size () < 2 || encoded.size () > runtime::protocol::metadataBytes
        || encoded[0] != version)
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
        if (!zlink::framework::detail::is_valid_non_nul_utf8 (key) || offset + 2 > encoded.size ())
            return false;
        const std::size_t value_size =
          (static_cast<std::size_t> (encoded[offset]) << 8u) | encoded[offset + 1];
        offset += 2;
        if (offset + value_size > encoded.size ())
            return false;
        std::string value (encoded.begin () + static_cast<std::ptrdiff_t> (offset),
                           encoded.begin () + static_cast<std::ptrdiff_t> (offset + value_size));
        offset += value_size;
        if (!zlink::framework::detail::is_valid_non_nul_utf8 (value)
            || !metadata.emplace (std::move (key), std::move (value)).second)
            return false;
    }
    return offset == encoded.size ();
}

} // namespace zlink::framework::detail
