/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/async_submit_runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace
{

using namespace zlink::framework;
namespace admission = zlink::framework::runtime::messaging;

result_t<void> full ()
{
    return result_t<void>::failure (
      framework_error_kind_t::capacity_exceeded, "capacity is not available");
}

int reservation_covers_in_flight_retry ()
{
    admission::reset_async_submit_runtime_for_tests ();
    int owner = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool retry_entered = false;
    bool release_retry = false;
    std::atomic_int attempts{0};
    auto first = detail::submit_one_way_task ([&] {
        admission::note_submit_attempt (
          "reservation", &owner, std::chrono::milliseconds (500), 1);
        if (attempts.fetch_add (1) == 0) {
            return full ();
        }
        std::unique_lock lock (mutex);
        retry_entered = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release_retry; });
        return result_t<void>::success ();
    });
    admission::notify_submit_ready ("reservation", &owner);
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (
              lock, std::chrono::milliseconds (250), [&] { return retry_entered; })) {
            release_retry = true;
            lock.unlock ();
            changed.notify_all ();
            return 1;
        }
    }

    std::atomic_int rejected_attempts{0};
    auto rejected = detail::submit_one_way_task ([&] {
        admission::note_submit_attempt (
          "reservation", &owner, std::chrono::milliseconds (30), 1);
        ++rejected_attempts;
        return full ();
    });
    /* 04-async-execution-policy.ko.md §1.3: once the internal bounded waiter capacity is
     * already fully in use (here, the in-flight retry still holding the owner's one
     * reservation), a new admission hits the hard overload boundary and completes
     * immediately with DeadlineExceeded rather than being queued. */
    const bool hit_hard_overload_boundary =
      rejected.await_ready () && rejected_attempts.load () == 1;
    {
        std::lock_guard lock (mutex);
        release_retry = true;
    }
    changed.notify_all ();
    first.result ().value ();
    const auto &rejected_result = rejected.result ();
    return hit_hard_overload_boundary && !rejected_result
             && rejected_result.error_kind ()
                  == framework_error_kind_t::deadline_exceeded
             && admission::pending_submit_count_for_tests () == 0
           ? 0
           : 2;
}

int retry_credit_does_not_cross_operation ()
{
    admission::reset_async_submit_runtime_for_tests ();
    int owner = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool retry_entered = false;
    bool release_retry = false;
    std::atomic_int first_attempts{0};
    auto first = detail::submit_one_way_task ([&] {
        admission::note_submit_attempt (
          "credit", &owner, std::chrono::milliseconds (500), 1);
        if (first_attempts.fetch_add (1) == 0) {
            return full ();
        }
        std::unique_lock lock (mutex);
        retry_entered = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release_retry; });
        return result_t<void>::success ();
    });
    admission::notify_submit_ready ("credit", &owner);
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (
              lock, std::chrono::milliseconds (250), [&] { return retry_entered; })) {
            release_retry = true;
            lock.unlock ();
            changed.notify_all ();
            return 1;
        }
    }
    admission::notify_submit_ready ("credit", &owner);
    {
        std::lock_guard lock (mutex);
        release_retry = true;
    }
    changed.notify_all ();
    first.result ().value ();

    std::atomic_int second_attempts{0};
    auto second = detail::submit_one_way_task ([&] {
        admission::note_submit_attempt (
          "credit", &owner, std::chrono::milliseconds (500), 1);
        return second_attempts.fetch_add (1) < 2 ? full ()
                                                 : result_t<void>::success ();
    });
    admission::notify_submit_ready ("credit", &owner);
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (100);
    while (second_attempts.load () < 2 && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (second_attempts.load () != 2 || second.await_ready ()) {
        return 3;
    }
    admission::notify_submit_ready ("credit", &owner);
    second.result ().value ();
    return second_attempts.load () == 3
           ? 0
           : 4;
}

int stopped_owner_epoch_cannot_reserve_after_restart ()
{
    admission::reset_async_submit_runtime_for_tests ();
    int owner = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool attempt_entered = false;
    bool release_attempt = false;
    std::promise<framework_error_kind_t> terminal_source;
    auto terminal = terminal_source.get_future ();
    std::thread submitter ([&] {
        auto operation = detail::submit_one_way_task ([&] {
            admission::note_submit_attempt (
              "epoch", &owner, std::chrono::milliseconds (500), 1);
            std::unique_lock lock (mutex);
            attempt_entered = true;
            changed.notify_all ();
            changed.wait (lock, [&] { return release_attempt; });
            return full ();
        });
        terminal_source.set_value (operation.result ().error_kind ());
    });
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (
              lock, std::chrono::milliseconds (250), [&] { return attempt_entered; })) {
            release_attempt = true;
            lock.unlock ();
            changed.notify_all ();
            submitter.join ();
            return 1;
        }
    }
    admission::shutdown_submit_owner (&owner);
    admission::activate_submit_owner (&owner);
    {
        std::lock_guard lock (mutex);
        release_attempt = true;
    }
    changed.notify_all ();
    submitter.join ();
    return terminal.get () == framework_error_kind_t::shutting_down
             && admission::pending_submit_count_for_tests () == 0
           ? 0
           : 2;
}

int shutdown_during_retry_rejects_late_success ()
{
    admission::reset_async_submit_runtime_for_tests ();
    int owner = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool retry_entered = false;
    bool release_retry = false;
    std::atomic_int attempts{0};
    auto operation = detail::submit_one_way_task ([&] {
        admission::note_submit_attempt (
          "late-success", &owner, std::chrono::milliseconds (500), 1);
        if (attempts.fetch_add (1) == 0) {
            return full ();
        }
        std::unique_lock lock (mutex);
        retry_entered = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release_retry; });
        return result_t<void>::success ();
    });
    admission::notify_submit_ready ("late-success", &owner);
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (
              lock, std::chrono::milliseconds (250), [&] { return retry_entered; })) {
            release_retry = true;
            lock.unlock ();
            changed.notify_all ();
            operation.result ();
            return 1;
        }
    }
    admission::shutdown_submit_owner (&owner);
    {
        std::lock_guard lock (mutex);
        release_retry = true;
    }
    changed.notify_all ();
    const auto terminal = operation.result ();
    admission::activate_submit_owner (&owner);
    return !terminal
             && terminal.error_kind () == framework_error_kind_t::shutting_down
             && attempts.load () == 2
             && admission::pending_submit_count_for_tests () == 0
           ? 0
           : 2;
}

int public_call_terminator_is_one_shot ()
{
    std::atomic_int attempts{0};
    send_call_t call (
      "one-shot", [&] (const std::string &, const send_call_t::metadata_map_t &) {
          ++attempts;
          return result_t<void>::success ();
      });
    auto copy = call;
    call.submit ().result ().value ();
    try {
        /* send_call_t::_submission is a shared_ptr specifically so a copy shares the same
         * one-shot claim as the original -- copying a call must not be a way to bypass
         * single-use. 04-async-execution-policy.ko.md §1.3's failure table classifies
         * re-running the same call's terminal as `AlreadySubmitted`, not a protocol error
         * (that kind is reserved for a call that was never bound to a submit function). */
        (void) copy.submit ().result ().value ();
    }
    catch (const framework_exception_t &error) {
        return error.kind () == framework_error_kind_t::invalid_operation
                 && attempts.load () == 1
               ? 0
               : 2;
    }
    return 3;
}

} // namespace

int main ()
{
    if (const auto value = reservation_covers_in_flight_retry (); value != 0) {
        return 10 + value;
    }
    if (const auto value = retry_credit_does_not_cross_operation (); value != 0) {
        return 20 + value;
    }
    if (const auto value = stopped_owner_epoch_cannot_reserve_after_restart (); value != 0) {
        return 30 + value;
    }
    if (const auto value = shutdown_during_retry_rejects_late_success (); value != 0) {
        return 40 + value;
    }
    if (const auto value = public_call_terminator_is_one_shot (); value != 0) {
        return 50 + value;
    }
    admission::reset_async_submit_runtime_for_tests ();
    return 0;
}
