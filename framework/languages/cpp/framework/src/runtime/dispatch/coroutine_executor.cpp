/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/coroutine_executor.hpp"

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

void coroutine_executor_t::post_native_continuation (
  std::function<void ()> work)
{
    std::lock_guard lock (_mutex);
    if (_drained) {
        throw std::runtime_error ("handler coroutine executor is drained");
    }
    boost::asio::post (_pool, [work = std::move (work)] () mutable {
        try {
            if (work) {
                work ();
            }
        }
        catch (...) {
        }
    });
}

coroutine_executor_t &handler_coroutine_executor ()
{
    auto *ready = executor_fast_path ().load (std::memory_order_acquire);
    if (ready != nullptr) {
        return *ready;
    }
    std::lock_guard lock (executor_mutex ());
    auto &executor = executor_instance ();
    if (!executor) {
        if (executor_shutdown_requested ()) {
            throw std::runtime_error ("handler coroutine executor is shut down");
        }
        const auto workers =
          configured_worker_count () == 0 ? default_worker_count () : configured_worker_count ();
        executor = std::make_unique<coroutine_executor_t> (workers);
        executor_fast_path ().store (executor.get (), std::memory_order_release);
    }
    return *executor;
}

void configure_handler_coroutine_executor (std::size_t worker_count)
{
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
        return;
    }
    executor_shutdown_requested () = false;
    configured_worker_count () = worker_count;
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

task_scheduler_t capture_runtime_native_continuation_scheduler ()
{
    {
        std::lock_guard lock (runtime::executor_mutex ());
        if (runtime::executor_shutdown_requested ()
            || runtime::executor_owner_count () == 0) {
            return {};
        }
    }
    return [] (std::function<void ()> work) {
        std::lock_guard lock (runtime::executor_mutex ());
        if (runtime::executor_shutdown_requested ()
            || runtime::executor_owner_count () == 0) {
            throw std::runtime_error (
              "handler coroutine executor is not accepting continuations");
        }
        auto &configured = runtime::executor_instance ();
        if (!configured) {
            const auto workers = runtime::configured_worker_count () == 0
                                   ? runtime::default_worker_count ()
                                   : runtime::configured_worker_count ();
            configured = std::make_unique<runtime::coroutine_executor_t> (workers);
            runtime::executor_fast_path ().store (
              configured.get (), std::memory_order_release);
        }
        configured->post_native_continuation (std::move (work));
    };
}

} // namespace zlink::framework::detail
