/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::runtime::locations
{

enum class service_descriptor_kind_t
{
    route_mesh,
    client_server,
    fanout
};

struct service_descriptor_key_t
{
    service_descriptor_kind_t kind;
    std::string scope;
    std::vector<std::uint8_t> routing_id;

    friend bool operator< (const service_descriptor_key_t &left,
                           const service_descriptor_key_t &right) noexcept;
    friend bool operator== (const service_descriptor_key_t &,
                            const service_descriptor_key_t &) = default;
};

struct service_descriptor_record_t
{
    service_descriptor_key_t key;
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::string endpoint;
    std::string security_identity;
    std::uint32_t effective_max_message_bytes = 0;
    mesh::service_node_state_t state =
      mesh::service_node_state_t::preparing;
    int weight = 100;
    std::string owner_id;
    std::int64_t owner_lease_generation = 0;

    friend bool operator== (const service_descriptor_record_t &,
                            const service_descriptor_record_t &) = default;
};

enum class service_descriptor_publish_status_t
{
    inserted,
    updated,
    conflict,
    invalid
};

enum class service_descriptor_change_t
{
    upserted,
    removed
};

struct service_descriptor_event_t
{
    service_descriptor_change_t change;
    service_descriptor_record_t record;
    std::uint64_t change_stamp;
};

struct service_descriptor_watch_filter_t
{
    std::optional<service_descriptor_kind_t> kind;
    std::optional<std::string> scope;
};

struct service_descriptor_snapshot_t
{
    std::uint64_t change_stamp;
    std::vector<service_descriptor_record_t> records;
};

class service_descriptor_registry_t
{
  public:
    using watch_callback_t = std::function<void (service_descriptor_event_t)>;

    service_descriptor_publish_status_t publish (
      service_descriptor_record_t record,
      std::optional<std::uint64_t> expected_revision);
    bool remove (const service_descriptor_key_t &key,
                 std::uint64_t expected_revision,
                 const std::string &expected_owner_id,
                 std::int64_t expected_owner_lease_generation);
    service_descriptor_snapshot_t snapshot (
      service_descriptor_watch_filter_t filter = {}) const;

    std::uint64_t watch (service_descriptor_watch_filter_t filter,
                         watch_callback_t callback);
    bool unwatch (std::uint64_t watch_id);

  private:
    struct watcher_t
    {
        service_descriptor_watch_filter_t filter;
        watch_callback_t callback;
    };

    static bool valid (const service_descriptor_record_t &record);
    static bool immutable_identity_matches (
      const service_descriptor_record_t &current,
      const service_descriptor_record_t &candidate);
    static bool matches (const service_descriptor_watch_filter_t &filter,
                         const service_descriptor_record_t &record);
    static void notify (std::vector<watch_callback_t> callbacks,
                        service_descriptor_event_t event) noexcept;

    mutable std::mutex _mutex;
    std::map<service_descriptor_key_t, service_descriptor_record_t> _records;
    std::map<std::uint64_t, watcher_t> _watchers;
    std::uint64_t _change_stamp = 0;
    std::uint64_t _next_watch_id = 1;
};

} // namespace zlink::framework::runtime::locations
