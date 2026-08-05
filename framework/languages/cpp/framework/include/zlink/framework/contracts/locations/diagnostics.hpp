/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/locations/keys.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace zlink::framework
{

struct location_runtime_status_t
{
    bool store_healthy = false;
    bool watch_enabled = false;
    std::chrono::milliseconds polling_interval{0};
    std::optional<std::chrono::system_clock::time_point> last_refresh_at;
    std::optional<std::string> last_error;
    bool owner_lease_healthy = false;
    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at;
};

/* A compact location projection embedded in channel runtime snapshots.  The
 * channel contracts expose only the health and refresh facts needed to judge
 * whether a location-backed target projection is trustworthy.  Detailed
 * provider diagnostics remain on location_runtime_status_t. */
struct location_runtime_snapshot_t
{
    bool store_healthy = false;
    std::optional<std::chrono::system_clock::time_point> last_refresh_at;
    bool owner_lease_healthy = false;
    std::optional<std::chrono::system_clock::time_point> owner_lease_renewed_at;

    friend bool operator== (const location_runtime_snapshot_t &,
                            const location_runtime_snapshot_t &) = default;
};

enum class location_topology_state_t
{
    discovered = 1,
    connecting = 2,
    ready = 3,
    lost = 4,
    error = 5,
    stopped = 6
};

struct location_topology_filter_t
{
    std::optional<std::string> mesh_name;
    std::optional<zlink::routing_id_t> node_rid;
    std::optional<location_topology_state_t> state;
};

struct location_topology_entry_t
{
    std::string mesh_name;
    zlink::routing_id_t node_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::string endpoint;
    bool draining = false;
    location_topology_state_t state = location_topology_state_t::discovered;
    std::chrono::system_clock::time_point updated_at{};
};

struct location_service_summary_filter_t
{
    std::optional<std::string> mesh_name;
};

struct location_service_summary_t
{
    std::string mesh_name;
    std::uint32_t total_count = 0;
    std::uint32_t ready_count = 0;
    std::uint32_t error_count = 0;
    std::uint32_t stopped_count = 0;
    std::chrono::system_clock::time_point last_updated_at{};
};

} // namespace zlink::framework
