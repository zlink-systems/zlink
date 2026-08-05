/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
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
              const auto redis_key =
                value_key (key.value);
              const auto payload = redis.hget (
                redis_key, "bytes");
              const auto now = redis_now (redis);
              if (!payload)
                  return store_read_result_t{
                    store_missing_t{now}};
              const auto version = redis.hget (
                redis_key, "version");
              if (!version)
                  throw sw::redis::Error (
                    "opaque location row has no version");
              const auto ttl = redis.pttl (redis_key);
              return store_read_result_t{
                store_found_t{
                  store_value_t{
                    string_to_bytes (*payload),
                    store_version_t{*version},
                    ttl > 0
                      ? std::optional{
                          now + std::chrono::milliseconds{ttl}}
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
              keys.reserve (logical_keys.size () + 1);
              for (const auto &key : logical_keys)
                  keys.push_back (value_key (key));
              keys.push_back (version_key ());

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
                            args.push_back ("erase");
                            args.push_back ({});
                            args.push_back ({});
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
                  const auto pattern =
                    value_key (request.prefix) + "*";
                  const auto keys =
                    redis.command<std::vector<std::string>> (
                      "KEYS", pattern);
                  for (const auto &redis_key : keys) {
                      const auto payload =
                        redis.hget (redis_key, "bytes");
                      const auto version =
                        redis.hget (redis_key, "version");
                      if (!payload || !version)
                          continue;
                      const auto ttl = redis.pttl (redis_key);
                      snapshot.items.push_back (
                        {{decode_value_key (redis_key)},
                         {string_to_bytes (*payload),
                          store_version_t{*version},
                          ttl > 0
                            ? std::optional{
                                now + std::chrono::milliseconds{ttl}}
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
    static constexpr std::string_view write_script = R"(
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000
  + math.floor(tonumber(time[2]) / 1000)
local offset = 1
local conditionCount = tonumber(ARGV[offset])
offset = offset + 1
for i = 1, conditionCount do
  local keyIndex = tonumber(ARGV[offset])
  local kind = ARGV[offset + 1]
  local expected = ARGV[offset + 2]
  offset = offset + 3
  local exists = redis.call('EXISTS', KEYS[keyIndex]) == 1
  if kind == 'missing' then
    if exists then return {'conflict', tostring(nowMs)} end
  elseif not exists
      or redis.call('HGET', KEYS[keyIndex], 'version') ~= expected then
    return {'conflict', tostring(nowMs)}
  end
end
local mutationCount = tonumber(ARGV[offset])
offset = offset + 1
local result = {'applied', tostring(nowMs)}
for i = 1, mutationCount do
  local keyIndex = tonumber(ARGV[offset])
  local kind = ARGV[offset + 1]
  local bytes = ARGV[offset + 2]
  local retentionMs = tonumber(ARGV[offset + 3])
  offset = offset + 4
  if kind == 'put' then
    local version = tostring(redis.call('INCR', KEYS[#KEYS]))
    redis.call('HSET', KEYS[keyIndex],
      'bytes', bytes, 'version', version)
    if retentionMs >= 0 then
      redis.call('PEXPIRE', KEYS[keyIndex], retentionMs)
    else
      redis.call('PERSIST', KEYS[keyIndex])
    end
    table.insert(result, tostring(keyIndex))
    table.insert(result, version)
  else
    redis.call('DEL', KEYS[keyIndex])
  end
end
return result
)";

    template <typename T> static task_t<T> unavailable ()
    {
        return task_t<T> (result_t<T>::failure (
          framework_error_kind_t::internal_failure,
          "redis-plus-plus client is not available in this build",
          true));
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

    static std::string hex (std::string_view value)
    {
        static constexpr char digits[] =
          "0123456789abcdef";
        std::string result;
        result.reserve (value.size () * 2);
        for (const auto character : value) {
            const auto byte =
              static_cast<unsigned char> (character);
            result.push_back (digits[byte >> 4]);
            result.push_back (digits[byte & 0x0f]);
        }
        return result;
    }

    static std::string unhex (std::string_view value)
    {
        const auto nibble = [] (char character) {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            throw std::invalid_argument (
              "invalid opaque Redis key");
        };
        if (value.size () % 2 != 0)
            throw std::invalid_argument (
              "invalid opaque Redis key");
        std::string result;
        result.reserve (value.size () / 2);
        for (std::size_t index = 0;
             index < value.size (); index += 2)
            result.push_back (static_cast<char> (
              (nibble (value[index]) << 4)
              | nibble (value[index + 1])));
        return result;
    }

    std::string value_key (std::string_view key) const
    {
        return _options.key_prefix + ":opaque:" + hex (key);
    }

    std::string decode_value_key (
      const std::string &key) const
    {
        const auto prefix =
          _options.key_prefix + ":opaque:";
        if (!key.starts_with (prefix))
            throw std::invalid_argument (
              "Redis key is outside the location namespace");
        return unhex (std::string_view (key).substr (
          prefix.size ()));
    }

    std::string version_key () const
    {
        return _options.key_prefix + ":opaque:version";
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
          "redis-plus-plus client is not available in this build",
          true));
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
          "redis-plus-plus client is not available in this build",
          true));
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

    std::string payload_key (
      std::string_view reference) const
    {
        return _options.key_prefix + ":blob:"
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
