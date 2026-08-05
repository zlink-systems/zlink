/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>
#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>

#include "runtime/dispatch/inbound_dispatch_budget.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace zlink::framework::detail
{
class monitoring_runtime_state_t;
class mesh_node_runtime_t;
} // namespace zlink::framework::detail

namespace zlink::framework::runtime
{

class stream_host_service_t final : public hosted_service_t,
                                     public hosted_service_lifecycle_t
{
  public:
    stream_host_service_t (
      detail::stream_runtime_t runtime,
      std::vector<stream_snapshot_t> streams,
      std::map<std::string, detail::stream_session_factory_t> session_factories,
      std::shared_ptr<detail::mesh_node_runtime_t> mesh_node = nullptr,
      std::shared_ptr<inbound_dispatch_budget_t> inbound_budget = nullptr);
    ~stream_host_service_t () override;

    void start (service_provider_t &services) override;
    void request_stop () noexcept override;
    void stop () noexcept override;
    int shutdown_request_priority () const noexcept override { return 100; }

    /* graceful-drain-handoff §5: a draining node closes brand-new STREAM
     * connections with session-closing(server_drain); existing sessions
     * continue until handoff/deadline. */
    void bind_drain_flag (std::shared_ptr<std::atomic_bool> flag) noexcept
    {
        _drain_flag = std::move (flag);
    }

    void bind_monitoring (
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring) noexcept
    {
        _monitoring = std::move (monitoring);
    }

    /* graceful-drain-handoff §7: sends session-closing(reason) to every
     * active session (bounded, best effort). The caller decides whether to
     * close connections afterwards; heartbeat/idle policies own their own
     * reasons. */
    void notify_sessions_closing (stream_close_reason_t reason,
                                  std::string_view diagnostic) noexcept;

    /* graceful-drain-handoff §7 forced teardown: notifies and closes every
     * active session so the client-visible reason stays the forced one. */
    void force_close_sessions (stream_close_reason_t reason,
                               std::string_view diagnostic) noexcept;

    int shutdown_stop_priority () const noexcept override { return 90; }
    bool drain_sessions_until (
      std::chrono::steady_clock::time_point deadline) noexcept override;
    void force_close_sessions () noexcept override
    {
        force_close_sessions (stream_close_reason_t::server_drain,
                              "drain force stop");
    }

  private:
    class listener_t;

    std::shared_ptr<std::atomic_bool> _drain_flag;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    detail::stream_runtime_t _runtime;
    std::vector<stream_snapshot_t> _streams;
    std::map<std::string, detail::stream_session_factory_t> _session_factories;
    std::shared_ptr<detail::mesh_node_runtime_t> _mesh_node;
    std::shared_ptr<inbound_dispatch_budget_t> _inbound_budget;
    service_provider_t *_services = nullptr;
    std::atomic_bool _stop{false};
    std::vector<std::unique_ptr<listener_t>> _listeners;
    std::vector<std::thread> _threads;
    std::thread _liveness_thread;
};

} // namespace zlink::framework::runtime
