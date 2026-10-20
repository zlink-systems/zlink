/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <optional>

namespace
{
using namespace zlink::framework;
namespace runtime = zlink::framework::runtime;

class ready_void_call_t : public detail::call_facade_t<ready_void_call_t, void>
{
  public:
    ready_void_call_t () : call_facade_t (result_t<void>::success ()) {}
};

struct probe_t
{
    std::atomic_int first_submits{0};
    std::atomic_int second_submits{0};
    std::atomic_int resumed_submits{0};
    int first_value = 0;
    bool rejected = false;
    bool facade_rejected = false;
    bool open_resume = false;
    bool fresh_resume = false;
    bool next_accepted = false;
    std::shared_ptr<detail::serial_turn_t> original;
};

task_t<void>
two_calls (request_call_t<int> &first, worker_call_t<int> &second, bool ready, probe_t &probe)
{
    probe.original = detail::capture_current_serial_turn ();
    // The first task has not been awaited, including when its result is ready.
    auto first_task = first.yield ();
    auto second_task = second.yield ();
    const auto &rejected = second_task.result ();
    probe.rejected =
      !rejected && rejected.error_kind () == framework_error_kind_t::invalid_operation;
    ready_void_call_t facade;
    const auto facade_result = facade.yield ().result ();
    probe.facade_rejected =
      !facade_result && facade_result.error_kind () == framework_error_kind_t::invalid_operation;
    probe.first_value = co_await first_task;
    const auto turn = detail::capture_current_serial_turn ();
    probe.open_resume = turn && !turn->released ();
    probe.fresh_resume = turn != probe.original;
    if (!ready) {
        worker_call_t<int> next ([&probe] (std::stop_token) {
            ++probe.resumed_submits;
            return task_t<int> (result_t<int>::success (11));
        });
        probe.next_accepted = (co_await next.yield ()) == 11;
    }
}

bool verify_two_calls (bool ready)
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             {}, runtime::serial_lane_policy_t::spot_wide ());
    probe_t probe;
    task_completion_source_t<int> reply;
    request_call_t<int> first ("yield.first", [&] (const auto &, auto, const auto &) {
        ++probe.first_submits;
        if (ready)
            return request_task_t<int> (result_t<int>::success (7));
        auto admitted = std::make_shared<task_completion_source_t<void>> ();
        admitted->complete (result_t<void>::success ());
        return request_task_t<int> (reply.task (), std::move (admitted));
    });
    worker_call_t<int> second ([&] (std::stop_token) {
        ++probe.second_submits;
        return task_t<int> (result_t<int>::success (9));
    });
    std::optional<task_t<void>> handler;
    std::promise<void> submitted;
    std::promise<void> finished;
    auto submitted_future = submitted.get_future ();
    auto finished_future = finished.get_future ();
    queue.post_async ("two-yields", [&] (auto complete) {
        handler.emplace (two_calls (first, second, ready, probe));
        detail::observe_task_completion (*handler, [&, complete] (const auto &) mutable {
            finished.set_value ();
            complete ([] {});
        });
        submitted.set_value ();
    });
    // Both terminals and the handler's await registration have run. No sleeps.
    submitted_future.get ();
    reply.complete (result_t<int>::success (7));
    finished_future.get ();
    queue.drain ();
    auto late = [&] {
        detail::serial_turn_scope_t ended (probe.original);
        return first.yield ();
    }();
    const bool passed =
      probe.rejected && probe.facade_rejected && probe.second_submits == 0
      && probe.first_submits == 1 && probe.first_value == 7 && probe.open_resume && !late.result ()
      && late.result ().error_kind () == framework_error_kind_t::invalid_operation
      && (ready || (probe.fresh_resume && probe.next_accepted && probe.resumed_submits == 1));
    std::cout << "ready=" << ready << " first_submits=" << probe.first_submits
              << " second_submits=" << probe.second_submits << " rejected=" << probe.rejected
              << " facade_rejected=" << probe.facade_rejected
              << " first_value=" << probe.first_value << " open_resume=" << probe.open_resume
              << " fresh_resume=" << probe.fresh_resume << " next_accepted=" << probe.next_accepted
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_ready_await_stays_in_same_turn ()
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             {}, runtime::serial_lane_policy_t::spot_wide ());
    std::promise<bool> finished;
    auto result = finished.get_future ();
    queue.post ("ready-yield", [&] {
        request_call_t<int> call (result_t<int>::success (7));
        // Consuming a ready result does not release the current turn.
        auto first = call.yield ();
        if (!first.await_ready ()) {
            finished.set_value (false);
            return;
        }
        try {
            const auto value = first.await_resume ();
            const auto second = call.yield ().result ();
            finished.set_value (value == 7 && !second
                                && second.error_kind ()
                                     == framework_error_kind_t::invalid_operation);
        }
        catch (const framework_exception_t &) {
            finished.set_value (false);
        }
    });
    const bool passed = result.get ();
    queue.drain ();
    std::cout << "same_ready_turn_rejected=" << passed << '\n';
    return passed;
}
} // namespace

int main ()
{
    const bool pending = verify_two_calls (false);
    const bool ready = verify_two_calls (true);
    const bool same_ready_turn = verify_ready_await_stays_in_same_turn ();
    return pending && ready && same_ready_turn ? 0 : 1;
}
