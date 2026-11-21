/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace zlink::framework::test::host_lifecycle
{
class blocking_stop_service_t final : public zlink::framework::hosted_service_t
{
  public:
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        {
            std::lock_guard lock (_mutex);
            _started = true;
        }
        _changed.notify_all ();
        co_return;
    }

    void stop () noexcept override
    {
        std::unique_lock lock (_mutex);
        _stop_entered = true;
        _changed.notify_all ();
        _changed.wait (lock, [&] { return _release; });
    }

    void wait_started ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] { return _started; });
    }

    void wait_stop_entered ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] { return _stop_entered; });
    }

    void release ()
    {
        {
            std::lock_guard lock (_mutex);
            _release = true;
        }
        _changed.notify_all ();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _started = false;
    bool _stop_entered = false;
    bool _release = false;
};
}
