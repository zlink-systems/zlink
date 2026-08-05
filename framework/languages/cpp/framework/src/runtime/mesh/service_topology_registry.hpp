/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zlink::framework::runtime::mesh
{

std::uint64_t
sum_service_weights (std::span<const int> weights);

enum class service_node_state_t
{
    preparing,
    serving,
    retiring,
    draining,
    stopped,
    error
};

enum class service_object_role_t
{
    none,
    client,
    server
};

struct service_channel_descriptor_t
{
    std::string name;
    int weight = 100;

    friend bool operator== (const service_channel_descriptor_t &,
                            const service_channel_descriptor_t &) = default;
};

struct service_node_descriptor_t
{
    std::string mesh_name;
    std::vector<std::uint8_t> node_routing_id;
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::string advertised_endpoint;
    std::vector<service_channel_descriptor_t> channels;
    service_node_state_t state = service_node_state_t::preparing;
    std::string security_identity = "default";
    std::uint32_t effective_max_message_bytes = 4u * 1024u * 1024u;
    std::int64_t application_version = 0;
    std::vector<std::string> protocol_capabilities{"framework-service-v11"};
    service_object_role_t object_role = service_object_role_t::none;
    int placement_weight = 100;
    std::uint32_t active_capacity_limit = 10000;
    std::uint32_t pending_capacity_limit = 128;
    std::uint32_t active_capacity_used = 0;
    std::uint32_t pending_capacity_used = 0;

    friend bool operator== (const service_node_descriptor_t &,
                            const service_node_descriptor_t &) = default;
};

enum class peer_admission_result_t
{
    admitted,
    duplicate_connection,
    not_required,
    mesh_mismatch,
    invalid_descriptor,
    stale_descriptor
};

enum class service_connection_direction_t
{
    inbound,
    outbound
};

struct admitted_peer_t
{
    service_node_descriptor_t descriptor;
    std::vector<std::uint8_t> connection_id;
    service_connection_direction_t direction =
      service_connection_direction_t::inbound;
};

bool route_mesh_connection_not_required (
  const service_node_descriptor_t &local,
  const service_node_descriptor_t &remote) noexcept;

class service_topology_registry_t
{
  public:
    explicit service_topology_registry_t (service_node_descriptor_t local);

    void publish_local (service_node_descriptor_t descriptor);
    service_node_descriptor_t local_descriptor () const;
    void set_change_handler (std::function<void ()> handler);

    peer_admission_result_t admit (
      service_node_descriptor_t descriptor,
      std::vector<std::uint8_t> connection_id);
    peer_admission_result_t admit (
      service_node_descriptor_t descriptor,
      std::vector<std::uint8_t> connection_id,
      service_connection_direction_t direction);
    peer_admission_result_t admit (
      service_node_descriptor_t descriptor,
      std::vector<std::uint8_t> connection_id,
      service_connection_direction_t direction,
      const service_node_descriptor_t &expected_descriptor);
    bool disconnect (const std::vector<std::uint8_t> &node_routing_id,
                     const std::vector<std::uint8_t> &connection_id);

    std::vector<admitted_peer_t> peers () const;
    std::vector<service_node_descriptor_t> not_required_peers () const;
    std::optional<admitted_peer_t>
    peer (const std::vector<std::uint8_t> &node_routing_id) const;
    std::optional<admitted_peer_t> select (const std::string &channel_name);
    std::vector<admitted_peer_t>
    multicast_targets (const std::string &channel_name) const;

  private:
    struct byte_vector_less_t
    {
        bool operator() (const std::vector<std::uint8_t> &left,
                         const std::vector<std::uint8_t> &right) const noexcept;
    };

    static bool valid_descriptor (const service_node_descriptor_t &descriptor);
    static bool selectable (const service_node_descriptor_t &descriptor,
                            const std::string &channel_name);
    peer_admission_result_t admit_impl (
      service_node_descriptor_t descriptor,
      std::vector<std::uint8_t> connection_id,
      std::optional<service_connection_direction_t> direction,
      const service_node_descriptor_t *expected_descriptor);

    mutable std::mutex _mutex;
    service_node_descriptor_t _local;
    std::map<std::vector<std::uint8_t>, admitted_peer_t, byte_vector_less_t> _peers;
    std::map<std::vector<std::uint8_t>,
             service_node_descriptor_t,
             byte_vector_less_t>
      _not_required_peers;
    struct selection_state_t
    {
        bool initialized = false;
        std::uint64_t topology_version = 0;
        std::uint64_t total_weight = 0;
        std::map<std::vector<std::uint8_t>,
                 std::uint64_t,
                 byte_vector_less_t>
          weights;
        std::map<std::vector<std::uint8_t>, std::int64_t, byte_vector_less_t>
          cumulative;
        std::vector<std::vector<std::uint8_t>> ordered_node_ids;
        std::vector<std::uint64_t> ordered_weights;
        std::vector<std::int64_t> precomputed_initial_cumulative;
        std::vector<std::int64_t> precomputed_cycle_start_cumulative;
        std::vector<std::size_t> precomputed_schedule;
        std::vector<std::size_t> precomputed_selected_counts;
        std::size_t precomputed_total_selections = 0;
        std::size_t precomputed_cursor = 0;
        std::size_t precomputed_cycle_start = 0;
        std::size_t precomputed_schedule_end = 0;
        bool precomputed = false;
    };

    void materialize_selection_state (selection_state_t &state);
    void rebuild_selection_schedule (selection_state_t &state);

    std::map<std::string, selection_state_t> _selection_state;
    std::uint64_t _topology_version = 0;
    std::function<void ()> _change_handler;
};

} // namespace zlink::framework::runtime::mesh
