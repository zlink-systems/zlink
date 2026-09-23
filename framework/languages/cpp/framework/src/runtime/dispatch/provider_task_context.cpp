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
std::atomic<const ambient_context_hooks_t *> ambient_context_hooks{nullptr};
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
