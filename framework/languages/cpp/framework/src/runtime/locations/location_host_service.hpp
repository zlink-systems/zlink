/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/location_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"

#include <zlink/framework/contracts/configuration/module.hpp>

#include <cstdint>
#include <utility>

namespace zlink::framework::runtime
{

class location_host_service_t final : public hosted_service_t, public hosted_service_lifecycle_t
{
  public:
    explicit location_host_service_t (zlink::routing_id_t node_rid) :
        _node_rid (std::move (node_rid))
    {
    }

    task_t<void> start (service_provider_t &services) override
    {
        _runtime = &services.get_required<location_runtime_t> ();
        _runtime->start (_node_rid);
        return task_t<void> (result_t<void>::success ());
    }

    void set_teardown_end (std::chrono::steady_clock::time_point end) noexcept override
    {
        _cleanup_end = end;
    }

    void stop () noexcept override
    {
        if (_runtime != nullptr) {
            _runtime->stop (_cleanup_end);
            _runtime = nullptr;
        }
    }

  private:
    zlink::routing_id_t _node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    location_runtime_t *_runtime = nullptr;
    std::optional<std::chrono::steady_clock::time_point> _cleanup_end;
};

} // namespace zlink::framework::runtime
