/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/timers/core_timer_drain_loop.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <thread>

namespace zlink::framework::detail
{

// The one-shot worker owns its native resources until cleanup finishes. The
// waiting task observes both timer operation failures and cleanup failures.
inline task_t<void> delay (std::chrono::milliseconds duration)
{
    auto source = std::make_shared<task_completion_source_t<void>> ();
    auto pending = source->task ();
    std::thread ([source, duration] {
        runtime::runtime_failure_collector_t failures;
        std::unique_ptr<zlink::timer_t> timer;
        std::unique_ptr<zlink::poller_t> poller;
        failures.capture ([&] {
            timer = std::make_unique<zlink::timer_t> ();
            poller = std::make_unique<zlink::poller_t> ();
            poller->add (*timer, 1);
            const auto interval = duration > std::chrono::milliseconds::zero ()
                                    ? duration
                                    : std::chrono::milliseconds (1);
            timer->start (interval, 1);
            std::array<zlink::poll_event_t, 1> events{};
            for (;;) {
                if (poller->wait (events.data (), events.size (),
                                  core_timer_drain_loop_t::poll_interval)
                    == 0)
                    continue;
                if (const auto count = timer->recv (); count && *count)
                    break;
            }
        });
        if (poller)
            failures.capture ([&] { poller->close (); });
        if (timer)
            failures.capture ([&] { timer->close (); });
        auto result = result_t<void>::success ();
        try {
            failures.rethrow_if_failed ();
        }
        catch (const std::exception &) {
            result = current_exception_result<void> ();
        }
        source->complete (std::move (result));
    }).detach ();
    return pending;
}

} // namespace zlink::framework::detail
