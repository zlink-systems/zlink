/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/execution/state_lane.hpp"

#include <chrono>
#include <atomic>
#include <memory>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zlink::framework::runtime::client_server
{
class raw_client_server_server_t;
class raw_client_server_client_t;
}

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

    static constexpr std::chrono::milliseconds default_probe_interval{std::chrono::seconds (5)};
    static constexpr std::chrono::milliseconds default_peer_timeout{std::chrono::seconds (15)};
    // The caller owns this lane and keeps it alive until the registry is destroyed.
    explicit service_liveness_registry_t (
      runtime::state_lane_t &lane,
      std::chrono::milliseconds probe_interval = default_probe_interval,
      std::chrono::milliseconds peer_timeout = default_peer_timeout);

    service_liveness_registry_t (const service_liveness_registry_t &) = delete;
    service_liveness_registry_t &operator= (const service_liveness_registry_t &) = delete;

    struct peer_t
    {
        peer_t (std::vector<std::uint8_t> identity,
                clock_t::time_point now,
                std::chrono::milliseconds timeout) :
            connection_id (std::move (identity)),
            deadline (now + timeout),
            next_probe (now),
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
    // True when a probe, timeout or admission retry may be due at now. The registry keeps an
    // early bound of its next due instant (spec 08 section 4.2): a rotation
    // with nothing due neither enters the lane nor visits the peers.
    bool due (clock_t::time_point now) const noexcept;
    // Publish whether admission work needs a retry at the probe interval.
    // The registry lane serializes this write with deadline publication.
    void set_admission_pending (bool pending);
    service_liveness_tick_t tick (clock_t::time_point now);
    std::optional<clock_t::time_point> next_activity () const;
    std::size_t size () const;

  private:
    // Raw mesh, ClientServer and topology already enter the borrowed owner.
    // Their mutation bodies must not schedule a second turn on that owner.
    friend class client_server::raw_client_server_server_t;
    friend class client_server::raw_client_server_client_t;
    friend class raw_mesh_node_owner_t;
    friend class service_topology_registry_t;
    void set_admission_pending_owned (bool pending);
    connection_t admit_owned (std::vector<std::uint8_t> node_routing_id,
                              std::vector<std::uint8_t> connection_id,
                              clock_t::time_point now);
    bool disconnect_owned (const std::vector<std::uint8_t> &node_routing_id,
                           const std::vector<std::uint8_t> &connection_id);
    bool acknowledge_owned (const std::vector<std::uint8_t> &node_routing_id,
                            std::span<const std::uint8_t> connection_id,
                            std::uint64_t probe_id,
                            clock_t::time_point now);
    std::optional<service_probe_t>
    acknowledge_probe_owned (const std::vector<std::uint8_t> &node_routing_id,
                             std::span<const std::uint8_t> connection_id,
                             std::uint64_t probe_id) const;
    service_liveness_tick_t tick_owned (clock_t::time_point now);
    std::optional<clock_t::time_point> next_activity_owned () const;
    static std::optional<clock_t::time_point> activity_from_due (clock_t::rep next);

    struct byte_vector_less_t
    {
        bool operator() (const std::vector<std::uint8_t> &left,
                         const std::vector<std::uint8_t> &right) const noexcept;
    };

    const std::chrono::milliseconds _probe_interval;
    const std::chrono::milliseconds _peer_timeout;

    runtime::state_lane_t &_lane;
    std::map<std::vector<std::uint8_t>, connection_t, byte_vector_less_t> _peers;
    std::uint64_t _next_probe_id = 1;
    // Earliest instant a peer or admission retry can be due, written by its owner. A receive only
    // moves a deadline later, so the value stays an early bound. unknown_due
    // asks the next tick or next_activity to recompute it.
    static constexpr clock_t::rep unknown_due = std::numeric_limits<clock_t::rep>::min ();
    // The start of the current admission retry interval; absent after ADMIT.
    std::optional<clock_t::time_point> _admission_activity;
    static constexpr clock_t::rep never_due = std::numeric_limits<clock_t::rep>::max ();
    mutable std::atomic<clock_t::rep> _next_due{unknown_due};
    void publish_next_due_owned () const;
};

} // namespace zlink::framework::runtime::mesh
