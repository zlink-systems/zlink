/* SPDX-License-Identifier: MPL-2.0 */

#include "async_operation_state.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::detail
{
namespace
{

class async_continuation_dispatcher_t
{
  public:
    async_continuation_dispatcher_t ()
    {
        try {
            // Completion delivery is separate from physical admission. Two
            // workers keep one blocking consumer continuation from owning the
            // only path that can complete an unrelated operation.
            _workers.reserve (2);
            for (std::size_t index = 0; index != 2; ++index)
                _workers.emplace_back ([this] { run (); });
        }
        catch (...) {
            {
                std::lock_guard<std::mutex> lock (_mutex);
                _stopping = true;
            }
            _changed.notify_all ();
            for (auto &worker : _workers) {
                if (worker.joinable ())
                    worker.join ();
            }
            throw;
        }
    }

    ~async_continuation_dispatcher_t ()
    {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _stopping = true;
        }
        _changed.notify_all ();
        for (auto &worker : _workers) {
            if (worker.joinable ())
                worker.join ();
        }
    }

    bool post (std::shared_ptr<std::function<void ()>> work_) noexcept
    {
        try {
            {
                std::lock_guard<std::mutex> lock (_mutex);
                if (_stopping)
                    return false;
                _pending.push_back (std::move (work_));
            }
            _changed.notify_one ();
            return true;
        }
        catch (...) {
            return false;
        }
    }

  private:
    void run () noexcept
    {
        for (;;) {
            std::shared_ptr<std::function<void ()>> work;
            {
                std::unique_lock<std::mutex> lock (_mutex);
                _changed.wait (lock, [this] {
                    return _stopping || !_pending.empty ();
                });
                if (_pending.empty ()) {
                    if (_stopping)
                        return;
                    continue;
                }
                work = std::move (_pending.front ());
                _pending.pop_front ();
            }
            try {
                if (work && *work)
                    (*work) ();
            }
            catch (...) {
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<std::function<void ()>>> _pending;
    std::vector<std::thread> _workers;
    bool _stopping = false;
};

async_continuation_dispatcher_t &async_continuation_dispatcher ()
{
    static async_continuation_dispatcher_t dispatcher;
    return dispatcher;
}

} // namespace

void ensure_async_continuation_dispatcher ()
{
    (void) async_continuation_dispatcher ();
}

void dispatch_async_continuation (std::function<void ()> work_) noexcept
{
    if (!work_)
        return;
    std::shared_ptr<std::function<void ()>> work;
    try {
        work = std::make_shared<std::function<void ()>> (
          std::move (work_));
        if (!async_continuation_dispatcher ().post (work))
            std::terminate ();
    }
    catch (...) {
        // Terminal state is already visible. Dropping this one-shot resume or
        // running it on the producer would violate the async contract, so an
        // executor allocation/lifecycle failure is not recoverable here.
        std::terminate ();
    }
}

} // namespace zlink::detail
