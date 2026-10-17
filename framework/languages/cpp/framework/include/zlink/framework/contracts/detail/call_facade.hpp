/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <utility>

namespace zlink::framework::detail
{

template <typename T> class immediate_call_state_t
{
  public:
    explicit immediate_call_state_t (result_t<T> result) : _result (std::move (result)) {}

    void set_timeout (std::chrono::milliseconds) {}

    task_t<T> async () { return task_t<T> (_result); }

  private:
    result_t<T> _result;
};

template <typename T> class async_call_state_t
{
  public:
    explicit async_call_state_t (task_t<T> task) :
        _completion (std::make_shared<task_completion_source_t<T>> ()),
        _observed (std::make_shared<task_t<T>> (std::move (task)))
    {
        detail::observe_task_completion (*_observed,
                                         [completion = _completion] (const result_t<T> &result) {
                                             completion->complete (result);
                                         });
    }

    void set_timeout (std::chrono::milliseconds) {}
    task_t<T> async () { return _completion->task (); }

  private:
    std::shared_ptr<task_completion_source_t<T>> _completion;
    std::shared_ptr<task_t<T>> _observed;
};

template <typename TDerived, typename TResult> class call_facade_t
{
  public:
    TDerived &timeout (std::chrono::milliseconds timeout)
    {
        (void) timeout;
        return static_cast<TDerived &> (*this);
    }

    task_t<TResult> async () { return _submit (); }
    task_t<TResult> yield ()
    {
        return yield_call<TResult> ([this] { return _submit (); });
    }

  protected:
    explicit call_facade_t (result_t<TResult> result)
    {
        auto state = std::make_shared<immediate_call_state_t<TResult>> (std::move (result));
        _submit = [state] { return state->async (); };
    }

    explicit call_facade_t (task_t<TResult> task)
    {
        auto state = std::make_shared<async_call_state_t<TResult>> (std::move (task));
        _submit = [state] { return state->async (); };
    }

  private:
    std::function<task_t<TResult> ()> _submit;
};

} // namespace zlink::framework::detail
