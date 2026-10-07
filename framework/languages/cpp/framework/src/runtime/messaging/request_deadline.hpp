/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/detail/deadline_scheduler.hpp>

#include <optional>

namespace zlink::framework::runtime::messaging
{

// The caller budget covers admission and reply. Expiry only closes this
// completion; binding retains the operation and drains its eventual terminal.
template <typename T>
task_t<T> with_request_deadline (task_t<T> operation,
                                 std::optional<std::chrono::steady_clock::time_point> at)
{
    if (!at || operation.await_ready ())
        return operation;
    auto source = std::make_shared<task_completion_source_t<T>> ();
    auto output = source->task ();
    auto observed = std::make_shared<task_t<T>> (std::move (operation));
    auto deadline = std::make_shared<detail::deadline_state_t> ();
    detail::observe_task_completion (
      *observed, [source, observed, deadline] (const result_t<T> &result) {
          deadline->cancelled.store (true, std::memory_order_release);
          detail::deadline_scheduler_t::instance ().wake ();
          source->complete (result);
      });
    if (!output.await_ready ()) {
        detail::deadline_scheduler_t::instance ().schedule (
          *at, deadline, [weak = std::weak_ptr (source)] {
              if (auto current = weak.lock ())
                  current->complete (detail::boundary_failure<T> (
                    detail::boundary_error_t::timed_out, "request deadline expired"));
          });
    }
    return output;
}

} // namespace zlink::framework::runtime::messaging
