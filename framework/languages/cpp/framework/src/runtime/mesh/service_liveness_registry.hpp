/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
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

    void admit (std::vector<std::uint8_t> node_routing_id,
                std::vector<std::uint8_t> connection_id,
                clock_t::time_point now);
    bool disconnect (const std::vector<std::uint8_t> &node_routing_id,
                     const std::vector<std::uint8_t> &connection_id);
    bool acknowledge (const std::vector<std::uint8_t> &node_routing_id,
                      const std::vector<std::uint8_t> &connection_id,
                      std::uint64_t probe_id,
                      clock_t::time_point now);
    std::optional<service_probe_t>
    acknowledge_probe (const std::vector<std::uint8_t> &node_routing_id,
                       const std::vector<std::uint8_t> &connection_id,
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

    struct peer_t
    {
        std::vector<std::uint8_t> connection_id;
        clock_t::time_point deadline;
        clock_t::time_point next_probe;
        std::optional<std::uint64_t> outstanding_probe;
    };

    const std::chrono::milliseconds _probe_interval;
    const std::chrono::milliseconds _peer_timeout;
    mutable std::mutex _mutex;
    std::map<std::vector<std::uint8_t>, peer_t, byte_vector_less_t> _peers;
    std::uint64_t _next_probe_id = 1;
};

} // namespace zlink::framework::runtime::mesh
