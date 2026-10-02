/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/channels/call.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace zlink::framework::detail
{
namespace
{

runtime::offload_executor_t &blocking_call_executor ()
{
    static runtime::offload_executor_t executor (
      0, std::max<std::size_t> (1, std::thread::hardware_concurrency ()),
      std::chrono::milliseconds (100), "zlink-call");
    return executor;
}

result_t<void> terminal_result (const result_t<void> &result)
{
    if (result)
        return result_t<void>::success ();
    return propagate_failure<void> (result, "one-way submit failed");
}

} // namespace

void check_blocking_submit_context ()
{
    if (application_job_context_t::current () != nullptr || capture_current_serial_turn ()
        || current_callback_context || !runtime::current_actor_execution.actor_key.empty ()
        || !runtime::current_actor_execution.spot_id.empty ()
        || runtime::state_lane_t::current () != nullptr) {
        throw framework_exception_t (
          framework_error_kind_t::invalid_operation,
          "blocking submit is not allowed in a runtime execution context");
    }
}

bool submit_blocking_call (std::function<void ()> work)
{
    if (!work)
        return false;
    try {
        return blocking_call_executor ().try_submit (std::move (work));
    }
    catch (...) {
        return false;
    }
}

task_t<void> submit_one_way_task (std::function<result_t<void> ()> submit)
{
    if (!submit) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "one-way call is not bound to a submit operation"));
    }
    try {
        return task_t<void> (terminal_result (submit ()));
    }
    catch (...) {
        return task_t<void> (current_exception_result<void> ());
    }
}

} // namespace zlink::framework::detail
