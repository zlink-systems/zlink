/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace zlink::framework::test::host_lifecycle
{
class ready_host_probe_t final : public zlink::framework::hosted_service_t
{
  public:
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _observation = services.get_required<zlink::framework::framework_runtime_t> ().observe (
          1, [this] (const auto &event) {
              {
                  std::lock_guard lock (_mutex);
                  _state = event.status.state;
              }
              _changed.notify_all ();
          });
        co_return;
    }

    void stop () noexcept override
    {
        if (_observation)
            _observation->close ();
    }

    bool wait_ready ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] {
            return _state == zlink::framework::framework_runtime_state_t::serving
                   || _state == zlink::framework::framework_runtime_state_t::stopped
                   || _state == zlink::framework::framework_runtime_state_t::error;
        });
        return _state == zlink::framework::framework_runtime_state_t::serving;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    zlink::framework::framework_runtime_state_t _state =
      zlink::framework::framework_runtime_state_t::preparing;
    std::unique_ptr<zlink::framework::runtime_observation_t> _observation;
};
}
