/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/execution/state_lane.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace zlink::framework::runtime
{

namespace
{

std::mutex &executor_mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<coroutine_executor_t> &executor_instance ()
{
    static std::unique_ptr<coroutine_executor_t> executor;
    return executor;
}

std::atomic<coroutine_executor_t *> &executor_fast_path ()
{
    static std::atomic<coroutine_executor_t *> executor{nullptr};
    return executor;
}

std::size_t &configured_worker_count ()
{
    static std::size_t worker_count = 0;
    return worker_count;
}

bool &executor_shutdown_requested ()
{
    static bool requested = false;
    return requested;
}

std::size_t &executor_owner_count ()
{
    static std::size_t count = 0;
    return count;
}

std::size_t default_worker_count ()
{
    return std::max (1u, std::thread::hardware_concurrency ());
}

// Caller holds executor_mutex; capture and late-bound delivery share creation.
coroutine_executor_t &ensure_executor_locked ()
{
    auto &executor = executor_instance ();
    if (!executor) {
        const auto workers =
          configured_worker_count () == 0 ? default_worker_count () : configured_worker_count ();
        executor = std::make_unique<coroutine_executor_t> (workers);
        executor_fast_path ().store (executor.get (), std::memory_order_release);
    }
    return *executor;
}

} // namespace

coroutine_executor_t::coroutine_executor_t (std::size_t worker_count) :
    _pool (worker_count == 0 ? 1 : worker_count)
{
}

coroutine_executor_t::~coroutine_executor_t ()
{
    drain ();
}

void coroutine_executor_t::drain ()
{
    {
        std::lock_guard lock (_mutex);
        if (_drained) {
            return;
        }
        _drained = true;
    }
    _pool.join ();
}

void coroutine_executor_t::post_native_continuation (std::function<void ()> work)
{
    std::lock_guard lock (_mutex);
    if (_drained) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "handler coroutine executor is drained");
    }
    boost::asio::post (_pool, [work = std::move (work)] () noexcept {
        if (work)
            work ();
    });
}

bool coroutine_executor_t::is_current () noexcept
{
    return _pool.get_executor ().running_in_this_thread ();
}

coroutine_executor_t &handler_coroutine_executor ()
{
    auto *ready = executor_fast_path ().load (std::memory_order_acquire);
    if (ready != nullptr) {
        return *ready;
    }
    std::lock_guard lock (executor_mutex ());
    if (!executor_instance () && executor_shutdown_requested ()) {
        throw std::runtime_error ("handler coroutine executor is shut down");
    }
    return ensure_executor_locked ();
}

void configure_handler_coroutine_executor (std::size_t worker_count,
                                           detail::completion_dispatcher_borrow_t dispatcher)
{
    if (!dispatcher)
        dispatcher = detail::borrow_completion_dispatcher ();
    if (worker_count == 0) {
        worker_count = default_worker_count ();
    }
    std::unique_ptr<coroutine_executor_t> stopped_executor;
    std::lock_guard lock (executor_mutex ());
    ++executor_owner_count ();
    auto &executor = executor_instance ();
    if (executor_shutdown_requested () && executor) {
        executor_fast_path ().store (nullptr, std::memory_order_release);
        stopped_executor = std::move (executor);
    }
    if (executor) {
        executor->bind_completion_dispatcher (std::move (dispatcher));
        return;
    }
    executor_shutdown_requested () = false;
    configured_worker_count () = worker_count;
    auto &configured = ensure_executor_locked ();
    configured.bind_completion_dispatcher (std::move (dispatcher));
}

void shutdown_handler_coroutine_executor () noexcept
{
    std::unique_ptr<coroutine_executor_t> executor;
    {
        std::lock_guard lock (executor_mutex ());
        auto &owners = executor_owner_count ();
        if (owners > 0) {
            --owners;
        }
        if (owners != 0) {
            return;
        }
        executor_shutdown_requested () = true;
        executor_fast_path ().store (nullptr, std::memory_order_release);
        executor = std::move (executor_instance ());
    }
    if (executor) {
        executor->drain ();
    }
}

} // namespace zlink::framework::runtime

namespace zlink::framework::detail
{

task_scheduler_t
capture_host_continuation_scheduler (bool scope_exit_only,
                                     completion_dispatcher_borrow_t *dispatcher = nullptr)
{
    // Execution gate §13: a Store chain awaited by an infrastructure wait
    // resumes on the thread that completes each Store step, not behind
    // application handlers.
    {
        std::lock_guard lock (runtime::executor_mutex ());
        if (runtime::executor_shutdown_requested () || runtime::executor_owner_count () == 0) {
            return {};
        }
        auto &configured = runtime::ensure_executor_locked ();
        if (dispatcher != nullptr)
            *dispatcher = configured.completion_dispatcher ();
    }
    if (current_infrastructure_wait ())
        return {};
    const auto *caller_job = application_job_context_t::current ();
    // Classify the registered caller for the provider's reuse decision; this
    // snapshot grants no execution authority on the completing thread.
    const bool caller_owned_turn = caller_job != nullptr || current_held_serial_turn () != nullptr;
    return [caller_job, scope_exit_only, caller_owned_turn] (std::function<void ()> &&work) {
        {
            std::lock_guard lock (runtime::executor_mutex ());
            if (runtime::executor_shutdown_requested () || runtime::executor_owner_count () == 0) {
                throw framework_exception_t (
                  framework_error_kind_t::shutting_down,
                  "handler coroutine executor is not accepting continuations");
            }
            auto &configured = runtime::executor_instance ();
            const bool completion_turn = can_reuse_completion_turn (
              configured && configured->is_current (), caller_job, caller_owned_turn);
            if (runtime::state_lane_t::current () != nullptr
                || (!scope_exit_only && !completion_turn)) {
                runtime::ensure_executor_locked ().post_native_continuation (std::move (work));
                return;
            }
        }
        if (work)
            work ();
    };
}

void check_blocking_submit_context ();

namespace
{
task_scheduler_t capture_native_scheduler (completion_dispatcher_borrow_t *dispatcher)
{
    return capture_host_continuation_scheduler (false, dispatcher);
}

task_scheduler_t capture_scope_exit_scheduler ()
{
    return capture_host_continuation_scheduler (true);
}

completion_dispatcher_borrow_t borrow_host_dispatcher ()
{
    const std::lock_guard lock (runtime::executor_mutex ());
    const auto &executor = runtime::executor_instance ();
    return executor ? executor->completion_dispatcher () : nullptr;
}

constexpr runtime_execution_hooks_t host_execution_hooks{
  &borrow_host_dispatcher, &capture_native_scheduler, &capture_scope_exit_scheduler,
  &check_blocking_submit_context};
} // namespace

} // namespace zlink::framework::detail

namespace zlink::framework::runtime
{

void install_host_context_hooks () noexcept
{
    detail::set_runtime_execution_hooks (&detail::host_execution_hooks);
    detail::set_ambient_context_hooks (&ambient_flow_hooks ());
}

} // namespace zlink::framework::runtime
