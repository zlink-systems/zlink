/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace zlink::framework::test::execution
{
class serial_test_signal_t
{
  public:
    void set ()
    {
        // Notify under the lock so a waiter cannot destroy this object mid-notify.
        std::lock_guard lock (_mutex);
        _set = true;
        _changed.notify_all ();
    }

    bool wait_for (std::chrono::milliseconds timeout = std::chrono::seconds (1))
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, timeout, [this] { return _set; });
    }

    bool is_set () const
    {
        std::lock_guard lock (_mutex);
        return _set;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    bool _set = false;
};
}
