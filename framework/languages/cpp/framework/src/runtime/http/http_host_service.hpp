/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/eventing/health.hpp>
#include <zlink/framework/contracts/http/http.hpp>

#include "runtime/host/hosted_service_lifecycle.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace zlink::framework::runtime
{

class http_host_service_t final : public hosted_service_t,
                                   public hosted_service_lifecycle_t
{
  public:
    http_host_service_t (http_options_snapshot_t options,
                         health_builder_t &health,
                         std::size_t handler_worker_count);
    ~http_host_service_t () override;

    void start (service_provider_t &services) override;
    void request_stop () noexcept override;
    void stop () noexcept override;
    int shutdown_stop_priority () const noexcept override { return 100; }

  private:
    class listener_t;

    http_options_snapshot_t _options;
    health_builder_t *_health;
    std::size_t _handler_worker_count;
    std::atomic_bool _stop{false};
    std::vector<std::unique_ptr<listener_t>> _listeners;
    std::vector<std::thread> _threads;
};

} // namespace zlink::framework::runtime
