/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/backend/raw_route_port.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/mesh/service_liveness_registry.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink
{
class context_t;
class dealer_socket_t;
class poller_t;
class router_socket_t;
class socket_monitor_t;
}

namespace zlink::framework::runtime::client_server
{

struct client_server_request_completion_t
{
    foundation::operation_terminal_t terminal =
      foundation::operation_terminal_t::transport_failed;
    protocol::reply_header_t reply_header{};
    std::vector<std::uint8_t> payload;
};

using client_server_request_callback_t =
  std::function<void (client_server_request_completion_t)>;

enum class client_server_pump_result_t
{
    no_data,
    infrastructure,
    application,
    backpressured,
    protocol_error
};

struct raw_client_server_server_options_t
{
    protocol::client_server_server_admission_t descriptor;
    std::optional<std::string> advertise_host;
    std::size_t mailbox_message_budget =
      dispatch_limits::application_mailbox_messages;
    std::size_t mailbox_byte_budget =
      dispatch_limits::application_mailbox_bytes;
    zlink::poller_t *transport_poller = nullptr;
    std::uintptr_t transport_poller_slot = 0;
};

class raw_client_server_server_t
{
  public:
    explicit raw_client_server_server_t (
      raw_client_server_server_options_t options);
    ~raw_client_server_server_t () noexcept;

    void start ();
    void close () noexcept;
    std::string endpoint () const;
    protocol::client_server_server_admission_t descriptor () const;
    void update_descriptor (
      protocol::client_server_server_admission_t descriptor);
    mesh::service_mailbox_t &mailbox () noexcept;

    std::size_t drain_monitor_events (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    client_server_pump_result_t pump_one (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    std::size_t last_pump_bytes () const noexcept { return _last_pump_bytes; }
    mesh::service_liveness_tick_t tick_liveness (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
    next_liveness_activity () const;
    bool reply (
      const mesh::service_mailbox_record_t &request,
      const protocol::application_payload_t &payload);
    bool reply (
      const mesh::service_mailbox_record_t &request,
      std::uint32_t terminal_result,
      protocol::framework_error_code failure_code);

  private:
    struct byte_vector_less_t
    {
        bool operator() (const std::vector<std::uint8_t> &left,
                         const std::vector<std::uint8_t> &right) const noexcept;
    };

    raw_client_server_server_options_t _options;
    mutable std::mutex _mutex;
    std::mutex _socket_mutex;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::router_socket_t> _router;
    std::unique_ptr<zlink::poller_t> _monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> _monitor;
    std::shared_ptr<detail::backend::raw_route_port_t> _port;
    mesh::service_mailbox_t _mailbox;
    std::optional<mesh::service_mailbox_record_t> _pending_received;
    mesh::service_liveness_registry_t _liveness;
    // The route id is the stable identity available from the public monitor
    // surface; monitor event values are ready counts or disconnect reasons.
    std::map<std::vector<std::uint8_t>, std::vector<std::uint8_t>,
             byte_vector_less_t>
      _connections;
    bool _closed = false;
    std::size_t _last_pump_bytes = 0;
};

struct raw_client_server_client_options_t
{
    std::vector<std::uint8_t> client_routing_id;
    protocol::client_server_client_admission_t admission;
    protocol::client_server_server_admission_t expected_server;
    zlink::poller_t *transport_poller = nullptr;
    std::uintptr_t transport_poller_slot = 0;
};

class raw_client_server_client_t
{
  public:
    explicit raw_client_server_client_t (
      raw_client_server_client_options_t options);
    ~raw_client_server_client_t () noexcept;

    void start ();
    void close () noexcept;
    bool ready () const noexcept;
    std::size_t drain_monitor_events (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    client_server_pump_result_t pump_one (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    std::size_t last_pump_bytes () const noexcept { return _last_pump_bytes; }
    mesh::service_liveness_tick_t tick_liveness (
      mesh::service_liveness_registry_t::clock_t::time_point now);
    std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
    next_liveness_activity () const;

    bool send (const protocol::application_payload_t &payload);
    bool request (
      const protocol::application_payload_t &payload,
      std::chrono::milliseconds timeout,
      client_server_request_callback_t callback);
    std::size_t pending_request_count () const noexcept;
    std::size_t expire_requests (
      foundation::operation_registry_t::clock_t::time_point now);

  private:
    static foundation::operation_id_t operation_id (
      std::uint64_t lifecycle_generation,
      std::uint64_t correlation);

    raw_client_server_client_options_t _options;
    mutable std::mutex _mutex;
    std::mutex _socket_mutex;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::dealer_socket_t> _dealer;
    std::unique_ptr<zlink::poller_t> _monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> _monitor;
    std::shared_ptr<detail::backend::raw_dealer_port_t> _port;
    std::shared_ptr<foundation::operation_registry_t> _operations;
    mesh::service_liveness_registry_t _liveness;
    std::vector<std::uint8_t> _connection_id;
    std::uint64_t _next_correlation = 1;
    std::size_t _last_pump_bytes = 0;
    bool _ready = false;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::client_server
