/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/compression.hpp"

#include "runtime/runtime_errors.hpp"
#include "runtime/text.hpp"

#include <boost/beast/zlib/error.hpp>
#include <boost/beast/zlib/inflate_stream.hpp>

#include <cstddef>
#include <cstdint>

namespace zlink::http_client::detail
{

namespace beast = boost::beast;

namespace
{
constexpr std::size_t gzip_header_size = 10;
constexpr std::size_t gzip_trailer_size = 8;
constexpr unsigned char gzip_signature_first = 0x1f;
constexpr unsigned char gzip_signature_second = 0x8b;
constexpr unsigned char deflate_method = 0x08;
constexpr unsigned char gzip_extra_flag = 0x04;
constexpr unsigned char gzip_name_flag = 0x08;
constexpr unsigned char gzip_comment_flag = 0x10;
constexpr unsigned char gzip_header_crc_flag = 0x02;
constexpr std::size_t gzip_flags_offset = 3;
constexpr std::size_t gzip_short_field_size = sizeof (std::uint16_t);
constexpr unsigned char zlib_method_mask = 0x0f;
constexpr unsigned zlib_header_modulus = 31;
} // namespace

[[noreturn]] void fail_decode ()
{
    throw request_protocol_error ("HTTP response compressed body is malformed");
}

std::string inflate_raw (const unsigned char *data, std::size_t size, std::size_t decoded_limit)
{
    if (size == 0) {
        fail_decode ();
    }

    beast::zlib::inflate_stream inflater;
    beast::zlib::z_params zs;
    zs.next_in = data;
    zs.avail_in = size;

    std::string decoded;
    constexpr std::size_t inflate_chunk_size = 16384;
    char chunk[inflate_chunk_size];
    for (;;) {
        zs.next_out = chunk;
        zs.avail_out = sizeof chunk;
        beast::error_code ec;
        inflater.write (zs, beast::zlib::Flush::sync, ec);
        decoded.append (chunk, sizeof chunk - zs.avail_out);
        if (decoded.size () > decoded_limit) {
            throw response_body_limit_error (
              "HTTP response compressed body exceeds max_response_body_size");
        }
        if (ec == beast::zlib::error::end_of_stream) {
            return decoded;
        }
        if (ec || (zs.avail_in == 0 && zs.avail_out != 0)) {
            fail_decode ();
        }
    }
}

std::string gunzip (const std::string &compressed, std::size_t decoded_limit)
{
    const auto *data = reinterpret_cast<const unsigned char *> (compressed.data ());
    const auto size = compressed.size ();
    if (size < gzip_header_size + gzip_trailer_size || data[0] != gzip_signature_first
        || data[1] != gzip_signature_second || data[2] != deflate_method) {
        fail_decode ();
    }

    const unsigned char flags = data[gzip_flags_offset];
    std::size_t offset = gzip_header_size;
    if (flags & gzip_extra_flag) {
        if (offset + gzip_short_field_size > size) {
            fail_decode ();
        }
        const std::size_t extra = data[offset] | (data[offset + 1] << 8);
        offset += gzip_short_field_size + extra;
    }
    for (const unsigned char flag : {gzip_name_flag, gzip_comment_flag}) {
        if (flags & flag) {
            while (offset < size && data[offset] != 0) {
                ++offset;
            }
            ++offset;
        }
    }
    if (flags & gzip_header_crc_flag) {
        offset += gzip_short_field_size;
    }
    if (offset >= size) {
        fail_decode ();
    }
    return inflate_raw (data + offset, size - offset, decoded_limit);
}

std::string inflate_deflate (const std::string &compressed, std::size_t decoded_limit)
{
    const auto *data = reinterpret_cast<const unsigned char *> (compressed.data ());
    const auto size = compressed.size ();
    if (size >= 2 && (data[0] & zlib_method_mask) == deflate_method
        && ((data[0] << 8 | data[1]) % zlib_header_modulus) == 0) {
        return inflate_raw (data + 2, size - 2, decoded_limit);
    }
    return inflate_raw (data, size, decoded_limit);
}

std::optional<std::string> find_header (const std::map<std::string, std::string> &headers,
                                        std::string_view name)
{
    for (const auto &[key, value] : headers) {
        if (iequals (key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

void erase_header (std::map<std::string, std::string> &headers, std::string_view name)
{
    for (auto it = headers.begin (); it != headers.end ();) {
        it = iequals (it->first, name) ? headers.erase (it) : std::next (it);
    }
}

} // namespace zlink::http_client::detail
