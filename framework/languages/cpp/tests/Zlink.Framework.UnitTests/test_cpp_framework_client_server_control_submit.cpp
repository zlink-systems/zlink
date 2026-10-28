/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/client_server/raw_client_server_owner.hpp"
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <type_traits>
namespace fw = zlink::framework;
namespace backend = fw::detail::backend;
namespace cs = fw::runtime::client_server;
template <class Observer>
bool returns_without_completion (Observer observer,
                                 backend::raw_send_submission_t submission,
                                 std::shared_ptr<fw::runtime::runtime_failure_collector_t> failures)
{
    if constexpr (std::is_void_v<std::invoke_result_t<
                    Observer, backend::raw_send_submission_t,
                    std::shared_ptr<fw::runtime::runtime_failure_collector_t>>>) {
        observer (std::move (submission), std::move (failures));
        return true;
    } else {
        auto result = observer (std::move (submission), std::move (failures));
        if (result.await_ready ()) {
            const auto terminal = result.result ();
            if (!terminal)
                std::cout << "unexpected helper terminal: " << terminal.error ()->what () << '\n';
        }
        return result.await_ready ();
    }
}
int main ()
{
    std::mutex error_mutex;
    std::condition_variable errors_changed;
    unsigned errors = 0;
    fw::completion_owner_t completion_owner;
    auto failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    const auto expect_errors = [&] (unsigned expected) {
        std::unique_lock lock (error_mutex);
        return errors_changed.wait_for (lock, std::chrono::seconds (2), [&] {
            return errors >= expected;
        }) && errors == expected;
    };
    failures->bind_runtime_reporter ([&] (std::exception_ptr error) {
        if (!error)
            std::abort ();
        std::lock_guard lock (error_mutex);
        ++errors;
        errors_changed.notify_all ();
    });
    fw::task_completion_source_t<zlink::submit_result_t> pending;
    auto task = std::make_shared<fw::task_t<zlink::submit_result_t>> (pending.task ());
    const bool returned = returns_without_completion (
      &cs::observe_control_submission,
      {backend::raw_send_submission_state_t::pending_backpressure, std::nullopt, task}, failures);
    std::cout << "control pending expected source-ready=1 actual=" << returned << '\n';
    if (!returned) {
        pending.complete (
          fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
        return 1;
    }
    {
        std::lock_guard lock (error_mutex);
        if (errors != 0 || task->await_ready ())
            return 2;
    }
    pending.complete (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_connected));
    if (!expect_errors (1))
        return 3;
    returns_without_completion (
      &cs::observe_control_submission,
      {backend::raw_send_submission_state_t::immediate,
       fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_found),
       {}},
      failures);
    if (!expect_errors (2))
        return 4;
    fw::task_completion_source_t<zlink::submit_result_t> failed;
    auto failed_task = std::make_shared<fw::task_t<zlink::submit_result_t>> (failed.task ());
    returns_without_completion (
      &cs::observe_control_submission,
      {backend::raw_send_submission_state_t::pending_backpressure, std::nullopt, failed_task},
      failures);
    failed.complete (fw::result_t<zlink::submit_result_t>::failure (
      fw::framework_error_kind_t::internal_failure, "control terminal failure"));
    if (!expect_errors (3))
        return 5;
    std::cout << "control terminal expected reported failures=3 actual=" << errors << '\n';
    return 0;
}
