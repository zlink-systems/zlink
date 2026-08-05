/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/async_submit_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::messaging
{
namespace
{

zlink::framework::runtime::offload_executor_t &blocking_call_executor ()
{
    static zlink::framework::runtime::offload_executor_t executor (
      0,
      std::max<std::size_t> (1, std::thread::hardware_concurrency ()),
      1024,
      std::chrono::milliseconds (100),
      "zlink-call");
    return executor;
}

bool schedule_blocking_call (std::function<void ()> work) noexcept
{
    try {
        return blocking_call_executor ().try_submit (std::move (work));
    }
    catch (...) {
        return false;
    }
}

thread_local submit_attempt_context_t last_attempt_context;
thread_local bool has_attempt_context = false;

void clear_attempt_context () noexcept
{
    last_attempt_context = {};
    has_attempt_context = false;
}

submit_attempt_context_t take_attempt_context ()
{
    if (!has_attempt_context) {
        return {"*", nullptr, 0, std::chrono::seconds (1), 1024};
    }
    auto result = std::move (last_attempt_context);
    clear_attempt_context ();
    return result;
}

bool is_backpressured (const result_t<void> &result) noexcept
{
    return !result && result.error_kind () == framework_error_kind_t::capacity_exceeded;
}

result_t<void> one_way_terminal_result (const result_t<void> &result)
{
    if (result) {
        return result_t<void>::success ();
    }
    const auto *error = result.error ();
    if (error == nullptr) {
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure, "one-way submit failed");
    }
    switch (detail::boundary_state (*error)) {
        case detail::boundary_error_t::timed_out:
            return result_t<void>::failure (
              framework_error_kind_t::deadline_exceeded,
              error->what ());
        case detail::boundary_error_t::shutdown:
            return result_t<void>::failure (
              framework_error_kind_t::shutting_down,
              error->what ());
        case detail::boundary_error_t::disconnected:
            return result_t<void>::failure (
              framework_error_kind_t::unavailable,
              error->what ());
        case detail::boundary_error_t::none:
        case detail::boundary_error_t::closed:
        case detail::boundary_error_t::cancelled:
        case detail::boundary_error_t::stale_generation:
            return detail::result_access_t::failure<void> (*error);
    }
    return detail::result_access_t::failure<void> (*error);
}

result_t<void> deadline_exceeded ()
{
    return result_t<void>::failure (
      framework_error_kind_t::deadline_exceeded,
      "one-way admission deadline was exceeded");
}

result_t<void> runtime_shutdown ()
{
    return result_t<void>::failure (
      framework_error_kind_t::shutting_down,
      "one-way admission runtime is stopped");
}

result_t<void> invoke_attempt (const std::function<result_t<void> ()> &submit,
                               submit_attempt_context_t &context)
{
    clear_attempt_context ();
    try {
        auto result = submit ();
        context = take_attempt_context ();
        return result;
    }
    catch (...) {
        (void) take_attempt_context ();
        throw;
    }
}

struct pending_entry_t
{
    std::uint64_t id = 0;
    std::uint64_t owner_epoch = 0;
    submit_attempt_context_t context;
    std::chrono::steady_clock::time_point deadline;
    std::function<result_t<void> ()> submit;
    std::shared_ptr<detail::task_completion_source_t<void>> completion;
};

class async_submit_runtime_t
{
  public:
    async_submit_runtime_t () : _timer ([this] { run_timer (); }) {}

    ~async_submit_runtime_t ()
    {
        std::vector<pending_entry_t> pending;
        {
            std::lock_guard lock (_mutex);
            _stopping = true;
            pending.assign (std::make_move_iterator (_pending.begin ()),
                            std::make_move_iterator (_pending.end ()));
            _pending.clear ();
            pending.insert (pending.end (), std::make_move_iterator (_ready.begin ()),
                            std::make_move_iterator (_ready.end ()));
            _ready.clear ();
        }
        _changed.notify_all ();
        if (_timer.joinable ()) {
            _timer.join ();
        }
        for (auto &entry : pending) {
            entry.completion->complete (runtime_shutdown ());
        }
    }

    task_t<void> submit (std::function<result_t<void> ()> submit)
    {
        if (!submit) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "one-way call is not bound to a submit operation"));
        }

        submit_attempt_context_t context;
        result_t<void> first = result_t<void>::failure (
          framework_error_kind_t::internal_failure, "one-way submit failed");
        try {
            first = invoke_attempt (submit, context);
        }
        catch (const framework_exception_t &error) {
            return task_t<void> (
              detail::result_access_t::failure<void> (error));
        }
        catch (const std::exception &error) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::internal_failure, "one-way submit failed"));
        }

        record_attempt (context.target);
        if (!is_backpressured (first)) {
            return task_t<void> (one_way_terminal_result (first));
        }

        const auto timeout = context.timeout > std::chrono::milliseconds::zero ()
                               ? context.timeout
                               : std::chrono::seconds (1);
        auto completion =
          std::make_shared<detail::task_completion_source_t<void>> ();
        auto task = completion->task ();
        bool overflow = false;
        {
            std::lock_guard lock (_mutex);
            if (_stopping
                || (context.owner != nullptr
                    && (_shutdown_owners.contains (context.owner)
                        || _owner_epochs[context.owner] != context.owner_epoch))) {
                completion->complete (runtime_shutdown ());
                return task;
            }
            const auto owner_pending = _reservations.find (context.owner);
            const auto owner_pending_count = owner_pending == _reservations.end ()
                                               ? 0
                                               : owner_pending->second;
            if (context.capacity != 0
                && owner_pending_count < context.capacity) {
                ++_reservations[context.owner];
                _pending.push_back ({++_next_id, context.owner_epoch,
                                     std::move (context),
                                     std::chrono::steady_clock::now () + timeout,
                                     std::move (submit), completion});
            } else {
                overflow = true;
            }
        }
        if (overflow) {
            completion->complete (deadline_exceeded ());
        }
        _changed.notify_all ();
        return task;
    }

    void notify (const std::string &target, const void *owner)
    {
        pending_entry_t entry;
        {
            std::lock_guard lock (_mutex);
            const auto match = std::find_if (
              _pending.begin (), _pending.end (), [&] (const pending_entry_t &candidate) {
                  return candidate.context.owner == owner
                         && (candidate.context.target == target
                             || candidate.context.target == "*");
              });
            if (match == _pending.end ()) {
                const auto key = std::make_pair (owner, target);
                const auto retrying = _retrying.find (key);
                if (retrying != _retrying.end ()) {
                    ++_credits[retrying->second];
                }
                return;
            }
            entry = std::move (*match);
            _pending.erase (match);
            _ready.push_back (std::move (entry));
        }
        /* Socket callbacks forbid reentrant send. The runtime thread consumes
         * this single retry credit after the callback has returned. */
        _changed.notify_all ();
    }

    void notify (const void *owner)
    {
        pending_entry_t entry;
        {
            std::lock_guard lock (_mutex);
            const auto match = std::find_if (
              _pending.begin (), _pending.end (), [&] (const pending_entry_t &candidate) {
                  return candidate.context.owner == owner;
              });
            if (match == _pending.end ()) {
                const auto retrying = std::find_if (
                  _retrying.begin (), _retrying.end (), [&] (const auto &candidate) {
                      return candidate.first.first == owner;
                  });
                if (retrying != _retrying.end ()) {
                    ++_credits[retrying->second];
                }
                return;
            }
            entry = std::move (*match);
            _pending.erase (match);
            _ready.push_back (std::move (entry));
        }
        _changed.notify_all ();
    }

    void shutdown_owner (const void *owner) noexcept
    {
        if (owner == nullptr) {
            return;
        }
        std::vector<pending_entry_t> stopped;
        {
            std::lock_guard lock (_mutex);
            ++_owner_epochs[owner];
            _shutdown_owners.insert (owner);
            for (auto it = _pending.begin (); it != _pending.end ();) {
                if (it->context.owner == owner) {
                    release_reservation_locked (*it);
                    stopped.push_back (std::move (*it));
                    it = _pending.erase (it);
                } else {
                    ++it;
                }
            }
            for (auto it = _ready.begin (); it != _ready.end ();) {
                if (it->context.owner == owner) {
                    release_reservation_locked (*it);
                    stopped.push_back (std::move (*it));
                    it = _ready.erase (it);
                } else {
                    ++it;
                }
            }
        }
        for (auto &entry : stopped) {
            entry.completion->complete (runtime_shutdown ());
        }
        _changed.notify_all ();
    }

    void activate_owner (const void *owner)
    {
        if (owner == nullptr) {
            return;
        }
        std::lock_guard lock (_mutex);
        _shutdown_owners.erase (owner);
    }

    std::size_t pending_count () const
    {
        std::lock_guard lock (_mutex);
        std::size_t count = 0;
        for (const auto &[_, reservations] : _reservations) {
            count += reservations;
        }
        return count;
    }

    std::size_t attempt_count (const std::string &target) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _attempts.find (target);
        return found == _attempts.end () ? 0 : found->second;
    }

    std::uint64_t owner_epoch (const void *owner) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _owner_epochs.find (owner);
        return found == _owner_epochs.end () ? 0 : found->second;
    }

    void reset_for_tests ()
    {
        std::vector<pending_entry_t> stopped;
        {
            std::lock_guard lock (_mutex);
            stopped.assign (std::make_move_iterator (_pending.begin ()),
                            std::make_move_iterator (_pending.end ()));
            _pending.clear ();
            stopped.insert (stopped.end (), std::make_move_iterator (_ready.begin ()),
                            std::make_move_iterator (_ready.end ()));
            _ready.clear ();
            _attempts.clear ();
            _shutdown_owners.clear ();
            _credits.clear ();
            _retrying.clear ();
            _reservations.clear ();
            _owner_epochs.clear ();
        }
        for (auto &entry : stopped) {
            entry.completion->complete (runtime_shutdown ());
        }
        _changed.notify_all ();
    }

  private:
    void release_reservation_locked (const pending_entry_t &entry)
    {
        const auto found = _reservations.find (entry.context.owner);
        if (found == _reservations.end ()) {
            return;
        }
        if (found->second <= 1) {
            _reservations.erase (found);
        } else {
            --found->second;
        }
    }

    void finish_terminal_locked (const pending_entry_t &entry)
    {
        const auto key = std::make_pair (entry.context.owner, entry.context.target);
        const auto retrying = _retrying.find (key);
        if (retrying != _retrying.end () && retrying->second == entry.id) {
            _retrying.erase (retrying);
        }
        _credits.erase (entry.id);
        release_reservation_locked (entry);
    }

    void record_attempt (const std::string &target)
    {
        std::lock_guard lock (_mutex);
        ++_attempts[target];
    }

    void retry_once (pending_entry_t entry)
    {
        const auto retry_key =
          std::make_pair (entry.context.owner, entry.context.target);
        const auto finish_retry = [this, &entry] {
            std::lock_guard lock (_mutex);
            finish_terminal_locked (entry);
        };
        if (std::chrono::steady_clock::now () >= entry.deadline) {
            finish_retry ();
            entry.completion->complete (deadline_exceeded ());
            return;
        }

        submit_attempt_context_t retry_context;
        result_t<void> result = result_t<void>::failure (
          framework_error_kind_t::internal_failure, "one-way submit failed");
        try {
            result = invoke_attempt (entry.submit, retry_context);
        }
        catch (const framework_exception_t &error) {
            finish_retry ();
            entry.completion->complete (
              detail::result_access_t::failure<void> (error));
            return;
        }
        catch (const std::exception &error) {
            finish_retry ();
            entry.completion->complete (result_t<void>::failure (
              framework_error_kind_t::internal_failure, error.what ()));
            return;
        }
        catch (...) {
            finish_retry ();
            entry.completion->complete (result_t<void>::failure (
              framework_error_kind_t::internal_failure, "one-way submit failed"));
            return;
        }
        record_attempt (entry.context.target);
        bool stopped_after_attempt = false;
        {
            std::lock_guard lock (_mutex);
            stopped_after_attempt = _stopping
                                    || (entry.context.owner != nullptr
                                        && (_shutdown_owners.contains (entry.context.owner)
                                            || _owner_epochs[entry.context.owner]
                                                 != entry.owner_epoch));
        }
        if (stopped_after_attempt) {
            finish_retry ();
            entry.completion->complete (runtime_shutdown ());
            return;
        }
        if (!is_backpressured (result)) {
            finish_retry ();
            entry.completion->complete (one_way_terminal_result (result));
            return;
        }

        /* The logical destination is fixed by the first admission attempt.
         * A retry may observe a new physical route, but it cannot change the
         * operation identity or extend its deadline. */
        bool stopped = false;
        {
            std::lock_guard lock (_mutex);
            const auto retrying = _retrying.find (retry_key);
            if (retrying != _retrying.end () && retrying->second == entry.id) {
                _retrying.erase (retrying);
            }
            stopped = _stopping
                      || (entry.context.owner != nullptr
                          && (_shutdown_owners.contains (entry.context.owner)
                              || _owner_epochs[entry.context.owner]
                                   != entry.owner_epoch));
            if (!stopped) {
                const auto credit = _credits.find (entry.id);
                if (credit != _credits.end () && credit->second > 0) {
                    if (--credit->second == 0) {
                        _credits.erase (credit);
                    }
                    _ready.push_back (std::move (entry));
                } else {
                    _pending.push_back (std::move (entry));
                }
            } else {
                _credits.erase (entry.id);
                release_reservation_locked (entry);
            }
        }
        if (stopped) {
            entry.completion->complete (runtime_shutdown ());
        }
        _changed.notify_all ();
    }

    void run_timer ()
    {
        for (;;) {
            std::vector<pending_entry_t> expired;
            std::optional<pending_entry_t> ready;
            {
                std::unique_lock lock (_mutex);
                if (_stopping) {
                    return;
                }
                if (!_ready.empty ()) {
                    ready.emplace (std::move (_ready.front ()));
                    _ready.pop_front ();
                } else if (_pending.empty ()) {
                    _changed.wait (lock, [&] {
                        return _stopping || !_pending.empty () || !_ready.empty ();
                    });
                } else {
                    auto next_deadline = std::chrono::steady_clock::time_point::max ();
                    for (const auto &entry : _pending) {
                        next_deadline = std::min (next_deadline, entry.deadline);
                    }
                    _changed.wait_until (lock, next_deadline);
                }
                if (_stopping) {
                    return;
                }
                if (!ready && !_ready.empty ()) {
                    ready.emplace (std::move (_ready.front ()));
                    _ready.pop_front ();
                }
                if (ready) {
                    _retrying[std::make_pair (ready->context.owner,
                                              ready->context.target)] = ready->id;
                }
                const auto now = std::chrono::steady_clock::now ();
                for (auto it = _pending.begin (); it != _pending.end ();) {
                    if (it->deadline <= now) {
                        release_reservation_locked (*it);
                        expired.push_back (std::move (*it));
                        it = _pending.erase (it);
                    } else {
                        ++it;
                    }
                }
            }
            if (ready) {
                retry_once (std::move (*ready));
            }
            for (auto &entry : expired) {
                entry.completion->complete (deadline_exceeded ());
            }
        }
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<pending_entry_t> _pending;
    std::deque<pending_entry_t> _ready;
    std::map<std::string, std::size_t> _attempts;
    std::map<std::uint64_t, std::size_t> _credits;
    std::map<std::pair<const void *, std::string>, std::uint64_t> _retrying;
    std::map<const void *, std::size_t> _reservations;
    std::map<const void *, std::uint64_t> _owner_epochs;
    std::set<const void *> _shutdown_owners;
    std::uint64_t _next_id = 0;
    bool _stopping = false;
    std::thread _timer;
};

class logical_multicast_executor_t
{
  public:
    logical_multicast_executor_t ()
    {
        const auto count = std::max<std::size_t> (
          2, std::thread::hardware_concurrency ());
        _available = count;
        _workers.reserve (count);
        for (std::size_t index = 0; index < count; ++index) {
            _workers.emplace_back ([this] { run (); });
        }
        _handoff_worker = std::thread ([this] { run_handoff (); });
    }

    ~logical_multicast_executor_t ()
    {
        std::optional<multicast_job_t> waiting;
        {
            std::lock_guard lock (_mutex);
            _stopping = true;
            waiting = std::move (_handoff);
            _handoff.reset ();
        }
        _changed.notify_all ();
        if (waiting) {
            waiting->completion->complete (runtime_shutdown ());
        }
        if (_handoff_worker.joinable ()) {
            _handoff_worker.join ();
        }
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    }

    task_t<void> submit (std::function<result_t<void> ()> work,
                         std::chrono::milliseconds timeout)
    {
        if (!work) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "logical multicast call is not bound to a publisher"));
        }
        auto completion =
          std::make_shared<detail::task_completion_source_t<void>> ();
        auto task = completion->task ();
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        bool overflow = false;
        std::optional<multicast_job_t> expired;
        {
            std::lock_guard lock (_mutex);
            if (_stopping) {
                completion->complete (runtime_shutdown ());
                return task;
            }
            multicast_job_t job{
              std::move (work), completion, deadline};
            if (_available != 0 && _handoff) {
                if (_handoff->deadline <= std::chrono::steady_clock::now ()) {
                    expired = std::move (_handoff);
                    _handoff.reset ();
                    --_available;
                    _jobs.push_back (std::move (job));
                } else {
                    --_available;
                    _jobs.push_back (std::move (*_handoff));
                    _handoff = std::move (job);
                }
            } else if (_available != 0) {
                --_available;
                _jobs.push_back (std::move (job));
            } else if (!_handoff) {
                _handoff = std::move (job);
            } else {
                overflow = true;
            }
        }
        if (overflow) {
            completion->complete (deadline_exceeded ());
        }
        if (expired) {
            expired->completion->complete (deadline_exceeded ());
        }
        _changed.notify_all ();
        return task;
    }

    std::size_t worker_count () const noexcept { return _workers.size (); }

    /* Idle means every worker slot has been returned, so no job body from an
     * earlier publish is still running. */
    void wait_until_idle ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [this] {
            return _stopping
                   || (_available == _workers.size () && _jobs.empty ()
                       && !_handoff);
        });
    }

  private:
    struct multicast_job_t
    {
        std::function<result_t<void> ()> work;
        std::shared_ptr<detail::task_completion_source_t<void>> completion;
        std::chrono::steady_clock::time_point deadline;
    };

    void release_slot ()
    {
        std::lock_guard lock (_mutex);
        ++_available;
        _changed.notify_all ();
    }

    void run ()
    {
        for (;;) {
            multicast_job_t job;
            {
                std::unique_lock lock (_mutex);
                _changed.wait (lock, [&] { return _stopping || !_jobs.empty (); });
                if (_stopping && _jobs.empty ()) {
                    return;
                }
                job = std::move (_jobs.front ());
                _jobs.pop_front ();
            }
            // Dequeue is the public terminal boundary. Target processing starts
            // after this handoff and cannot change the caller's completed result.
            job.completion->complete (result_t<void>::success ());
            try {
                (void) job.work ();
            }
            catch (const framework_exception_t &) {
            }
            catch (const std::exception &) {
            }
            catch (...) {
            }
            release_slot ();
        }
    }

    void run_handoff ()
    {
        for (;;) {
            std::optional<multicast_job_t> expired;
            {
                std::unique_lock lock (_mutex);
                _changed.wait (lock, [this] {
                    return _stopping || _handoff.has_value ();
                });
                if (_stopping) {
                    return;
                }
                while (_handoff && !_stopping) {
                    if (_handoff->deadline
                        <= std::chrono::steady_clock::now ()) {
                        expired = std::move (_handoff);
                        _handoff.reset ();
                        _changed.notify_all ();
                        break;
                    }
                    if (_available != 0) {
                        --_available;
                        _jobs.push_back (std::move (*_handoff));
                        _handoff.reset ();
                        _changed.notify_all ();
                        break;
                    }
                    const auto deadline = _handoff->deadline;
                    if (!_changed.wait_until (lock, deadline, [this] {
                            return _stopping || _available != 0;
                        })) {
                        expired = std::move (_handoff);
                        _handoff.reset ();
                        _changed.notify_all ();
                        break;
                    }
                }
                if (_stopping) {
                    return;
                }
            }
            if (expired) {
                expired->completion->complete (deadline_exceeded ());
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<multicast_job_t> _jobs;
    std::optional<multicast_job_t> _handoff;
    std::vector<std::thread> _workers;
    std::thread _handoff_worker;
    std::size_t _available = 0;
    bool _stopping = false;
};

async_submit_runtime_t &runtime ()
{
    static async_submit_runtime_t value;
    return value;
}

logical_multicast_executor_t &multicast_executor ()
{
    static logical_multicast_executor_t value;
    return value;
}

} // namespace

void note_submit_attempt (std::string target,
                          const void *owner,
                          std::chrono::milliseconds timeout,
                          std::size_t capacity)
{
    last_attempt_context = {
      std::move (target), owner, runtime ().owner_epoch (owner), timeout, capacity};
    has_attempt_context = true;
}

void notify_submit_ready (const std::string &target, const void *owner)
{
    runtime ().notify (target, owner);
}

void notify_submit_ready (const void *owner)
{
    runtime ().notify (owner);
}

void shutdown_submit_owner (const void *owner) noexcept
{
    runtime ().shutdown_owner (owner);
}

void activate_submit_owner (const void *owner)
{
    runtime ().activate_owner (owner);
}

std::size_t pending_submit_count_for_tests ()
{
    return runtime ().pending_count ();
}

std::size_t submit_attempt_count_for_tests (const std::string &target)
{
    return runtime ().attempt_count (target);
}

void reset_async_submit_runtime_for_tests ()
{
    runtime ().reset_for_tests ();
}

std::size_t multicast_worker_count_for_tests ()
{
    return multicast_executor ().worker_count ();
}

void wait_for_idle_multicast_executor_for_tests ()
{
    multicast_executor ().wait_until_idle ();
}

} // namespace zlink::framework::runtime::messaging

namespace zlink::framework::detail
{

bool submit_blocking_call (std::function<void ()> work)
{
    return runtime::messaging::schedule_blocking_call (std::move (work));
}

task_t<void>
submit_one_way_task (std::function<result_t<void> ()> submit)
{
    return runtime::messaging::runtime ().submit (std::move (submit));
}

task_t<void>
submit_logical_multicast_task (std::function<result_t<void> ()> submit,
                               std::chrono::milliseconds timeout)
{
    return runtime::messaging::multicast_executor ().submit (
      std::move (submit), timeout);
}

} // namespace zlink::framework::detail
