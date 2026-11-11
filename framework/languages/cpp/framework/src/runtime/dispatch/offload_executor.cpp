/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "../utils/available_processor_count.hpp"

#include "runtime/dispatch/offload_executor.hpp"
#include "receive_role_measurement.hpp"

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

thread_local offload_executor_t *offload_executor_t::_publishing_executor = nullptr;

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
    _active_cancellations.reserve (_max_worker_count);
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
    {
        std::lock_guard lock (_mutex);
        // Already-admitted owner queues keep draining through scheduler
        // continuations. Teardown closes this path only after quiescence;
        // public work submission remains sealed as soon as stopping begins.
        if (_stopping && _active == _role_active && application_queue_empty_locked ()) {
            return false;
        }
        if (idle_worker_count_locked () <= _queue.size () && _live_workers < _max_worker_count) {
            start_worker_locked ();
        }
        _queue.push_back (work_item_t{std::move (work), std::nullopt});
    }
    if (!publishing_role ())
        dispatch_pending ();
    return true;
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
    {
        std::lock_guard lock (_mutex);
        if (_stopping) {
            return false;
        }
        if (idle_worker_count_locked () <= _queue.size () && _live_workers < _max_worker_count) {
            start_worker_locked ();
        }
        _queue.push_back (work_item_t{std::move (work), std::stop_source{}});
    }
    dispatch_pending ();
    return true;
}

bool offload_executor_t::start_role (std::shared_ptr<role_t> role)
{
    {
        std::lock_guard lock (_mutex);
        if (_stopping)
            return false;
        const auto application =
          std::find_if (_queue.begin (), _queue.end (), [] (const auto &item) {
              return !std::holds_alternative<std::shared_ptr<role_t>> (item.work);
          });
        _queue.insert (application, work_item_t{std::move (role), std::nullopt});
    }
    dispatch_pending ();
    return true;
}

void offload_executor_t::drain_application ()
{
    {
        std::lock_guard lock (_mutex);
        _stopping = true;
    }
    request_stop ();
    wake_workers ();
    std::unique_lock lock (_mutex);
    _empty.wait (lock,
                 [this] { return application_queue_empty_locked () && _active == _role_active; });
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
    {
        std::unique_lock lock (_mutex);
        _stopping = true;
    }
    request_stop ();
    wake_workers ();
    {
        std::unique_lock lock (_mutex);
        if (!_empty.wait_until (lock, deadline,
                                [this] { return _queue.empty () && _active == 0; })) {
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
    return _queue.empty () && _active == 0;
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
    auto *slot = worker.get ();
    worker->thread = std::thread ([this, slot] { worker_loop (*slot); });
    _workers.push_back (std::move (worker));
    ++_live_workers;
}

// A returning worker leaves the running range. It no longer needs the mutex,
// so joining it while holding the mutex waits only for its thread exit.
void offload_executor_t::retire_current_worker_locked ()
{
    const auto running = _workers.begin () + static_cast<std::ptrdiff_t> (_live_workers);
    const auto self = std::find_if (_workers.begin (), running, [] (const auto &worker) {
        return worker->thread.get_id () == std::this_thread::get_id ();
    });
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

bool offload_executor_t::application_queue_empty_locked () const noexcept
{
    // Initial roles form the queue prefix; the tail classifies the application
    // queue without retaining a second queue or count.
    return _queue.empty () || std::holds_alternative<std::shared_ptr<role_t>> (_queue.back ().work);
}

void offload_executor_t::complete_work_locked ()
{
    --_active;
    if (application_queue_empty_locked () && _active == _role_active)
        _empty.notify_all ();
}

bool offload_executor_t::publishing_role () const noexcept
{
    // The publication scope identifies the executor deferring its normal wakes.
    return _publishing_executor == this;
}

std::size_t offload_executor_t::idle_worker_count_locked () const noexcept
{
    std::size_t count = 0;
    for (auto *worker = _idle_head; worker; worker = worker->next_idle)
        ++count;
    return count;
}

void offload_executor_t::add_idle_locked (worker_t &worker) noexcept
{
    worker.next_idle = _idle_head;
    worker.previous_idle = nullptr;
    if (_idle_head)
        _idle_head->previous_idle = &worker;
    _idle_head = &worker;
}

void offload_executor_t::remove_idle_locked (worker_t &worker) noexcept
{
    if (worker.previous_idle)
        worker.previous_idle->next_idle = worker.next_idle;
    else
        _idle_head = worker.next_idle;
    if (worker.next_idle)
        worker.next_idle->previous_idle = worker.previous_idle;
    worker.previous_idle = worker.next_idle = nullptr;
}

std::shared_ptr<offload_executor_t::worker_t>
offload_executor_t::assign_worker_locked (worker_t *current)
{
    if (_queue.empty ())
        return nullptr;
    // An assigned current worker is the receiver returning from publication;
    // an unassigned current worker has just registered its idle membership.
    auto *receiver = current && current->assignment ? current : nullptr;
    if (receiver
        && (_stopping || std::holds_alternative<std::shared_ptr<role_t>> (_queue.front ().work)))
        return nullptr;
    // E counts rotations with an eligible application queue head after the
    // scheduler try-lock succeeds, before idle availability is considered.
    if (receiver && receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().eligible.fetch_add (1, std::memory_order_relaxed);
    auto *available = current && !receiver ? current : _idle_head;
    if (!available)
        return nullptr;
    auto assigned = available->shared_from_this ();
    remove_idle_locked (*available);
    auto *queue_worker = available;
    if (receiver) {
        available->assignment = std::move (receiver->assignment);
        receiver->assignment.reset ();
        queue_worker = receiver;
    }
    queue_worker->assignment.emplace (std::move (_queue.front ()));
    _queue.pop_front ();
    // Moving a role preserves its credit. Only the queue head acquires a credit.
    ++_active;
    if (std::holds_alternative<std::shared_ptr<role_t>> (queue_worker->assignment->work))
        ++_role_active;
    else if (queue_worker->assignment->cancellation)
        _active_cancellations.push_back (&*queue_worker->assignment->cancellation);
    if (receiver && receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().successes.fetch_add (1, std::memory_order_relaxed);
    return assigned;
}

void offload_executor_t::dispatch_pending ()
{
    std::unique_lock lock (_mutex);
    while (auto worker = assign_worker_locked (nullptr)) {
        lock.unlock ();
        worker->ready.notify_one ();
        lock.lock ();
    }
}

void offload_executor_t::wake_workers ()
{
    std::vector<std::shared_ptr<worker_t>> workers;
    {
        std::lock_guard lock (_mutex);
        workers = _workers;
    }
    for (const auto &worker : workers)
        worker->ready.notify_one ();
}

std::shared_ptr<offload_executor_t::worker_t> offload_executor_t::try_handoff (worker_t &worker)
{
    if (receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().attempts.fetch_add (1, std::memory_order_relaxed);
    std::unique_lock lock (_mutex, std::try_to_lock);
    if (!lock.owns_lock ())
        return nullptr;
    return assign_worker_locked (&worker);
}

void offload_executor_t::worker_loop (worker_t &worker)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    const auto previous_executor = std::exchange (_current_worker_executor, this);
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
        if (!worker.assignment) {
            std::unique_lock lock (_mutex);
            add_idle_locked (worker);
            const auto ready = [&] { return worker.assignment || !_queue.empty () || _stopping; };
            const bool woke = _idle_timeout.count () == 0
                                ? (worker.ready.wait (lock, ready), true)
                                : worker.ready.wait_for (lock, _idle_timeout, ready);
            if (!worker.assignment && !assign_worker_locked (&worker)) {
                remove_idle_locked (worker);
                if (_stopping || (!woke && _live_workers > _min_worker_count)) {
                    retire_current_worker_locked ();
                    trace_exit ();
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
                    _current_worker_executor = previous_executor;
#endif
                    return;
                }
                continue;
            }
        }
        auto &item = *worker.assignment;
        if (auto *role = std::get_if<std::shared_ptr<role_t>> (&item.work)) {
            // Neither receive nor publication exceptions enter the application
            // exception boundary. Both stacks return before a handoff.
            receive_role_measurement::begin_rotation ();
            if ((*role)->run ()) {
                receive_role_measurement::end_rotation ();
                bool published;
                {
                    struct publication_scope_t
                    {
                        offload_executor_t *previous;
                        ~publication_scope_t () { _publishing_executor = previous; }
                    } publication{std::exchange (_publishing_executor, this)};
                    published = (*role)->publish ();
                }
                auto successor = try_handoff (worker);
                receive_role_measurement::enter_application (static_cast<bool> (successor));
                if (successor)
                    successor->ready.notify_one ();
                if (published || successor)
                    dispatch_pending ();
                continue;
            }
            receive_role_measurement::receive_rotation.reset ();
        } else {
            run_application_turn ([&] {
                std::visit (
                  [&] (auto &callback) {
                      if constexpr (std::is_same_v<std::decay_t<decltype (callback)>,
                                                   std::shared_ptr<role_t>>) {
                          // Receive roles never enter this boundary.
                      } else if (callback) {
                          if constexpr (std::is_invocable_v<decltype (callback), std::stop_token>)
                              callback (item.cancellation ? item.cancellation->get_token ()
                                                          : std::stop_token{});
                          else
                              callback ();
                      }
                  },
                  item.work);
            });
        }
        receive_role_measurement::local_turn.reset ();
        const bool was_role = std::holds_alternative<std::shared_ptr<role_t>> (item.work);
        item.work = std::function<void ()>{};
        {
            std::lock_guard lock (_mutex);
            if (item.cancellation) {
                const auto active = std::find (_active_cancellations.begin (),
                                               _active_cancellations.end (), &*item.cancellation);
                if (active != _active_cancellations.end ())
                    _active_cancellations.erase (active);
            }
            worker.assignment.reset ();
            if (was_role)
                --_role_active;
            complete_work_locked ();
        }
    }
}

} // namespace zlink::framework::runtime
