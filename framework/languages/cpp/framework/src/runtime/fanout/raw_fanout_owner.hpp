/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>

#include <algorithm>
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
class topic_message_t;
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
    // The binding receive envelope carries the Core retained-credit owner.
    // It remains live through the caller's dispatch terminal path.
    std::shared_ptr<zlink::topic_message_t> retained;
};

struct fanout_publisher_intent_t
{
    std::vector<std::uint8_t> publisher_routing_id;
    std::uint64_t lifecycle_generation = 0;
    std::string endpoint;
    mesh::service_node_state_t state =
      mesh::service_node_state_t::preparing;
};

enum class raw_fanout_connection_state_t
{
    connecting,
    ready,
    disconnected,
    reconnecting
};

struct raw_fanout_connection_snapshot_t
{
    std::vector<std::uint8_t> publisher_routing_id;
    std::uint64_t lifecycle_generation = 0;
    bool connection_intent = false;
    bool ready = false;
    raw_fanout_connection_state_t state =
      raw_fanout_connection_state_t::disconnected;
    std::optional<std::string> last_failure;
};

class raw_fanout_publisher_t
{
  public:
    explicit raw_fanout_publisher_t (
      std::string endpoint,
      std::shared_ptr<zlink::context_t> context = {});
    ~raw_fanout_publisher_t () noexcept;

    void start ();
    void close () noexcept;
    std::string endpoint () const;
    std::chrono::steady_clock::time_point next_activity () const;
    task_t<void> publish (std::string topic,
                          protocol::application_payload_t payload,
                          std::chrono::milliseconds timeout =
                            std::chrono::milliseconds{-1});
    bool tick (std::chrono::steady_clock::time_point now);

    static const std::string &reserved_topic ();
    static const std::vector<std::uint8_t> &beacon_payload ();

  private:
    std::string _configured_endpoint;
    mutable std::mutex _mutex;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::pub_socket_t> _socket;
    std::string _endpoint;
    std::chrono::steady_clock::time_point _next_beacon{};
    bool _closed = false;
};

class raw_fanout_subscriber_t
{
  public:
    explicit raw_fanout_subscriber_t (zlink::poller_t *poller = nullptr);
    raw_fanout_subscriber_t (
      std::shared_ptr<zlink::context_t> context,
      zlink::poller_t *poller = nullptr);
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
    std::vector<raw_fanout_connection_snapshot_t>
    connection_snapshots () const;

  private:
    struct publisher_intent_key_t
    {
        std::vector<std::uint8_t> routing_id;
        std::uint64_t lifecycle_generation = 0;

        friend bool operator< (const publisher_intent_key_t &left,
                               const publisher_intent_key_t &right) noexcept
        {
            if (std::lexicographical_compare (
                  left.routing_id.begin (), left.routing_id.end (),
                  right.routing_id.begin (), right.routing_id.end ())) {
                return true;
            }
            if (std::lexicographical_compare (
                  right.routing_id.begin (), right.routing_id.end (),
                  left.routing_id.begin (), left.routing_id.end ())) {
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
        bool reconnecting = false;
        std::chrono::steady_clock::time_point deadline{};
    };

    bool connect_locked (std::vector<std::uint8_t> publisher_routing_id,
                         std::uint64_t lifecycle_generation,
                         std::string endpoint,
                         bool automatic);
    void close_connection_locked (connection_t &connection) noexcept;
    void reopen_locked (connection_t &connection);

    mutable std::mutex _mutex;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller = nullptr;
    std::map<publisher_intent_key_t, connection_t> _connections;
    std::optional<bool> _automatic_mode;
    std::size_t _receive_cursor = 0;
    // One caller-owned binding envelope is reused across subscriber sockets.
    zlink::topic_message_t _received;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::fanout
