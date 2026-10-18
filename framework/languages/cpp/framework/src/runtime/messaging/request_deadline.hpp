/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/detail/deadline_scheduler.hpp>

#include <optional>
#include <cmath>
#include <cstdint>
#include <limits>

namespace zlink::framework::runtime::messaging
{

// Preserve the source operation's deadline across the steady/system clock boundary.
inline std::uint64_t request_deadline_unix_ms (std::chrono::steady_clock::time_point at)
{
    using milliseconds_t = std::chrono::duration<long double, std::milli>;
    const auto steady_now = std::chrono::steady_clock::now ();
    const auto unix_now = std::chrono::system_clock::now ();
    const auto unix_ms = std::ceil (milliseconds_t (unix_now.time_since_epoch ()).count ()
                                    + milliseconds_t (at.time_since_epoch ()).count ()
                                    - milliseconds_t (steady_now.time_since_epoch ()).count ());
    if (unix_ms <= 1)
        return 1;
    if (unix_ms >= static_cast<long double> (std::numeric_limits<std::uint64_t>::max ()))
        return std::numeric_limits<std::uint64_t>::max ();
    return static_cast<std::uint64_t> (unix_ms);
}

inline std::chrono::steady_clock::time_point request_deadline_from_unix_ms (std::uint64_t unix_ms)
{
    using clock_t = std::chrono::steady_clock;
    using milliseconds_t = std::chrono::duration<long double, std::milli>;
    const auto now = clock_t::now ();
    const auto remaining =
      static_cast<long double> (unix_ms)
      - milliseconds_t (std::chrono::system_clock::now ().time_since_epoch ()).count ();
    if (remaining <= 0)
        return now;
    const auto available = milliseconds_t (clock_t::time_point::max ().time_since_epoch ()).count ()
                           - milliseconds_t (now.time_since_epoch ()).count ();
    if (remaining >= available)
        return clock_t::time_point::max ();
    return now + std::chrono::duration_cast<clock_t::duration> (milliseconds_t{remaining});
}

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

template <typename T>
request_task_t<T> with_request_deadline (request_task_t<T> operation,
                                         std::optional<std::chrono::steady_clock::time_point> at)
{
    auto admitted = detail::request_task_access_t::admission_source (operation);
    auto reply = with_request_deadline (std::move (static_cast<task_t<T> &> (operation)), at);
    detail::observe_request_failure (reply, admitted);
    static_cast<task_t<T> &> (operation) = std::move (reply);
    return operation;
}

} // namespace zlink::framework::runtime::messaging
