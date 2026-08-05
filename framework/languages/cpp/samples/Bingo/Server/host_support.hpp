/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include "../Shared/Contracts/messages.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace zlink::samples::bingo
{

using namespace framework;

class stop_after_start_service_t final : public hosted_service_t
{
  public:
    explicit stop_after_start_service_t (app_t &app) : _app (app) {}

    void start (service_provider_t &) override
    {
        started = true;
        _app.stop ();
    }

    void stop () noexcept override { stopped = true; }

    bool started = false;
    bool stopped = false;

  private:
    app_t &_app;
};

class route_mesh_readiness_service_t final : public hosted_service_t
{
  public:
    route_mesh_readiness_service_t (std::string node_name,
                                    std::string mesh_name,
                                    std::string label) :
        _node_name (std::move (node_name)),
        _mesh_name (std::move (mesh_name)),
        _label (std::move (label))
    {
    }

    void start (service_provider_t &services) override
    {
        auto state = std::make_shared<state_t> ();
        _state = state;
        auto &runtime = services.get_required<route_mesh_runtime_t> ();
        _observation = runtime.observe (
          _mesh_name, 64,
          [state, node_name = _node_name, label = _label] (
            const observed_status_t<mesh_node_snapshot_t> &observed) {
              const auto &snapshot = observed.status;
              if (!snapshot.is_ready
                  || state->reported.exchange (true, std::memory_order_acq_rel))
                  return;
              std::cout << "bingo route ready node=" << node_name
                        << " mesh=" << label << std::endl;
          });
    }

    void request_stop () noexcept override
    {
        if (_observation)
            _observation->close ();
    }

    void stop () noexcept override
    {
        request_stop ();
        _observation.reset ();
        _state.reset ();
    }

  private:
    struct state_t
    {
        std::atomic_bool reported{false};
    };

    std::string _node_name;
    std::string _mesh_name;
    std::string _label;
    std::shared_ptr<state_t> _state;
    std::unique_ptr<mesh_runtime_observation_t> _observation;
};

class play_api_channel_readiness_service_t final : public hosted_service_t
{
  public:
    explicit play_api_channel_readiness_service_t (std::string node_name) :
        _node_name (std::move (node_name))
    {
    }

    void start (service_provider_t &services) override
    {
        auto state = std::make_shared<state_t> ();
        _state = state;
        state->client = std::make_shared<channel_client_t> (
          services.get_required<channel_client_t> ());
        _worker = std::thread (
          [state, node_name = _node_name] () mutable {
              while (!state->stopping.load (std::memory_order_acquire)) {
                  struct attempt_t
                  {
                      std::condition_variable completed;
                      std::mutex mutex;
                      bool done = false;
                      bool accepted = false;
                  };
                  auto attempt = std::make_shared<attempt_t> ();
                  auto request = state->client
                                   ->request (
                                     sample_names_t::api_channel,
                                     authenticate_player_req_t{"player-readiness"})
                                   .timeout (std::chrono::milliseconds (500))
                                   .submit<authenticate_player_res_t> ();
                  observe_task_completion (
                    request,
                    [attempt] (
                      const result_t<authenticate_player_res_t> &result) {
                        {
                            std::lock_guard lock (attempt->mutex);
                            attempt->accepted =
                              result && result.value ().accepted;
                            attempt->done = true;
                        }
                        attempt->completed.notify_one ();
                    });
                  std::unique_lock lock (attempt->mutex);
                  attempt->completed.wait_for (
                    lock, std::chrono::milliseconds (500),
                    [&attempt] { return attempt->done; });
                  if (!attempt->accepted)
                      continue;
                  if (!state->reported.exchange (
                        true, std::memory_order_acq_rel)) {
                      std::cout << "bingo play api channel ready node="
                                << node_name << std::endl;
                  }
                  return;
              }
          });
    }

    void request_stop () noexcept override
    {
        if (_state)
            _state->stopping.store (true, std::memory_order_release);
    }

    void stop () noexcept override
    {
        request_stop ();
        if (_worker.joinable ())
            _worker.join ();
        _state.reset ();
    }

  private:
    struct state_t
    {
        std::atomic_bool stopping{false};
        std::atomic_bool reported{false};
        std::shared_ptr<channel_client_t> client;
    };

    std::string _node_name;
    std::shared_ptr<state_t> _state;
    std::thread _worker;
};

} // namespace zlink::samples::bingo
