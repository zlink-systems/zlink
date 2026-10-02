/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/detail/sha256.hpp>

#include <algorithm>
#include <array>
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

inline constexpr std::chrono::milliseconds default_operation_timeout{5000};

struct redis_location_options_t
{
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{default_operation_timeout};
};

struct redis_relocation_options_t
{
    std::string connection_string;
    std::string key_prefix;
    std::chrono::milliseconds operation_timeout{default_operation_timeout};
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

using zlink::framework::detail::sha256_hex;

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

inline constexpr std::uint8_t msgpack_fixed_string_mask = 0xe0;
inline constexpr std::uint8_t msgpack_fixed_string_tag = 0xa0;
inline constexpr std::uint8_t msgpack_fixed_string_length_mask = 0x1f;
inline constexpr std::uint8_t msgpack_str8_tag = 0xd9;
inline constexpr std::uint8_t msgpack_str16_tag = 0xda;
inline constexpr std::uint8_t msgpack_str32_tag = 0xdb;
inline constexpr std::uint8_t msgpack_positive_fixint_mask = 0x80;
inline constexpr std::uint8_t msgpack_uint8_tag = 0xcc;
inline constexpr std::uint8_t msgpack_uint16_tag = 0xcd;
inline constexpr std::uint8_t msgpack_uint32_tag = 0xce;
inline constexpr std::uint8_t msgpack_uint64_tag = 0xcf;
inline constexpr std::uint8_t msgpack_false_tag = 0xc2;
inline constexpr std::uint8_t msgpack_true_tag = 0xc3;
inline constexpr std::uint8_t opaque_record_format_tag = 0x01;
inline constexpr std::uint8_t msgpack_fixed_array_mask = 0xf0;
inline constexpr std::uint8_t msgpack_fixed_array_tag = 0x90;
inline constexpr std::uint8_t msgpack_fixed_array_length_mask = 0x0f;
inline constexpr std::size_t opaque_record_member_count = 5;

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
    if ((tag & msgpack_fixed_string_mask) == msgpack_fixed_string_tag) {
        length = tag & msgpack_fixed_string_length_mask;
    } else if (tag == msgpack_str8_tag) {
        length = msgpack_next (bytes, offset);
    } else if (tag == msgpack_str16_tag) {
        length = (static_cast<std::size_t> (msgpack_next (bytes, offset)) << 8)
                 | msgpack_next (bytes, offset);
    } else if (tag == msgpack_str32_tag) {
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
    if ((tag & msgpack_positive_fixint_mask) == 0)
        return tag;
    if (tag == msgpack_uint8_tag)
        return msgpack_next (bytes, offset);
    if (tag == msgpack_uint16_tag)
        return (static_cast<std::uint64_t> (msgpack_next (bytes, offset)) << 8)
               | msgpack_next (bytes, offset);
    if (tag == msgpack_uint32_tag) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 4; ++shift)
            value = (value << 8) | msgpack_next (bytes, offset);
        return value;
    }
    if (tag == msgpack_uint64_tag) {
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
    if (tag == msgpack_false_tag)
        return false;
    if (tag == msgpack_true_tag)
        return true;
    throw std::invalid_argument ("opaque record value has an unrecognized bool tag");
}

// `raw` is the full stored value: a 1-byte format tag (opaque_record_format_tag) followed by the
// cmsgpack-encoded 5-element array. 22-location-store-redis.md#7 requires an
// unrecognized tag to fail explicitly rather than be guessed at.
inline opaque_member_t decode_opaque_value (const std::string &raw)
{
    if (raw.empty () || static_cast<std::uint8_t> (raw[0]) != opaque_record_format_tag)
        throw std::invalid_argument ("unrecognized opaque record format tag");
    std::size_t offset = 1;
    const auto array_tag = msgpack_next (raw, offset);
    if ((array_tag & msgpack_fixed_array_mask) != msgpack_fixed_array_tag
        || (array_tag & msgpack_fixed_array_length_mask) != opaque_record_member_count)
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
