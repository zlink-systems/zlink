/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/execution/state_lane.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/eventing/runtime_wake_timer.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/client_server_runtime.hpp>
#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::runtime::client_server
{

mesh::service_node_state_t client_server_service_state (framework_runtime_state_t state);
framework_runtime_state_t client_server_framework_state (mesh::service_node_state_t state);

/* One server receive turn reads the queued records until no data, backpressure, the receive
 * batch budget, or the Application Job Queue supply stops it, as the client turn does. Each
 * record takes its own permit: the first is `application_permit`, the rest are reserved only
 * while the queue has room. */
task_t<void>
pump_server_transport (std::shared_ptr<raw_client_server_server_t> server,
                       mesh::service_liveness_registry_t::clock_t::time_point now,
                       std::shared_ptr<application_job_queue_t> application_jobs,
                       std::shared_ptr<application_job_queue_t::permit_t> application_permit,
                       receive_batch_budget_t budget = {});

class client_server_location_runtime_t final : public client_server_runtime_t
{
  public:
    client_server_location_runtime_t (
      message_bus_t bus,
      std::vector<channel_snapshot_t> channels,
      location_runtime_t &locations,
      location_repository_t &store,
      location_repository_t &leases,
      service_provider_t &services,
      serializer_registry_t &serializers,
      const handler_registry_t &handlers,
      std::map<std::string, std::string> advertise_hosts = {},
      std::shared_ptr<listener_status_registry_t> listener_statuses = {},
      std::shared_ptr<application_job_queue_t> application_jobs = {});
    ~client_server_location_runtime_t () noexcept;

    client_server_location_runtime_t (const client_server_location_runtime_t &) = delete;
    client_server_location_runtime_t &operator= (const client_server_location_runtime_t &) = delete;

    void start ();
    void seal_application_dispatch () noexcept;
    bool
    wait_for_accepted_callbacks_until (std::chrono::steady_clock::time_point deadline) noexcept;
    void stop ();
    bool empty () const noexcept;
    bool publish_descriptor_state (framework_runtime_state_t state) noexcept;
    bool republish_after_store_recovery ();

    client_server_channel_snapshot_t snapshot (std::string channel_name) const override;
    std::unique_ptr<mesh_runtime_observation_t>
    observe (std::string channel_name,
             std::size_t capacity,
             std::function<void (const observed_status_t<client_server_channel_snapshot_t> &)>
               observer) override;
    bool is_ready (std::string channel_name) const override;

    using observer_t = zlink::framework::observation_detail::runtime_observer_state_t<
      client_server_channel_snapshot_t>;

  private:
    struct server_entry_t;
    struct client_connection_t;
    struct client_channel_t;
    struct pump_task_state_t;
    struct ready_waiter_t;
    struct snapshot_connection_t;
    struct snapshot_source_t;
    struct worker_lane_snapshot_t;

    void start_server (const channel_snapshot_t &channel,
                       const std::optional<location_owner_token_t> &publication_owner);
    void start_client (const channel_snapshot_t &channel);
    void run ();
    void reconcile ();
    task_t<void> reconcile_task ();
    task_t<void> reconcile_channel_task (client_channel_t &channel);
    bool publish_servers ();
    task_t<bool> publish_servers_task ();
    task_t<void> pump ();
    task_t<worker_lane_snapshot_t> refresh_client_pump_snapshot ();
    task_t<void> publish_snapshot_changes ();
    client_server_channel_snapshot_t
    publish_snapshot_locked (client_server_channel_snapshot_t current,
                             const std::shared_ptr<observer_t> &initial_observer = {}) const;
    boost::asio::awaitable<result_t<void>>
    drain_server_owner (std::shared_ptr<raw_client_server_server_t> server, std::string owner);
    void stop_servers ();
    void stop_clients ();

    task_t<void> send (const std::string &channel_name,
                       std::string packet_name,
                       std::string content_type,
                       zlink::message_t message,
                       std::map<std::string, std::string> metadata);
    task_t<zlink::message_t> request (const std::string &channel_name,
                                      std::string packet_name,
                                      std::string content_type,
                                      zlink::message_t message,
                                      std::chrono::milliseconds timeout,
                                      std::map<std::string, std::string> metadata);
    task_t<std::shared_ptr<raw_client_server_client_t>> select_ready (std::string channel_name);
    result_t<client_channel_t *> select_channel_locked (const std::string &channel_name);
    result_t<std::shared_ptr<raw_client_server_client_t>>
    select_ready_locked (const std::string &channel_name,
                         std::chrono::steady_clock::time_point deadline);
    task_t<void> complete_ready_waiters ();

    static std::uint64_t make_lifecycle_generation ();
    static std::uint32_t
    effective_max_message_bytes (const channel_capability_snapshot_t &capability);
    static std::vector<std::uint8_t> client_routing_id (const channel_snapshot_t &channel);
    static std::vector<std::uint8_t> server_routing_id (const channel_snapshot_t &channel);
    static protocol::client_server_server_admission_t
    to_admission (const client_server_server_descriptor_t &descriptor,
                  std::uint32_t effective_max_message_bytes);
    static client_server_server_descriptor_t
    to_descriptor (const protocol::client_server_server_admission_t &admission,
                   const location_owner_token_t &owner);
    task_t<bool> owner_is_live_task (client_server_server_descriptor_t descriptor) const;
    snapshot_source_t snapshot_source_locked (const std::string &channel_name) const;
    task_t<client_server_channel_snapshot_t> snapshot_task (std::string channel_name) const;
    task_t<client_server_channel_snapshot_t> build_snapshot_task (snapshot_source_t source) const;
    client_server_channel_snapshot_t build_snapshot (snapshot_source_t source) const;
    static bool snapshot_equivalent (const client_server_channel_snapshot_t &left,
                                     const client_server_channel_snapshot_t &right) noexcept;

    message_bus_t _bus;
    detail::channel_runtime_t _channel_runtime;
    std::vector<channel_snapshot_t> _channels;
    location_runtime_t *_locations;
    location_repository_t *_store;
    location_repository_t *_leases;
    service_provider_t _services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::unique_ptr<application_supply_slot_t> _application_supply;
    std::map<std::string, std::string> _advertise_hosts;
    std::shared_ptr<listener_status_registry_t> _listener_statuses;
    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    std::map<std::string, std::unique_ptr<server_entry_t>> _servers;
    std::map<std::string, std::unique_ptr<client_channel_t>> _clients;
    mutable std::map<std::string, std::uint64_t> _snapshot_sequences;
    mutable std::map<std::string, client_server_channel_snapshot_t> _last_snapshots;
    mutable std::map<std::string, std::vector<std::weak_ptr<observer_t>>> _observers;
    std::size_t _server_pump_cursor = 0;
    std::size_t _client_pump_cursor = 0;
    std::vector<server_entry_t *> _server_pump_snapshot;
    std::vector<client_connection_t *> _client_pump_snapshot;
    std::vector<std::unique_ptr<ready_waiter_t>> _ready_waiters;
    std::unique_ptr<zlink::poller_t> _transport_poller;
    std::shared_ptr<eventing::runtime_wake_timer_t> _wake_timer =
      std::make_shared<eventing::runtime_wake_timer_t> ();
    std::atomic_bool _stop{false};
    std::mutex _descriptor_publish_mutex;
    std::condition_variable _descriptor_publish_changed;
    bool _descriptor_publish_pending = false;
    bool _descriptor_publish_result = false;
    std::size_t _active_application_drains = 0;
    std::thread _thread;
};

} // namespace zlink::framework::runtime::client_server
