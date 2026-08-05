/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace zlink::framework::runtime
{

template <typename TResult>
boost::asio::awaitable<result_t<TResult>> await_task_result (task_t<TResult> task)
{
    auto executor = co_await boost::asio::this_coro::executor;
    auto token = boost::asio::use_awaitable;
    co_return co_await boost::asio::async_initiate<decltype (token), void (result_t<TResult>)> (
      [executor, task = std::move (task)] (auto handler) mutable {
          auto shared_handler =
            std::make_shared<std::decay_t<decltype (handler)>> (std::move (handler));
          detail::observe_task_completion (
            task, [executor, shared_handler] (const result_t<TResult> &result) mutable {
                auto result_copy = result;
                boost::asio::dispatch (
                  executor, [shared_handler, result = std::move (result_copy)] () mutable {
                      (*shared_handler) (std::move (result));
                  });
            });
      },
      token);
}

class coroutine_executor_t
{
  public:
    explicit coroutine_executor_t (std::size_t worker_count = 1);
    ~coroutine_executor_t ();

    coroutine_executor_t (const coroutine_executor_t &) = delete;
    coroutine_executor_t &operator= (const coroutine_executor_t &) = delete;

    template <typename TResult>
    task_t<TResult> submit (std::function<boost::asio::awaitable<result_t<TResult>> ()> work)
    {
        detail::task_completion_source_t<TResult> completion;
        auto task = completion.task ();
        boost::asio::co_spawn (
          _pool,
          [work = std::move (work),
           completion = std::move (completion)] () mutable -> boost::asio::awaitable<void> {
              result_t<TResult> result = result_t<TResult>::failure (
                framework_error_kind_t::internal_failure, "coroutine handler failed");
              try {
                  result = co_await work ();
              }
              catch (const framework_exception_t &error) {
                  result = detail::result_access_t::failure<TResult> (error);
              }
              catch (...) {
                  result = result_t<TResult>::failure (framework_error_kind_t::internal_failure,
                                                       "coroutine handler threw an exception");
              }
              completion.complete (std::move (result));
          },
          boost::asio::detached);
        return task;
    }

    template <typename TResult>
    result_t<TResult> run (std::function<boost::asio::awaitable<result_t<TResult>> ()> work)
    {
        return submit<TResult> (std::move (work)).result ();
    }

    void drain ();

  private:
    std::mutex _mutex;
    bool _drained = false;
    boost::asio::thread_pool _pool;
};

coroutine_executor_t &handler_coroutine_executor ();
void configure_handler_coroutine_executor (std::size_t worker_count);
void shutdown_handler_coroutine_executor () noexcept;

} // namespace zlink::framework::runtime
