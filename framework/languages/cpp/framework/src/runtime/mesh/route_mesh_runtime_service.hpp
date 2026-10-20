/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/mesh/mesh_node_runtime.hpp"
#include <runtime/locations/location_repository.hpp>

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/locations/runtime_query.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <zlink/framework/contracts/configuration/lifecycle.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zlink::framework::detail
{
class monitoring_runtime_state_t;
}

namespace zlink::framework::runtime
{

struct route_mesh_runtime_service_test_access_t;

class route_mesh_runtime_service_t final : public route_mesh_runtime_t
{
  public:
    struct state_t;

    route_mesh_runtime_service_t (
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes,
      location_runtime_query_t *location_runtime,
      location_repository_t *location_store = nullptr,
      std::shared_ptr<zlink::framework::detail::monitoring_runtime_state_t> monitoring = {},
      std::function<framework_runtime_state_t ()> host_state = {});
    ~route_mesh_runtime_service_t ();

    mesh_node_snapshot_t snapshot (std::string mesh_name) const override;
    std::unique_ptr<mesh_runtime_observation_t> observe (
      std::string mesh_name,
      std::size_t capacity,
      std::function<void (const observed_status_t<mesh_node_snapshot_t> &)> observer) override;
    bool is_ready (std::string mesh_name) const override;

    void start ();
    void stop () noexcept;

  private:
    friend struct route_mesh_runtime_service_test_access_t;

    static constexpr auto management_refresh_interval = std::chrono::milliseconds (100);

    template <class Now, class Refresh, class Wait>
    static void pump_management (Now now, Refresh refresh, Wait wait)
    {
        // Topology changes publish through their existing callback. This owner waits
        // only for descriptor management and shutdown (hot path §4.1 I0, §4.2).
        // The local capacity projection also needs this deadline without a Store.
        while (wait (now () + management_refresh_interval))
            refresh ();
    }

    // A status query in two steps: the projection of the node, then its publication.
    mesh_node_snapshot_t query_projection (const std::string &mesh_name) const;
    mesh_node_snapshot_t complete_query (const std::string &mesh_name,
                                         mesh_node_snapshot_t projection) const;

    std::shared_ptr<state_t> _state;
};

class route_mesh_runtime_host_service_t final : public hosted_service_t
{
  public:
    explicit route_mesh_runtime_host_service_t (
      std::shared_ptr<route_mesh_runtime_service_t> runtime);

    task_t<void> start (service_provider_t &) override;
    void request_stop () noexcept override;
    void stop () noexcept override;

  private:
    std::shared_ptr<route_mesh_runtime_service_t> _runtime;
};

} // namespace zlink::framework::runtime
