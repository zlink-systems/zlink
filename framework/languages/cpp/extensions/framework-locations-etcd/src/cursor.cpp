/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "cursor.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <limits>

namespace zlink::framework::etcd::detail
{
std::int64_t monotonic_ms ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
             std::chrono::steady_clock::now ().time_since_epoch ())
      .count ();
}
bool snapshot_t::expired () const
{
    const auto current = monotonic_ms ();
    return started > current || current - started >= 300000;
}
cursor_t::cursor_t ()
{
    if (RAND_bytes (_secret.data (), static_cast<int> (_secret.size ())) != 1)
        store_failure ();
}
std::string cursor_t::sign (std::string_view value) const
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (!HMAC (EVP_sha256 (), _secret.data (), static_cast<int> (_secret.size ()),
               reinterpret_cast<const unsigned char *> (value.data ()), value.size (), digest,
               &length))
        store_failure ();
    return {reinterpret_cast<const char *> (digest), length};
}
namespace
{
void integer (std::string &out, std::uint64_t value)
{
    for (int i = 7; i >= 0; --i)
        out += static_cast<char> (value >> (i * 8));
}
void text (std::string &out, std::string_view value)
{
    out += static_cast<char> (value.size () >> 8);
    out += static_cast<char> (value.size ());
    out += value;
}
class reader_t
{
  public:
    explicit reader_t (std::string_view input) : bytes (input) {}
    std::optional<std::uint64_t> integer ()
    {
        if (bytes.size () < 8)
            return std::nullopt;
        std::uint64_t result = 0;
        for (unsigned char c : bytes.substr (0, 8))
            result = (result << 8) | c;
        bytes.remove_prefix (8);
        return result;
    }
    std::optional<std::string> text ()
    {
        if (bytes.size () < 2)
            return std::nullopt;
        const auto length =
          (static_cast<unsigned char> (bytes[0]) << 8) | static_cast<unsigned char> (bytes[1]);
        bytes.remove_prefix (2);
        if (bytes.size () < static_cast<std::size_t> (length))
            return std::nullopt;
        std::string result (bytes.substr (0, length));
        bytes.remove_prefix (length);
        return result;
    }
    std::string_view bytes;
};
constexpr std::string_view alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
std::string base64 (std::string_view value)
{
    std::string out;
    unsigned bits = 0;
    unsigned count = 0;
    for (unsigned char c : value) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += alphabet[(bits >> count) & 63];
        }
    }
    if (count)
        out += alphabet[(bits << (6 - count)) & 63];
    return out;
}
std::optional<std::string> unbase64 (std::string_view value)
{
    if (value.size () % 4 == 1)
        return std::nullopt;
    std::string out;
    unsigned bits = 0;
    unsigned count = 0;
    for (char c : value) {
        const auto index = alphabet.find (c);
        if (index == std::string_view::npos)
            return std::nullopt;
        bits = (bits << 6) | static_cast<unsigned> (index);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char> (bits >> count);
        }
    }
    if (count && (bits & ((1U << count) - 1)))
        return std::nullopt;
    return out;
}
}
std::string
cursor_t::encode (const snapshot_t &snapshot, std::string_view ns, std::string_view prefix) const
{
    std::string body;
    text (body, ns);
    text (body, prefix);
    integer (body, snapshot.cluster);
    integer (body, snapshot.revision);
    integer (body, snapshot.now);
    integer (body, snapshot.started);
    text (body, snapshot.last);
    body += sign (body);
    auto result = base64 (body);
    if (result.size () > 4096)
        store_failure ();
    return result;
}
std::optional<snapshot_t>
cursor_t::decode (std::string_view value, std::string_view ns, std::string_view prefix) const
{
    if (value.size () > 4096)
        return std::nullopt;
    const auto decoded = unbase64 (value);
    if (!decoded || decoded->size () < 32)
        return std::nullopt;
    const std::string_view body (decoded->data (), decoded->size () - 32);
    const auto signature = sign (body);
    if (CRYPTO_memcmp (signature.data (), decoded->data () + body.size (), 32) != 0)
        return std::nullopt;
    reader_t reader (body);
    const auto stored_ns = reader.text (), stored_prefix = reader.text ();
    if (!stored_ns || !stored_prefix || *stored_ns != ns || *stored_prefix != prefix)
        return std::nullopt;
    const auto cluster = reader.integer (), revision = reader.integer (), now = reader.integer (),
               started = reader.integer ();
    const auto last = reader.text ();
    if (!cluster || !revision || !now || !started || !last || !reader.bytes.empty ()
        || *revision == 0
        || *revision > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
        || *now > max_time
        || *started > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
        || last->size () > 1024 || !last->starts_with (prefix))
        return std::nullopt;
    snapshot_t result{*cluster, static_cast<std::int64_t> (*revision), *now,
                      static_cast<std::int64_t> (*started), *last};
    if (result.expired ())
        return std::nullopt;
    return result;
}
} // namespace zlink::framework::etcd::detail
