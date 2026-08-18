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

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
#include <sw/redis++/redis++.h>
#endif

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
        const auto high =
          static_cast<std::uint64_t> (source ()) << 32;
        return high ^ static_cast<std::uint64_t> (source ());
    } ();
    return next.fetch_add (1, std::memory_order_relaxed);
}

// -- SHA-256 (FIPS 180-4), self-contained -----------------------------------
// This extension is a header-only package that links no OpenSSL/hashing
// library, and it cannot reach the framework-internal
// runtime/locations/sha256.hpp across the package boundary (that header is
// private to the zlink_framework target). This is the same minimal,
// from-scratch implementation used by the store-record golden test and by
// the framework-internal sha256.hpp -- verified against real Redis Lua
// cmsgpack output via the golden fixture.
inline std::array<std::uint8_t, 32> sha256_bytes (std::string_view input)
{
    static constexpr std::array<std::uint32_t, 64> k{
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
            const auto s0 = rotr (w[index - 15], 7) ^ rotr (w[index - 15], 18)
              ^ (w[index - 15] >> 3);
            const auto s1 = rotr (w[index - 2], 17) ^ rotr (w[index - 2], 19)
              ^ (w[index - 2] >> 10);
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
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
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
    }
    else if (tag == 0xd9) {
        length = msgpack_next (bytes, offset);
    }
    else if (tag == 0xda) {
        length = (static_cast<std::size_t> (msgpack_next (bytes, offset)) << 8)
          | msgpack_next (bytes, offset);
    }
    else if (tag == 0xdb) {
        for (int shift = 0; shift < 4; ++shift)
            length = (length << 8) | msgpack_next (bytes, offset);
    }
    else {
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
        for (int shift = 0; shift < 4; ++shift) value = (value << 8) | msgpack_next (bytes, offset);
        return value;
    }
    if (tag == 0xcf) {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 8; ++shift) value = (value << 8) | msgpack_next (bytes, offset);
        return value;
    }
    throw std::invalid_argument ("opaque record value has an unrecognized int tag");
}

inline bool msgpack_read_bool (const std::string &bytes, std::size_t &offset)
{
    const auto tag = msgpack_next (bytes, offset);
    if (tag == 0xc2) return false;
    if (tag == 0xc3) return true;
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

} // namespace detail

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
namespace detail
{

inline std::string normalize_connection_string (
  const std::string &connection_string)
{
    return connection_string.find ("://")
               != std::string::npos
             ? connection_string
             : "tcp://" + connection_string;
}

inline std::unique_ptr<sw::redis::Redis> make_redis (
  const std::string &connection_string,
  std::chrono::milliseconds operation_timeout)
{
    const sw::redis::Uri uri (
      normalize_connection_string (connection_string));
    auto options = uri.connection_options ();
    options.connect_timeout = operation_timeout;
    options.socket_timeout = operation_timeout;
    return std::make_unique<sw::redis::Redis> (
      options, uri.connection_pool_options ());
}

class redis_location_worker_t
{
  public:
    redis_location_worker_t () : _thread ([this] { run (); }) {}

    ~redis_location_worker_t ()
    {
        {
            std::lock_guard lock (_gate);
            _stopping = true;
        }
        _ready.notify_all ();
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

    redis_location_worker_t (const redis_location_worker_t &) = delete;
    redis_location_worker_t &operator= (const redis_location_worker_t &) = delete;

    template <typename T, typename TFunc> task_t<T> submit (TFunc &&func)
    {
        zlink::framework::detail::task_completion_source_t<T> completion;
        auto task = completion.task ();
        {
            std::lock_guard lock (_gate);
            _queue.emplace_back (
              [completion, func = std::forward<TFunc> (func)] () mutable {
                  try {
                      if constexpr (std::is_void_v<T>) {
                          func ();
                          completion.complete (
                            result_t<void>::success ());
                      }
                      else {
                          completion.complete (
                            result_t<T>::success (func ()));
                      }
                  }
                  catch (const framework_exception_t &error) {
                      completion.complete (result_t<T>::failure (
                        error.kind (), error.what ()));
                  }
                  catch (const std::exception &error) {
                      completion.complete (
                        result_t<T>::failure (framework_error_kind_t::internal_failure,
                                              error.what ()));
                  }
                  catch (...) {
                      completion.complete (
                        result_t<T>::failure (framework_error_kind_t::internal_failure,
                                              "redis worker failure"));
                  }
              });
        }
        _ready.notify_one ();
        return task;
    }

  private:
    void run ()
    {
        for (;;) {
            std::function<void ()> work;
            {
                std::unique_lock lock (_gate);
                _ready.wait (lock, [&] { return _stopping || !_queue.empty (); });
                if (_stopping && _queue.empty ()) {
                    return;
                }
                work = std::move (_queue.front ());
                _queue.pop_front ();
            }
            work ();
        }
    }

    std::mutex _gate;
    std::condition_variable _ready;
    std::deque<std::function<void ()>> _queue;
    bool _stopping = false;
    std::thread _thread;
};

} // namespace detail
#endif


class redis_location_store_t final : public location_store_t
{
  public:
    explicit redis_location_store_t (
      redis_location_options_t options) :
        _options (std::move (options))
    {
        if (_options.connection_string.empty ()
            || _options.key_prefix.empty ())
            throw std::invalid_argument (
              "Redis location connection and key prefix are required");
        if (_options.key_prefix.find ('{') != std::string::npos
            || _options.key_prefix.find ('}') != std::string::npos)
            throw std::invalid_argument (
              "Redis location key prefix must not contain '{' or '}'");
        if (_options.operation_timeout
            <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument (
              "Redis location operation timeout must be positive");
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        _redis = detail::make_redis (
          _options.connection_string,
          _options.operation_timeout);
#endif
    }

    ~redis_location_store_t () override = default;

  private:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        validate_key (key.value);
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<store_read_result_t> (
          [this, key = std::move (key)] {
              auto &redis = *_redis;
              const std::vector<std::string> keys{record_key (key.value)};
              const std::vector<std::string> args{};
              const auto result =
                redis.eval<std::vector<std::string>> (
                  std::string (read_script),
                  keys.begin (), keys.end (),
                  args.begin (), args.end ());
              if (result.size () < 2)
                  throw sw::redis::Error ("invalid opaque location read result");
              const auto now = from_unix_ms (std::stoll (result[1]));
              if (result[0] == "missing")
                  return store_read_result_t{store_missing_t{now}};
              if (result[0] != "found" || result.size () != 5)
                  throw sw::redis::Error ("unknown opaque location read result");
              const auto expires_at_ms = std::stoll (result[4]);
              return store_read_result_t{
                store_found_t{
                  store_value_t{
                    string_to_bytes (result[2]),
                    store_version_t{result[3]},
                    expires_at_ms > 0
                      ? std::optional{from_unix_ms (expires_at_ms)}
                      : std::nullopt,
                    now}}};
          });
#else
        (void) key;
        return unavailable<store_read_result_t> ();
#endif
    }

    task_t<store_write_result_t> write (
      store_write_request_t request) override
    {
        validate_write (request);
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<store_write_result_t> (
          [this, request = std::move (request)] {
              std::vector<std::string> logical_keys;
              std::map<std::string, std::size_t> key_indexes;
              const auto add_key = [&] (const std::string &key) {
                  if (key_indexes.contains (key))
                      return;
                  key_indexes.emplace (
                    key, logical_keys.size () + 1);
                  logical_keys.push_back (key);
              };
              for (const auto &condition : request.conditions)
                  std::visit (
                    [&] (const auto &value) {
                        add_key (value.key.value);
                    },
                    condition);
              for (const auto &mutation : request.mutations)
                  std::visit (
                    [&] (const auto &value) {
                        add_key (value.key.value);
                    },
                    mutation);

              std::vector<std::string> keys;
              keys.reserve (logical_keys.size () + 3);
              for (const auto &key : logical_keys)
                  keys.push_back (record_key (key));
              keys.push_back (index_key ());
              keys.push_back (map_key ());
              keys.push_back (sequence_key ());

              std::vector<std::string> args;
              args.push_back (
                std::to_string (request.conditions.size ()));
              for (const auto &condition : request.conditions) {
                  const auto &key = std::visit (
                    [] (const auto &value)
                      -> const store_key_t & {
                        return value.key;
                    },
                    condition);
                  args.push_back (std::to_string (
                    key_indexes.at (key.value)));
                  if (const auto *version =
                        std::get_if<
                          store_version_condition_t> (
                          &condition)) {
                      args.push_back ("version");
                      args.push_back (
                        version->expected.value);
                  }
                  else {
                      args.push_back ("missing");
                      args.push_back ({});
                  }
              }
              args.push_back (
                std::to_string (request.mutations.size ()));
              for (const auto &mutation : request.mutations) {
                  std::visit (
                    [&] (const auto &value) {
                        args.push_back (std::to_string (
                          key_indexes.at (value.key.value)));
                        using value_t =
                          std::decay_t<decltype (value)>;
                        args.push_back (value.key.value);
                        if constexpr (
                          std::is_same_v<value_t, store_put_t>) {
                            args.push_back ("put");
                            args.push_back (
                              bytes_to_string (value.bytes));
                            args.push_back (
                              value.retention
                                ? std::to_string (
                                    value.retention->count ())
                                : std::string{"-1"});
                        }
                        else {
                            args.push_back ("delete");
                            args.push_back ({});
                            args.push_back ({"-1"});
                        }
                    },
                    mutation);
              }

              auto &redis = *_redis;
              const auto result =
                redis.eval<std::vector<std::string>> (
                  std::string (write_script),
                  keys.begin (), keys.end (),
                  args.begin (), args.end ());
              if (result.size () < 2)
                  throw sw::redis::Error (
                    "invalid opaque location write result");
              const auto now = from_unix_ms (
                std::stoll (result[1]));
              if (result[0] == "conflict")
                  return store_write_result_t{
                    store_write_conflict_t{now}};
              if (result[0] != "applied"
                  || (result.size () - 2) % 2 != 0)
                  throw sw::redis::Error (
                    "unknown opaque location write result");
              store_write_applied_t applied;
              applied.store_now = now;
              for (std::size_t index = 2;
                   index < result.size (); index += 2) {
                  const auto logical_index =
                    static_cast<std::size_t> (
                      std::stoull (result[index]));
                  if (logical_index == 0
                      || logical_index > logical_keys.size ())
                      throw sw::redis::Error (
                        "invalid opaque location key index");
                  applied.put_versions.push_back (
                    {{logical_keys[logical_index - 1]},
                     store_version_t{
                       result[index + 1]}});
              }
              return store_write_result_t{
                std::move (applied)};
          });
#else
        (void) request;
        return unavailable<store_write_result_t> ();
#endif
    }

    task_t<store_scan_result_t> scan (
      store_scan_request_t request) override
    {
        validate_prefix (request.prefix);
        if (request.limit == 0 || request.limit > 1000)
            throw std::invalid_argument (
              "location scan limit must be 1..1000");
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<store_scan_result_t> (
          [this, request = std::move (request)] {
              std::lock_guard lock (_scan_gate);
              const auto now = std::chrono::system_clock::now ();
              const auto steady_now =
                std::chrono::steady_clock::now ();
              for (auto item = _scan_snapshots.begin ();
                   item != _scan_snapshots.end ();) {
                  if (item->second.expires_at <= steady_now)
                      item = _scan_snapshots.erase (item);
                  else
                      ++item;
              }
              std::uint64_t snapshot_id = 0;
              std::size_t offset = 0;
              if (request.cursor) {
                  const auto first_separator =
                    request.cursor->value.find (':');
                  const auto second_separator =
                    first_separator == std::string::npos
                      ? std::string::npos
                      : request.cursor->value.find (
                          ':', first_separator + 1);
                  if (first_separator == std::string::npos
                      || second_separator == std::string::npos)
                      return store_scan_result_t{
                        store_scan_expired_t{}};
                  try {
                      const auto epoch = std::stoull (
                        request.cursor->value.substr (
                          0, first_separator));
                      if (epoch != _scan_epoch)
                          return store_scan_result_t{
                            store_scan_expired_t{}};
                      snapshot_id = std::stoull (
                        request.cursor->value.substr (
                          first_separator + 1,
                          second_separator
                            - first_separator - 1));
                      offset = static_cast<std::size_t> (
                        std::stoull (
                          request.cursor->value.substr (
                            second_separator + 1)));
                  }
                  catch (...) {
                      return store_scan_result_t{
                        store_scan_expired_t{}};
                  }
              }
              else {
                  if (_scan_snapshots.size ()
                      >= max_scan_snapshots)
                      _scan_snapshots.erase (
                        _scan_snapshots.begin ());
                  snapshot_id = ++_next_snapshot;
                  auto &snapshot =
                    _scan_snapshots[snapshot_id];
                  snapshot.expires_at =
                    steady_now + scan_snapshot_retention;
                  auto &redis = *_redis;
                  // The physical opaque key is sha256(logical key), so it
                  // carries no prefix relationship to the logical key
                  // (22-location-store-redis.md#7's clean-break scheme).
                  // The index ZSET below is this provider's private
                  // secondary index -- it exists purely to answer prefix
                  // scans and is not part of the cross-language contract.
                  const auto original_keys =
                    redis.command<std::vector<std::string>> (
                      "ZRANGE", index_key (), 0, -1);
                  for (const auto &original_key : original_keys) {
                      if (!original_key.starts_with (request.prefix))
                          continue;
                      const auto mapped =
                        redis.hget (map_key (), original_key);
                      if (!mapped)
                          continue;
                      const auto members =
                        redis.command<std::vector<std::string>> (
                          "ZREVRANGE", *mapped, 0, 0);
                      if (members.empty ())
                          continue;
                      const auto decoded =
                        detail::decode_opaque_value (members[0]);
                      if (decoded.original_key != original_key)
                          continue;
                      if (decoded.tombstone
                          || (decoded.expires_at_ms > 0
                              && decoded.expires_at_ms
                                   <= static_cast<std::uint64_t> (
                                     unix_ms (now))))
                          continue;
                      snapshot.items.push_back (
                        {{decoded.original_key},
                         {string_to_bytes (decoded.raw_bytes),
                          store_version_t{decoded.version},
                          decoded.expires_at_ms > 0
                            ? std::optional{
                                from_unix_ms (static_cast<std::int64_t> (
                                  decoded.expires_at_ms))}
                            : std::nullopt,
                          now}});
                  }
                  std::sort (
                    snapshot.items.begin (),
                    snapshot.items.end (),
                    [] (const auto &left, const auto &right) {
                        return left.key.value
                               < right.key.value;
                    });
              }

              const auto snapshot =
                _scan_snapshots.find (snapshot_id);
              if (snapshot == _scan_snapshots.end ()
                  || offset > snapshot->second.items.size ())
                  return store_scan_result_t{
                    store_scan_expired_t{}};
              store_scan_page_t page;
              page.store_now = now;
              auto end = offset;
              std::size_t encoded_size = 0;
              while (
                end < snapshot->second.items.size ()
                && end - offset < request.limit) {
                  const auto item_size =
                    encoded_scan_item_size (
                      snapshot->second.items[end]);
                  if (end != offset
                      && encoded_size + item_size
                           > max_scan_page_bytes)
                      break;
                  encoded_size += item_size;
                  ++end;
              }
              page.items.insert (
                page.items.end (),
                snapshot->second.items.begin ()
                  + static_cast<std::ptrdiff_t> (offset),
                snapshot->second.items.begin ()
                  + static_cast<std::ptrdiff_t> (end));
              if (end < snapshot->second.items.size ())
                  page.next_cursor = store_scan_cursor_t{
                    std::to_string (_scan_epoch) + ":"
                    + std::to_string (snapshot_id) + ":"
                    + std::to_string (end)};
              else
                  _scan_snapshots.erase (snapshot);
              return store_scan_result_t{std::move (page)};
          });
#else
        (void) request;
        return unavailable<store_scan_result_t> ();
#endif
    }

  private:
    // KEYS = { record_key } ; ARGV = {} (none needed: the record key alone
    // identifies the ZSET append-log).
    static constexpr std::string_view read_script = R"(
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000
  + math.floor(tonumber(time[2]) / 1000)
local members = redis.call('ZREVRANGE', KEYS[1], 0, 0)
if #members == 0 then return {'missing', tostring(nowMs)} end
if string.byte(members[1], 1) ~= 1 then
  error('unrecognized opaque record format tag')
end
local record = cmsgpack.unpack(string.sub(members[1], 2))
local expiresAt = tonumber(record[4])
if record[5] == true or (expiresAt > 0 and expiresAt <= nowMs) then
  return {'missing', tostring(nowMs)}
end
return {'found', tostring(nowMs), record[2], record[3], tostring(expiresAt)}
)";

    // KEYS = { record_key_1..N, index_key, map_key, sequence_key }
    // ARGV = conditionCount, {keyIndex, kind, expected}*,
    //        mutationCount, {keyIndex, originalKey, kind, bytes, retentionMs}*
    static constexpr std::string_view write_script = R"(
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000
  + math.floor(tonumber(time[2]) / 1000)

local function currentVersion(keyIndex)
  local members = redis.call('ZREVRANGE', KEYS[keyIndex], 0, 0)
  if #members == 0 then return nil end
  if string.byte(members[1], 1) ~= 1 then
    error('unrecognized opaque record format tag')
  end
  local record = cmsgpack.unpack(string.sub(members[1], 2))
  local expiresAt = tonumber(record[4])
  if record[5] == true or (expiresAt > 0 and expiresAt <= nowMs) then
    return nil
  end
  return record[3]
end

local offset = 1
local conditionCount = tonumber(ARGV[offset])
offset = offset + 1
for i = 1, conditionCount do
  local keyIndex = tonumber(ARGV[offset])
  local kind = ARGV[offset + 1]
  local expected = ARGV[offset + 2]
  offset = offset + 3
  local current = currentVersion(keyIndex)
  if kind == 'missing' then
    if current ~= nil then return {'conflict', tostring(nowMs)} end
  elseif current ~= expected then
    return {'conflict', tostring(nowMs)}
  end
end

local mutationCount = tonumber(ARGV[offset])
offset = offset + 1
local indexKey = KEYS[#KEYS - 2]
local mapKey = KEYS[#KEYS - 1]
local sequenceKey = KEYS[#KEYS]
local result = {'applied', tostring(nowMs)}
for i = 1, mutationCount do
  local keyIndex = tonumber(ARGV[offset])
  local originalKey = ARGV[offset + 1]
  local kind = ARGV[offset + 2]
  local bytes = ARGV[offset + 3]
  local retentionMs = tonumber(ARGV[offset + 4])
  offset = offset + 5
  local recordKey = KEYS[keyIndex]
  local tombstone = kind ~= 'put'
  local version = tostring(redis.call('INCR', sequenceKey))
  local expiresAt = 0
  if retentionMs >= 0 then expiresAt = nowMs + retentionMs end
  local member = '\1' .. cmsgpack.pack({
    originalKey, tombstone and '' or bytes, version, expiresAt, tombstone
  })
  redis.call('ZADD', recordKey, tonumber(version), member)
  redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
  redis.call('ZADD', indexKey, 0, originalKey)
  redis.call('HSET', mapKey, originalKey, recordKey)
  table.insert(result, tostring(keyIndex))
  table.insert(result, version)
end
return result
)";

    template <typename T> static task_t<T> unavailable ()
    {
        return task_t<T> (result_t<T>::failure (
          framework_error_kind_t::internal_failure,
          "redis-plus-plus client is not available in this build"));
    }

    static void validate_key (const std::string &key)
    {
        if (key.empty () || key.size () > 1024)
            throw std::invalid_argument (
              "location store key must contain 1..1024 bytes");
    }

    static void validate_prefix (const std::string &prefix)
    {
        if (prefix.size () > 1024)
            throw std::invalid_argument (
              "location scan prefix exceeds 1024 bytes");
    }

    static void validate_write (
      const store_write_request_t &request)
    {
        std::set<std::string> conditions;
        std::set<std::string> mutations;
        std::set<std::string> keys;
        std::size_t encoded = 0;
        for (const auto &condition : request.conditions) {
            const auto &key = std::visit (
              [] (const auto &value)
                -> const store_key_t & {
                  return value.key;
              },
              condition);
            validate_key (key.value);
            if (!conditions.insert (key.value).second)
                throw std::invalid_argument (
                  "location write repeats a condition key");
            keys.insert (key.value);
            encoded += key.value.size ();
            if (const auto *version =
                  std::get_if<
                    store_version_condition_t> (
                    &condition)) {
                if (version->expected.value.empty ()
                    || version->expected.value.size ()
                         > 4096)
                    throw std::invalid_argument (
                      "version condition requires 1..4096 bytes");
                encoded += version->expected.value.size ();
            }
        }
        for (const auto &mutation : request.mutations) {
            std::visit (
              [&] (const auto &value) {
                  validate_key (value.key.value);
                  if (!mutations.insert (
                        value.key.value).second)
                      throw std::invalid_argument (
                        "location write repeats a mutation key");
                  keys.insert (value.key.value);
                  encoded += value.key.value.size ();
                  using value_t =
                    std::decay_t<decltype (value)>;
                  if constexpr (
                    std::is_same_v<value_t, store_put_t>) {
                      if (value.bytes.size ()
                            > 1024u * 1024u)
                          throw std::invalid_argument (
                            "location value exceeds 1 MiB");
                      if (value.retention
                          && *value.retention
                               <= std::chrono::milliseconds::zero ())
                          throw std::invalid_argument (
                            "location retention must be positive");
                      encoded += value.bytes.size ();
                  }
              },
              mutation);
        }
        if (keys.size () > 2048)
            throw std::invalid_argument (
              "location write exceeds 2048 unique keys");
        if (encoded > 4u * 1024u * 1024u)
            throw std::invalid_argument (
              "location write exceeds 4 MiB");
    }

    static std::string bytes_to_string (
      const std::vector<std::byte> &bytes)
    {
        return {
          reinterpret_cast<const char *> (bytes.data ()),
          bytes.size ()};
    }

    static std::vector<std::byte> string_to_bytes (
      const std::string &value)
    {
        const auto *first =
          reinterpret_cast<const std::byte *> (value.data ());
        return {first, first + value.size ()};
    }

    static std::size_t encoded_scan_item_size (
      const store_scan_item_t &item) noexcept
    {
        return item.key.value.size ()
               + item.value.version.value.size ()
               + item.value.bytes.size ();
    }

    // 22-location-store-redis.md#7: the public opaque-record key. The
    // `{zlink-location-v3}` segment is a Redis Cluster hashtag: everything
    // inside the braces (and only that) is hashed to a slot, so the record
    // key, the secondary-index key, the original-key map, and the sequence
    // counter below all land on the same slot -- required for the write
    // script's multi-key EVAL to be valid under Cluster.
    std::string hash_base () const
    {
        return _options.key_prefix + ":{zlink-location-v3}";
    }

    std::string record_key (std::string_view logical_key) const
    {
        return hash_base () + ":opaque:" + detail::sha256_hex (logical_key);
    }

    std::string index_key () const { return hash_base () + ":opaque:index"; }
    std::string map_key () const { return hash_base () + ":opaque:map"; }
    std::string sequence_key () const { return hash_base () + ":opaque:sequence"; }

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    static std::chrono::system_clock::time_point redis_now (
      sw::redis::Redis &redis)
    {
        const auto reply =
          redis.command<std::vector<std::string>> ("TIME");
        if (reply.size () != 2)
            throw sw::redis::Error (
              "invalid Redis TIME result");
        return std::chrono::system_clock::time_point{
          std::chrono::seconds (std::stoll (reply[0]))
          + std::chrono::microseconds (
              std::stoll (reply[1]))};
    }
#endif

    static std::chrono::system_clock::time_point from_unix_ms (
      std::int64_t value)
    {
        return std::chrono::system_clock::time_point{
          std::chrono::milliseconds (value)};
    }

    static std::int64_t unix_ms (std::chrono::system_clock::time_point value)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
          value.time_since_epoch ())
          .count ();
    }

    redis_location_options_t _options;
    struct scan_snapshot_t
    {
        std::vector<store_scan_item_t> items;
        std::chrono::steady_clock::time_point expires_at;
    };
    static constexpr std::size_t max_scan_snapshots = 128;
    static constexpr std::size_t max_scan_page_bytes =
      4u * 1024u * 1024u;
    static constexpr auto scan_snapshot_retention =
      std::chrono::minutes (5);
    std::mutex _scan_gate;
    std::map<std::uint64_t, scan_snapshot_t>
      _scan_snapshots;
    std::uint64_t _scan_epoch =
      detail::next_scan_epoch ();
    std::uint64_t _next_snapshot = 0;
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    std::unique_ptr<sw::redis::Redis> _redis;
    detail::redis_location_worker_t _worker;
#endif
};


class redis_relocation_store_t final : public relocation_store_t
{
  public:
    explicit redis_relocation_store_t (
      redis_relocation_options_t options) :
        _options (std::move (options))
    {
        if (_options.connection_string.empty ()
            || _options.key_prefix.empty ())
            throw std::invalid_argument (
              "Redis relocation connection and key prefix are required");
        if (_options.key_prefix.find ('{') != std::string::npos
            || _options.key_prefix.find ('}') != std::string::npos)
            throw std::invalid_argument (
              "Redis relocation key prefix must not contain '{' or '}'");
        if (_options.operation_timeout
            <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument (
              "Redis relocation operation timeout must be positive");
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        _redis = detail::make_redis (
          _options.connection_string,
          _options.operation_timeout);
#endif
    }

    ~redis_relocation_store_t () override = default;

  private:
    task_t<blob_put_result_t> put (
      blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) override
    {
        validate (reference, retention);
        if (payload.size () > 64u * 1024u * 1024u)
            throw std::invalid_argument (
              "relocation blob exceeds 64 MiB");
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        std::vector<std::byte> owned (
          payload.begin (), payload.end ());
        return _worker.submit<blob_put_result_t> (
          [this, reference = std::move (reference),
           payload = std::move (owned), retention] {
              auto &redis = *_redis;
              const auto keys = std::vector<std::string>{
                payload_key (reference.value)};
              const auto args = std::vector<std::string>{
                std::string{
                  reinterpret_cast<const char *> (
                    payload.data ()),
                  payload.size ()},
                std::to_string (retention.count ())};
              const auto result =
                redis.eval<std::vector<std::string>> (
                  std::string (put_script),
                  keys.begin (), keys.end (),
                  args.begin (), args.end ());
              if (result.size () != 3)
                  throw sw::redis::Error (
                    "invalid opaque relocation put result");
              const auto now = from_unix_ms (
                std::stoll (result[1]));
              const auto expires_at = from_unix_ms (
                std::stoll (result[2]));
              if (result[0] == "stored")
                  return blob_put_result_t{
                    blob_stored_t{expires_at, now}};
              if (result[0] == "same")
                  return blob_put_result_t{
                    blob_already_stored_t{
                      expires_at, now}};
              if (result[0] == "conflict")
                  return blob_put_result_t{
                    blob_conflict_t{now}};
              throw sw::redis::Error (
                "unknown opaque relocation put result");
          });
#else
        (void) reference;
        (void) payload;
        (void) retention;
        return unavailable<blob_put_result_t> ();
#endif
    }

    task_t<blob_read_result_t> read (
      blob_reference_t reference) override
    {
        validate_reference (reference);
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<blob_read_result_t> (
          [this, reference = std::move (reference)] {
              auto &redis = *_redis;
              const auto key = payload_key (reference.value);
              const auto payload = redis.get (key);
              const auto now = redis_now (redis);
              if (!payload)
                  return blob_read_result_t{
                    blob_missing_t{now}};
              const auto ttl = redis.pttl (key);
              if (ttl <= 0)
                  return blob_read_result_t{
                    blob_missing_t{now}};
              const auto *first =
                reinterpret_cast<const std::byte *> (
                  payload->data ());
              return blob_read_result_t{
                blob_found_t{
                  {first, first + payload->size ()},
                  now + std::chrono::milliseconds{ttl},
                  now}};
          });
#else
        (void) reference;
        return unavailable<blob_read_result_t> ();
#endif
    }

    task_t<blob_renew_result_t> renew (
      blob_reference_t reference,
      std::chrono::milliseconds retention) override
    {
        validate (reference, retention);
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<blob_renew_result_t> (
          [this, reference = std::move (reference), retention] {
              auto &redis = *_redis;
              const auto key = payload_key (reference.value);
              const auto now = redis_now (redis);
              if (!redis.pexpire (key, retention))
                  return blob_renew_result_t{
                    blob_missing_t{now}};
              return blob_renew_result_t{
                blob_renewed_t{
                  now + retention, now}};
          });
#else
        (void) reference;
        (void) retention;
        return unavailable<blob_renew_result_t> ();
#endif
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        validate_reference (reference);
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        return _worker.submit<void> (
          [this, reference = std::move (reference)] {
              auto &redis = *_redis;
              (void) redis.del (
                payload_key (reference.value));
          });
#else
        (void) reference;
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "redis-plus-plus client is not available in this build"));
#endif
    }

  private:
    static constexpr std::string_view put_script = R"(
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000
  + math.floor(tonumber(time[2]) / 1000)
local existing = redis.call('GET', KEYS[1])
if existing then
  local ttl = redis.call('PTTL', KEYS[1])
  local expiresAt = ttl > 0 and nowMs + ttl or nowMs
  if existing == ARGV[1] then
    return {'same', tostring(nowMs), tostring(expiresAt)}
  end
  return {'conflict', tostring(nowMs), tostring(expiresAt)}
end
redis.call('PSETEX', KEYS[1], ARGV[2], ARGV[1])
return {'stored', tostring(nowMs),
        tostring(nowMs + tonumber(ARGV[2]))}
)";

    template <typename T> static task_t<T> unavailable ()
    {
        return task_t<T> (result_t<T>::failure (
          framework_error_kind_t::internal_failure,
          "redis-plus-plus client is not available in this build"));
    }

    static void validate_reference (
      const blob_reference_t &reference)
    {
        if (reference.value.empty ()
            || reference.value.size () > 4096)
            throw std::invalid_argument (
              "relocation reference must contain 1..4096 bytes");
    }

    static void validate (
      const blob_reference_t &reference,
      std::chrono::milliseconds retention)
    {
        validate_reference (reference);
        if (retention <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument (
              "relocation retention must be positive");
    }

    // 23-relocation-store-redis.md#8: `{prefix}:{zlink-relocation-v1}:blob:
    // {reference}`, a separately versioned domain tag independent of the
    // Location Store's `{zlink-location-v3}` opaque record namespace, even
    // when both Stores share one Redis deployment.
    std::string payload_key (
      std::string_view reference) const
    {
        return _options.key_prefix + ":{zlink-relocation-v1}:blob:"
               + std::string (reference);
    }

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    static std::chrono::system_clock::time_point redis_now (
      sw::redis::Redis &redis)
    {
        const auto reply =
          redis.command<std::vector<std::string>> ("TIME");
        if (reply.size () != 2)
            throw sw::redis::Error (
              "invalid Redis TIME result");
        return std::chrono::system_clock::time_point{
          std::chrono::seconds (std::stoll (reply[0]))
          + std::chrono::microseconds (
              std::stoll (reply[1]))};
    }
#endif

    static std::chrono::system_clock::time_point from_unix_ms (
      std::int64_t value)
    {
        return std::chrono::system_clock::time_point{
          std::chrono::milliseconds (value)};
    }

    redis_relocation_options_t _options;
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    std::unique_ptr<sw::redis::Redis> _redis;
    detail::redis_location_worker_t _worker;
#endif
};

} // namespace zlink::framework::redis
