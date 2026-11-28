/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_ticket.hpp"

#include "runtime/locations/base64.hpp"
#include <zlink/detail/admission_ticket_wire.hpp>
#include <zlink/framework/detail/utf8.hpp>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>

namespace zlink::framework::runtime::admission_queue
{
namespace
{

using zlink::detail::admission_ticket_encoded_size;
using zlink::detail::admission_ticket_header_size;
using zlink::detail::admission_ticket_mac_size;
using zlink::detail::admission_ticket_size;
constexpr std::uint64_t clock_skew_ms = 2000;

void append_u64 (bytes_t &output, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back (static_cast<std::byte> ((value >> shift) & 0xff));
}

std::uint64_t read_u64 (std::span<const std::byte> bytes)
{
    std::uint64_t value = 0;
    for (auto byte : bytes)
        value = (value << 8) | std::to_integer<unsigned> (byte);
    return value;
}

bool valid_rid (const bytes_t &value)
{
    return !value.empty () && value.size () <= max_rid_bytes;
}

bool valid_connection (const connection_t &value)
{
    return valid_rid (value.node_rid) && value.node_generation != 0
           && valid_stream_name (value.stream_name) && valid_rid (value.session_rid);
}

bytes_t decode_key (std::string_view value)
{
    auto key = decode_base64url (value);
    if (!key || key->size () < 32 || key->size () > 64)
        throw std::invalid_argument ("Invalid STREAM admission queue key");
    return std::move (*key);
}

bytes_t mac_input (const connection_t &connection, std::span<const std::byte> header)
{
    constexpr std::string_view prefix = "zlink-admission-ticket-v1";
    const auto prefix_bytes = std::as_bytes (std::span (prefix));
    bytes_t input (prefix_bytes.begin (), prefix_bytes.end ());
    input.reserve (prefix.size () + 28 + connection.node_rid.size ()
                   + connection.stream_name.size () + connection.session_rid.size ());
    input.push_back (std::byte{0});
    const auto append_field = [&] (std::span<const std::byte> field) {
        input.push_back (static_cast<std::byte> (field.size ()));
        input.insert (input.end (), field.begin (), field.end ());
    };
    append_field (connection.node_rid);
    append_u64 (input, connection.node_generation);
    append_field (std::as_bytes (std::span (connection.stream_name)));
    append_field (connection.session_rid);
    input.insert (input.end (), header.begin () + 1, header.end ());
    return input;
}

std::array<unsigned char, admission_ticket_mac_size> sign (const bytes_t &key, const bytes_t &input)
{
    std::array<unsigned char, admission_ticket_mac_size> mac{};
    unsigned size = 0;
    if (!HMAC (EVP_sha256 (), key.data (), static_cast<int> (key.size ()),
               reinterpret_cast<const unsigned char *> (input.data ()), input.size (), mac.data (),
               &size)
        || size != mac.size ())
        throw std::runtime_error ("STREAM admission ticket signing failed");
    return mac;
}

} // namespace

std::string encode_base64url (std::span<const std::byte> bytes)
{
    auto encoded = base64_encode (bytes);
    std::replace (encoded.begin (), encoded.end (), '+', '-');
    std::replace (encoded.begin (), encoded.end (), '/', '_');
    encoded.erase (encoded.find_last_not_of ('=') + 1);
    return encoded;
}

std::optional<bytes_t> decode_base64url (std::string_view encoded)
{
    if (encoded.size () % 4 == 1
        || encoded.find_first_not_of (
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
             != std::string_view::npos)
        return std::nullopt;

    std::string padded (encoded);
    std::replace (padded.begin (), padded.end (), '-', '+');
    std::replace (padded.begin (), padded.end (), '_', '/');
    padded.append ((4 - padded.size () % 4) % 4, '=');
    auto bytes = base64_decode (padded);
    if (encode_base64url (bytes) != encoded)
        return std::nullopt;
    return bytes;
}

bool valid_stream_name (std::string_view name) noexcept
{
    return !name.empty () && name.size () <= 255 && detail::is_valid_utf8 (name);
}

std::uint64_t random_nonzero_u64 ()
{
    std::uint64_t value = 0;
    do {
        if (RAND_bytes (reinterpret_cast<unsigned char *> (&value), sizeof (value)) != 1)
            throw std::runtime_error ("STREAM admission queue randomness unavailable");
    } while (value == 0);
    return value;
}

ticket_signer_t::ticket_signer_t (std::string_view current, std::optional<std::string> previous) :
    _current (decode_key (current)), _previous (previous ? decode_key (*previous) : bytes_t{})
{
}

std::string ticket_signer_t::issue (const connection_t &connection,
                                    std::uint64_t epoch,
                                    std::uint64_t expiration) const
{
    if (epoch == 0 || !valid_connection (connection))
        throw std::invalid_argument ("Invalid STREAM admission ticket identity");
    bytes_t bytes;
    bytes.reserve (admission_ticket_size);
    bytes.push_back (std::byte{1});
    append_u64 (bytes, expiration);
    append_u64 (bytes, epoch);
    const auto mac = sign (_current, mac_input (connection, bytes));
    const auto mac_bytes = std::as_bytes (std::span (mac));
    bytes.insert (bytes.end (), mac_bytes.begin (), mac_bytes.end ());
    return encode_base64url (bytes);
}

bool ticket_signer_t::verify (const connection_t &connection,
                              std::string_view ticket,
                              std::uint64_t epoch,
                              std::uint64_t now) const
{
    if (epoch == 0 || ticket.size () != admission_ticket_encoded_size
        || !valid_connection (connection))
        return false;
    const auto bytes = decode_base64url (ticket);
    if (!bytes || bytes->size () != admission_ticket_size || bytes->front () != std::byte{1})
        return false;
    const auto wire = std::span (*bytes);
    if (read_u64 (wire.subspan (9, 8)) != epoch)
        return false;
    const auto expiration = read_u64 (wire.subspan (1, 8));
    if (now > expiration && now - expiration > clock_skew_ms)
        return false;
    const auto input = mac_input (connection, wire.first (admission_ticket_header_size));
    const auto matches = [&] (const bytes_t &key) {
        const auto mac = sign (key, input);
        return CRYPTO_memcmp (wire.data () + admission_ticket_header_size, mac.data (), mac.size ())
               == 0;
    };
    return matches (_current) || (!_previous.empty () && matches (_previous));
}

} // namespace zlink::framework::runtime::admission_queue
