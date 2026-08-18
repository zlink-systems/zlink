/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Target-contract pin for checklist C-3 (store record golden fixture:
// 21-location-runtime.md#2.4, 22-location-store-redis.md#7). This test
// consumes golden/store-record-v1.json directly. Both the SHA-256 and the
// cmsgpack member decoder below are written from scratch against,
// respectively, FIPS 180-4 and the MessagePack type table in
// 22-location-store-redis.md#7 (str family for strings/bytes, unsigned int
// family for expiresAtMs, bool for tombstone), so this is a fourth
// independent cross-check alongside the node/java/dotnet decoders and the
// golden's own generation script (verified against real Redis Lua
// cmsgpack.pack output).
//
// The sol-review finding-3 block near the end of main() also drives a
// record through the real production writer (provider_location_repository_t
// over a fake in-memory store), which is why this target links zlink::framework
// unlike the rest of the file's from-scratch/no-link-library checks above it.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

// Checklist C-4 (cpp store-record convergence): reuse the production
// SHA-256 and base64 codecs here (POSDDD) instead of hand-rolling a fourth
// copy of either. zlink/locations/redis.hpp's format-tag/cmsgpack decode
// helpers (zlink::framework::redis::detail::*) are header-only outside the
// ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT guard, so they link here
// with no redis++ dependency. in_memory_store_providers.hpp and
// provider_location_repository.hpp (below) are the real production writer
// driven by the finding-3 block near the end of main(); unlike the headers
// above, that does pull in zlink::framework (see the CMakeLists.txt target).
#include "runtime/locations/base64.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/sha256.hpp"
#include <zlink/locations/redis.hpp>

#include <chrono>

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

        // Production redis.hpp cross-check: the real provider's value
        // decoder (zlink::framework::redis::detail::decode_opaque_value),
        // not just this file's from-scratch one, must decode every vector
        // identically.
        const auto full_bytes = from_hex (vector.at ("fullValueHex").get<std::string> ());
        const std::string full_string (
          reinterpret_cast<const char *> (full_bytes.data ()), full_bytes.size ());
        const auto production_decoded =
          zlink::framework::redis::detail::decode_opaque_value (full_string);
        assert (production_decoded.original_key
                == unescape_nul (vector.at ("originalKey").get<std::string> ()));
        assert (production_decoded.version == vector.at ("version").get<std::string> ());
        assert (production_decoded.expires_at_ms
                == std::stoull (vector.at ("expiresAtMs").get<std::string> ()));
        assert (production_decoded.tombstone == vector.at ("tombstone").get<bool> ());
        assert (production_decoded.raw_bytes.size () == decoded.raw_bytes.size ());
        assert (std::memcmp (production_decoded.raw_bytes.data (), decoded.raw_bytes.data (),
                             decoded.raw_bytes.size ())
                == 0);
    }

    // Checklist C-4: the authority record's `payload` field converts from
    // hex to base64 (21-location-runtime.md#2.4, 22-location-store-redis.md
    // #7). Round-trip every authority payload vector in the fixture through
    // the production base64 codec, and confirm it's the standard ('+'/'/')
    // alphabet, not URL-safe -- both authority vectors' payloads contain
    // '+', which a URL-safe decoder would reject.
    bool checked_base64_payload = false;
    for (const auto &vector : root.at ("valueVectors").at ("genericOpaqueRecord")) {
        if (vector.at ("tombstone").get<bool> () || !vector.at ("decoded").contains ("payload"))
            continue;
        const auto payload_b64 = vector.at ("decoded").at ("payload").get<std::string> ();
        const auto decoded_payload = zlink::framework::runtime::base64_decode (payload_b64);
        const auto reencoded = zlink::framework::runtime::base64_encode (decoded_payload);
        assert (reencoded == payload_b64);
        checked_base64_payload = true;
    }
    assert (checked_base64_payload);

    // Production sha256.hpp cross-check, independent of this file's
    // from-scratch sha256() above: every key-derivation vector must also
    // hash correctly through the real header the production store uses.
    for (const auto &key : root.at ("keyDerivation")) {
        const auto preimage_hex = key.at ("preimageHex").get<std::string> ();
        const auto raw = from_hex (preimage_hex);
        std::vector<std::byte> preimage_bytes;
        preimage_bytes.reserve (raw.size ());
        for (const auto byte : raw)
            preimage_bytes.push_back (static_cast<std::byte> (byte));
        const auto digest = zlink::framework::runtime::sha256 (preimage_bytes);
        std::string digest_hex;
        static const char digits[] = "0123456789abcdef";
        for (const auto byte : digest) {
            const auto value = std::to_integer<unsigned char> (byte);
            digest_hex.push_back (digits[value >> 4]);
            digest_hex.push_back (digits[value & 0x0f]);
        }
        assert (digest_hex == key.at ("sha256Hex").get<std::string> ());
    }

    // Clean break (22-location-store-redis.md#7): an unrecognized format tag
    // must fail explicitly against the real production decoder, never be
    // guessed at. Byte 0 is the format tag; flip 0x01 to 0x02 on a real
    // vector and confirm zlink::framework::redis::detail::decode_opaque_value
    // throws instead of silently reading it as a tag-0x01 record.
    {
        const auto &vector = root.at ("valueVectors").at ("genericOpaqueRecord").at (0);
        auto tampered = from_hex (vector.at ("fullValueHex").get<std::string> ());
        assert (tampered[0] == 0x01);
        tampered[0] = 0x02;
        const std::string tampered_string (
          reinterpret_cast<const char *> (tampered.data ()), tampered.size ());
        bool threw = false;
        try {
            (void) zlink::framework::redis::detail::decode_opaque_value (tampered_string);
        }
        catch (const std::invalid_argument &) {
            threw = true;
        }
        assert (threw && "unrecognized format tag must fail explicitly, not be guessed at");
    }

    // Sol review [M] (cpp-store-record-convergence e14bce0297, finding 3):
    // every check above drives a standalone or production *decoder*, but
    // nothing drives a record through the production *writer*
    // (provider_location_repository_t) and compares the result against the
    // golden. Do that for the owner-lease record against a fake in-memory
    // store, field-compared (order-independent) against the golden's
    // "decoded" object per notes.jsonByteLayering.
    //
    // NOT byte-exact: driving this test surfaced that
    // provider_location_repository.hpp's owner-lease payload is built with
    // plain `nlohmann::json{...}` (claim_owner_lease, near the top of the
    // class), whose default ObjectType is std::map -- so `.dump()` emits
    // keys in *alphabetical* order ({"leaseGeneration",...,"ownerId",...,
    // "recordVersion",...}) while the golden's jsonBytesHex pins *insertion*
    // order ({"recordVersion",...,"ownerId",...,"leaseGeneration",...}, see
    // notes.jsonByteLayering: "the byte-compare step pins one exact
    // serialization"). The same alphabetical-vs-insertion mismatch applies
    // to json_t (= plain nlohmann::json, provider_location_repository.hpp)
    // and therefore to every other encode_* function in the file, including
    // encode_authority.
    //
    // This is one of five confirmed divergences between the current cpp
    // encoders and the golden's canonical §2.4 schema, none of them fixed
    // by this pass (the authority "storeVersion" extra field was
    // investigated and found genuinely load-bearing -- a production
    // consumer at framework/src/runtime/stateful/public_host_runtime.cpp
    // compares a freshly re-read `authority_snapshot_t::store_version`
    // against a wire-echoed "expected_store_version" that only stays valid
    // because storeVersion round-trips through the record body today;
    // removing it broke that check and is reported, not fixed, see the
    // handoff notes for this finding). The other four:
    // encode_mesh_record's "generation" field, encode_target's
    // "owner"/"nodeLifecycleGeneration" fields, encode_bundle's "slots" vs
    // golden's "count" (plus int vs string objectKind), and mesh descriptor
    // encode()'s ~12 additional fields. All five plus this json_t ordering
    // defect are recommended as a single follow-up unit of work -- fixing
    // storeVersion alone (or the ordering alone) does not move the golden
    // byte-compare goal forward, so they should land together with the
    // production consumer(s) each one touches audited in the same pass.
    // Until then this block stays a field-compare, not a byte-compare.
    {
        using namespace zlink::framework;
        using namespace zlink::framework::runtime;

        in_memory_location_store_t store;
        provider_location_repository_t repository (store);

        // provider_location_repository_t's private owner-generation counter
        // key (framework/src/runtime/locations/provider_location_repository
        // .hpp, `counter_key`: "zlink:v11:owner-counter"). It is
        // provider-private, not part of the cross-language contract --
        // pre-seeding it here is test setup (to reach the golden vector's
        // leaseGeneration "5" deterministically), not a protocol
        // assumption.
        const store_key_t counter_key{"zlink:v11:owner-counter"};
        const std::string five = "5";
        std::vector<std::byte> counter_bytes (five.size ());
        for (std::size_t index = 0; index < five.size (); ++index)
            counter_bytes[index] = static_cast<std::byte> (five[index]);
        const auto seeded =
          store
            .write ({.conditions = {store_missing_condition_t{counter_key}},
                     .mutations = {store_put_t{counter_key, counter_bytes, std::nullopt}}})
            .result ()
            .value ();
        assert (std::holds_alternative<store_write_applied_t> (seeded));

        const auto claimed =
          repository.claim_owner_lease ("owner-a", std::chrono::seconds (30)).result ().value ();
        const auto *claim = std::get_if<owner_lease_claimed_t> (&claimed);
        assert (claim != nullptr);
        assert (claim->token.owner_id == "owner-a");
        assert (claim->token.lease_generation == 5);

        // key_owner(owner_id) == preimage2("owner-lease", owner_id) ==
        // "owner-lease\0" + owner_id -- exactly the golden's own "owner-lease"
        // keyDerivation preimage, so this is not a guess at the provider's
        // private key scheme.
        const std::string owner_key_value = std::string ("owner-lease") + '\0' + "owner-a";
        const auto stored = store.read ({owner_key_value}).result ().value ();
        const auto *found = std::get_if<store_found_t> (&stored);
        assert (found != nullptr);

        const auto &owner_lease_vector =
          root.at ("valueVectors").at ("genericOpaqueRecord").at (4);
        assert (owner_lease_vector.at ("name").get<std::string> () == "ownerLease-expired");

        // Field-compare (order-independent) against the golden's "decoded"
        // object -- see the block comment above for why this is not yet a
        // byte-exact compare.
        const auto produced_json = nlohmann::json::parse (
          std::string (reinterpret_cast<const char *> (found->value.bytes.data ()),
                       found->value.bytes.size ()));
        assert (produced_json == owner_lease_vector.at ("decoded"));
    }

    return 0;
}
