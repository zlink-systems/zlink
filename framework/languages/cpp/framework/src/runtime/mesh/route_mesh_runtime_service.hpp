/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/mesh_node_runtime.hpp"
#include <runtime/locations/location_repository.hpp>

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/locations/runtime_query.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zlink::framework::runtime
{

class route_mesh_runtime_service_t final : public route_mesh_runtime_t
{
  public:
    struct state_t;

    route_mesh_runtime_service_t (
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes,
      location_runtime_query_t *location_runtime,
      location_repository_t *location_store = nullptr);
    ~route_mesh_runtime_service_t ();

    mesh_node_snapshot_t snapshot (std::string mesh_name) const override;
    std::unique_ptr<mesh_runtime_observation_t>
    observe (std::string mesh_name,
             std::size_t capacity,
             std::function<void (
               const observed_status_t<mesh_node_snapshot_t> &)> observer) override;
    bool is_ready (std::string mesh_name) const override;

    void start ();
    void stop () noexcept;

  private:
    std::shared_ptr<state_t> _state;
};

class route_mesh_runtime_host_service_t final : public hosted_service_t
{
  public:
    explicit route_mesh_runtime_host_service_t (
      std::shared_ptr<route_mesh_runtime_service_t> runtime);

    void start (service_provider_t &) override;
    void request_stop () noexcept override;
    void stop () noexcept override;

  private:
    std::shared_ptr<route_mesh_runtime_service_t> _runtime;
};

} // namespace zlink::framework::runtime
