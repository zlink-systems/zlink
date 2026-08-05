/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/compression.hpp"

#include "runtime/text.hpp"

#include <boost/beast/zlib/error.hpp>
#include <boost/beast/zlib/inflate_stream.hpp>

#include <cstddef>

namespace zlink::http_client::detail
{

namespace beast = boost::beast;

[[noreturn]] void fail_decode ()
{
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::protocol_error,
      "HTTP response compressed body is malformed");
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
    char chunk[16384];
    for (;;) {
        zs.next_out = chunk;
        zs.avail_out = sizeof chunk;
        beast::error_code ec;
        inflater.write (zs, beast::zlib::Flush::sync, ec);
        decoded.append (chunk, sizeof chunk - zs.avail_out);
        if (decoded.size () > decoded_limit) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::capacity_exceeded,
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
    if (size < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 0x08) {
        fail_decode ();
    }

    const unsigned char flags = data[3];
    std::size_t offset = 10;
    if (flags & 0x04) {
        if (offset + 2 > size) {
            fail_decode ();
        }
        const std::size_t extra = data[offset] | (data[offset + 1] << 8);
        offset += 2 + extra;
    }
    for (const unsigned char flag :
         {static_cast<unsigned char> (0x08), static_cast<unsigned char> (0x10)}) {
        if (flags & flag) {
            while (offset < size && data[offset] != 0) {
                ++offset;
            }
            ++offset;
        }
    }
    if (flags & 0x02) {
        offset += 2;
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
    if (size >= 2 && (data[0] & 0x0f) == 8 && ((data[0] << 8 | data[1]) % 31) == 0) {
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
