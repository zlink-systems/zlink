/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>

#include "completion_turn.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

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
class completion_dispatcher_t;
std::atomic<completion_dispatcher_t *> active_completion_dispatcher{nullptr};

class completion_dispatcher_t
{
  public:
    completion_dispatcher_t ()
    {
        // A blocked callback must leave another worker available for independent turns.
        constexpr unsigned minimum_worker_count = 2;
        const auto count = std::max (minimum_worker_count, std::thread::hardware_concurrency ());
        _workers.reserve (count);
        for (unsigned index = 0; index < count; ++index)
            _workers.emplace_back ([this] { _context.run (); });
        active_completion_dispatcher.store (this, std::memory_order_release);
    }

    ~completion_dispatcher_t ()
    {
        // Let run() drain accepted turns, including work posted by those turns.
        // stop() would abandon queued callbacks and their registry reservations.
        active_completion_dispatcher.store (nullptr, std::memory_order_release);
        _work.reset ();
        _workers.clear ();
    }

    template <typename Work> void post (Work &&work)
    {
        boost::asio::post (_context, std::forward<Work> (work));
    }
    void poll_one () { _context.poll_one (); }
    std::size_t worker_count () const noexcept { return _workers.size (); }

  private:
    boost::asio::io_context _context;
    std::vector<std::jthread> _workers;
    // Declared last so construction failure releases work before joining started workers.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work{
      _context.get_executor ()};
};

completion_dispatcher_t &completion_dispatcher ()
{
    // Submit/completion §11 requires one process dispatcher. Provider tasks and
    // registries also exist without a host and outlive an individual host stop.
    static completion_dispatcher_t dispatcher;
    return dispatcher;
}

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
    completion_dispatcher ().post ([work = std::move (work), owner] {
        const completion_turn_scope_t scope{
          std::exchange (completion_owner, owner != nullptr ? owner : &process_completion_owner)};
        work ();
    });
}

bool dispatch_completion_receive_handoff (const std::function<bool ()> &handoff)
{
    // Completion publication and lane-current scopes have returned before entry.
    // Only the executor can reserve an idle resource for the next receive turn.
    if (!handoff ())
        return false;
    // Only Framework completions enter this context. Application system-executor
    // handlers cannot be consumed by the resource that just handed off receive.
    // An empty first rotation must not create workers just to attempt a dequeue.
    if (auto *dispatcher = active_completion_dispatcher.load (std::memory_order_acquire))
        dispatcher->poll_one ();
    return true;
}

std::size_t completion_dispatcher_worker_count () noexcept
{
    const auto *dispatcher = active_completion_dispatcher.load (std::memory_order_acquire);
    return dispatcher != nullptr ? dispatcher->worker_count () : 0;
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
