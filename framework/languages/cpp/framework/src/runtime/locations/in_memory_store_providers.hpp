/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/execution/state_lane.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_store_write.hpp"
#include "../../../../../../runtime/protocol/generated/cpp/service_wire_constants.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{


class in_memory_location_store_t final : public location_store_t
{
    friend struct in_memory_store_continuation_test_access_t;

  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        validate_key (key.value);
        return _lane.run_task ([this, key = std::move (key)] {
            const auto now = std::chrono::system_clock::now ();
            expire (key.value, now);
            const auto found = _values.find (key.value);
            if (found == _values.end ())
                return store_read_result_t{store_missing_t{now}};
            found->second.store_now = now;
            return store_read_result_t{store_found_t{found->second}};
        });
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        validate_location_write (request, validate_key);
        return _lane.run_task ([this, request = std::move (request)] () mutable {
            const auto now = std::chrono::system_clock::now ();
            const auto live_value = [&] (const store_key_t &key) -> const store_value_t * {
                const auto found = _values.find (key.value);
                if (found == _values.end () || expired (found->second, now))
                    return nullptr;
                return &found->second;
            };
            const auto counter = [&] (const store_key_t &key) -> std::optional<std::int64_t> {
                const auto *value = live_value (key);
                if (!value)
                    return 0;
                if (value->bytes.empty ())
                    return std::nullopt;
                return parse_location_counter (
                  {reinterpret_cast<const char *> (value->bytes.data ()), value->bytes.size ()});
            };
            for (const auto &condition : request.conditions) {
                const auto result = std::visit (
                  [&] (const auto &item) {
                      using T = std::decay_t<decltype (item)>;
                      if constexpr (std::is_same_v<T, store_sum_at_most_condition_t>) {
                          return evaluate_location_sum (item, counter);
                      } else {
                          const auto *value = live_value (item.key);
                          bool matches;
                          if constexpr (std::is_same_v<T, store_missing_condition_t>)
                              matches = !value;
                          else if constexpr (std::is_same_v<T, store_version_condition_t>)
                              matches = value && value->version.value == item.expected.value;
                          else
                              matches = value && value->bytes == item.expected;
                          return matches ? location_sum_result_t::matches
                                         : location_sum_result_t::exceeds;
                      }
                  },
                  condition);
                if (result == location_sum_result_t::corrupt)
                    return store_write_result_t{store_write_corrupt_t{now}};
                if (result == location_sum_result_t::exceeds)
                    return store_write_result_t{store_write_conflict_t{now}};
            }

            for (auto &mutation : request.mutations) {
                if (const auto *add = std::get_if<store_add_t> (&mutation)) {
                    auto put = prepare_location_add (*add, counter (add->key));
                    if (!put)
                        return store_write_result_t{store_write_corrupt_t{now}};
                    mutation = std::move (*put);
                }
            }

            store_write_applied_t applied;
            applied.store_now = now;
            for (auto &mutation : request.mutations) {
                if (auto *put = std::get_if<store_put_t> (&mutation)) {
                    const store_version_t version{next_version ()};
                    auto expires_at =
                      put->retention ? std::optional{now + *put->retention} : std::nullopt;
                    _values[put->key.value] =
                      store_value_t{std::move (put->bytes), version, expires_at, now};
                    applied.put_versions.push_back ({std::move (put->key), version});
                } else {
                    _values.erase (std::get<store_delete_t> (mutation).key.value);
                }
            }
            return store_write_result_t{std::move (applied)};
        });
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        validate_prefix (request.prefix);
        if (request.limit == 0 || request.limit > location_page_item_limit)
            throw std::invalid_argument ("location scan limit must be 1..1000");

        return _lane.run_task ([this, request = std::move (request)] {
            const auto now = std::chrono::system_clock::now ();
            std::uint64_t snapshot_id = 0;
            std::size_t offset = 0;
            if (request.cursor) {
                const auto first_separator = request.cursor->value.find (':');
                const auto second_separator =
                  first_separator == std::string::npos
                    ? std::string::npos
                    : request.cursor->value.find (':', first_separator + 1);
                if (first_separator == std::string::npos || second_separator == std::string::npos)
                    return completed<store_scan_result_t> (store_scan_expired_t{});
                try {
                    const auto epoch =
                      std::stoull (request.cursor->value.substr (0, first_separator));
                    if (epoch != _scan_epoch)
                        return completed<store_scan_result_t> (store_scan_expired_t{});
                    snapshot_id = std::stoull (request.cursor->value.substr (
                      first_separator + 1, second_separator - first_separator - 1));
                    offset = static_cast<std::size_t> (
                      std::stoull (request.cursor->value.substr (second_separator + 1)));
                }
                catch (...) {
                    return completed<store_scan_result_t> (store_scan_expired_t{});
                }
            } else {
                if (_snapshots.size () >= max_scan_snapshots)
                    _snapshots.erase (_snapshots.begin ());
                snapshot_id = ++_next_snapshot;
                auto &snapshot = _snapshots[snapshot_id];
                for (auto iterator = _values.begin (); iterator != _values.end ();) {
                    if (expired (iterator->second, now)) {
                        iterator = _values.erase (iterator);
                        continue;
                    }
                    if (iterator->first.starts_with (request.prefix))
                        snapshot.push_back ({{iterator->first}, iterator->second});
                    ++iterator;
                }
            }

            const auto snapshot = _snapshots.find (snapshot_id);
            if (snapshot == _snapshots.end () || offset > snapshot->second.size ())
                return completed<store_scan_result_t> (store_scan_expired_t{});

            store_scan_page_t page;
            page.store_now = now;
            auto end = offset;
            std::size_t encoded_size = 0;
            while (end < snapshot->second.size () && end - offset < request.limit) {
                const auto item_size = encoded_scan_item_size (snapshot->second[end]);
                if (end != offset && encoded_size + item_size > max_scan_page_bytes)
                    break;
                encoded_size += item_size;
                ++end;
            }
            page.items.insert (page.items.end (),
                               snapshot->second.begin () + static_cast<std::ptrdiff_t> (offset),
                               snapshot->second.begin () + static_cast<std::ptrdiff_t> (end));
            if (end < snapshot->second.size ())
                page.next_cursor =
                  store_scan_cursor_t{std::to_string (_scan_epoch) + ":"
                                      + std::to_string (snapshot_id) + ":" + std::to_string (end)};
            else
                _snapshots.erase (snapshot);
            return completed<store_scan_result_t> (std::move (page));
        });
    }

  private:
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    static void validate_key (const std::string &key)
    {
        if (key.empty () || key.size () > location_store_limits::key_bytes)
            throw std::invalid_argument ("location store key must contain 1..1024 bytes");
    }

    static void validate_prefix (const std::string &prefix)
    {
        if (prefix.size () > location_store_limits::key_bytes)
            throw std::invalid_argument ("location scan prefix must contain at most 1024 bytes");
    }

    static bool expired (const store_value_t &value, std::chrono::system_clock::time_point now)
    {
        return value.expires_at && *value.expires_at <= now;
    }

    void expire (const std::string &key, std::chrono::system_clock::time_point now)
    {
        const auto found = _values.find (key);
        if (found != _values.end () && expired (found->second, now))
            _values.erase (found);
    }

    std::string next_version () { return std::to_string (++_next_version); }

    static std::size_t encoded_scan_item_size (const store_scan_item_t &item) noexcept
    {
        return item.key.value.size () + item.value.version.value.size () + item.value.bytes.size ();
    }

    static std::uint64_t make_scan_epoch ()
    {
        std::random_device source;
        return (static_cast<std::uint64_t> (source ()) << 32)
               ^ static_cast<std::uint64_t> (source ());
    }

    state_lane_t _lane;
    std::map<std::string, store_value_t> _values;
    std::map<std::uint64_t, std::vector<store_scan_item_t>> _snapshots;
    static constexpr std::size_t max_scan_snapshots = 128;
    static constexpr std::size_t max_scan_page_bytes = protocol::descriptorPageBytes;
    const std::uint64_t _scan_epoch = make_scan_epoch ();
    std::uint64_t _next_version = 0;
    std::uint64_t _next_snapshot = 0;
};

class in_memory_relocation_store_t final : public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (blob_reference_t reference,
                                   std::span<const std::byte> payload,
                                   std::chrono::milliseconds retention) override
    {
        validate_reference (reference);
        if (payload.size () > 64u * 1024u * 1024u)
            throw std::invalid_argument ("relocation blob exceeds 64 MiB");
        if (retention <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("relocation retention must be positive");
        return _lane
          .run ([&] {
              const auto now = std::chrono::system_clock::now ();
              expire (reference.value, now);
              const auto found = _values.find (reference.value);
              if (found != _values.end ()) {
                  if (std::equal (payload.begin (), payload.end (), found->second.bytes.begin (),
                                  found->second.bytes.end ()))
                      return completed<blob_put_result_t> (
                        blob_already_stored_t{found->second.expires_at, now});
                  return completed<blob_put_result_t> (blob_conflict_t{now});
              }
              const auto expires_at = now + retention;
              _values.emplace (std::move (reference.value),
                               entry_t{{payload.begin (), payload.end ()}, expires_at});
              return completed<blob_put_result_t> (blob_stored_t{expires_at, now});
          })
          .get ();
    }

    task_t<blob_read_result_t> read (blob_reference_t reference) override
    {
        validate_reference (reference);
        return _lane
          .run ([&] {
              const auto now = std::chrono::system_clock::now ();
              expire (reference.value, now);
              const auto found = _values.find (reference.value);
              if (found == _values.end ())
                  return completed<blob_read_result_t> (blob_missing_t{now});
              return completed<blob_read_result_t> (
                blob_found_t{found->second.bytes, found->second.expires_at, now});
          })
          .get ();
    }

    task_t<blob_renew_result_t> renew (blob_reference_t reference,
                                       std::chrono::milliseconds retention) override
    {
        validate_reference (reference);
        if (retention <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument ("relocation retention must be positive");
        return _lane
          .run ([&] {
              const auto now = std::chrono::system_clock::now ();
              expire (reference.value, now);
              const auto found = _values.find (reference.value);
              if (found == _values.end ())
                  return completed<blob_renew_result_t> (blob_missing_t{now});
              found->second.expires_at = now + retention;
              return completed<blob_renew_result_t> (blob_renewed_t{found->second.expires_at, now});
          })
          .get ();
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        validate_reference (reference);
        return _lane
          .run ([&] {
              _values.erase (reference.value);
              return task_t<void> (result_t<void>::success ());
          })
          .get ();
    }

  private:
    struct entry_t
    {
        std::vector<std::byte> bytes;
        std::chrono::system_clock::time_point expires_at;
    };

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (result_t<T>::success (std::move (value)));
    }

    static void validate_reference (const blob_reference_t &reference)
    {
        if (reference.value.empty ()
            || reference.value.size () > protocol::relocationReferenceBytes)
            throw std::invalid_argument ("relocation reference must contain 1..4096 bytes");
    }

    void expire (const std::string &reference, std::chrono::system_clock::time_point now)
    {
        const auto found = _values.find (reference);
        if (found != _values.end () && found->second.expires_at <= now)
            _values.erase (found);
    }

    state_lane_t _lane;
    std::map<std::string, entry_t> _values;
};

} // namespace zlink::framework::runtime
