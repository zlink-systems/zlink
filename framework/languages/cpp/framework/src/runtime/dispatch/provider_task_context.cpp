/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <atomic>
#include <optional>
#include <utility>

namespace zlink::framework::detail
{

namespace
{
thread_local std::shared_ptr<serial_turn_t> current_serial_turn_handle;
thread_local std::optional<serial_resume_failure_t> current_serial_resume_failure;
thread_local const void *current_application_job = nullptr;
thread_local std::stop_token current_wait_owner_token;
std::atomic<const ambient_context_hooks_t *> ambient_context_hooks{nullptr};
std::atomic<const runtime_execution_hooks_t *> runtime_execution_hooks{nullptr};
}

task_scheduler_t capture_runtime_native_continuation_scheduler ()
{
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    return hooks != nullptr ? hooks->capture_scheduler () : task_scheduler_t{};
}

std::stop_token current_wait_owner ()
{
    return current_wait_owner_token;
}
std::stop_token exchange_wait_owner (std::stop_token token)
{
    return std::exchange (current_wait_owner_token, std::move (token));
}

void ensure_blocking_submit_allowed ()
{
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    if (hooks != nullptr)
        hooks->ensure_blocking_allowed ();
}

void set_runtime_execution_hooks (const runtime_execution_hooks_t *hooks) noexcept
{
    runtime_execution_hooks.store (hooks, std::memory_order_release);
}

std::shared_ptr<serial_turn_t> capture_current_serial_turn ()
{
    return current_serial_turn_handle;
}

std::shared_ptr<serial_turn_t> exchange_current_serial_turn (std::shared_ptr<serial_turn_t> turn)
{
    return std::exchange (current_serial_turn_handle, std::move (turn));
}

void set_serial_resume_failure (framework_error_kind_t kind, std::string message)
{
    current_serial_resume_failure = serial_resume_failure_t{kind, std::move (message)};
}

std::optional<serial_resume_failure_t> take_serial_resume_failure ()
{
    auto failure = std::move (current_serial_resume_failure);
    current_serial_resume_failure.reset ();
    return failure;
}

const void *application_job_context_t::current () noexcept
{
    return current_application_job;
}

const void *application_job_context_t::exchange (const void *job) noexcept
{
    return std::exchange (current_application_job, job);
}

const ambient_context_hooks_t *current_ambient_context_hooks () noexcept
{
    return ambient_context_hooks.load (std::memory_order_acquire);
}

void set_ambient_context_hooks (const ambient_context_hooks_t *hooks) noexcept
{
    ambient_context_hooks.store (hooks, std::memory_order_release);
}

} // namespace zlink::framework::detail
