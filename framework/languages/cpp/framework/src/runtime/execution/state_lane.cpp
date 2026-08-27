/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/execution/state_lane.hpp"

#include <string>

namespace zlink::framework::runtime
{

thread_local state_lane_t *state_lane_t::_current_lane = nullptr;

state_lane_t::state_lane_t (offload_executor_t &executor) noexcept :
    _executor (executor)
{
}

state_lane_t::~state_lane_t ()
{
    try {
        close ();
    }
    catch (...) {
        // Destruction cannot report an executor shutdown race.  Any run()
        // caller has already received the terminal exception in that case.
    }
}

bool state_lane_t::try_post (std::function<void ()> work)
{
    if (!work) {
        throw std::invalid_argument ("state lane work is empty");
    }
    throw_if_reentrant ();
    if (_closed.load (std::memory_order_acquire)) {
        return false;
    }
    return enqueue (std::move (work), [] (std::exception_ptr) {});
}

void state_lane_t::throw_if_reentrant () const
{
    if (is_on_lane ()) {
        throw std::logic_error (
          "code already runs on the state lane it is trying to enter; call the "
          "component's private state method directly instead of re-entering its public surface");
    }
}

bool state_lane_t::is_on_lane () const noexcept
{
    return _current_lane == this;
}

state_lane_t *state_lane_t::current () noexcept
{
    return _current_lane;
}

void state_lane_t::close ()
{
    throw_if_reentrant ();
    _closed.store (true, std::memory_order_release);
    schedule_drain ();

    std::unique_lock lock (_mailbox_mutex);
    _drained.wait (lock, [this] {
        return _mailbox.empty ()
               && !_scheduled.load (std::memory_order_acquire);
    });
}

bool state_lane_t::enqueue (
  std::function<void ()> work,
  std::function<void (std::exception_ptr)> abandon)
{
    {
        std::lock_guard lock (_mailbox_mutex);
        if (_closed.load (std::memory_order_acquire)) {
            return false;
        }
        _mailbox.push_back ({std::move (work), std::move (abandon)});
    }
    schedule_drain ();
    return true;
}

void state_lane_t::schedule_drain ()
{
    bool expected = false;
    if (!_scheduled.compare_exchange_strong (
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (_executor.try_submit_internal ([this] { drain_loop (); })) {
        return;
    }

    _scheduled.store (false, std::memory_order_release);
    _closed.store (true, std::memory_order_release);
    abandon_pending (std::make_exception_ptr (
      std::runtime_error ("state lane executor is stopping")));
}

void state_lane_t::drain_loop ()
{
    auto *previous_lane = _current_lane;
    _current_lane = this;
    std::size_t processed = 0;
    try {
        while (processed < drain_batch_limit) {
            mailbox_item_t item;
            {
                std::lock_guard lock (_mailbox_mutex);
                if (_mailbox.empty ()) {
                    break;
                }
                item = std::move (_mailbox.front ());
                _mailbox.pop_front ();
            }
            try {
                item.work ();
            }
            catch (...) {
                // run() completes its own promise.  A try_post callback owns
                // its own failures, so it must not tear down this lane.
            }
            ++processed;
        }
    }
    catch (...) {
        // The loop itself has no caller.  Preserve liveness for the mailbox.
    }
    _current_lane = previous_lane;

    _scheduled.store (false, std::memory_order_release);
    bool has_more = false;
    {
        std::lock_guard lock (_mailbox_mutex);
        has_more = !_mailbox.empty ();
        if (!has_more && _closed.load (std::memory_order_acquire)) {
            _drained.notify_all ();
        }
    }
    if (has_more) {
        schedule_drain ();
    }
}

void state_lane_t::abandon_pending (std::exception_ptr error) noexcept
{
    std::deque<mailbox_item_t> abandoned;
    {
        std::lock_guard lock (_mailbox_mutex);
        abandoned = std::move (_mailbox);
        _drained.notify_all ();
    }
    for (auto &item : abandoned) {
        try {
            item.abandon (error);
        }
        catch (...) {
        }
    }
}

} // namespace zlink::framework::runtime
