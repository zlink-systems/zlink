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
struct completion_turn_scope_t;
thread_local const completion_turn_scope_t *completion_owner = nullptr;
const char process_completion_owner = 0;

struct completion_turn_scope_t
{
    const completion_turn_scope_t *previous;
    const void *identity;
    completion_dispatcher_t *dispatcher;
    completion_turn_scope_t (const void *owner, completion_dispatcher_t *target) :
        previous (completion_owner), identity (owner), dispatcher (target)
    {
        completion_owner = this;
    }
    ~completion_turn_scope_t () { completion_owner = previous; }
};
std::weak_ptr<completion_dispatcher_t> active_completion_dispatcher;
std::mutex completion_creation_mutex;
}


// Publishers and posted turns borrow a live completion owner's context.
// Closing its last owner drains accepted work before joining workers.
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
    }

    ~completion_dispatcher_t ()
    {
        _work.reset ();
        for (auto &worker : _workers)
            worker.join ();
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

namespace
{
std::atomic<const ambient_context_hooks_t *> ambient_context_hooks{nullptr};
std::atomic<const runtime_execution_hooks_t *> runtime_execution_hooks{nullptr};
}

namespace
{
void ensure_completion_owner_lifecycle_allowed ()
{
    if (completion_owner)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "completion owner lifecycle requires an external turn");
}
}

std::shared_ptr<void> create_completion_owner ()
{
    ensure_completion_owner_lifecycle_allowed ();
    const std::lock_guard lock (completion_creation_mutex);
    auto dispatcher = active_completion_dispatcher.lock ();
    if (!dispatcher) {
        dispatcher = std::make_shared<completion_dispatcher_t> ();
        active_completion_dispatcher = dispatcher;
    }
    return dispatcher;
}

void close_completion_owner (std::shared_ptr<void> &state)
{
    if (!state)
        return;
    ensure_completion_owner_lifecycle_allowed ();
    const std::lock_guard lock (completion_creation_mutex);
    state.reset ();
}

completion_dispatcher_borrow_t borrow_completion_dispatcher ()
{
    if (completion_owner)
        return completion_owner->dispatcher;
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    if (hooks != nullptr) {
        if (auto *dispatcher = hooks->borrow_dispatcher ())
            return dispatcher;
    }
    const std::lock_guard lock (completion_creation_mutex);
    const auto dispatcher = active_completion_dispatcher.lock ();
    if (!dispatcher)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "asynchronous completion requires a live completion owner");
    return dispatcher.get ();
}

std::weak_ptr<completion_dispatcher_t> observe_completion_dispatcher ()
{
    const std::lock_guard lock (completion_creation_mutex);
    return active_completion_dispatcher;
}

const void *current_completion_owner () noexcept
{
    return completion_owner ? completion_owner->identity : nullptr;
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

void dispatch_completion (std::function<void ()> work,
                          bool inline_allowed,
                          const void *owner,
                          completion_dispatcher_borrow_t dispatcher)
{
    if (!work)
        return;
    if (inline_allowed || (owner == nullptr && can_reuse_completion_turn ())) {
        work ();
        return;
    }
    if (!dispatcher)
        dispatcher = borrow_completion_dispatcher ();
    auto *target = dispatcher;
    target->post ([work = std::move (work), owner, target] {
        const completion_turn_scope_t scope{owner != nullptr ? owner : &process_completion_owner,
                                            target};
        work ();
    });
}

bool dispatch_completion_receive_handoff (const std::function<bool ()> &handoff,
                                          const completion_dispatcher_borrow_t &existing_dispatcher)
{
    // Completion publication and lane-current scopes have returned before entry.
    // Only the executor can reserve an idle resource for the next receive turn.
    if (!handoff ())
        return false;
    // Only Framework completions enter this context. Application system-executor
    // handlers cannot be consumed by the resource that just handed off receive.
    // An empty first rotation must not create workers just to attempt a dequeue.
    if (existing_dispatcher) {
        existing_dispatcher->poll_one ();
    } else {
        const auto observed = observe_completion_dispatcher ();
        if (!observed.expired ())
            borrow_completion_dispatcher ()->poll_one ();
    }
    return true;
}

std::size_t completion_dispatcher_worker_count () noexcept
{
    if (completion_owner)
        return completion_owner->dispatcher->worker_count ();
    const std::lock_guard lock (completion_creation_mutex);
    const auto dispatcher = active_completion_dispatcher.lock ();
    return dispatcher != nullptr ? dispatcher->worker_count () : 0;
}

task_scheduler_t
capture_runtime_native_continuation_scheduler (completion_dispatcher_borrow_t *dispatcher)
{
    const auto *hooks = runtime_execution_hooks.load (std::memory_order_acquire);
    return hooks != nullptr ? hooks->capture_scheduler (dispatcher) : task_scheduler_t{};
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

namespace zlink::framework
{
completion_owner_t::completion_owner_t () : _state (detail::create_completion_owner ())
{
}
completion_owner_t::~completion_owner_t () noexcept
{
    close ();
}
void completion_owner_t::close ()
{
    detail::close_completion_owner (_state);
}
}
