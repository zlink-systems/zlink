/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
// Completion notification helpers for C++ bench rows.
//
// Observers preserve each library's native completion resource.
#ifndef ZLINK_CPP_BENCH_ASYNC_HPP
#define ZLINK_CPP_BENCH_ASYNC_HPP

#include <zlink.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

namespace zlink_cpp_bench
{

// Ready tasks are consumed inline; pending observers stay on Framework's
// completion resource. Neither path posts a continuation to a submit thread.
template <typename T, typename Callback>
void observe_framework (zlink::framework::task_t<T> &task, Callback callback)
{
    if (task.await_ready ())
        callback (task.result ());
    else
        zlink::framework::detail::observe_task_completion (task, std::move (callback));
}

namespace
{

// Binding continuations resume on its completion poller. No scheduler is bound
// to this coroutine; its captured stream survives a bounded terminal snapshot.
struct binding_observer_t
{
    struct promise_type
    {
        binding_observer_t get_return_object () { return {}; }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { std::terminate (); }
    };
};

template <typename T, typename Success, typename Failure>
binding_observer_t observe_binding (zlink::async_result_t<T> result,
                                    Success success, Failure failure)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move (result);
            success ();
        } else {
            auto value = co_await std::move (result);
            success (value);
        }
    } catch (const std::exception &error) {
        failure (error);
    }
}

} // namespace

} // namespace zlink_cpp_bench

#endif
