/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "../utils/available_processor_count.hpp"

#include "runtime/dispatch/offload_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <mutex>

namespace zlink::framework::runtime
{

std::shared_ptr<offload_executor_t> infrastructure_executor ()
{
    static std::mutex mutex;
    static std::weak_ptr<offload_executor_t> shared;
    std::lock_guard lock (mutex);
    auto executor = shared.lock ();
    if (!executor) {
        auto workers = std::max (1u, available_processor_count ());
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
        if (const auto *probe = std::getenv ("ZLINK_CPP_TEST_SINGLE_INFRA_WORKER");
            probe && std::string_view (probe) == "1")
            workers = 1;
#endif
        executor = std::make_shared<offload_executor_t> (workers, "zlink-infra");
        shared = executor;
    }
    return executor;
}

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
thread_local const offload_executor_t *offload_executor_t::_current_worker_executor = nullptr;

const offload_executor_t *offload_executor_t::current_worker_executor () noexcept
{
    return _current_worker_executor;
}

#endif

offload_executor_t::offload_executor_t (std::size_t worker_count, std::string thread_name) :
    offload_executor_t (worker_count == 0 ? 1 : worker_count,
                        worker_count == 0 ? 1 : worker_count,
                        std::chrono::milliseconds{0},
                        std::move (thread_name))
{
}

offload_executor_t::offload_executor_t (std::size_t min_worker_count,
                                        std::size_t max_worker_count,
                                        std::chrono::milliseconds idle_timeout,
                                        std::string thread_name) :
    _min_worker_count (min_worker_count),
    _max_worker_count (std::max (min_worker_count, max_worker_count)),
    _idle_timeout (idle_timeout),
    _thread_name (std::move (thread_name))
{
    if (_max_worker_count == 0) {
        _max_worker_count = 1;
    }
    std::lock_guard lock (_mutex);
    _workers.reserve (_max_worker_count);
    for (std::size_t i = 0; i < _min_worker_count; ++i) {
        start_worker_locked ();
    }
}

offload_executor_t::~offload_executor_t ()
{
    drain ();
}

void offload_executor_t::submit (std::function<void ()> work)
{
    if (!try_submit (std::move (work))) {
        throw std::runtime_error ("offload executor is stopped");
    }
}

bool offload_executor_t::try_submit (std::function<void ()> work)
{
    return try_submit_application (std::move (work));
}

bool offload_executor_t::try_submit_internal (std::function<void ()> work)
{
    notifications_t notifications;
    {
        std::lock_guard lock (_mutex);
        // Continuations of admitted work may publish until application quiescence.
        if (_stopping && _active == _role_active && _queue.empty ())
            return false;
        _queue.push_back (work_item_t{std::move (work), std::nullopt});
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
        ++_submitted_items;
#endif
        notifications = reserve_workers_locked (true);
    }
    notify_workers (notifications);
    return true;
}

bool offload_executor_t::try_submit_drain (std::shared_ptr<drain_source_t> source)
{
    notifications_t notifications;
    {
        std::lock_guard lock (_mutex);
        if (_stopping && _active == _role_active && _queue.empty ())
            return false;
        const auto found = find_drain_locked (*source);
        if (found == _queue.end ()) {
            _queue.push_back (work_item_t{std::move (source), std::nullopt});
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
            ++_submitted_items;
#endif
        }
        notifications = reserve_workers_locked (true);
    }
    notify_workers (notifications);
    return true;
}

std::deque<offload_executor_t::work_item_t>::iterator
offload_executor_t::find_drain_locked (const drain_source_t &source)
{
    return std::find_if (_queue.begin (), _queue.end (), [&] (const auto &entry) {
        const auto existing = std::get_if<std::shared_ptr<drain_source_t>> (&entry.work);
        return existing && (*existing)->group == source.group && (*existing)->owner == source.owner;
    });
}

offload_executor_t::drain_source_t::record_t
offload_executor_t::take_drain_locked (std::deque<work_item_t>::iterator entry)
{
    drain_source_t::record_t record;
    try {
        record = std::get<std::shared_ptr<drain_source_t>> (entry->work)->take ();
    }
    catch (...) {
        _queue.erase (entry);
        throw;
    }
    if (!record.more)
        _queue.erase (entry);
    else
        std::rotate (entry, entry + 1, _queue.end ());
    return record;
}

offload_executor_t::drain_source_t::record_t
offload_executor_t::try_take_drain (const std::shared_ptr<drain_source_t> &source)
{
    // Retain captures outside the lock even if a later publication replaced this
    // running turn's exhausted source with another entry for the same owner.
    std::shared_ptr<drain_source_t> selected;
    drain_source_t::record_t record;
    {
        std::lock_guard lock (_mutex);
        const auto found = find_drain_locked (*source);
        if (found != _queue.end ()) {
            selected = std::get<std::shared_ptr<drain_source_t>> (found->work);
            record = take_drain_locked (found);
        }
    }
    return record;
}

// Only publication can grow the existing elastic pool. A take never wakes or
// starts another worker. The worker phase is the sole wake-reservation state.
offload_executor_t::notifications_t offload_executor_t::reserve_workers_locked (bool grow,
                                                                                worker_t *current)
{
    std::size_t demand = std::min (_max_worker_count, _roles.size ());
    for (const auto &entry : _queue) {
        if (demand == _max_worker_count)
            break;
        const auto source = std::get_if<std::shared_ptr<drain_source_t>> (&entry.work);
        // An exhausted entry still needs one selection to release its captures.
        const auto count = source ? std::max<std::size_t> (1, (*source)->pending ()) : 1;
        demand += std::min (count, _max_worker_count - demand);
    }
    std::size_t available = 0, reserved = 0;
    for (const auto &worker : _workers) {
        if (worker->phase == worker_phase_t::reserved) {
            ++available;
            ++reserved;
        } else if (worker->phase == worker_phase_t::idle)
            ++available;
    }
    if (grow && available < demand && _live_workers < _max_worker_count) {
        start_worker_locked ();
        ++reserved;
    }
    demand -= std::min (demand, reserved);
    // A worker about to wait first rechecks ready work for itself.
    if (current && demand != 0) {
        current->phase = worker_phase_t::reserved;
        --demand;
    }
    notifications_t notifications;
    for (const auto &worker : _workers) {
        if (demand == 0)
            break;
        if (worker->phase == worker_phase_t::idle) {
            notifications.push_back (worker);
            worker->phase = worker_phase_t::reserved;
            --demand;
        }
    }
    return notifications;
}

offload_executor_t::notifications_t offload_executor_t::stop_workers_locked ()
{
    notifications_t notifications;
    for (const auto &worker : _workers) {
        if (worker->phase == worker_phase_t::idle || worker->phase == worker_phase_t::reserved) {
            notifications.push_back (worker);
            worker->phase = worker_phase_t::reserved;
        }
    }
    return notifications;
}

void offload_executor_t::notify_workers (const notifications_t &notifications)
{
    for (const auto &worker : notifications)
        worker->ready.notify_one ();
}

offload_executor_t::retained_work_t::retained_work_t (offload_executor_t &executor) noexcept :
    _executor (&executor)
{
    std::lock_guard lock (executor._mutex);
    ++executor._active;
}

offload_executor_t::retained_work_t::retained_work_t (retained_work_t &&other) noexcept :
    _executor (std::exchange (other._executor, nullptr))
{
}

offload_executor_t::retained_work_t &
offload_executor_t::retained_work_t::operator= (retained_work_t &&other) noexcept
{
    if (this != &other) {
        reset ();
        _executor = std::exchange (other._executor, nullptr);
    }
    return *this;
}

offload_executor_t::retained_work_t::~retained_work_t () noexcept
{
    reset ();
}

void offload_executor_t::retained_work_t::reset () noexcept
{
    if (auto *executor = std::exchange (_executor, nullptr)) {
        std::lock_guard lock (executor->_mutex);
        executor->complete_work_locked ();
    }
}

offload_executor_t::retained_work_t offload_executor_t::retain_internal_work () noexcept
{
    return retained_work_t (*this);
}

bool offload_executor_t::try_submit_cancellable (std::function<void (std::stop_token)> work)
{
    return try_submit_application (std::move (work));
}

bool offload_executor_t::try_submit_application (work_t work)
{
    notifications_t notifications;
    {
        std::lock_guard lock (_mutex);
        if (_stopping)
            return false;
        _queue.push_back (work_item_t{std::move (work), std::stop_source{}});
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
        ++_submitted_items;
#endif
        notifications = reserve_workers_locked (true);
    }
    notify_workers (notifications);
    return true;
}

bool offload_executor_t::start_role (std::shared_ptr<role_t> role)
{
    notifications_t notifications;
    {
        std::lock_guard lock (_mutex);
        if (_stopping)
            return false;
        _roles.push (std::move (role));
        notifications = reserve_workers_locked (false);
    }
    notify_workers (notifications);
    return true;
}

void offload_executor_t::drain_application ()
{
    notifications_t notifications;
    {
        std::lock_guard lock (_mutex);
        _stopping = true;
        notifications = stop_workers_locked ();
    }
    request_stop ();
    notify_workers (notifications);
    std::unique_lock lock (_mutex);
    _empty.wait (lock, [this] { return _queue.empty () && _active == _role_active; });
}

void offload_executor_t::request_stop () noexcept
{
    std::vector<std::stop_source> cancellations;
    {
        std::lock_guard lock (_mutex);
        cancellations.reserve (_active_cancellations.size ());
        for (auto *cancellation : _active_cancellations) {
            if (cancellation != nullptr) {
                cancellations.emplace_back (*cancellation);
            }
        }
    }
    for (auto &cancellation : cancellations) {
        cancellation.request_stop ();
    }
}

void offload_executor_t::drain ()
{
    (void) drain_until (std::chrono::steady_clock::time_point::max ());
}

bool offload_executor_t::drain_until (std::chrono::steady_clock::time_point deadline)
{
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                               && std::string_view (trace_value) != "";
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=offload-drain-begin name=" << _thread_name
                  << " workers=" << _workers.size () << std::endl;
    }
    notifications_t notifications;
    {
        std::unique_lock lock (_mutex);
        _stopping = true;
        notifications = stop_workers_locked ();
    }
    request_stop ();
    notify_workers (notifications);
    {
        std::unique_lock lock (_mutex);
        if (!_empty.wait_until (lock, deadline, [this] {
                return _roles.empty () && _queue.empty () && _active == 0;
            })) {
            if (trace_enabled) {
                std::cerr << "zlink-cpp-host-stop stage=offload-drain-timeout name=" << _thread_name
                          << std::endl;
            }
            return false;
        }
        // Stopping with no work: every worker returns and none starts again.
        _empty.wait (lock, [this] { return _live_workers == 0; });
        join_retired_workers_locked ();
    }
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=offload-drain-end name=" << _thread_name
                  << std::endl;
    }
    return true;
}

bool offload_executor_t::drained () const
{
    std::lock_guard lock (_mutex);
    return _roles.empty () && _queue.empty () && _active == 0;
}

std::size_t offload_executor_t::live_worker_count () const
{
    std::lock_guard lock (_mutex);
    return _live_workers;
}

std::size_t offload_executor_t::owned_thread_count () const
{
    std::lock_guard lock (_mutex);
    return _workers.size ();
}

void offload_executor_t::start_worker_locked ()
{
    join_retired_workers_locked ();
    auto worker = std::make_shared<worker_t> ();
    worker->thread = std::thread ([this, worker] { worker_loop (worker); });
    _workers.push_back (std::move (worker));
    ++_live_workers;
}

// A returning worker leaves the running range. It no longer needs the mutex,
// so joining it while holding the mutex waits only for its thread exit.
void offload_executor_t::retire_current_worker_locked (worker_t &worker)
{
    const auto running = _workers.begin () + static_cast<std::ptrdiff_t> (_live_workers);
    const auto self = std::find_if (_workers.begin (), running,
                                    [&] (const auto &entry) { return entry.get () == &worker; });
    std::iter_swap (self, running - 1);
    --_live_workers;
    _empty.notify_all ();
}

void offload_executor_t::join_retired_workers_locked ()
{
    const auto retired = _workers.begin () + static_cast<std::ptrdiff_t> (_live_workers);
    for (auto worker = retired; worker != _workers.end (); ++worker)
        (*worker)->thread.join ();
    _workers.erase (retired, _workers.end ());
}

void offload_executor_t::complete_work_locked ()
{
    --_active;
    if (_queue.empty () && _active == _role_active)
        _empty.notify_all ();
}

void offload_executor_t::worker_loop (const std::shared_ptr<worker_t> &worker)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    struct current_worker_scope_t
    {
        const offload_executor_t *previous;
        ~current_worker_scope_t () { _current_worker_executor = previous; }
    } current_worker_scope{std::exchange (_current_worker_executor, this)};
#endif
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                               && std::string_view (trace_value) != "";
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=offload-worker-start name=" << _thread_name
                  << " executor=" << this << std::endl;
    }
    auto trace_exit = [this, trace_enabled] {
        if (trace_enabled) {
            std::cerr << "zlink-cpp-host-stop stage=offload-worker-exit name=" << _thread_name
                      << " executor=" << this << std::endl;
        }
    };
    while (true) {
        work_item_t work;
        drain_source_t::record_t record;
        std::exception_ptr preparation_failure;
        std::shared_ptr<role_t> role;
        notifications_t notifications;
        {
            std::unique_lock lock (_mutex);
            worker->phase = worker_phase_t::idle;
            notifications = reserve_workers_locked (false, worker.get ());
            const auto reserved = [&] {
                return _stopping || worker->phase == worker_phase_t::reserved;
            };
            bool ready = true;
            if (_idle_timeout.count () == 0)
                worker->ready.wait (lock, reserved);
            else
                ready = worker->ready.wait_for (lock, _idle_timeout, reserved);
            worker->phase = worker_phase_t::running;
            if (!ready && _roles.empty () && _queue.empty ()) {
                if (_live_workers > _min_worker_count) {
                    retire_current_worker_locked (*worker);
                    trace_exit ();
                    return;
                }
                continue;
            }
            if (_stopping && _roles.empty () && _queue.empty ()) {
                retire_current_worker_locked (*worker);
                trace_exit ();
                return;
            }
            // Another reserved worker can have consumed the last ready record.
            if (_roles.empty () && _queue.empty ())
                continue;
            if (!_roles.empty ()) {
                role = std::move (_roles.front ());
                _roles.pop ();
                ++_role_active;
            } else if (const auto source =
                         std::get_if<std::shared_ptr<drain_source_t>> (&_queue.front ().work)) {
                // This entry stays scheduled while an untaken record remains.
                work.work = *source;
                // Capture turn entry before W1 preparation. The owner drain
                // retains its existing budget and deadline decision.
                const auto turn_started = std::chrono::steady_clock::now ();
                try {
                    record = take_drain_locked (_queue.begin ());
                    record.turn_started = turn_started;
                }
                catch (...) {
                    // Preserve application failure isolation even when preparing
                    // a claim allocates. Report through its owner after unlock.
                    preparation_failure = std::current_exception ();
                }
            } else {
                work = std::move (_queue.front ());
                _queue.pop_front ();
                if (work.cancellation)
                    _active_cancellations.push_back (&*work.cancellation);
            }
            ++_active;
        }
        notify_workers (notifications);

        // A receive role is deliberately outside the application exception
        // boundary.
        if (role) {
            role->run ();
        } else {
            run_application_turn ([&] {
                std::visit (
                  [&] (auto &callback) {
                      if (callback) {
                          if constexpr (std::is_same_v<std::decay_t<decltype (callback)>,
                                                       std::shared_ptr<drain_source_t>>) {
                              if (preparation_failure)
                                  callback->failed (std::move (preparation_failure));
                              else if (record.value)
                                  callback->run (std::move (record));
                          } else if constexpr (std::is_invocable_v<decltype (callback),
                                                                   std::stop_token>)
                              callback (work.cancellation ? work.cancellation->get_token ()
                                                          : std::stop_token{});
                          else
                              callback ();
                      }
                  },
                  work.work);
            });
        }
        work.work = std::function<void ()>{};

        {
            std::lock_guard lock (_mutex);
            if (work.cancellation) {
                const auto active = std::find (_active_cancellations.begin (),
                                               _active_cancellations.end (), &*work.cancellation);
                if (active != _active_cancellations.end ()) {
                    _active_cancellations.erase (active);
                }
            }
            if (role)
                --_role_active;
            complete_work_locked ();
        }
    }
}

} // namespace zlink::framework::runtime
