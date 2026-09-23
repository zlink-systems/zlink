/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// This header is the official Redis Location/Relocation Store provider
// (21-location-runtime.md#2.4, 22-location-store-redis.md#7,
// 23-relocation-store-redis.md#8). The Location Store's opaque record is a
// cross-language public contract: the Redis key derivation
// (SHA-256(logical-key preimage), Cluster-hashtagged namespace) and the
// stored value's cmsgpack wire shape must match dotnet/java byte-for-byte so
// any language can read a record another language wrote. Everything else
// here (the write/scan Lua scripts, the scan snapshot/cursor bookkeeping,
// the secondary index used to answer prefix scans) is this provider's
// private implementation, per the same spec sections.

namespace zlink::framework::redis
{

struct redis_location_options_t
{
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{5000};
};

struct redis_relocation_options_t
{
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{5000};
};

class redis_location_options_builder_t
{
  public:
    explicit redis_location_options_builder_t (std::shared_ptr<redis_location_options_t> options) :
        _options (std::move (options))
    {
    }

    redis_location_options_builder_t &set_connection_string (std::string value)
    {
        _options->connection_string = std::move (value);
        return *this;
    }

    redis_location_options_builder_t &set_key_prefix (std::string value)
    {
        _options->key_prefix = std::move (value);
        return *this;
    }

    redis_location_options_builder_t &set_operation_timeout (std::chrono::milliseconds value)
    {
        _options->operation_timeout = value;
        return *this;
    }

  private:
    std::shared_ptr<redis_location_options_t> _options;
};

class redis_relocation_options_builder_t
{
  public:
    explicit redis_relocation_options_builder_t (
      std::shared_ptr<redis_relocation_options_t> options) :
        _options (std::move (options))
    {
    }

    redis_relocation_options_builder_t &set_connection_string (std::string value)
    {
        _options->connection_string = std::move (value);
        return *this;
    }

    redis_relocation_options_builder_t &set_key_prefix (std::string value)
    {
        _options->key_prefix = std::move (value);
        return *this;
    }

    redis_relocation_options_builder_t &set_operation_timeout (std::chrono::milliseconds value)
    {
        _options->operation_timeout = value;
        return *this;
    }

  private:
    std::shared_ptr<redis_relocation_options_t> _options;
};

namespace detail
{

/* redis_location_store_t seeds its scan epoch from this on every build, so it
 * cannot live behind the async-client guard the way the connection helpers do.
 * It needs only <atomic> and <random>, both of which this header includes
 * unconditionally. */
inline std::uint64_t next_scan_epoch ()
{
    static std::atomic<std::uint64_t> next = [] {
        std::random_device source;
        const auto high = static_cast<std::uint64_t> (source ()) << 32;
        return high ^ static_cast<std::uint64_t> (source ());
    }();
    return next.fetch_add (1, std::memory_order_relaxed);
}

// -- SHA-256 (FIPS 180-4), self-contained -----------------------------------
// This extension links no OpenSSL/hashing library, and it cannot reach the
// framework-internal
// runtime/locations/sha256.hpp across the package boundary (that header is
// private to the zlink_framework target). This is the same minimal,
// from-scratch implementation used by the store-record golden test and by
// the framework-internal sha256.hpp -- verified against real Redis Lua
// cmsgpack output via the golden fixture.
inline std::array<std::uint8_t, 32> sha256_bytes (std::string_view input)
{
    static constexpr std::array<std::uint32_t, 64> k{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<std::uint8_t> bytes (input.begin (), input.end ());
    const auto bit_length = static_cast<std::uint64_t> (input.size ()) * 8;
    bytes.push_back (0x80);
    while (bytes.size () % 64 != 56)
        bytes.push_back (0x00);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back (static_cast<std::uint8_t> (bit_length >> shift));

    const auto rotr = [] (std::uint32_t value, int bits) -> std::uint32_t {
        return (value >> bits) | (value << (32 - bits));
    };

    for (std::size_t block = 0; block < bytes.size (); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto p = block + index * 4;
            w[index] = (static_cast<std::uint32_t> (bytes[p]) << 24)
                       | (static_cast<std::uint32_t> (bytes[p + 1]) << 16)
                       | (static_cast<std::uint32_t> (bytes[p + 2]) << 8)
                       | static_cast<std::uint32_t> (bytes[p + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const auto s0 =
              rotr (w[index - 15], 7) ^ rotr (w[index - 15], 18) ^ (w[index - 15] >> 3);
            const auto s1 =
              rotr (w[index - 2], 17) ^ rotr (w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3];
        auto e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t index = 0; index < 64; ++index) {
            const auto s1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto temp1 = hh + s1 + ch + k[index] + w[index];
            const auto s0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::array<std::uint8_t, 32> digest{};
    for (int index = 0; index < 8; ++index) {
        digest[index * 4] = static_cast<std::uint8_t> (h[index] >> 24);
        digest[index * 4 + 1] = static_cast<std::uint8_t> (h[index] >> 16);
        digest[index * 4 + 2] = static_cast<std::uint8_t> (h[index] >> 8);
        digest[index * 4 + 3] = static_cast<std::uint8_t> (h[index]);
    }
    return digest;
}

inline std::string sha256_hex (std::string_view input)
{
    static constexpr char digits[] = "0123456789abcdef";
    const auto digest = sha256_bytes (input);
    std::string result;
    result.reserve (64);
    for (const auto byte : digest) {
        result.push_back (digits[byte >> 4]);
        result.push_back (digits[byte & 0x0f]);
    }
    return result;
}

// -- cmsgpack decode (read path only; writes go through the Lua script,
// which uses Redis's own cmsgpack.pack so cpp never needs a client-side
// msgpack encoder) -----------------------------------------------------
struct opaque_member_t
{
    std::string original_key;
    std::string raw_bytes;
    std::string version;
    std::uint64_t expires_at_ms = 0;
    bool tombstone = false;
};

inline std::uint8_t msgpack_next (const std::string &bytes, std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw std::invalid_argument ("opaque record value is truncated");
    return static_cast<std::uint8_t> (bytes[offset++]);
}

inline std::string msgpack_read_str (const std::string &bytes, std::size_t &offset)
{
    const auto tag = msgpack_next (bytes, offset);
    std::size_t length = 0;
    if ((tag & 0xe0) == 0xa0) {
        length = tag & 0x1f;
    } else if (tag == 0xd9) {
        length = msgpack_next (bytes, offset);
    } else if (tag == 0xda) {
        length = (static_cast<std::size_t> (msgpack_next (bytes, offset)) << 8)
                 | msgpack_next (bytes, offset);
    } else if (tag == 0xdb) {
        for (int shift = 0; shift < 4; ++shift)
            length = (length << 8) | msgpack_next (bytes, offset);
    } else {
        throw std::invalid_argument ("opaque record value has an unrecognized str tag");
    }
    if (offset + length > bytes.size ())
        throw std::invalid_argument ("opaque record value is truncated");
    auto result = bytes.substr (offset, length);
    offset += length;
    return result;
}

inline std::uint64_t msgpack_read_uint (const std::string &bytes, std::size_t &offset)
{
    const auto tag = msgpack_next (bytes, offset);
    if ((tag & 0x80) == 0)
        return tag;
    if (tag == 0xcc)
        return msgpack_next (bytes, offset);
    if (tag == 0xcd)
        return (static_cast<std::uint64_t> (msgpack_next (bytes, offset)) << 8)
               | msgpack_next (bytes, offset);
    if (tag == 0xce) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 4; ++shift)
            value = (value << 8) | msgpack_next (bytes, offset);
        return value;
    }
    if (tag == 0xcf) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 8; ++shift)
            value = (value << 8) | msgpack_next (bytes, offset);
        return value;
    }
    throw std::invalid_argument ("opaque record value has an unrecognized int tag");
}

inline bool msgpack_read_bool (const std::string &bytes, std::size_t &offset)
{
    const auto tag = msgpack_next (bytes, offset);
    if (tag == 0xc2)
        return false;
    if (tag == 0xc3)
        return true;
    throw std::invalid_argument ("opaque record value has an unrecognized bool tag");
}

// `raw` is the full stored value: a 1-byte format tag (0x01) followed by the
// cmsgpack-encoded 5-element array. 22-location-store-redis.md#7 requires an
// unrecognized tag to fail explicitly rather than be guessed at.
inline opaque_member_t decode_opaque_value (const std::string &raw)
{
    if (raw.empty () || static_cast<std::uint8_t> (raw[0]) != 0x01)
        throw std::invalid_argument ("unrecognized opaque record format tag");
    std::size_t offset = 1;
    const auto array_tag = msgpack_next (raw, offset);
    if ((array_tag & 0xf0) != 0x90 || (array_tag & 0x0f) != 5)
        throw std::invalid_argument ("opaque record value is not a 5-element array");
    opaque_member_t member;
    member.original_key = msgpack_read_str (raw, offset);
    member.raw_bytes = msgpack_read_str (raw, offset);
    member.version = msgpack_read_str (raw, offset);
    member.expires_at_ms = msgpack_read_uint (raw, offset);
    member.tombstone = msgpack_read_bool (raw, offset);
    return member;
}

inline std::string normalize_connection_string (const std::string &connection_string)
{
    return connection_string.find ("://") != std::string::npos ? connection_string
                                                               : "tcp://" + connection_string;
}

} // namespace detail

// The Redis client (redis-plus-plus, hiredis, libuv) is a private
// implementation detail of the zlink_framework_locations_redis library: this
// header names no third-party type, so a consumer links the library without
// redis-plus-plus or libuv on its own include or link path.
class redis_location_store_t final : public location_store_t
{
  public:
    using options_type = redis_location_options_t;
    using options_builder_type = redis_location_options_builder_t;

    explicit redis_location_store_t (redis_location_options_t options);
    ~redis_location_store_t () override;

  private:
    task_t<store_read_result_t> read (store_key_t key) override;
    task_t<store_write_result_t> write (store_write_request_t request) override;
    task_t<store_scan_result_t> scan (store_scan_request_t request) override;

    std::unique_ptr<location_store_t> _store;
};

class redis_relocation_store_t final : public relocation_store_t
{
  public:
    using options_type = redis_relocation_options_t;
    using options_builder_type = redis_relocation_options_builder_t;

    explicit redis_relocation_store_t (redis_relocation_options_t options);
    ~redis_relocation_store_t () override;

  private:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override;
    task_t<blob_read_result_t> read (blob_reference_t reference) override;
    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override;
    task_t<void> erase (blob_reference_t reference) override;

    std::unique_ptr<relocation_store_t> _store;
};

} // namespace zlink::framework::redis
