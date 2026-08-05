/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/locations/stores.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

class in_memory_location_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        validate_key (key.value);
        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
        expire (key.value, now);
        const auto found = _values.find (key.value);
        if (found == _values.end ())
            return completed<store_read_result_t> (
              store_missing_t{now});
        found->second.store_now = now;
        return completed<store_read_result_t> (
          store_found_t{found->second});
    }

    task_t<store_write_result_t> write (
      store_write_request_t request) override
    {
        validate_write (request);
        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
        for (const auto &condition : request.conditions)
            std::visit (
              [&] (const auto &value) {
                  expire (value.key.value, now);
              },
              condition);

        for (const auto &condition : request.conditions) {
            const auto key = std::visit (
              [] (const auto &value) {
                  return value.key.value;
              },
              condition);
            const auto found = _values.find (key);
            if (std::holds_alternative<
                  store_missing_condition_t> (condition)) {
                if (found != _values.end ())
                    return completed<store_write_result_t> (
                      store_write_conflict_t{now});
                continue;
            }
            const auto &expected =
              std::get<store_version_condition_t> (
                condition)
                .expected;
            if (found == _values.end ()
                || found->second.version
                     .value
                     != expected.value)
                return completed<store_write_result_t> (
                  store_write_conflict_t{now});
        }

        store_write_applied_t applied;
        applied.store_now = now;
        for (auto &mutation : request.mutations) {
            if (auto *put = std::get_if<store_put_t> (&mutation)) {
                const store_version_t version{
                  next_version ()};
                auto expires_at =
                  put->retention
                    ? std::optional{
                        now + *put->retention}
                    : std::nullopt;
                _values[put->key.value] =
                  store_value_t{std::move (put->bytes),
                                version,
                                expires_at,
                                now};
                applied.put_versions.push_back (
                  {std::move (put->key), version});
            }
            else {
                _values.erase (
                  std::get<store_delete_t> (mutation).key.value);
            }
        }
        return completed<store_write_result_t> (
          std::move (applied));
    }

    task_t<store_scan_result_t> scan (
      store_scan_request_t request) override
    {
        validate_prefix (request.prefix);
        if (request.limit == 0 || request.limit > 1000)
            throw std::invalid_argument (
              "location scan limit must be 1..1000");

        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
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
                return completed<store_scan_result_t> (
                  store_scan_expired_t{});
            try {
                const auto epoch = std::stoull (
                  request.cursor->value.substr (
                    0, first_separator));
                if (epoch != _scan_epoch)
                    return completed<store_scan_result_t> (
                      store_scan_expired_t{});
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
                return completed<store_scan_result_t> (
                  store_scan_expired_t{});
            }
        }
        else {
            if (_snapshots.size () >= max_scan_snapshots)
                _snapshots.erase (_snapshots.begin ());
            snapshot_id = ++_next_snapshot;
            auto &snapshot = _snapshots[snapshot_id];
            for (auto iterator = _values.begin ();
                 iterator != _values.end ();) {
                if (expired (iterator->second, now)) {
                    iterator = _values.erase (iterator);
                    continue;
                }
                if (iterator->first.starts_with (request.prefix))
                    snapshot.push_back (
                      {{iterator->first}, iterator->second});
                ++iterator;
            }
        }

        const auto snapshot = _snapshots.find (snapshot_id);
        if (snapshot == _snapshots.end ()
            || offset > snapshot->second.size ())
            return completed<store_scan_result_t> (
              store_scan_expired_t{});

        store_scan_page_t page;
        page.store_now = now;
        auto end = offset;
        std::size_t encoded_size = 0;
        while (end < snapshot->second.size ()
               && end - offset < request.limit) {
            const auto item_size =
              encoded_scan_item_size (snapshot->second[end]);
            if (end != offset
                && encoded_size + item_size
                     > max_scan_page_bytes)
                break;
            encoded_size += item_size;
            ++end;
        }
        page.items.insert (
          page.items.end (),
          snapshot->second.begin ()
            + static_cast<std::ptrdiff_t> (offset),
          snapshot->second.begin ()
            + static_cast<std::ptrdiff_t> (end));
        if (end < snapshot->second.size ())
            page.next_cursor = store_scan_cursor_t{
              std::to_string (_scan_epoch) + ":"
              + std::to_string (snapshot_id) + ":"
              + std::to_string (end)};
        else
            _snapshots.erase (snapshot);
        return completed<store_scan_result_t> (std::move (page));
    }

  private:
    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (
          result_t<T>::success (std::move (value)));
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
              "location scan prefix must contain at most 1024 bytes");
    }

    static void validate_write (
      const store_write_request_t &request)
    {
        std::set<std::string> conditions;
        std::set<std::string> mutations;
        std::set<std::string> keys;
        std::size_t encoded_size = 0;
        for (const auto &condition : request.conditions) {
            const auto &key = std::visit (
              [] (const auto &value) -> const store_key_t & {
                  return value.key;
              },
              condition);
            validate_key (key.value);
            if (!conditions.insert (key.value).second)
                throw std::invalid_argument (
                  "location write repeats a condition key");
            keys.insert (key.value);
            encoded_size += key.value.size ();
            if (const auto *version =
                  std::get_if<store_version_condition_t> (
                    &condition)) {
                if (version->expected.value.empty ()
                    || version->expected.value.size () > 4096)
                    throw std::invalid_argument (
                      "location version condition requires 1..4096 bytes");
                encoded_size +=
                  version->expected.value.size ();
            }
        }
        for (const auto &mutation : request.mutations) {
            const auto *key =
              std::visit (
                [] (const auto &value) {
                    return &value.key;
                },
                mutation);
            validate_key (key->value);
            if (!mutations.insert (key->value).second)
                throw std::invalid_argument (
                  "location write repeats a mutation key");
            keys.insert (key->value);
            encoded_size += key->value.size ();
            if (const auto *put =
                  std::get_if<store_put_t> (&mutation)) {
                if (put->bytes.size () > 1024u * 1024u)
                    throw std::invalid_argument (
                      "location value exceeds 1 MiB");
                if (put->retention
                    && *put->retention
                         <= std::chrono::milliseconds::zero ())
                    throw std::invalid_argument (
                      "location retention must be positive");
                encoded_size += put->bytes.size ();
            }
        }
        if (keys.size () > 2048)
            throw std::invalid_argument (
              "location write exceeds 2048 unique keys");
        if (encoded_size > 4u * 1024u * 1024u)
            throw std::invalid_argument (
              "location write exceeds 4 MiB");
    }

    static bool expired (
      const store_value_t &value,
      std::chrono::system_clock::time_point now)
    {
        return value.expires_at && *value.expires_at <= now;
    }

    void expire (
      const std::string &key,
      std::chrono::system_clock::time_point now)
    {
        const auto found = _values.find (key);
        if (found != _values.end ()
            && expired (found->second, now))
            _values.erase (found);
    }

    std::string next_version ()
    {
        return std::to_string (++_next_version);
    }

    static std::size_t encoded_scan_item_size (
      const store_scan_item_t &item) noexcept
    {
        return item.key.value.size ()
               + item.value.version.value.size ()
               + item.value.bytes.size ();
    }

    static std::uint64_t make_scan_epoch ()
    {
        std::random_device source;
        return (
                 static_cast<std::uint64_t> (source ())
                 << 32)
               ^ static_cast<std::uint64_t> (source ());
    }

    std::mutex _gate;
    std::map<std::string, store_value_t> _values;
    std::map<std::uint64_t, std::vector<store_scan_item_t>>
      _snapshots;
    static constexpr std::size_t max_scan_snapshots = 128;
    static constexpr std::size_t max_scan_page_bytes =
      4u * 1024u * 1024u;
    const std::uint64_t _scan_epoch = make_scan_epoch ();
    std::uint64_t _next_version = 0;
    std::uint64_t _next_snapshot = 0;
};

class in_memory_relocation_store_t final :
    public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (
      blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) override
    {
        validate_reference (reference);
        if (payload.size () > 64u * 1024u * 1024u)
            throw std::invalid_argument (
              "relocation blob exceeds 64 MiB");
        if (retention <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument (
              "relocation retention must be positive");
        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
        expire (reference.value, now);
        const auto found = _values.find (reference.value);
        if (found != _values.end ()) {
            if (std::equal (
                  payload.begin (), payload.end (),
                  found->second.bytes.begin (),
                  found->second.bytes.end ()))
                return completed<blob_put_result_t> (
                  blob_already_stored_t{
                    found->second.expires_at, now});
            return completed<blob_put_result_t> (
              blob_conflict_t{now});
        }
        const auto expires_at = now + retention;
        _values.emplace (
          std::move (reference.value),
          entry_t{{payload.begin (), payload.end ()},
                  expires_at});
        return completed<blob_put_result_t> (
          blob_stored_t{expires_at, now});
    }

    task_t<blob_read_result_t> read (
      blob_reference_t reference) override
    {
        validate_reference (reference);
        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
        expire (reference.value, now);
        const auto found = _values.find (reference.value);
        if (found == _values.end ())
            return completed<blob_read_result_t> (
              blob_missing_t{now});
        return completed<blob_read_result_t> (
          blob_found_t{found->second.bytes,
                       found->second.expires_at,
                       now});
    }

    task_t<blob_renew_result_t> renew (
      blob_reference_t reference,
      std::chrono::milliseconds retention) override
    {
        validate_reference (reference);
        if (retention <= std::chrono::milliseconds::zero ())
            throw std::invalid_argument (
              "relocation retention must be positive");
        std::lock_guard lock (_gate);
        const auto now = std::chrono::system_clock::now ();
        expire (reference.value, now);
        const auto found = _values.find (reference.value);
        if (found == _values.end ())
            return completed<blob_renew_result_t> (
              blob_missing_t{now});
        found->second.expires_at = now + retention;
        return completed<blob_renew_result_t> (
          blob_renewed_t{found->second.expires_at, now});
    }

    task_t<void> erase (blob_reference_t reference) override
    {
        validate_reference (reference);
        std::lock_guard lock (_gate);
        _values.erase (reference.value);
        return task_t<void> (result_t<void>::success ());
    }

  private:
    struct entry_t
    {
        std::vector<std::byte> bytes;
        std::chrono::system_clock::time_point expires_at;
    };

    template <typename T> static task_t<T> completed (T value)
    {
        return task_t<T> (
          result_t<T>::success (std::move (value)));
    }

    static void validate_reference (
      const blob_reference_t &reference)
    {
        if (reference.value.empty ()
            || reference.value.size () > 4096)
            throw std::invalid_argument (
              "relocation reference must contain 1..4096 bytes");
    }

    void expire (
      const std::string &reference,
      std::chrono::system_clock::time_point now)
    {
        const auto found = _values.find (reference);
        if (found != _values.end ()
            && found->second.expires_at <= now)
            _values.erase (found);
    }

    std::mutex _gate;
    std::map<std::string, entry_t> _values;
};

} // namespace zlink::framework::runtime
