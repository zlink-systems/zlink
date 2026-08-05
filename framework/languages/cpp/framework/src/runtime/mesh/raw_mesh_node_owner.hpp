/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/mesh/service_liveness_registry.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace zlink
{
class context_t;
class poller_t;
class router_socket_t;
class socket_monitor_t;
}

namespace zlink::framework::runtime::mesh
{

enum class raw_mesh_pump_result_t
{
    no_data,
    infrastructure,
    application,
    backpressured,
    capacity_exceeded,
    protocol_error
};

struct raw_mesh_node_options_t
{
    service_node_descriptor_t descriptor;
    std::size_t application_message_budget =
      dispatch_limits::application_mailbox_messages;
    std::size_t application_byte_budget =
      dispatch_limits::application_mailbox_bytes;
    std::size_t infrastructure_message_budget =
      dispatch_limits::control_mailbox_messages;
    std::size_t infrastructure_byte_budget =
      dispatch_limits::control_mailbox_bytes;
    std::uint64_t send_high_water_mark = 4'096'000;
    std::uint64_t receive_high_water_mark = 4'096'000;
    std::optional<std::string> advertise_host;
    zlink::auto_hwm_profile auto_hwm_profile =
      zlink::auto_hwm_profile::balanced;
};

struct raw_mesh_byte_vector_less_t
{
    bool operator() (const std::vector<std::uint8_t> &left,
                     const std::vector<std::uint8_t> &right) const noexcept;
};

struct raw_mesh_connection_candidate_t
{
    std::vector<std::uint8_t> connection_id;
    std::string remote_endpoint;
    service_connection_direction_t direction =
      service_connection_direction_t::inbound;
    std::uint64_t ready_sequence = 0;
};

class raw_mesh_connection_candidates_t
{
  public:
    void ready (
      const std::vector<std::uint8_t> &node_routing_id,
      std::vector<std::uint8_t> connection_id,
      service_connection_direction_t direction,
      std::string remote_endpoint = {});
    std::optional<raw_mesh_connection_candidate_t>
    for_handshake (
      const std::vector<std::uint8_t> &node_routing_id,
      service_connection_direction_t preferred_direction) const;
    bool disconnect (
      const std::vector<std::uint8_t> &node_routing_id,
      const std::vector<std::uint8_t> &connection_id);
    std::size_t size (
      const std::vector<std::uint8_t> &node_routing_id) const;

  private:
    std::map<
      std::vector<std::uint8_t>,
      std::map<std::vector<std::uint8_t>,
               raw_mesh_connection_candidate_t,
               raw_mesh_byte_vector_less_t>,
      raw_mesh_byte_vector_less_t>
      _candidates;
    std::uint64_t _next_ready_sequence = 1;
};

class raw_mesh_node_owner_t
{
  public:
    explicit raw_mesh_node_owner_t (raw_mesh_node_options_t options);
    ~raw_mesh_node_owner_t () noexcept;

    raw_mesh_node_owner_t (const raw_mesh_node_owner_t &) = delete;
    raw_mesh_node_owner_t &operator= (const raw_mesh_node_owner_t &) = delete;

    void start ();
    void close () noexcept;
    bool started () const noexcept;
    void set_send_ready_handler (std::function<void ()> handler);

    std::string endpoint () const;
    zlink::context_t &context ();
    service_topology_registry_t &topology () noexcept;
    service_liveness_registry_t &liveness () noexcept;
    service_mailbox_t &mailbox () noexcept;

    bool connect_peer (const std::string &endpoint);
    bool connect_peer (const std::string &endpoint,
                       service_node_descriptor_t expected_descriptor);
    void expect_peer (service_node_descriptor_t expected_descriptor);
    void forget_peer (const std::vector<std::uint8_t> &node_routing_id,
                      const std::string &endpoint);
    peer_admission_result_t admit_peer (
      service_node_descriptor_t descriptor,
      std::vector<std::uint8_t> connection_id,
      service_liveness_registry_t::clock_t::time_point now);
    bool send_to_node (const std::vector<std::uint8_t> &target_routing_id,
                       const protocol::application_payload_t &application_payload);
    bool request_to_node (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool request_to_channel (
      const std::string &channel_name,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool reply (const service_mailbox_record_t &request,
                const protocol::application_payload_t &application_payload);
    bool reply_failure (const service_mailbox_record_t &request,
                        std::uint32_t terminal_result,
                        std::uint32_t failure_code);
    std::size_t expire_requests (
      foundation::operation_registry_t::clock_t::time_point now);
    bool send_to_channel (const std::string &channel_name,
                          const protocol::application_payload_t &application_payload);
    bool send_to_spot (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::string &source_spot_id,
      const protocol::spot_route_fence_t &target,
      const protocol::application_payload_t &application_payload);
    bool request_to_spot (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::string &source_spot_id,
      const protocol::spot_route_fence_t &target,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback,
      std::optional<protocol::wire_operation_id_t> operation = std::nullopt);
    bool send_to_actor (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
      const protocol::actor_route_fence_t &target,
      const protocol::application_payload_t &application_payload);
    bool request_to_actor (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
      const protocol::actor_route_fence_t &target,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback,
      std::optional<protocol::wire_operation_id_t> operation = std::nullopt);
    bool request_user_spot_create (
      const std::vector<std::uint8_t> &target_routing_id,
      protocol::user_spot_create_header_t request,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool request_actor_create (
      const std::vector<std::uint8_t> &target_routing_id,
      protocol::actor_create_header_t request,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool request_instance_spot_activation (
      const std::vector<std::uint8_t> &target_routing_id,
      protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      protocol::application_payload_t application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool send_instance_spot_activation (
      const std::vector<std::uint8_t> &target_routing_id,
      protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      protocol::application_payload_t application_payload);
    bool request_user_spot_close (
      const std::vector<std::uint8_t> &target_routing_id,
      protocol::user_spot_close_header_t request,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool reply_user_spot_create (
      const service_mailbox_record_t &request,
      const protocol::user_spot_create_reply_t &reply,
      std::optional<protocol::application_payload_t>
        application_reply = std::nullopt);
    bool reply_actor_create (
      const service_mailbox_record_t &request,
      const protocol::actor_create_reply_t &reply,
      std::optional<protocol::application_payload_t>
        application_reply = std::nullopt);
    bool reply_instance_spot_activation (
      const service_mailbox_record_t &request,
      std::uint32_t terminal_result,
      std::uint32_t failure_code,
      std::optional<protocol::application_payload_t> application_reply = std::nullopt);
    bool reply_user_spot_close (
      const service_mailbox_record_t &request,
      const protocol::user_spot_close_reply_t &reply);
    bool send_session_relocation_route (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_route_t &route);
    bool request_session_relocation_route (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_route_t &route,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool send_session_relocation_seal (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_seal_t &seal);
    bool request_session_relocation_seal (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_seal_t &seal,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool send_session_relocation_sealed (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_sealed_t &sealed);
    bool send_session_relocation_routed (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::session_relocation_routed_t &routed);
    bool send_reply_relay (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::reply_relay_t &relay,
      std::optional<protocol::application_payload_t>
        application_reply = std::nullopt);
    bool send_reply_relay_ack (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::reply_relay_ack_t &ack);
    bool send_message_follow (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::message_follow_notice_t &notice);
    bool send_relocation_control (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::relocation_control_t &control);
    raw_mesh_pump_result_t
    pump_one (service_liveness_registry_t::clock_t::time_point now,
              bool accept_application_receive = true);
    /* Wait for ROUTER input or an already admitted mailbox/control record.
     * This is the blocking wake-up used by the host ingress loop; it avoids
     * turning an idle node into a timed polling loop. */
    bool wait_for_activity (std::chrono::milliseconds timeout,
                            bool accept_application_receive = true) noexcept;
    std::size_t last_pump_bytes () const noexcept
    {
        return _last_pump_bytes;
    }
    std::size_t drain_monitor_events (
      service_liveness_registry_t::clock_t::time_point now);
    service_liveness_tick_t
    tick_liveness (service_liveness_registry_t::clock_t::time_point now);

  private:
    struct pending_received_mailbox_record_t
    {
        service_mailbox_record_t record;
        raw_mesh_pump_result_t accepted_result;
    };

    struct pending_unadmitted_application_t
    {
        detail::backend::raw_received_t received;
        std::size_t bytes = 0;
    };

    struct pending_admission_t
    {
        detail::backend::raw_received_t received;
        std::size_t bytes = 0;
    };

    static std::string owner_key (const std::vector<std::uint8_t> &routing_id);
    static foundation::operation_id_t operation_id (
      std::uint64_t lifecycle_generation,
      std::uint64_t correlation);
    std::uint64_t next_operation_sequence ();
    bool request_to_target (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback,
      const std::optional<std::string> &channel_name);
    bool send_with_header (
      const std::vector<std::uint8_t> &target_routing_id,
      std::vector<std::uint8_t> header,
      const protocol::application_payload_t &application_payload);
    bool send_header_only (
      const std::vector<std::uint8_t> &target_routing_id,
      std::vector<std::uint8_t> header);
    bool request_with_header (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
      const protocol::application_payload_t &application_payload,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    bool request_infrastructure (
      const std::vector<std::uint8_t> &target_routing_id,
      const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
      const std::function<std::vector<std::uint8_t> (
        const detail::backend::raw_message_t &)> &decode_reply,
      std::chrono::milliseconds timeout,
      foundation::operation_registry_t::callback_t callback);
    struct pending_request_t
    {
        std::vector<std::uint8_t> target_routing_id;
        detail::backend::raw_message_t wire;
        foundation::operation_id_t operation;
        std::uint64_t correlation = 0;
    };
    bool submit_request (
      const std::shared_ptr<detail::backend::raw_route_port_t> &port,
      const pending_request_t &request,
      std::chrono::milliseconds timeout);
    void trace_admission_phase (
      const std::vector<std::uint8_t> &node_routing_id,
      std::uint64_t lifecycle_generation,
      protocol::command command,
      peer_admission_result_t result);
    void discard_pending_unadmitted_applications (
      const std::vector<std::uint8_t> &node_routing_id);
    bool reply_infrastructure (
      const service_mailbox_record_t &request,
      std::vector<std::uint8_t> header);
    raw_mesh_pump_result_t enqueue_received_or_retain (
      service_mailbox_record_t record,
      raw_mesh_pump_result_t accepted_result);
    void accept_completion_control (
      detail::backend::raw_bytes_t source_routing_id,
      detail::backend::raw_message_t parts);
    void disconnect_completion_control_source (
      const detail::backend::raw_bytes_t &source_routing_id) noexcept;
    bool send_completion_control (
      const std::vector<std::uint8_t> &target_routing_id,
      detail::backend::raw_message_t parts);

    raw_mesh_node_options_t _options;
    mutable std::mutex _lifecycle_mutex;
    std::mutex _socket_mutex;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::router_socket_t> _router;
    std::unique_ptr<zlink::poller_t> _monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> _monitor;
    std::function<void ()> _send_ready_handler;
    std::shared_ptr<detail::backend::raw_route_port_t> _port;
    service_topology_registry_t _topology;
    service_liveness_registry_t _liveness;
    service_mailbox_t _mailbox;
    std::optional<pending_received_mailbox_record_t> _pending_received;
    std::deque<pending_unadmitted_application_t>
      _pending_unadmitted_applications;
    std::size_t _pending_unadmitted_application_bytes = 0;
    std::deque<pending_admission_t> _pending_admissions;
    std::size_t _pending_admission_bytes = 0;
    std::size_t _last_pump_bytes = 0;
    struct pending_completion_control_t
    {
        detail::backend::raw_received_t received;
        std::optional<std::uint64_t> admitted_peer_generation;
    };
    std::mutex _completion_control_mutex;
    std::deque<pending_completion_control_t> _completion_controls;
    struct completion_control_failure_t
    {
        detail::backend::raw_bytes_t source_routing_id;
        raw_mesh_pump_result_t result = raw_mesh_pump_result_t::protocol_error;
    };
    std::optional<completion_control_failure_t> _completion_control_failure;
    bool _accept_completion_controls = false;
    std::shared_ptr<foundation::operation_registry_t> _operations;
    std::map<std::vector<std::uint8_t>, service_node_descriptor_t,
             raw_mesh_byte_vector_less_t>
      _expected_peers;
    raw_mesh_connection_candidates_t _connections;
    std::set<std::string> _outbound_endpoints;
    std::uint64_t _next_correlation = 1;
    bool _closed = false;
};

} // namespace zlink::framework::runtime::mesh
