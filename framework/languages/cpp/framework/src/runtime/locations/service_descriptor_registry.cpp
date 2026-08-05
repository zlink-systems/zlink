/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/service_descriptor_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime::locations
{

bool operator< (const service_descriptor_key_t &left,
                const service_descriptor_key_t &right) noexcept
{
    return std::tie (left.kind, left.scope, left.routing_id)
           < std::tie (right.kind, right.scope, right.routing_id);
}

service_descriptor_publish_status_t service_descriptor_registry_t::publish (
  service_descriptor_record_t record,
  std::optional<std::uint64_t> expected_revision)
{
    if (!valid (record)) {
        return service_descriptor_publish_status_t::invalid;
    }

    std::vector<watch_callback_t> callbacks;
    service_descriptor_event_t event{};
    {
        std::lock_guard lock (_mutex);
        const auto found = _records.find (record.key);
        if (found == _records.end ()) {
            if (expected_revision) {
                return service_descriptor_publish_status_t::conflict;
            }
            if (_change_stamp == std::numeric_limits<std::uint64_t>::max ()) {
                return service_descriptor_publish_status_t::invalid;
            }
            ++_change_stamp;
            const auto [inserted, accepted] =
              _records.emplace (record.key, std::move (record));
            static_cast<void> (accepted);
            event = {service_descriptor_change_t::upserted, inserted->second,
                     _change_stamp};
        } else {
            if (!expected_revision
                || *expected_revision != found->second.descriptor_revision
                || record.descriptor_revision
                     <= found->second.descriptor_revision
                || !immutable_identity_matches (found->second, record)) {
                return service_descriptor_publish_status_t::conflict;
            }
            if (_change_stamp == std::numeric_limits<std::uint64_t>::max ()) {
                return service_descriptor_publish_status_t::invalid;
            }
            ++_change_stamp;
            found->second = std::move (record);
            event = {service_descriptor_change_t::upserted, found->second,
                     _change_stamp};
        }
        for (const auto &[id, watcher] : _watchers) {
            static_cast<void> (id);
            if (matches (watcher.filter, event.record)) {
                callbacks.push_back (watcher.callback);
            }
        }
    }
    notify (std::move (callbacks), std::move (event));
    return expected_revision ? service_descriptor_publish_status_t::updated
                             : service_descriptor_publish_status_t::inserted;
}

bool service_descriptor_registry_t::remove (
  const service_descriptor_key_t &key,
  std::uint64_t expected_revision,
  const std::string &expected_owner_id,
  std::int64_t expected_owner_lease_generation)
{
    std::vector<watch_callback_t> callbacks;
    service_descriptor_event_t event{};
    {
        std::lock_guard lock (_mutex);
        const auto found = _records.find (key);
        if (found == _records.end ()
            || found->second.descriptor_revision != expected_revision
            || found->second.owner_id != expected_owner_id
            || found->second.owner_lease_generation
                 != expected_owner_lease_generation
            || _change_stamp == std::numeric_limits<std::uint64_t>::max ()) {
            return false;
        }
        ++_change_stamp;
        event = {service_descriptor_change_t::removed, found->second,
                 _change_stamp};
        _records.erase (found);
        for (const auto &[id, watcher] : _watchers) {
            static_cast<void> (id);
            if (matches (watcher.filter, event.record)) {
                callbacks.push_back (watcher.callback);
            }
        }
    }
    notify (std::move (callbacks), std::move (event));
    return true;
}

service_descriptor_snapshot_t service_descriptor_registry_t::snapshot (
  service_descriptor_watch_filter_t filter) const
{
    std::lock_guard lock (_mutex);
    service_descriptor_snapshot_t result{_change_stamp, {}};
    for (const auto &[key, record] : _records) {
        static_cast<void> (key);
        if (matches (filter, record)) {
            result.records.push_back (record);
        }
    }
    return result;
}

std::uint64_t service_descriptor_registry_t::watch (
  service_descriptor_watch_filter_t filter,
  watch_callback_t callback)
{
    if (!callback) {
        throw std::invalid_argument ("descriptor watch callback is required");
    }
    std::lock_guard lock (_mutex);
    if (_next_watch_id == 0) {
        throw std::overflow_error ("descriptor watch id is exhausted");
    }
    const auto id = _next_watch_id++;
    _watchers.emplace (
      id, watcher_t{std::move (filter), std::move (callback)});
    return id;
}

bool service_descriptor_registry_t::unwatch (std::uint64_t watch_id)
{
    std::lock_guard lock (_mutex);
    return _watchers.erase (watch_id) != 0;
}

bool service_descriptor_registry_t::valid (
  const service_descriptor_record_t &record)
{
    return !record.key.scope.empty () && !record.key.routing_id.empty ()
           && record.lifecycle_generation != 0
           && record.descriptor_revision != 0 && !record.endpoint.empty ()
           && !record.security_identity.empty ()
           && record.effective_max_message_bytes != 0
           && record.weight >= 0 && record.weight <= 10000
           && !record.owner_id.empty ()
           && record.owner_lease_generation > 0;
}

bool service_descriptor_registry_t::immutable_identity_matches (
  const service_descriptor_record_t &current,
  const service_descriptor_record_t &candidate)
{
    return current.key == candidate.key
           && current.lifecycle_generation
                == candidate.lifecycle_generation
           && current.endpoint == candidate.endpoint
           && current.security_identity == candidate.security_identity
           && current.effective_max_message_bytes
                == candidate.effective_max_message_bytes
           && current.owner_id == candidate.owner_id
           && current.owner_lease_generation
                == candidate.owner_lease_generation;
}

bool service_descriptor_registry_t::matches (
  const service_descriptor_watch_filter_t &filter,
  const service_descriptor_record_t &record)
{
    return (!filter.kind || *filter.kind == record.key.kind)
           && (!filter.scope || *filter.scope == record.key.scope);
}

void service_descriptor_registry_t::notify (
  std::vector<watch_callback_t> callbacks,
  service_descriptor_event_t event) noexcept
{
    for (auto &callback : callbacks) {
        try {
            callback (event);
        }
        catch (...) {
        }
    }
}

} // namespace zlink::framework::runtime::locations
