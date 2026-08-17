/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/channels/call.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::messaging
{
namespace
{

result_t<void> deadline_exceeded ()
{
    return result_t<void>::failure (
      framework_error_kind_t::deadline_exceeded,
      "logical multicast admission deadline was exceeded");
}

result_t<void> runtime_shutdown ()
{
    return result_t<void>::failure (
      framework_error_kind_t::shutting_down,
      "logical multicast runtime is stopped");
}

class logical_multicast_executor_t
{
  public:
    logical_multicast_executor_t ()
    {
        const auto count = std::max<std::size_t> (
          2, std::thread::hardware_concurrency ());
        _available = count;
        _workers.reserve (count);
        for (std::size_t index = 0; index < count; ++index)
            _workers.emplace_back ([this] { run (); });
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
        if (waiting)
            waiting->completion->complete (runtime_shutdown ());
        if (_handoff_worker.joinable ())
            _handoff_worker.join ();
        for (auto &worker : _workers) {
            if (worker.joinable ())
                worker.join ();
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
        return enqueue ({std::move (work), {}, {}, {}}, timeout);
    }

    task_t<void> submit (std::function<task_t<void> ()> work,
                         std::chrono::milliseconds timeout)
    {
        if (!work) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "logical multicast call is not bound to a publisher"));
        }
        return enqueue ({{}, std::move (work), {}, {}}, timeout);
    }

  private:
    struct multicast_job_t
    {
        std::function<result_t<void> ()> work;
        std::function<task_t<void> ()> async_work;
        std::shared_ptr<detail::task_completion_source_t<void>> completion;
        std::chrono::steady_clock::time_point deadline;
    };

    task_t<void> enqueue (multicast_job_t job,
                          std::chrono::milliseconds timeout)
    {
        job.completion =
          std::make_shared<detail::task_completion_source_t<void>> ();
        auto task = job.completion->task ();
        job.deadline = std::chrono::steady_clock::now () + timeout;
        bool overflow = false;
        bool stopping = false;
        std::optional<multicast_job_t> expired;
        {
            std::lock_guard lock (_mutex);
            if (_stopping) {
                stopping = true;
            }
            else if (_available != 0 && _handoff) {
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
        if (stopping) {
            job.completion->complete (runtime_shutdown ());
            return task;
        }
        if (overflow)
            job.completion->complete (deadline_exceeded ());
        if (expired)
            expired->completion->complete (deadline_exceeded ());
        _changed.notify_all ();
        return task;
    }

    void release_slot ()
    {
        {
            std::lock_guard lock (_mutex);
            ++_available;
        }
        _changed.notify_all ();
    }

    void run ()
    {
        for (;;) {
            multicast_job_t job;
            {
                std::unique_lock lock (_mutex);
                _changed.wait (lock, [&] { return _stopping || !_jobs.empty (); });
                if (_stopping && _jobs.empty ())
                    return;
                job = std::move (_jobs.front ());
                _jobs.pop_front ();
            }
            // Dequeue is the public terminal boundary. Target processing starts
            // after this handoff and cannot change the caller's completed result.
            job.completion->complete (result_t<void>::success ());
            try {
                if (job.async_work) {
                    auto observed = std::make_shared<task_t<void>> (job.async_work ());
                    detail::observe_task_completion (
                      *observed, [this, observed] (const result_t<void> &) {
                          release_slot ();
                      });
                    continue;
                }
                (void) job.work ();
            }
            catch (...) {
                // Application-bound publishers install their structured
                // post-admission observer before reaching this executor.
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
                _changed.wait (lock, [this] { return _stopping || _handoff.has_value (); });
                if (_stopping)
                    return;
                while (_handoff && !_stopping) {
                    if (_handoff->deadline <= std::chrono::steady_clock::now ()) {
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
                if (_stopping)
                    return;
            }
            if (expired)
                expired->completion->complete (deadline_exceeded ());
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

logical_multicast_executor_t &multicast_executor ()
{
    static logical_multicast_executor_t value;
    return value;
}

} // namespace

} // namespace zlink::framework::runtime::messaging

namespace zlink::framework::detail
{

task_t<void>
submit_logical_multicast_task (std::function<result_t<void> ()> submit,
                               std::chrono::milliseconds timeout)
{
    return runtime::messaging::multicast_executor ().submit (
      std::move (submit), timeout);
}

task_t<void>
submit_logical_multicast_async_task (std::function<task_t<void> ()> submit,
                                     std::chrono::milliseconds timeout)
{
    return runtime::messaging::multicast_executor ().submit (
      std::move (submit), timeout);
}

} // namespace zlink::framework::detail
