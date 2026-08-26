/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace zlink::samples::gamequest
{

/* This reports the sample-owned capability after the Framework host has started
 * its registered services.  It deliberately does not inspect Framework logs. */
class sample_readiness_service_t final : public framework::hosted_service_t
{
  public:
    sample_readiness_service_t (std::string kind, std::string node) :
        _kind (std::move (kind)), _node (std::move (node))
    {
    }

    framework::task_t<void> start (framework::service_provider_t &) override
    {
        std::cout << "gamequest-ready kind=" << _kind << " node=" << _node << std::endl;
        co_return;
    }

    void request_stop () noexcept override {}
    void stop () noexcept override {}

  private:
    std::string _kind;
    std::string _node;
};

/* Reports the sample-owned route capability only after the public RouteMesh
 * runtime says this mesh is usable.  The observer covers state changes while
 * the snapshot poll closes the registration race. */
class spot_route_readiness_service_t final : public framework::hosted_service_t
{
  private:
    struct state_t
    {
        std::atomic_bool reported{false};
        std::atomic_bool stopping{false};
    };

  public:
    spot_route_readiness_service_t (std::string mesh_name, std::string node_name) :
        _mesh_name (std::move (mesh_name)), _node_name (std::move (node_name))
    {
    }

    framework::task_t<void> start (framework::service_provider_t &services) override
    {
        auto state = std::make_shared<state_t> ();
        _state = state;
        auto &runtime = services.get_required<framework::route_mesh_runtime_t> ();
        _observation = runtime.observe (
          _mesh_name, 64,
          [state, node_name = _node_name, mesh_name = _mesh_name] (
            const framework::observed_status_t<framework::mesh_node_snapshot_t> &observed) {
              report_if_ready (state, node_name, mesh_name, observed.status);
          });
        _worker = std::thread (
          [state, runtime = &runtime, mesh_name = _mesh_name, node_name = _node_name] () mutable {
              while (!state->stopping.load (std::memory_order_acquire)) {
                  try {
                      report_if_ready (state, node_name, mesh_name, runtime->snapshot (mesh_name));
                  }
                  catch (...) {
                  }
                  if (state->reported.load (std::memory_order_acquire))
                      return;
                  std::this_thread::sleep_for (std::chrono::milliseconds (50));
              }
          });
        co_return;
    }

    void request_stop () noexcept override
    {
        if (_state)
            _state->stopping.store (true, std::memory_order_release);
        if (_observation)
            _observation->close ();
    }

    void stop () noexcept override
    {
        request_stop ();
        if (_worker.joinable ())
            _worker.join ();
        _observation.reset ();
        _state.reset ();
    }

  private:
    static void report_if_ready (const std::shared_ptr<state_t> &state,
                                 const std::string &node_name,
                                 const std::string &mesh_name,
                                 const framework::mesh_node_snapshot_t &snapshot)
    {
        if (!snapshot.is_ready || state->reported.exchange (true, std::memory_order_acq_rel))
            return;
        std::cout << "gamequest-ready kind=spot-route node=" << node_name
                  << " mesh=" << mesh_name << std::endl;
    }

    std::string _mesh_name;
    std::string _node_name;
    std::shared_ptr<state_t> _state;
    std::unique_ptr<framework::mesh_runtime_observation_t> _observation;
    std::thread _worker;
};

} // namespace zlink::samples::gamequest
