/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

enum class exactly_once_claim_state
{
    claimed,
    pending,
    completed
};

template <typename TValue>
struct exactly_once_claim_t
{
    exactly_once_claim_state state = exactly_once_claim_state::pending;
    std::optional<TValue> value;
};

// A reservation and its terminal value share one lock and one key space. This
// keeps duplicate delivery from being accepted between the reservation and
// completion steps; callers may remove a pending entry only when the work
// itself failed and can be retried.
template <typename TKey,
          typename TValue,
          typename THash = std::hash<TKey>>
class exactly_once_table_t
{
  public:
    using key_type = TKey;
    using value_type = TValue;
    using claim_type = exactly_once_claim_t<value_type>;

    explicit exactly_once_table_t (std::size_t capacity = 0) : _capacity (capacity) {}

    bool reserve (const key_type &key)
    {
        std::lock_guard lock (_mutex);
        if (_entries.contains (key)
            || (_capacity != 0 && _entries.size () >= _capacity)) {
            return false;
        }
        _entries.emplace (key, std::nullopt);
        return true;
    }

    claim_type claim (const key_type &key)
    {
        std::lock_guard lock (_mutex);
        const auto found = _entries.find (key);
        if (found == _entries.end ()) {
            if (_capacity != 0 && _entries.size () >= _capacity) {
                return claim_type{};
            }
            _entries.emplace (key, std::nullopt);
            return claim_type{exactly_once_claim_state::claimed, std::nullopt};
        }
        if (!found->second) {
            return claim_type{exactly_once_claim_state::pending, std::nullopt};
        }
        return claim_type{exactly_once_claim_state::completed, found->second};
    }

    bool complete (const key_type &key, value_type value)
    {
        std::lock_guard lock (_mutex);
        auto found = _entries.find (key);
        if (found == _entries.end ()) {
            if (_capacity != 0 && _entries.size () >= _capacity)
                return false;
            found = _entries.emplace (key, std::nullopt).first;
        }
        if (found->second) {
            return false;
        }
        found->second.emplace (std::move (value));
        return true;
    }

    bool take (const key_type &key, value_type &value)
    {
        std::lock_guard lock (_mutex);
        const auto found = _entries.find (key);
        if (found == _entries.end () || !found->second) {
            return false;
        }
        value = std::move (*found->second);
        _entries.erase (found);
        return true;
    }

    bool contains (const key_type &key) const
    {
        std::lock_guard lock (_mutex);
        return _entries.contains (key);
    }

    bool erase (const key_type &key)
    {
        std::lock_guard lock (_mutex);
        return _entries.erase (key) != 0;
    }

    template <typename TPredicate>
    std::size_t erase_if (TPredicate predicate)
    {
        std::lock_guard lock (_mutex);
        std::size_t erased = 0;
        for (auto entry = _entries.begin (); entry != _entries.end ();) {
            if (!predicate (entry->first)) {
                ++entry;
                continue;
            }
            entry = _entries.erase (entry);
            ++erased;
        }
        return erased;
    }

    std::vector<std::pair<key_type, value_type>> take_completed ()
    {
        std::vector<std::pair<key_type, value_type>> completed;
        std::lock_guard lock (_mutex);
        completed.reserve (_entries.size ());
        for (auto entry = _entries.begin (); entry != _entries.end ();) {
            if (!entry->second) {
                ++entry;
                continue;
            }
            completed.emplace_back (entry->first, std::move (*entry->second));
            entry = _entries.erase (entry);
        }
        return completed;
    }

    void clear () noexcept
    {
        std::lock_guard lock (_mutex);
        _entries.clear ();
    }

  private:
    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::unordered_map<key_type, std::optional<value_type>, THash> _entries;
};

} // namespace zlink::framework::runtime
