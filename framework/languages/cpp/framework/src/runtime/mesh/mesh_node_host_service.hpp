/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/mesh/mesh_node_runtime.hpp"

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zlink::framework::detail
{
class route_handler_registry_t;
}

namespace zlink::framework::runtime
{

class location_runtime_t;

// Owns every resource charged to one admitted application dispatch until its
// synchronous or deferred terminal settles. This internal value is separate
// from the host object so a late terminal never needs to retain or dereference
// the host service itself.
class application_dispatch_terminal_owner_t final
{
  public:
    application_dispatch_terminal_owner_t (
      std::shared_ptr<detail::mesh_node_runtime_t> node,
      std::function<void ()> complete_stateful_dispatch,
      std::function<void ()> release_mailbox_reservation);
    ~application_dispatch_terminal_owner_t () noexcept;

    application_dispatch_terminal_owner_t (
      const application_dispatch_terminal_owner_t &) = delete;
    application_dispatch_terminal_owner_t &operator= (
      const application_dispatch_terminal_owner_t &) = delete;

    void settle () noexcept;

  private:
    static void invoke (const std::function<void ()> &callback) noexcept;

    std::atomic_bool _settled{false};
    std::weak_ptr<detail::mesh_node_runtime_t> _node;
    std::function<void ()> _complete_stateful_dispatch;
    std::function<void ()> _release_mailbox_reservation;
};

class mesh_node_host_service_t final : public hosted_service_t,
                                       public hosted_service_lifecycle_t
{
  public:
    mesh_node_host_service_t (
      std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
      serializer_registry_t &serializers,
      dispatch_options_t dispatch_options = {},
      std::shared_ptr<listener_status_registry_t> listener_statuses = {},
      std::shared_ptr<application_job_queue_t> application_jobs = {});
    mesh_node_host_service_t (
      std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
      serializer_registry_t &serializers,
      handler_registry_t &filters,
      dispatch_options_t dispatch_options = {},
      std::shared_ptr<listener_status_registry_t> listener_statuses = {},
      std::shared_ptr<application_job_queue_t> application_jobs = {});
    ~mesh_node_host_service_t () override;

    void start (service_provider_t &services) override;
    void request_stop () noexcept override;
    void stop () noexcept override;
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes () const;
    actor_manager_t actor_manager ();
    zlink::submit_result_t submit_local_node_send (
      const std::shared_ptr<detail::mesh_node_runtime_t> &node,
      std::vector<zlink::message_t> parts);
    void seal_application_dispatch () noexcept override;
    bool wait_for_accepted_callbacks_until (
      std::chrono::steady_clock::time_point deadline) noexcept override;
    bool publish_descriptor_state (
      framework_runtime_state_t state) noexcept override;
    void visit_relocation_nodes (
      const std::function<void (
        const std::shared_ptr<detail::mesh_node_runtime_t> &)> &visitor)
      const override;
    bool republish_after_store_recovery () noexcept;

  private:
    struct actor_destroy_callback_gate_t;

    task_t<spot_create_result_t> create_user_spot (
      const std::shared_ptr<detail::mesh_node_runtime_t> &source,
      bool exclusive,
      std::optional<spot_id_t> spot_id,
      std::string stable_type,
      std::optional<std::string> mesh_name,
      std::optional<message_t> request,
      std::chrono::milliseconds timeout);
    task_t<std::optional<spot_ref_t>> find_user_spot (
      spot_id_t spot_id);
    task_t<bool> close_user_spot (
      const std::shared_ptr<detail::mesh_node_runtime_t> &source,
      spot_ref_t spot);
    task_t<actor_create_result_t> create_actor (
      bool exclusive,
      actor_id_t actor_id,
      std::string stable_type,
      std::optional<std::string> mesh_name,
      std::optional<message_t> request,
      std::chrono::milliseconds timeout,
      creation_operation_identity_t operation);
    task_t<actor_create_result_t> complete_remote_actor_creation (
      std::shared_ptr<detail::mesh_node_runtime_t> source,
      mesh_node_descriptor_t target,
      protocol::actor_create_header_t command,
      std::chrono::milliseconds timeout,
      actor_id_t actor_id,
      std::string stable_type,
      object_creation_key_t reserve_key,
      object_reservation_fence_t fence,
      creation_operation_identity_t operation,
      std::chrono::system_clock::time_point operation_deadline);
    task_t<std::optional<actor_ref_t>> find_actor (
      actor_id_t actor_id);
    task_t<std::optional<spot_ref_t>> find_actor_spot (
      actor_id_t actor_id);
    result_t<void> finalize_local_actor_destroy (const actor_ref_t &actor);
    task_t<bool> destroy_actor (actor_ref_t actor);

    std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> _registrations;
    serializer_registry_t *_serializers;
    handler_registry_t *_filters;
    dispatch_options_t _dispatch_options;
    service_provider_t *_services = nullptr;
    std::shared_ptr<location_repository_t> _location_store;
    location_runtime_t *_location_runtime = nullptr;
    std::optional<location_owner_token_t> _location_owner;
    std::vector<mesh_node_descriptor_key_t> _published_mesh_nodes;
    std::vector<mesh_node_descriptor_t> _published_mesh_descriptors;
    std::mutex _descriptor_publish_mutex;
    std::atomic_bool _stop{false};
    std::atomic_bool _accept_application_dispatch{false};
    mutable std::mutex _dispatch_gate_mutex;
    std::condition_variable _dispatch_gate_changed;
    std::uint64_t _active_direct_dispatch = 0;
    std::unique_ptr<offload_executor_t> _application_dispatch;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::shared_ptr<listener_status_registry_t> _listener_statuses;
    std::shared_ptr<actor_destroy_callback_gate_t> _actor_destroy_gate;
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> _nodes;
    std::vector<std::thread> _threads;

    std::optional<location_owner_token_t> current_location_owner () const;
};

} // namespace zlink::framework::runtime
