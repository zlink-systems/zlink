/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <utility>

namespace zlink::framework::detail
{

template <typename T> class immediate_call_state_t
{
  public:
    explicit immediate_call_state_t (result_t<T> result) : _result (std::move (result)) {}

    void set_timeout (std::chrono::milliseconds) {}

    task_t<T> submit () { return task_t<T> (_result); }
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

    task_t<void> submit () { return task_t<void> (_result); }
    task_t<void> yield ()
    {
        return current_serial_turn_allows_yield ()
                 ? task_t<void> (_result)
                 : unsupported_yield_task<void> ();
    }

  private:
    result_t<void> _result;
};

template <typename TDerived, typename TResult> class call_facade_t
{
  public:
    TDerived &timeout (std::chrono::milliseconds timeout)
    {
        _state.set_timeout (timeout);
        return static_cast<TDerived &> (*this);
    }

    task_t<TResult> submit () { return _state.submit (); }
    task_t<TResult> yield () { return _state.yield (); }

  protected:
    explicit call_facade_t (result_t<TResult> result) : _state (std::move (result)) {}

  private:
    immediate_call_state_t<TResult> _state;
};

} // namespace zlink::framework::detail
