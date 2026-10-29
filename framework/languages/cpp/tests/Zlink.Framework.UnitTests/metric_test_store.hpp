/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/framework/contracts/locations/stores.hpp>
#include <map>
#include <mutex>

namespace metric_test
{
// Public Location Store fixture shared by the contract and installed consumers.
class store_t final : public zlink::framework::location_store_t
{
    using clock_t = std::chrono::system_clock;
    template <typename T> static auto done (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }
    void expire (clock_t::time_point now)
    {
        std::erase_if (_values, [now] (const auto &entry) {
            return entry.second.expires_at && *entry.second.expires_at <= now;
        });
    }

  public:
    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        using namespace zlink::framework;
        std::lock_guard lock (_mutex);
        const auto now = clock_t::now ();
        expire (now);
        auto found = _values.find (key.value);
        if (found == _values.end ())
            return done (store_read_result_t{store_missing_t{now}});
        auto value = found->second;
        value.store_now = now;
        return done (store_read_result_t{store_found_t{std::move (value)}});
    }
    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        using namespace zlink::framework;
        std::lock_guard lock (_mutex);
        const auto now = clock_t::now ();
        expire (now);
        for (const auto &condition : request.conditions) {
            const bool matched = std::visit (
              [&] (const auto &value) {
                  const auto found = _values.find (value.key.value);
                  using T = std::decay_t<decltype (value)>;
                  if constexpr (std::is_same_v<T, store_missing_condition_t>)
                      return found == _values.end ();
                  else if constexpr (std::is_same_v<T, store_version_condition_t>)
                      return found != _values.end ()
                             && found->second.version.value == value.expected.value;
                  else
                      return found != _values.end () && found->second.bytes == value.expected;
              },
              condition);
            if (!matched)
                return done (store_write_result_t{store_write_conflict_t{now}});
        }
        store_write_applied_t applied{{}, now};
        for (auto &mutation : request.mutations) {
            if (auto *put = std::get_if<store_put_t> (&mutation)) {
                store_version_t version{std::to_string (++_version)};
                _values[put->key.value] = {
                  std::move (put->bytes), version,
                  put->retention ? std::optional{now + *put->retention} : std::nullopt, now};
                applied.put_versions.push_back ({put->key, version});
            } else
                _values.erase (std::get<store_delete_t> (mutation).key.value);
        }
        return done (store_write_result_t{std::move (applied)});
    }
    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        using namespace zlink::framework;
        std::lock_guard lock (_mutex);
        const auto now = clock_t::now ();
        expire (now);
        std::uint64_t id = 0;
        std::size_t offset = 0;
        if (request.cursor) {
            const auto separator = request.cursor->value.find (':');
            id = std::stoull (request.cursor->value.substr (0, separator));
            offset = std::stoull (request.cursor->value.substr (separator + 1));
        } else {
            id = ++_snapshot;
            for (const auto &[key, value] : _values)
                if (key.starts_with (request.prefix))
                    _scans[id].push_back ({{key}, value});
            _scans.try_emplace (id);
        }
        const auto found = _scans.find (id);
        if (found == _scans.end () || offset > found->second.size ())
            return done (store_scan_result_t{store_scan_expired_t{}});
        store_scan_page_t page{{}, {}, now};
        const auto end = std::min (offset + request.limit, found->second.size ());
        page.items.assign (found->second.begin () + offset, found->second.begin () + end);
        if (end < found->second.size ())
            page.next_cursor =
              store_scan_cursor_t{std::to_string (id) + ':' + std::to_string (end)};
        else
            _scans.erase (found);
        return done (store_scan_result_t{std::move (page)});
    }

  private:
    std::mutex _mutex;
    std::map<std::string, zlink::framework::store_value_t> _values;
    std::map<std::uint64_t, std::vector<zlink::framework::store_scan_item_t>> _scans;
    std::uint64_t _version = 0, _snapshot = 0;
};
} // namespace metric_test
