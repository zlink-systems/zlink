/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace zlink::samples::shoppingmall
{

/* These are sample-owned observations.  In particular, object-route readiness
 * is reported from the passive RouteMesh snapshot, never by a runner probe. */
class shoppingmall_http_readiness_service_t final : public framework::hosted_service_t
{
  public:
    explicit shoppingmall_http_readiness_service_t (std::string node_name) :
        _node_name (std::move (node_name))
    {
    }

    framework::task_t<void> start (framework::service_provider_t &) override
    {
        std::cout << "shoppingmall-ready kind=http node=" << _node_name << std::endl;
        co_return;
    }

    void request_stop () noexcept override {}
    void stop () noexcept override {}

  private:
    std::string _node_name;
};

class shoppingmall_object_route_readiness_service_t final : public framework::hosted_service_t
{
  public:
    shoppingmall_object_route_readiness_service_t (std::string mesh_name,
                                                    std::string node_name,
                                                    std::string target_rid,
                                                    std::string target_node_name) :
        _mesh_name (std::move (mesh_name)), _node_name (std::move (node_name)),
        _target_rid (std::move (target_rid)), _target_node_name (std::move (target_node_name))
    {
    }

    framework::task_t<void> start (framework::service_provider_t &services) override
    {
        auto state = std::make_shared<state_t> ();
        _state = state;
        auto &runtime = services.get_required<framework::route_mesh_runtime_t> ();
        _observation = runtime.observe (
          _mesh_name, 64,
          [state, node_name = _node_name, target_rid = _target_rid,
           target_node_name = _target_node_name] (
            const framework::observed_status_t<framework::mesh_node_snapshot_t> &observed) {
              report_if_ready (state, node_name, target_rid, target_node_name, observed.status);
          });
        /* A peer can become Ready while the observer is installed.  Poll the
         * same passive snapshot to close that registration race. */
        _worker = std::thread ([state, runtime = &runtime, mesh_name = _mesh_name,
                                node_name = _node_name, target_rid = _target_rid,
                                target_node_name = _target_node_name] () mutable {
            while (!state->stopping.load (std::memory_order_acquire)) {
                try {
                    report_if_ready (state, node_name, target_rid, target_node_name,
                                     runtime->snapshot (mesh_name));
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
    struct state_t
    {
        std::atomic_bool reported{false};
        std::atomic_bool stopping{false};
    };

    static void report_if_ready (const std::shared_ptr<state_t> &state,
                                 const std::string &node_name,
                                 const std::string &target_rid,
                                 const std::string &target_node_name,
                                 const framework::mesh_node_snapshot_t &snapshot)
    {
        const auto target_ready = std::any_of (
          snapshot.peers.begin (), snapshot.peers.end (), [&target_rid] (const auto &peer) {
              return peer.node_rid.to_string () == target_rid
                     && peer.state == framework::peer_state_t::ready;
          });
        if (!target_ready || state->reported.exchange (true, std::memory_order_acq_rel))
            return;
        std::cout << "shoppingmall-ready kind=object-route node=" << node_name
                  << " target=" << target_node_name << std::endl;
    }

    std::string _mesh_name;
    std::string _node_name;
    std::string _target_rid;
    std::string _target_node_name;
    std::shared_ptr<state_t> _state;
    std::unique_ptr<framework::mesh_runtime_observation_t> _observation;
    std::thread _worker;
};

} // namespace zlink::samples::shoppingmall
