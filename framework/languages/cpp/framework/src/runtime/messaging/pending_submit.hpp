/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/pending_operation_state.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace zlink::framework::runtime::messaging
{

class pending_submit_t
{
  public:
    using clock_t = std::chrono::steady_clock;
    using try_submit_fn = std::function<bool ()>;
    using wake_fn = std::function<void ()>;

    static pending_submit_t
    create_command (try_submit_fn try_submit,
                    std::optional<clock_t::time_point> deadline = std::nullopt,
                    wake_fn wake = {});

    static pending_submit_t
    create_request (try_submit_fn try_submit,
                    std::optional<clock_t::time_point> deadline = std::nullopt,
                    wake_fn wake = {});

    pending_operation_t operation () const noexcept { return _operation; }
    bool complete_on_accepted () const noexcept { return _complete_on_accepted; }
    bool completed () const noexcept { return _operation.completed (); }
    bool expired (clock_t::time_point now = clock_t::now ()) const noexcept;

    bool try_submit ();
    void complete ();
    void cancel ();
    void fail (std::exception_ptr error);

  private:
    pending_submit_t (try_submit_fn try_submit,
                      std::optional<clock_t::time_point> deadline,
                      wake_fn wake,
                      bool complete_on_accepted);

    pending_operation_t _operation;
    try_submit_fn _try_submit;
    std::optional<clock_t::time_point> _deadline;
    wake_fn _wake;
    bool _complete_on_accepted = false;
};

} // namespace zlink::framework::runtime::messaging
