/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/channels/call.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace zlink::framework::runtime
{

template <typename T> task_t<T> run_blocking_step (std::function<task_t<T> ()> step)
{
    auto completion = std::make_shared<task_completion_source_t<T>> ();
    auto result = completion->task ();
    auto owned_step = std::make_shared<std::function<task_t<T> ()>> (std::move (step));
    auto run = [owned_step, completion] () mutable {
        try {
            auto running = std::make_shared<task_t<T>> ((*owned_step) ());
            detail::observe_task_terminal (
              *running, [running, completion, owned_step] (const result_t<T> &value) mutable {
                  completion->complete (value);
              });
        }
        catch (const framework_exception_t &error) {
            completion->complete (detail::result_access_t::failure<T> (error));
        }
        catch (const std::exception &error) {
            completion->complete (
              result_t<T>::failure (framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            completion->complete (result_t<T>::failure (framework_error_kind_t::internal_failure,
                                                        "Framework blocking step failed"));
        }
    };
    if (!detail::submit_blocking_call (std::move (run)))
        completion->complete (result_t<T>::failure (
          framework_error_kind_t::shutting_down, "Framework blocking step executor is stopping"));
    return result;
}

} // namespace zlink::framework::runtime
