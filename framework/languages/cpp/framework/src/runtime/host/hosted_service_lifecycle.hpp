/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/lifecycle.hpp>

#include "runtime/host/runtime_failure_collector.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace zlink::framework::detail
{
class mesh_node_runtime_t;
}

namespace zlink::framework::runtime
{

/*
 * Internal capability surface used by the process host to coordinate
 * topology-owned resources. The host owns the order; a topology implements
 * only the operation that closes its own resources.
 */
class hosted_service_lifecycle_t
{
  public:
    virtual ~hosted_service_lifecycle_t () = default;

    void bind_runtime_failures (std::shared_ptr<runtime_failure_collector_t> failures)
    {
        _runtime_failures = std::move (failures);
    }

  protected:
    std::shared_ptr<runtime_failure_collector_t> _runtime_failures =
      std::make_shared<runtime_failure_collector_t> ();

  public:
    virtual int shutdown_request_priority () const noexcept { return 0; }

    virtual int shutdown_stop_priority () const noexcept { return 0; }

    virtual void seal_application_dispatch () noexcept {}

    virtual bool wait_for_accepted_callbacks_until (std::chrono::steady_clock::time_point) noexcept
    {
        return true;
    }

    virtual bool publish_descriptor_state (framework_runtime_state_t) noexcept { return true; }

    virtual bool drain_sessions_until (std::chrono::steady_clock::time_point) noexcept
    {
        return true;
    }

    virtual void cancel_execution_waits () noexcept {}

    virtual void force_close_sessions () noexcept {}

    virtual bool participates_in_drain_propagation () const noexcept { return false; }

    virtual void visit_relocation_nodes (
      const std::function<void (const std::shared_ptr<detail::mesh_node_runtime_t> &)> &) const
    {
    }
};

} // namespace zlink::framework::runtime
