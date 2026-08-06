/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>
#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zlink::framework
{

enum class fanout_publisher_connection_state_t
{
    connecting,
    ready,
    disconnected,
    reconnecting,
    excluded_draining,
    excluded_stale
};

struct fanout_publisher_connection_snapshot_t
{
    zlink::routing_id_t publisher_rid;
    std::uint64_t lifecycle_generation = 0;
    bool connection_intent = false;
    bool ready = false;
    fanout_publisher_connection_state_t state =
      fanout_publisher_connection_state_t::disconnected;
    std::optional<std::string> last_failure;

    friend bool operator== (const fanout_publisher_connection_snapshot_t &,
                            const fanout_publisher_connection_snapshot_t &) = default;
};

struct fanout_channel_snapshot_t
{
    std::string channel_name;
    std::size_t connection_intent_count = 0;
    std::size_t ready_connection_count = 0;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at{};
    std::vector<fanout_publisher_connection_snapshot_t> publishers;
    location_runtime_snapshot_t location;

    friend bool operator== (const fanout_channel_snapshot_t &,
                            const fanout_channel_snapshot_t &) = default;
};

struct fanout_publisher_changed_event_t
{
    static constexpr std::string_view event_identifier =
      "zlink.runtime.fanout.publisher_changed";

    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point timestamp{};
    std::string channel_name;
    fanout_publisher_connection_snapshot_t entry;

    constexpr std::string_view identifier () const noexcept
    {
        return event_identifier;
    }
};

struct fanout_location_changed_event_t
{
    static constexpr std::string_view event_identifier =
      "zlink.runtime.location.store_changed";

    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point timestamp{};
    std::string channel_name;
    location_runtime_snapshot_t location;

    constexpr std::string_view identifier () const noexcept
    {
        return event_identifier;
    }
};

using fanout_runtime_event_t = std::variant<
  fanout_publisher_changed_event_t,
  fanout_location_changed_event_t>;

class fanout_runtime_observation_t
{
  public:
    virtual ~fanout_runtime_observation_t () = default;
    virtual void close () = 0;
};

class fanout_runtime_t
{
  public:
    virtual ~fanout_runtime_t () = default;

    virtual fanout_channel_snapshot_t snapshot (
      std::string channel_name) const = 0;
    virtual std::unique_ptr<fanout_runtime_observation_t> observe (
      std::string channel_name,
      std::size_t capacity,
      std::function<void (
        const observed_status_t<fanout_runtime_event_t> &)> observer) = 0;
};

} // namespace zlink::framework
