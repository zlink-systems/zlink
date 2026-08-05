/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include "runtime/dispatch/inbound_dispatch_budget.hpp"
#include "runtime/dispatch/completion_admission_owner.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace zlink::framework::runtime
{

class channel_host_service_t final : public hosted_service_t
{
  public:
    channel_host_service_t (message_bus_t bus,
                            std::vector<channel_snapshot_t> channels,
                            handler_registry_t &handlers,
                            serializer_registry_t &serializers,
                            std::shared_ptr<inbound_dispatch_budget_t> inbound_budget = {},
                            std::shared_ptr<completion_admission_owner_t> completion_admission = {});
    ~channel_host_service_t () override;

    void start (service_provider_t &services) override;
    void request_stop () noexcept override;
    void stop () noexcept override;

  private:
    class server_loop_t;
    class subscriber_loop_t;

    message_bus_t _bus;
    std::vector<channel_snapshot_t> _channels;
    handler_registry_t *_handlers;
    serializer_registry_t *_serializers;
    std::shared_ptr<inbound_dispatch_budget_t> _inbound_budget;
    std::shared_ptr<completion_admission_owner_t> _completion_admission;
    service_provider_t *_services = nullptr;
    std::atomic_bool _stop{false};
    std::vector<std::unique_ptr<server_loop_t>> _loops;
    std::vector<std::unique_ptr<subscriber_loop_t>> _subscriber_loops;
    std::vector<std::thread> _threads;
};

} // namespace zlink::framework::runtime
