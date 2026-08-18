/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Target-contract pin for checklist C-3 (store record golden fixture:
// 21-location-runtime.md#2.4, 22-location-store-redis.md#7). This test
// consumes golden/store-record-v1.json directly, independent of any
// production opaque-record store codec -- no language has implemented the
// zlink-location-v3 opaque record write path yet (checklist C-4). It stays
// green today because sha256 key derivation and cmsgpack value decoding
// need nothing from C-4. Both the SHA-256 and the cmsgpack member decoder
// below are written from scratch (this test target links neither OpenSSL
// nor a msgpack library) against, respectively, FIPS 180-4 and the
// MessagePack type table in 22-location-store-redis.md#7 (str family for
// strings/bytes, unsigned int family for expiresAtMs, bool for tombstone),
// so this is a fourth independent cross-check alongside the node/java/dotnet
// decoders and the golden's own generation script (verified against real
// Redis Lua cmsgpack.pack output).

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

std::vector<std::uint8_t> from_hex (const std::string &value)
{
    const auto digit = [] (char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9')
            return static_cast<unsigned char> (ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<unsigned char> (ch - 'a' + 10);
        assert (false);
        return 0;
    };
    assert (value.size () % 2 == 0);
    std::vector<std::uint8_t> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (static_cast<std::uint8_t> (
          (digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

std::string to_hex (const std::uint8_t *data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve (size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back (digits[data[index] >> 4]);
        result.push_back (digits[data[index] & 0x0f]);
    }
    return result;
}

// Minimal from-scratch SHA-256 (FIPS 180-4), test-only.
std::array<std::uint8_t, 32> sha256 (const std::vector<std::uint8_t> &input)
{
    static const std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<std::uint8_t> message (input);
    const std::uint64_t bit_length = static_cast<std::uint64_t> (input.size ()) * 8;
    message.push_back (0x80);
    while (message.size () % 64 != 56)
        message.push_back (0x00);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back (static_cast<std::uint8_t> (bit_length >> shift));

    const auto rotr = [] (std::uint32_t value, int bits) -> std::uint32_t {
        return (value >> bits) | (value << (32 - bits));
    };

    for (std::size_t chunk = 0; chunk < message.size (); chunk += 64) {
        std::uint32_t w[64];
        for (int index = 0; index < 16; ++index) {
            w[index] = (static_cast<std::uint32_t> (message[chunk + index * 4]) << 24)
              | (static_cast<std::uint32_t> (message[chunk + index * 4 + 1]) << 16)
              | (static_cast<std::uint32_t> (message[chunk + index * 4 + 2]) << 8)
              | (static_cast<std::uint32_t> (message[chunk + index * 4 + 3]));
        }
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotr (w[index - 15], 7) ^ rotr (w[index - 15], 18)
              ^ (w[index - 15] >> 3);
            const std::uint32_t s1 = rotr (w[index - 2], 17) ^ rotr (w[index - 2], 19)
              ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[index] + w[index];
            const std::uint32_t s0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<std::uint8_t, 32> digest {};
    for (int index = 0; index < 8; ++index) {
        digest[index * 4] = static_cast<std::uint8_t> (h[index] >> 24);
        digest[index * 4 + 1] = static_cast<std::uint8_t> (h[index] >> 16);
        digest[index * 4 + 2] = static_cast<std::uint8_t> (h[index] >> 8);
        digest[index * 4 + 3] = static_cast<std::uint8_t> (h[index]);
    }
    return digest;
}

struct opaque_member_t
{
    std::string original_key;
    std::vector<std::uint8_t> raw_bytes;
    std::string version;
    std::uint64_t expires_at_ms = 0;
    bool tombstone = false;
};

std::uint8_t next_byte (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    assert (offset < bytes.size ());
    return bytes[offset++];
}

std::vector<std::uint8_t> read_str (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const std::uint8_t tag = next_byte (bytes, offset);
    std::size_t length = 0;
    if ((tag & 0xe0) == 0xa0) {
        length = tag & 0x1f;
    } else if (tag == 0xd9) {
        length = next_byte (bytes, offset);
    } else if (tag == 0xda) {
        length = (static_cast<std::size_t> (next_byte (bytes, offset)) << 8)
          | next_byte (bytes, offset);
    } else if (tag == 0xdb) {
        for (int shift = 0; shift < 4; ++shift)
            length = (length << 8) | next_byte (bytes, offset);
    } else {
        assert (false && "invalid msgpack str tag");
    }
    assert (offset + length <= bytes.size ());
    std::vector<std::uint8_t> value (bytes.begin () + static_cast<long> (offset),
                                     bytes.begin () + static_cast<long> (offset + length));
    offset += length;
    return value;
}

std::uint64_t read_uint (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const std::uint8_t tag = next_byte (bytes, offset);
    if ((tag & 0x80) == 0) return tag;
    if (tag == 0xcc) return next_byte (bytes, offset);
    if (tag == 0xcd)
        return (static_cast<std::uint64_t> (next_byte (bytes, offset)) << 8)
          | next_byte (bytes, offset);
    if (tag == 0xce) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 4; ++shift) value = (value << 8) | next_byte (bytes, offset);
        return value;
    }
    if (tag == 0xcf) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 8; ++shift) value = (value << 8) | next_byte (bytes, offset);
        return value;
    }
    assert (false && "invalid msgpack uint tag");
    return 0;
}

bool read_bool (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const std::uint8_t tag = next_byte (bytes, offset);
    if (tag == 0xc2) return false;
    if (tag == 0xc3) return true;
    assert (false && "invalid msgpack bool tag");
    return false;
}

opaque_member_t decode_opaque_member (const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    const std::uint8_t array_tag = next_byte (bytes, offset);
    assert ((array_tag & 0xf0) == 0x90 && (array_tag & 0x0f) == 5);
    opaque_member_t member;
    {
        const auto value = read_str (bytes, offset);
        member.original_key.assign (value.begin (), value.end ());
    }
    member.raw_bytes = read_str (bytes, offset);
    {
        const auto value = read_str (bytes, offset);
        member.version.assign (value.begin (), value.end ());
    }
    member.expires_at_ms = read_uint (bytes, offset);
    member.tombstone = read_bool (bytes, offset);
    assert (offset == bytes.size ());
    return member;
}

std::string unescape_nul (std::string value)
{
    const std::string marker = "\\u0000";
    std::size_t position = 0;
    while ((position = value.find (marker, position)) != std::string::npos) {
        value.replace (position, marker.size (), std::string (1, '\0'));
        position += 1;
    }
    return value;
}

}

int main ()
{
    std::ifstream fixture (ZLINK_STORE_RECORD_GOLDEN_PATH);
    assert (fixture.good ());
    const auto root = nlohmann::json::parse (fixture);
    const auto prefix = root.at ("prefixExample").get<std::string> ();
    const auto namespace_tag = root.at ("namespace").get<std::string> ();

    for (const auto &key : root.at ("keyDerivation")) {
        const auto preimage = from_hex (key.at ("preimageHex").get<std::string> ());
        const auto digest = sha256 (preimage);
        const auto sha256_hex = to_hex (digest.data (), digest.size ());
        assert (sha256_hex == key.at ("sha256Hex").get<std::string> ());
        const auto expected_key = prefix + ":" + namespace_tag + ":" + sha256_hex;
        assert (expected_key == key.at ("redisKey").get<std::string> ());
    }

    // zlink-location-v3 and zlink-relocation-v1 are Redis Cluster hashtags
    // ({...}) so a multi-key EVAL script (record + sequence counter + index,
    // 22-location-store-redis.md#7) stays same-slot atomic under Cluster; a
    // brace-less key would let Cluster route the keys to different slots.
    assert (namespace_tag == "{zlink-location-v3}:opaque");

    const auto &relocation_blob = root.at ("relocationBlob");
    const auto relocation_bytes = from_hex (relocation_blob.at ("rawBytesHex").get<std::string> ());
    assert (!relocation_bytes.empty ());
    const auto expected_relocation_key = prefix + ":{zlink-relocation-v1}:blob:"
      + relocation_blob.at ("reference").get<std::string> ();
    assert (expected_relocation_key == relocation_blob.at ("redisKey").get<std::string> ());

    for (const auto &vector : root.at ("valueVectors").at ("genericOpaqueRecord")) {
        const auto full = from_hex (vector.at ("fullValueHex").get<std::string> ());
        assert (full[0] == 0x01);
        const std::vector<std::uint8_t> member_bytes (full.begin () + 1, full.end ());
        const auto decoded = decode_opaque_member (member_bytes);

        const auto expected_original_key =
          unescape_nul (vector.at ("originalKey").get<std::string> ());
        assert (decoded.original_key == expected_original_key);
        assert (to_hex (decoded.raw_bytes.data (), decoded.raw_bytes.size ())
                == vector.at ("jsonBytesHex").get<std::string> ());
        assert (decoded.version == vector.at ("version").get<std::string> ());
        assert (std::to_string (decoded.expires_at_ms) == vector.at ("expiresAtMs").get<std::string> ());
        assert (decoded.tombstone == vector.at ("tombstone").get<bool> ());

        const auto expected_member = from_hex (vector.at ("cmsgpackMemberHex").get<std::string> ());
        assert (expected_member == member_bytes);

        if (!vector.at ("tombstone").get<bool> ()) {
            const auto parsed = nlohmann::json::parse (
              std::string (decoded.raw_bytes.begin (), decoded.raw_bytes.end ()));
            assert (parsed == vector.at ("decoded"));
        } else {
            assert (decoded.raw_bytes.empty ());
        }
    }

    return 0;
}
