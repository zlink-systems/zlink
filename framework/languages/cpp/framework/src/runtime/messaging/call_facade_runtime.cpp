/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/channels/call.hpp>

#include "runtime/dispatch/offload_executor.hpp"

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
      0,
      std::max<std::size_t> (1, std::thread::hardware_concurrency ()),
      1024,
      std::chrono::milliseconds (100),
      "zlink-call");
    return executor;
}

result_t<void> terminal_result (const result_t<void> &result)
{
    if (result)
        return result_t<void>::success ();
    const auto *error = result.error ();
    if (error == nullptr) {
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "one-way submit failed");
    }
    switch (boundary_state (*error)) {
        case boundary_error_t::timed_out:
            return result_t<void>::failure (
              framework_error_kind_t::deadline_exceeded, error->what ());
        case boundary_error_t::shutdown:
            return result_t<void>::failure (
              framework_error_kind_t::shutting_down, error->what ());
        case boundary_error_t::disconnected:
            return result_t<void>::failure (
              framework_error_kind_t::unavailable, error->what ());
        case boundary_error_t::none:
        case boundary_error_t::closed:
        case boundary_error_t::cancelled:
        case boundary_error_t::stale_generation:
            return result_access_t::failure<void> (*error);
    }
    return result_access_t::failure<void> (*error);
}

} // namespace

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

task_t<void>
submit_one_way_task (std::function<result_t<void> ()> submit)
{
    if (!submit) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "one-way call is not bound to a submit operation"));
    }
    try {
        return task_t<void> (terminal_result (submit ()));
    }
    catch (const framework_exception_t &error) {
        return task_t<void> (result_access_t::failure<void> (error));
    }
    catch (const std::exception &error) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "one-way submit failed"));
    }
}

} // namespace zlink::framework::detail
