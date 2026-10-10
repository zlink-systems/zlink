/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/execution/state_lane.hpp"

#include <chrono>
#include <atomic>
#include <memory>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zlink::framework::runtime::mesh
{

struct service_probe_t
{
    std::vector<std::uint8_t> node_routing_id;
    std::vector<std::uint8_t> connection_id;
    std::uint64_t probe_id;
};

struct service_liveness_tick_t
{
    std::vector<service_probe_t> probes;
    std::vector<std::vector<std::uint8_t>> timed_out_nodes;
};

class service_liveness_registry_t
{
  public:
    using clock_t = std::chrono::steady_clock;

    explicit service_liveness_registry_t (
      std::chrono::milliseconds probe_interval = std::chrono::seconds (5),
      std::chrono::milliseconds peer_timeout = std::chrono::seconds (15));

    struct peer_t
    {
        peer_t (std::vector<std::uint8_t> identity,
                clock_t::time_point now,
                std::chrono::milliseconds interval,
                std::chrono::milliseconds timeout) :
            connection_id (std::move (identity)),
            deadline (now + timeout),
            next_probe (now + interval),
            _timeout (timeout)
        {
        }

        void record_received (clock_t::time_point now) noexcept
        {
            const auto candidate = now + _timeout;
            auto previous = deadline.load (std::memory_order_relaxed);
            while (
              previous < candidate
              && !deadline.compare_exchange_weak (previous, candidate, std::memory_order_relaxed)) {
            }
        }

        std::vector<std::uint8_t> connection_id;
        std::atomic<clock_t::time_point> deadline;
        clock_t::time_point next_probe;
        std::optional<std::uint64_t> outstanding_probe;

      private:
        const std::chrono::milliseconds _timeout;
    };
    using connection_t = std::shared_ptr<peer_t>;

    connection_t admit (std::vector<std::uint8_t> node_routing_id,
                        std::vector<std::uint8_t> connection_id,
                        clock_t::time_point now);
    connection_t connection (const std::vector<std::uint8_t> &node_routing_id) const;
    bool disconnect (const std::vector<std::uint8_t> &node_routing_id,
                     const std::vector<std::uint8_t> &connection_id);
    bool acknowledge (const std::vector<std::uint8_t> &node_routing_id,
                      std::span<const std::uint8_t> connection_id,
                      std::uint64_t probe_id,
                      clock_t::time_point now);
    std::optional<service_probe_t>
    acknowledge_probe (const std::vector<std::uint8_t> &node_routing_id,
                       std::span<const std::uint8_t> connection_id,
                       std::uint64_t probe_id) const;
    service_liveness_tick_t tick (clock_t::time_point now);
    std::optional<clock_t::time_point> next_activity () const;
    std::size_t size () const;

  private:
    struct byte_vector_less_t
    {
        bool operator() (const std::vector<std::uint8_t> &left,
                         const std::vector<std::uint8_t> &right) const noexcept;
    };

    const std::chrono::milliseconds _probe_interval;
    const std::chrono::milliseconds _peer_timeout;
    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    std::map<std::vector<std::uint8_t>, connection_t, byte_vector_less_t> _peers;
    std::uint64_t _next_probe_id = 1;
};

} // namespace zlink::framework::runtime::mesh
