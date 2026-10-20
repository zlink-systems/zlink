/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>

#include "runtime/dispatch/completion_turn.hpp"

#include <boost/asio/detail/scheduler.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_context.hpp>
#include <boost/asio/system_executor.hpp>

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
thread_local bool current_infrastructure_wait_flag = false;
thread_local const void *completion_owner = nullptr;
const char process_completion_owner = 0;

struct completion_turn_scope_t
{
    const void *previous;
    ~completion_turn_scope_t () { completion_owner = previous; }
};
std::atomic<const ambient_context_hooks_t *> ambient_context_hooks{nullptr};
std::atomic<const runtime_execution_hooks_t *> runtime_execution_hooks{nullptr};
}

const void *current_completion_owner () noexcept
{
    return completion_owner;
}

bool can_reuse_completion_turn (bool known_runtime_worker,
                                const void *caller_job,
                                bool caller_owned_turn) noexcept
{
    if ((completion_owner == nullptr && !known_runtime_worker) || current_infrastructure_wait_flag)
        return false;
    if (caller_owned_turn)
        return current_application_job == nullptr || current_application_job == caller_job;
    return !current_serial_turn_handle && current_application_job == nullptr
           && !current_wait_owner_token.stop_possible ();
}

void dispatch_completion (std::function<void ()> work, bool inline_allowed, const void *owner)
{
    if (!work)
        return;
    if (inline_allowed || (owner == nullptr && can_reuse_completion_turn ())) {
        work ();
        return;
    }
    boost::asio::post (boost::asio::system_executor{}, [work = std::move (work), owner] {
        const completion_turn_scope_t scope{
          std::exchange (completion_owner, owner != nullptr ? owner : &process_completion_owner)};
        work ();
    });
}

bool dispatch_completion_receive_handoff (const std::function<bool ()> &handoff)
{
    // The receive stack and lane-current scopes have returned before entry.
    // Only the executor can reserve an idle resource for the next receive turn.
    if (!handoff ())
        return false;
    // Join the existing process dispatcher for one nonblocking turn. Its queue
    // determines the next callback; idle workers may already have consumed it.
    auto &context = boost::asio::system_executor{}.context ();
    auto &scheduler = boost::asio::use_service<boost::asio::detail::scheduler> (context);
    boost::system::error_code error;
    scheduler.poll_one (error);
    if (error)
        throw boost::system::system_error (error);
    return true;
}

task_scheduler_t capture_runtime_native_continuation_scheduler ()
{
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    return hooks != nullptr ? hooks->capture_scheduler () : task_scheduler_t{};
}

task_scheduler_t capture_runtime_scope_exit_scheduler ()
{
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    return hooks != nullptr ? hooks->capture_scope_exit_scheduler () : task_scheduler_t{};
}

std::stop_token current_wait_owner ()
{
    return current_wait_owner_token;
}
std::stop_token exchange_wait_owner (std::stop_token token)
{
    return std::exchange (current_wait_owner_token, std::move (token));
}

bool current_infrastructure_wait () noexcept
{
    return current_infrastructure_wait_flag;
}
bool exchange_infrastructure_wait (bool value) noexcept
{
    return std::exchange (current_infrastructure_wait_flag, value);
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
