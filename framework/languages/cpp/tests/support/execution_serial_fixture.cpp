/* SPDX-License-Identifier: Elastic-2.0 */

#include "execution_serial_fixture.hpp"

namespace zlink::framework::test::execution
{

serial_test_blocker_t::~serial_test_blocker_t ()
{
    release ();
}

serial_test_blocker_t::queue_t::async_work_t
serial_test_blocker_t::work (std::function<void ()> entered)
{
    return [this, entered = std::move (entered)] (auto complete) mutable {
        std::optional<queue_t::async_completion_t> release_now;
        std::function<void ()> finish;
        {
            std::lock_guard lock (_mutex);
            _entered = true;
            if (_release_requested) {
                release_now.emplace (std::move (complete));
                finish = std::move (_finish);
            } else {
                _completion.emplace (std::move (complete));
            }
        }
        if (entered)
            entered ();
        _changed.notify_all ();
        if (release_now)
            (*release_now) (std::move (finish));
    };
}

bool serial_test_blocker_t::wait_for_entry (std::chrono::milliseconds timeout)
{
    std::unique_lock lock (_mutex);
    return _changed.wait_for (lock, timeout, [this] { return _entered; });
}

void serial_test_blocker_t::release (std::function<void ()> finish)
{
    std::optional<queue_t::async_completion_t> completion;
    {
        std::lock_guard lock (_mutex);
        if (_release_requested)
            return;
        _release_requested = true;
        _finish = std::move (finish);
        if (_completion) {
            completion.emplace (std::move (*_completion));
            _completion.reset ();
            finish = std::move (_finish);
        }
    }
    if (completion)
        (*completion) (std::move (finish));
}

serial_executor_test_fixture_t::serial_executor_test_fixture_t (
  zlink::framework::runtime::serial_lane_policy_t policy,
  zlink::framework::runtime::serial_execution_queue_options_t spot_options,
  std::size_t workers) :
    worker (std::make_shared<zlink::framework::runtime::offload_executor_t> (workers)),
    lane (*worker),
    spot_queue (
      std::make_shared<queue_t> (*worker, spot_options, queue_t::error_handler_t{}, policy)),
    serial (worker, lane, std::move (policy), spot_queue)
{
}

serial_executor_test_fixture_t::~serial_executor_test_fixture_t ()
{
    serial.close ();
    spot_queue->drain ();
}

void serial_executor_test_fixture_t::drain ()
{
    spot_queue->drain ();
    if (const auto actors = serial.actor_executor_snapshot ()) {
        for (const auto &[_, executor] : *actors)
            executor->queue ()->drain ();
    }
    for (const auto &queue : serial.timer_queues ()) {
        if (queue)
            queue->drain ();
    }
}

}
