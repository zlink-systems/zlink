/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace zlink::framework::detail
{

struct deadline_state_t
{
    std::atomic_bool cancelled{false};
};

class deadline_scheduler_t
{
  public:
    static deadline_scheduler_t &instance ()
    {
        static deadline_scheduler_t scheduler;
        return scheduler;
    }

    void schedule (std::chrono::steady_clock::time_point at,
                   std::shared_ptr<deadline_state_t> state,
                   std::function<void ()> callback)
    {
        {
            std::lock_guard lock (_mutex);
            _deadlines.push (
              deadline_t{at, _next_sequence++, std::move (state), std::move (callback)});
        }
        _changed.notify_one ();
    }

    void wake () noexcept { _changed.notify_one (); }

  private:
    struct deadline_t
    {
        std::chrono::steady_clock::time_point at;
        std::uint64_t sequence;
        std::shared_ptr<deadline_state_t> state;
        std::function<void ()> callback;
    };

    struct later_deadline_t
    {
        bool operator() (const deadline_t &left, const deadline_t &right) const noexcept
        {
            return left.at == right.at ? left.sequence > right.sequence : left.at > right.at;
        }
    };

    deadline_scheduler_t () : _worker ([this] (std::stop_token stop) { run (stop); }) {}

    ~deadline_scheduler_t ()
    {
        _worker.request_stop ();
        _changed.notify_all ();
    }

    void run (std::stop_token stop)
    {
        std::unique_lock lock (_mutex);
        while (!stop.stop_requested ()) {
            while (!_deadlines.empty ()
                   && _deadlines.top ().state->cancelled.load (std::memory_order_acquire)) {
                _deadlines.pop ();
            }
            if (_deadlines.empty ()) {
                _changed.wait (lock,
                               [&] { return stop.stop_requested () || !_deadlines.empty (); });
                continue;
            }
            const auto at = _deadlines.top ().at;
            if (_changed.wait_until (lock, at) == std::cv_status::no_timeout) {
                continue;
            }
            auto deadline = std::move (const_cast<deadline_t &> (_deadlines.top ()));
            _deadlines.pop ();
            if (deadline.state->cancelled.exchange (true, std::memory_order_acq_rel)) {
                continue;
            }
            lock.unlock ();
            deadline.callback ();
            lock.lock ();
        }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    std::priority_queue<deadline_t, std::vector<deadline_t>, later_deadline_t> _deadlines;
    std::uint64_t _next_sequence = 1;
    std::jthread _worker;
};


} // namespace zlink::framework::detail
