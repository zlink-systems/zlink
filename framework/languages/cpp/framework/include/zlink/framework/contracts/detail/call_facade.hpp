/* SPDX-License-Identifier: FSL-1.1-ALv2 */
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
    task_t<T> yield ()
    {
        return current_serial_turn_allows_yield ()
                 ? task_t<T> (_result)
                 : unsupported_yield_task<T> ();
    }

  private:
    result_t<T> _result;
};

template <> class immediate_call_state_t<void>
{
  public:
    explicit immediate_call_state_t (result_t<void> result) : _result (std::move (result)) {}

    void set_timeout (std::chrono::milliseconds) {}

    task_t<void> async () { return task_t<void> (_result); }
    task_t<void> yield ()
    {
        return current_serial_turn_allows_yield ()
                 ? task_t<void> (_result)
                 : unsupported_yield_task<void> ();
    }

  private:
    result_t<void> _result;
};

template <typename T> class async_call_state_t
{
  public:
    explicit async_call_state_t (task_t<T> task) :
        _completion (std::make_shared<task_completion_source_t<T>> ()),
        _observed (std::make_shared<task_t<T>> (std::move (task)))
    {
        detail::observe_task_completion (
          *_observed, [completion = _completion] (const result_t<T> &result) {
              completion->complete (result);
          });
    }

    void set_timeout (std::chrono::milliseconds) {}
    task_t<T> async () { return _completion->task (); }
    task_t<T> yield ()
    {
        return current_serial_turn_allows_yield () ? _completion->task ()
                                                  : unsupported_yield_task<T> ();
    }

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
    task_t<TResult> yield () { return _yield (); }

  protected:
    explicit call_facade_t (result_t<TResult> result)
    {
        auto state = std::make_shared<immediate_call_state_t<TResult>> (
          std::move (result));
        _submit = [state] { return state->async (); };
        _yield = [state] { return state->yield (); };
    }

    explicit call_facade_t (task_t<TResult> task)
    {
        auto state = std::make_shared<async_call_state_t<TResult>> (
          std::move (task));
        _submit = [state] { return state->async (); };
        _yield = [state] { return state->yield (); };
    }

  private:
    std::function<task_t<TResult> ()> _submit;
    std::function<task_t<TResult> ()> _yield;
};

} // namespace zlink::framework::detail
