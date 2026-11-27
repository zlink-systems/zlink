/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/eventing/health.hpp>
#include <zlink/framework/contracts/http/http.hpp>

#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace zlink::framework::runtime
{

class http_host_service_t final : public hosted_service_t, public hosted_service_lifecycle_t
{
  public:
    http_host_service_t (http_options_snapshot_t options,
                         health_builder_t &health,
                         std::size_t handler_worker_count,
                         std::shared_ptr<listener_status_registry_t> listener_statuses);
    ~http_host_service_t () override;

    task_t<void> start (service_provider_t &services) override;
    void request_stop () noexcept override;
    void stop () noexcept override;
    int shutdown_stop_priority () const noexcept override { return 100; }
    void seal_application_dispatch () noexcept override;
    bool wait_for_accepted_callbacks_until (
      std::chrono::steady_clock::time_point deadline) noexcept override;
    void cancel_execution_waits () noexcept override;

  private:
    class listener_t;
    using listeners_t = std::vector<std::shared_ptr<listener_t>>;

    // Failed startup returns to inactive. A host shutdown seal is permanent.
    enum class admission_t
    {
        inactive,
        accepting,
        sealed
    };

    template <typename Operation> auto with_listeners (Operation operation)
    {
        const std::lock_guard lock (_listeners_mutex);
        return operation (_listeners);
    }

    admission_t set_admission_locked (admission_t next) noexcept;
    listeners_t listener_snapshot ();
    listeners_t close_admission (admission_t next);
    listeners_t request_listener_stop (admission_t next);
    void stop_listeners (admission_t next) noexcept;

    http_options_snapshot_t _options;
    health_builder_t *_health;
    std::size_t _handler_worker_count;
    std::shared_ptr<listener_status_registry_t> _listener_statuses;
    std::atomic<admission_t> _admission{admission_t::inactive};
    std::mutex _listeners_mutex;
    listeners_t _listeners;
};

} // namespace zlink::framework::runtime
