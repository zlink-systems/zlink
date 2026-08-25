/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/location_runtime.hpp"

#include <zlink/framework/contracts/configuration/module.hpp>

#include <cstdint>
#include <utility>

namespace zlink::framework::runtime
{

class location_host_service_t final : public hosted_service_t
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

    void stop () noexcept override
    {
        if (_runtime != nullptr) {
            _runtime->stop ();
            _runtime = nullptr;
        }
    }

  private:
    zlink::routing_id_t _node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    location_runtime_t *_runtime = nullptr;
};

} // namespace zlink::framework::runtime
