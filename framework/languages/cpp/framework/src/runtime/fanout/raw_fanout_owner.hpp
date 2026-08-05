/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink
{
class context_t;
class pub_socket_t;
class sub_socket_t;
class poller_t;
}

namespace zlink::framework::runtime::fanout
{

inline constexpr std::chrono::seconds fanout_beacon_interval{5};
inline constexpr std::chrono::seconds fanout_receive_deadline{15};

enum class fanout_receive_status_t
{
    no_data,
    beacon,
    application,
    protocol_error
};

struct fanout_received_t
{
    std::vector<std::uint8_t> publisher_routing_id;
    std::string topic;
    protocol::application_payload_t payload;
};

struct fanout_publisher_intent_t
{
    std::vector<std::uint8_t> publisher_routing_id;
    std::uint64_t lifecycle_generation = 0;
    std::string endpoint;
    mesh::service_node_state_t state =
      mesh::service_node_state_t::preparing;
};

class raw_fanout_publisher_t
{
  public:
    explicit raw_fanout_publisher_t (std::string endpoint);
    ~raw_fanout_publisher_t () noexcept;

    void start ();
    void close () noexcept;
    std::string endpoint () const;
    std::chrono::steady_clock::time_point next_activity () const;
    bool publish (const std::string &topic,
                  const protocol::application_payload_t &payload);
    bool tick (std::chrono::steady_clock::time_point now);

    static const std::string &reserved_topic ();
    static const std::vector<std::uint8_t> &beacon_payload ();

  private:
    std::string _configured_endpoint;
    mutable std::mutex _mutex;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::pub_socket_t> _socket;
    std::string _endpoint;
    std::chrono::steady_clock::time_point _next_beacon{};
    bool _closed = false;
};

class raw_fanout_subscriber_t
{
  public:
    explicit raw_fanout_subscriber_t (zlink::poller_t *poller = nullptr);
    ~raw_fanout_subscriber_t () noexcept;

    bool connect_manual (std::vector<std::uint8_t> publisher_routing_id,
                         std::string endpoint);
    void reconcile_automatic (
      const std::vector<fanout_publisher_intent_t> &publishers);
    bool disconnect (const std::vector<std::uint8_t> &publisher_routing_id);
    void close () noexcept;

    std::pair<fanout_receive_status_t, std::optional<fanout_received_t>>
    try_receive (std::chrono::steady_clock::time_point now);
    std::vector<std::vector<std::uint8_t>>
    tick (std::chrono::steady_clock::time_point now);
    bool ready (
      const std::vector<std::uint8_t> &publisher_routing_id) const;
    std::size_t publisher_count () const;

  private:
    struct publisher_intent_key_t
    {
        std::vector<std::uint8_t> routing_id;
        std::uint64_t lifecycle_generation = 0;

        friend bool operator< (const publisher_intent_key_t &left,
                               const publisher_intent_key_t &right) noexcept
        {
            if (left.routing_id < right.routing_id) {
                return true;
            }
            if (right.routing_id < left.routing_id) {
                return false;
            }
            return left.lifecycle_generation < right.lifecycle_generation;
        }
    };

    struct connection_t
    {
        std::string endpoint;
        std::unique_ptr<zlink::sub_socket_t> socket;
        std::uintptr_t poller_slot = 0;
        bool automatic = false;
        bool ready = false;
        std::chrono::steady_clock::time_point deadline{};
    };

    bool connect_locked (std::vector<std::uint8_t> publisher_routing_id,
                         std::uint64_t lifecycle_generation,
                         std::string endpoint,
                         bool automatic);
    void close_connection_locked (connection_t &connection) noexcept;
    void reopen_locked (connection_t &connection);

    mutable std::mutex _mutex;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller = nullptr;
    std::map<publisher_intent_key_t, connection_t> _connections;
    std::optional<bool> _automatic_mode;
    std::size_t _receive_cursor = 0;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::fanout
