/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "runtime/timers/async_delay.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <boost/asio/system_executor.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdlib>
#include <string>
#include <deque>
#include <functional>
#include <iostream>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>

static_assert (requires (zlink::framework::request_call_t<int> &call) {
    call.async ().admitted ();
});
static_assert (requires (zlink::framework::channel_request_call_t &call) {
    call.async<int> ().admitted ();
});
static_assert (
  std::is_same_v<decltype (std::declval<zlink::framework::request_call_t<int> &> ().yield ()),
                 zlink::framework::task_t<int>>);

namespace
{

zlink::framework::request_task_t<int>
retain_request_owner (std::shared_ptr<int> owner, zlink::framework::request_task_t<int> child)
{
    const auto reply = co_await zlink::framework::detail::request_submission (std::move (child));
    co_return reply + *owner;
}

zlink::framework::request_task_t<int> staged_request (zlink::framework::task_t<void> acceptance,
                                                      zlink::framework::task_t<int> reply)
{
    co_await acceptance;
    co_await zlink::framework::detail::request_admitted_t{};
    co_return co_await reply;
}

bool verify_request_admission_stages ()
{
    using namespace zlink::framework;
    task_completion_source_t<void> first_admission, second_admission;
    task_completion_source_t<int> first_reply, second_reply;
    auto first = staged_request (first_admission.task (), first_reply.task ());
    auto second = staged_request (second_admission.task (), second_reply.task ());
    auto admitted = first.admitted ();
    auto other_admitted = second.admitted ();
    if (admitted.await_ready () || first.await_ready () || other_admitted.await_ready ())
        return false;
    first_admission.complete (result_t<void>::success ());
    if (!admitted.result () || first.await_ready () || other_admitted.await_ready ()
        || second.await_ready ())
        return false;
    first_reply.complete (result_t<int>::success (42));
    if (first.result ().value () != 42 || second.await_ready ())
        return false;
    const auto cancelled = detail::make_cancellation_exception ("admission cancelled");
    second_admission.complete (detail::result_access_t::failure<void> (cancelled));
    if (other_admitted.result ().exception () != second.result ().exception ())
        return false;
    task_completion_source_t<void> expired_admission;
    task_completion_source_t<int> expired_reply;
    auto expired = staged_request (expired_admission.task (), expired_reply.task ());
    expired_admission.complete (detail::boundary_failure<void> (detail::boundary_error_t::timed_out,
                                                                "admission deadline expired"));
    auto expired_stage = expired.admitted ();
    if (expired_stage.result ().exception () != expired.result ().exception ())
        return false;

    // The local owner accepts the call while its handler reply remains pending.
    task_completion_source_t<int> local_reply;
    request_call_t<int> local ("local.request", [&] (const auto &, auto, const auto &) {
        auto accepted = std::make_shared<task_completion_source_t<void>> ();
        accepted->complete (result_t<void>::success ());
        return request_task_t<int> (local_reply.task (), std::move (accepted));
    });
    auto local_result = local.async ();
    auto local_admitted = local_result.admitted ();
    if (!local_admitted.await_ready () || !local_admitted.result () || local_result.await_ready ())
        return false;
    local_reply.complete (result_t<int>::success (7));
    if (local_result.result ().value () != 7)
        return false;

    // Admission failure closes the reply even if its physical terminal is later.
    auto rejected = std::make_shared<task_completion_source_t<void>> ();
    task_completion_source_t<int> late_reply;
    request_task_t<int> rejection (late_reply.task (), rejected);
    auto rejected_stage = rejection.admitted ();
    const auto cause = detail::make_cancellation_exception ("source admission cancelled");
    rejected->complete (detail::result_access_t::failure<void> (cause));
    if (!rejection.await_ready () || !rejected_stage.await_ready ()
        || rejection.result ().exception () != rejected_stage.result ().exception ())
        return false;
    std::promise<void> owner_released;
    auto released = owner_released.get_future ();
    auto owner = std::shared_ptr<int> (new int (1), [&] (int *value) {
        delete value;
        owner_released.set_value ();
    });
    const std::weak_ptr<int> retained = owner;
    auto parent = retain_request_owner (owner, std::move (rejection));
    owner.reset ();
    const bool parent_retained =
      parent.await_ready () && parent.result ().exception () == cause && !retained.expired ();
    late_reply.complete (result_t<int>::success (99));
    released.get ();
    return parent_retained && retained.expired () && parent.result ().exception () == cause;
}

// A configured request terminal can perform an infrastructure lookup before
// submitting its application request. These are distinct admission facts.
zlink::framework::request_task_t<int>
request_after_lookup (zlink::framework::request_task_t<int> lookup,
                      zlink::framework::task_t<void> application_acceptance,
                      zlink::framework::task_t<int> application_reply)
{
    (void) co_await lookup;
    co_await application_acceptance;
    co_await zlink::framework::detail::request_admitted_t{};
    co_return co_await application_reply;
}

bool verify_lookup_admission_isolation ()
{
    using namespace zlink::framework;
    task_completion_source_t<void> lookup_acceptance, acceptance;
    task_completion_source_t<int> lookup_reply, reply;
    request_call_t<int> call ("lookup.then.request", [&] (const auto &, auto, const auto &) {
        return request_after_lookup (
          staged_request (lookup_acceptance.task (), lookup_reply.task ()), acceptance.task (),
          reply.task ());
    });
    auto pending = call.async ();
    auto admitted = pending.admitted ();
    lookup_acceptance.complete (result_t<void>::success ());
    const bool isolated_at_lookup_admission = !admitted.await_ready () && !pending.await_ready ();
    lookup_reply.complete (result_t<int>::success (1));
    const bool isolated_at_lookup_reply = !admitted.await_ready () && !pending.await_ready ();
    acceptance.complete (result_t<void>::success ());
    const bool accepted_before_reply = admitted.result () && !pending.await_ready ();
    reply.complete (result_t<int>::success (42));
    if (!isolated_at_lookup_admission || !isolated_at_lookup_reply)
        std::cerr << "infrastructure request completed application admitted before submission\n";
    return isolated_at_lookup_admission && isolated_at_lookup_reply && accepted_before_reply
           && pending.result ().value () == 42;
}

bool verify_reply_success_completes_admission ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> physical;
    auto source = std::make_shared<task_completion_source_t<void>> ();
    request_task_t<int> pending (physical.task (), source);
    auto admitted = pending.admitted ();
    physical.complete (result_t<int>::success (42));
    if (!admitted.await_ready ()) {
        std::cerr << "successful reply left request admitted pending\n";
        return false;
    }
    const auto first = admitted.result ().exception ();
    task_completion_source_t<int> late_reply;
    auto rejected = std::make_shared<task_completion_source_t<void>> ();
    request_task_t<int> failed (late_reply.task (), rejected);
    auto failed_admission = failed.admitted ();
    const auto cancellation = detail::make_cancellation_exception ("admission already failed");
    rejected->complete (detail::result_access_t::failure<void> (cancellation));
    late_reply.complete (result_t<int>::success (99));
    if (failed.result ().exception () != cancellation
        || failed_admission.result ().exception () != cancellation)
        return false;
    request_task_t<int> immediate (result_t<int>::success (7));
    auto immediate_admission = immediate.admitted ();
    if (!immediate_admission.await_ready () || !immediate_admission.result ()
        || immediate.result ().value () != 7)
        return false;
    source->complete (detail::result_access_t::failure<void> (
      detail::make_cancellation_exception ("late admission failure")));
    return admitted.result () && admitted.result ().exception () == first
           && pending.result ().value () == 42;
}

thread_local int native_await_ambient_value = 0;
thread_local bool inside_native_completion = false;

struct ambient_guard_t
{
    explicit ambient_guard_t (int value) : previous (native_await_ambient_value)
    {
        native_await_ambient_value = value;
    }
    ~ambient_guard_t () { native_await_ambient_value = previous; }
    int previous;
};

std::shared_ptr<void> capture_test_ambient ()
{
    return std::make_shared<int> (native_await_ambient_value);
}

std::shared_ptr<void> enter_test_ambient (const std::shared_ptr<void> &snapshot)
{
    return std::make_shared<ambient_guard_t> (*std::static_pointer_cast<int> (snapshot));
}

class test_serial_turn_t final : public zlink::framework::detail::serial_turn_t
{
  public:
    explicit test_serial_turn_t (std::stop_token cancellation = {}) : cancellation (cancellation) {}
    std::stop_token wait_cancellation () const override { return cancellation; }
    bool release () override
    {
        released_value = true;
        return true;
    }
    bool released () const override { return released_value; }
    bool claim_yield () override { return !released_value && !std::exchange (yield_claimed, true); }
    zlink::framework::detail::task_scheduler_t resume_scheduler () override
    {
        return [] (std::function<void ()> work) { work (); };
    }
    bool belongs_to (const void *owner) const noexcept override { return owner == this; }
    bool is_after_active_phase () const noexcept override { return false; }
    bool allows_yield () const noexcept override { return true; }
    zlink::framework::result_t<void>
    defer (std::function<zlink::framework::task_t<void> ()> work,
           std::function<void ()>,
           std::shared_ptr<zlink::framework::detail::deferred_barrier_t>) override
    {
        auto pending = std::make_shared<zlink::framework::task_t<void>> (work ());
        zlink::framework::detail::observe_task_terminal (
          *pending,
          [pending, work = std::move (work)] (const zlink::framework::result_t<void> &) {});
        return zlink::framework::result_t<void>::success ();
    }
    void cancel_deferred () noexcept override {}
    bool finish_handler (std::function<void ()> completion, bool) override
    {
        completion ();
        return release ();
    }

  private:
    bool released_value = false;
    bool yield_claimed = false;
    std::stop_token cancellation;
};

bool verify_released_turn_wrapper_handoff ()
{
    using namespace zlink::framework;
    static thread_local bool delivering = false;
    for (const bool selected_completion_turn : {false, true}) {
        task_completion_source_t<int> source;
        auto pending = source.task ();
        std::promise<std::pair<std::thread::id, bool>> resumed;
        auto done = resumed.get_future ();
        auto turn = std::make_shared<test_serial_turn_t> ();
        (void) turn->release ();
        {
            const detail::serial_turn_scope_t scope (turn);
            detail::observe_task_completion (pending, [&] (const auto &result) {
                resumed.set_value ({std::this_thread::get_id (),
                                    result.value () == 1770
                                      && detail::capture_current_serial_turn () == nullptr
                                      && detail::current_completion_owner () != nullptr
                                      && delivering == selected_completion_turn});
            });
        }
        std::promise<std::thread::id> delivered;
        auto completed = delivered.get_future ();
        auto complete = [&] {
            delivering = true;
            source.complete (result_t<int>::success (1770));
            delivering = false;
            delivered.set_value (std::this_thread::get_id ());
        };
        if (selected_completion_turn)
            detail::dispatch_completion (complete);
        else
            complete ();
        const auto completion_thread = completed.get ();
        const auto [resumed_thread, kept_owner] = done.get ();
        const bool passed =
          (resumed_thread == completion_thread) == selected_completion_turn && kept_owner;
        std::cout << "released-turn-wrapper selected-completion=" << selected_completion_turn
                  << " passed=" << passed << std::endl;
        if (!passed)
            return false;
    }
    return true;
}

enum class completion_scope_probe_t
{
    serial_turn,
    application_job,
    wait_owner,
    infrastructure
};

bool verify_completion_scope (bool stopped_host,
                              completion_scope_probe_t scope_kind,
                              bool active_host = false)
{
    using namespace zlink::framework;
    static thread_local bool completing_in_scope = false;
    if (stopped_host || active_host) {
        runtime::configure_handler_coroutine_executor (1);
        runtime::install_host_context_hooks ();
    }
    task_completion_source_t<int> source;
    auto pending = source.task ();
    std::promise<bool> observed;
    auto result = observed.get_future ();
    detail::observe_task_completion (pending, [&] (const auto &) {
        observed.set_value (!completing_in_scope
                            && detail::capture_current_serial_turn () == nullptr
                            && detail::application_job_context_t::current () == nullptr
                            && !detail::current_wait_owner ().stop_possible ()
                            && !detail::current_infrastructure_wait ());
    });
    if (stopped_host)
        runtime::shutdown_handler_coroutine_executor ();
    std::promise<void> delivered;
    auto done = delivered.get_future ();
    detail::dispatch_completion ([&] {
        {
            std::optional<detail::serial_turn_scope_t> turn_scope;
            const int application_job = 1770;
            const auto *previous_job = detail::application_job_context_t::current ();
            std::stop_source wait_owner;
            const auto previous_owner = detail::current_wait_owner ();
            const bool previous_infrastructure = detail::current_infrastructure_wait ();
            if (scope_kind == completion_scope_probe_t::serial_turn) {
                turn_scope.emplace (std::make_shared<test_serial_turn_t> ());
            } else if (scope_kind == completion_scope_probe_t::application_job) {
                detail::application_job_context_t::exchange (&application_job);
            } else if (scope_kind == completion_scope_probe_t::wait_owner) {
                detail::exchange_wait_owner (wait_owner.get_token ());
            } else {
                detail::exchange_infrastructure_wait (true);
            }
            completing_in_scope = true;
            source.complete (result_t<int>::success (1770));
            completing_in_scope = false;
            detail::application_job_context_t::exchange (previous_job);
            detail::exchange_wait_owner (previous_owner);
            detail::exchange_infrastructure_wait (previous_infrastructure);
        }
        delivered.set_value ();
    });
    done.get ();
    const bool isolated = result.get ();
    if (active_host)
        runtime::shutdown_handler_coroutine_executor ();
    std::cout << "completion-scope active-host=" << active_host << " stopped-host=" << stopped_host
              << " scope=" << static_cast<int> (scope_kind) << " isolated=" << isolated
              << std::endl;
    detail::set_runtime_execution_hooks (nullptr);
    detail::set_ambient_context_hooks (nullptr);
    return isolated;
}

bool verify_owned_completion_turn (bool with_job, bool infrastructure_completer)
{
    using namespace zlink::framework;
    static thread_local bool delivering = false;
    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    task_completion_source_t<int> source;
    auto pending = source.task ();
    std::stop_source wait_owner;
    auto turn = std::make_shared<test_serial_turn_t> (wait_owner.get_token ());
    const int job = 1770;
    const void *caller_job = with_job ? &job : nullptr;
    std::promise<bool> observed;
    auto result = observed.get_future ();
    {
        const detail::serial_turn_scope_t turn_scope (turn);
        const detail::ambient_context_scope_t invocation (nullptr, caller_job,
                                                          wait_owner.get_token ());
        detail::observe_task_completion (pending, [&] (const auto &value) {
            observed.set_value (
              value.value () == 1770 && detail::capture_current_serial_turn () == turn
              && detail::application_job_context_t::current () == caller_job
              && detail::current_wait_owner () == wait_owner.get_token ()
              && !detail::current_infrastructure_wait () && delivering != infrastructure_completer);
        });
    }
    std::promise<void> delivered;
    auto done = delivered.get_future ();
    detail::dispatch_completion ([&] {
        {
            const detail::serial_turn_scope_t turn_scope (turn);
            const detail::ambient_context_scope_t invocation (nullptr, caller_job, {}, false,
                                                              infrastructure_completer);
            delivering = true;
            source.complete (result_t<int>::success (1770));
            delivering = false;
        }
        delivered.set_value ();
    });
    done.get ();
    const bool passed = result.get ();
    runtime::shutdown_handler_coroutine_executor ();
    detail::set_runtime_execution_hooks (nullptr);
    detail::set_ambient_context_hooks (nullptr);
    std::cout << "owned-completion with-job=" << with_job
              << " infrastructure-completer=" << infrastructure_completer << " passed=" << passed
              << std::endl;
    return passed;
}

class native_async_state_t final : public zlink::detail::async_result_state_t<int>
{
  public:
    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (mutex);
        return terminal;
    }

    bool suspend (std::coroutine_handle<> continuation_,
                  zlink::detail::async_continuation_scheduler_t scheduler_) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (terminal)
            return false;
        continuation = continuation_;
        scheduler = std::move (scheduler_);
        return true;
    }

    int take () override { return value; }
    void detach () noexcept override {}
    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (continuation == continuation_)
            continuation = {};
    }

    void complete (int value_)
    {
        std::coroutine_handle<> next;
        zlink::detail::async_continuation_scheduler_t next_scheduler;
        {
            std::lock_guard<std::mutex> lock (mutex);
            value = value_;
            terminal = true;
            next = std::exchange (continuation, {});
            next_scheduler = std::move (scheduler);
        }
        auto work = [next] { next.resume (); };
        if (next_scheduler)
            next_scheduler (std::move (work));
        else
            work ();
    }

  private:
    mutable std::mutex mutex;
    std::coroutine_handle<> continuation{};
    zlink::detail::async_continuation_scheduler_t scheduler;
    int value = 0;
    bool terminal = false;
};

zlink::framework::task_t<int>
await_native_binding_result (const std::shared_ptr<native_async_state_t> &state,
                             const std::shared_ptr<test_serial_turn_t> &turn,
                             std::atomic<int> &observed_ambient,
                             std::atomic<bool> &observed_serial_turn,
                             std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (inside_native_completion, std::memory_order_release);
    observed_ambient.store (native_await_ambient_value, std::memory_order_release);
    observed_serial_turn.store (zlink::framework::detail::capture_current_serial_turn () == turn,
                                std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int>
await_native_binding_outside_completion (const std::shared_ptr<native_async_state_t> &state,
                                         std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (inside_native_completion, std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int> await_shared (zlink::framework::task_t<int> &task, int offset)
{
    const auto value = co_await task;
    co_return value + offset;
}

zlink::framework::task_t<int> timeout_task ()
{
    co_return zlink::framework::detail::boundary_failure<int> (
      zlink::framework::detail::boundary_error_t::timed_out, "timeout preserved");
}

zlink::framework::task_t<int> await_timeout ()
{
    auto task = timeout_task ();
    co_return co_await task;
}

zlink::framework::task_t<int> await_async_delay (zlink::framework::detail::delay_owner_t &owner,
                                                 std::chrono::milliseconds duration,
                                                 int value)
{
    co_await zlink::framework::detail::delay (owner, duration);
    co_return value;
}

zlink::framework::task_t<void> observe_void_task (zlink::framework::task_t<void> task)
{
    co_await task;
}

zlink::framework::task_t<void> observe_void_with_lifetime (zlink::framework::task_t<void> task,
                                                           std::shared_ptr<int> lifetime)
{
    co_await task;
    (void) lifetime;
}

struct before_registration_t
{
    zlink::framework::task_t<void> task;
    std::function<void ()> before_registration;

    bool await_ready ()
    {
        const bool ready = task.await_ready ();
        before_registration ();
        return ready;
    }
    void await_suspend (std::coroutine_handle<> continuation) { task.await_suspend (continuation); }
    void await_resume () { task.await_resume (); }
};

zlink::framework::task_t<void>
observe_registration_boundary (zlink::framework::task_t<void> task,
                               std::function<void ()> before_registration)
{
    co_await before_registration_t{std::move (task), std::move (before_registration)};
}

bool verify_rejected_delivery_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    auto source = std::make_unique<task_completion_source_t<void>> ();
    std::optional<task_t<void>> original (source->task ());
    auto outside = observe_void_task (source->task ());
    auto marker = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = marker;
    auto view = detail::with_task_resume_scheduler (source->task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "executor rejected before owner cancellation");
    });
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_void_with_lifetime (std::move (view), std::move (marker));
    }();
    std::atomic_int terminal_calls{0};
    std::promise<void> terminal_delivered;
    auto terminal_ready = terminal_delivered.get_future ();
    detail::observe_task_completion (owned, [&] (const result_t<void> &) {
        ++terminal_calls;
        terminal_delivered.set_value ();
    });
    bool first = false, duplicate = true;
    std::exception_ptr failure;
    std::thread completion ([&] {
        try {
            first = source->complete (result_t<void>::success ());
            duplicate = source->complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
    });
    completion.join ();
    if (failure || !first || duplicate || !original->result () || !outside.result ()
        || owned.result_for (std::chrono::milliseconds (0)) || terminal_calls != 0)
        return false;
    original.reset ();
    source.reset ();
    if (lifetime.expired ())
        return false;
    owner.request_stop ();
    const auto cancelled = owned.result ();
    terminal_ready.get ();
    owner.request_stop ();
    return !cancelled && cancelled.error_kind () == framework_error_kind_t::shutting_down
           && terminal_calls == 1 && lifetime.expired ();
}

bool verify_rejected_ready_registration ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> source;
    auto view = detail::with_task_resume_scheduler (source.task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "ready delivery rejected before owner cancellation");
    });
    bool first = false;
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_registration_boundary (
          std::move (view), [&] { first = source.complete (result_t<void>::success ()); });
    }();
    std::atomic_int terminal_calls{0};
    std::promise<void> terminal_delivered;
    auto terminal_ready = terminal_delivered.get_future ();
    detail::observe_task_completion (owned, [&] (const result_t<void> &) {
        ++terminal_calls;
        terminal_delivered.set_value ();
    });
    if (!first || !source.task ().result () || source.complete (result_t<void>::success ())
        || owned.result_for (std::chrono::milliseconds (0)) || terminal_calls != 0)
        return false;
    owner.request_stop ();
    const auto cancelled = owned.result ();
    terminal_ready.get ();
    return !cancelled && cancelled.error_kind () == framework_error_kind_t::shutting_down
           && terminal_calls == 1;
}

bool verify_stopped_registration_state_lifetime ()
{
    using namespace zlink::framework;
    for (const bool complete : {false, true}) {
        std::stop_source owner;
        auto source = std::make_unique<task_completion_source_t<void>> ();
        auto task = source->task ();
        auto observed = [&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            return observe_registration_boundary (std::move (task), [&] {
                if (complete)
                    source->complete (result_t<void>::success ());
                source.reset ();
                owner.request_stop ();
            });
        }();
        const auto cancelled = observed.result ();
        if (source || cancelled || cancelled.error_kind () != framework_error_kind_t::shutting_down)
            return false;
    }
    return true;
}

// A wait registered outside any execution turn has no owner that can cancel
// it. When the context that captured its scheduler has stopped, the completing
// thread delivers the result instead of terminating the process.
bool verify_ownerless_wait_after_context_stop ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> source;
    auto view = detail::with_task_resume_scheduler (source.task (), [] (std::function<void ()> &&) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "registered context stopped");
    });
    auto waiter = await_shared (view, 1);
    if (!source.complete (result_t<int>::success (41)))
        return false;
    const auto delivered = waiter.result_for (std::chrono::seconds (1));
    return delivered && delivered->value () == 42;
}

bool verify_cancelled_registration_lease ()
{
    using namespace zlink::framework;
    for (const bool complete : {false, true}) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        auto marker = std::make_shared<int> (1);
        std::weak_ptr<int> scheduler_lease = marker;
        auto turn = std::make_shared<test_serial_turn_t> (owner.get_token ());
        std::weak_ptr<test_serial_turn_t> turn_lease = turn;
        auto view = detail::with_task_resume_scheduler (
          source.task (), [marker = std::move (marker)] (std::function<void ()>) {
              (void) marker;
              throw framework_exception_t (framework_error_kind_t::shutting_down,
                                           "scheduler rejects while owner is alive");
          });
        auto owned = [&] {
            const detail::serial_turn_scope_t scope (turn);
            return observe_void_task (std::move (view));
        }();
        turn.reset ();
        if (complete && !source.complete (result_t<void>::success ()))
            return false;
        if (turn_lease.expired () || scheduler_lease.expired ())
            return false;
        owner.request_stop ();
        const auto cancelled = owned.result ();
        if (cancelled || cancelled.error_kind () != framework_error_kind_t::shutting_down
            || !turn_lease.expired () || !scheduler_lease.expired ())
            return false;
        if (!complete && !source.complete (result_t<void>::success ()))
            return false;
        if (source.complete (result_t<void>::success ()) || !source.task ().result ())
            return false;
    }
    return true;
}

bool verify_observer_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    std::weak_ptr<int> pending_marker;
    int pending_calls = 0;
    {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        task_completion_source_t<int> source;
        auto task = source.task ();
        auto marker = std::make_shared<int> (1);
        pending_marker = marker;
        detail::observe_task_completion (task, [marker, &pending_calls] (const result_t<int> &) {
            (void) marker;
            ++pending_calls;
        });
    }
    if (!pending_marker.expired () || pending_calls != 0)
        return false;
    owner.request_stop ();
    if (pending_calls != 0)
        return false;

    std::stop_source queued_owner;
    std::deque<std::function<void ()>> queue;
    std::weak_ptr<int> queued_marker;
    int queued_calls = 0;
    {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, queued_owner.get_token ());
        task_completion_source_t<int> source;
        auto task =
          detail::with_task_resume_scheduler (source.task (), [&] (std::function<void ()> work) {
              queue.push_back (std::move (work));
          });
        auto marker = std::make_shared<int> (18);
        queued_marker = marker;
        detail::observe_task_completion (
          task, [marker, &queued_calls] (const result_t<int> &result) {
              if (result.value () != *marker)
                  throw std::logic_error ("queued observer result was lost");
              ++queued_calls;
          });
        if (!source.complete (result_t<int>::success (18)))
            return false;
    }
    if (queued_marker.expired () || queue.size () != 1 || queued_calls != 0)
        return false;
    auto work = std::move (queue.front ());
    queue.pop_front ();
    work ();
    queued_owner.request_stop ();
    return queued_marker.expired () && queued_calls == 1;
}

bool verify_released_observer_owner ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> pending_source, ready_source;
    auto pending = pending_source.task ();
    auto ready = ready_source.task ();
    ready_source.complete (result_t<int>::success (1));
    auto turn = std::make_shared<test_serial_turn_t> ();
    std::weak_ptr<test_serial_turn_t> released_turn = turn;
    turn->release ();
    std::promise<void> pending_delivered;
    auto delivered = pending_delivered.get_future ();
    int calls = 0;
    bool restored_released_turn = false;
    {
        const detail::serial_turn_scope_t scope (turn);
        detail::observe_task_completion (ready, [&] (const result_t<int> &) { ++calls; });
        detail::observe_task_completion (pending, [&] (const result_t<int> &) {
            restored_released_turn = static_cast<bool> (detail::capture_current_serial_turn ());
            ++calls;
            pending_delivered.set_value ();
        });
    }
    turn.reset ();
    if (!released_turn.expired ())
        return false;
    pending_source.complete (result_t<int>::success (1));
    delivered.get ();
    return calls == 2 && !restored_released_turn;
}

bool verify_registration_completion_cancellation_race ()
{
    using namespace zlink::framework;
    constexpr int race_cases = 32;
    for (int i = 0; i != race_cases; ++i) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        std::barrier start (4);
        std::optional<task_t<void>> observed;
        std::atomic_int observer_calls{0};
        std::promise<void> observer_delivered;
        auto delivered = observer_delivered.get_future ();
        bool first = false, duplicate = true;
        std::exception_ptr completion_failure;
        std::thread registration ([&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            start.arrive_and_wait ();
            auto task = source.task ();
            detail::observe_task_completion (task, [&] (const result_t<void> &) {
                ++observer_calls;
                observer_delivered.set_value ();
            });
            observed.emplace (observe_void_task (std::move (task)));
        });
        std::thread completion ([&] {
            start.arrive_and_wait ();
            try {
                first = source.complete (result_t<void>::success ());
                duplicate = source.complete (result_t<void>::success ());
            }
            catch (...) {
                completion_failure = std::current_exception ();
            }
        });
        std::thread cancellation ([&] {
            start.arrive_and_wait ();
            owner.request_stop ();
        });
        start.arrive_and_wait ();
        registration.join ();
        completion.join ();
        cancellation.join ();
        const auto result = observed->result ();
        delivered.get ();
        if (completion_failure || !first || duplicate || observer_calls != 1
            || (!result && result.error_kind () != framework_error_kind_t::shutting_down)
            || !source.task ().result ())
            return false;
    }
    return true;
}

struct nested_cleanup_probe_t
{
    bool child_finished = false;
    bool child_before_parent = false;
    int parent_observations = 0;
    std::promise<void> parent_observed;
};

// The child's destructor writes parent-owned memory, so the parent must
// resume only after the child's scope is gone.
struct nested_child_cleanup_t
{
    int &parent_value;
    nested_cleanup_probe_t &probe;
    ~nested_child_cleanup_t ()
    {
        parent_value = 42;
        probe.child_finished = true;
    }
};

zlink::framework::task_t<void>
cleanup_nested_child (zlink::framework::task_completion_source_t<void> &source,
                      int &parent_value,
                      nested_cleanup_probe_t &probe)
{
    nested_child_cleanup_t cleanup{parent_value, probe};
    auto pending = source.task ();
    try {
        co_await pending;
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () != zlink::framework::framework_error_kind_t::shutting_down)
            throw;
    }
    co_return;
}

zlink::framework::task_t<int>
return_nested_child (zlink::framework::task_completion_source_t<void> &source,
                     int &parent_value,
                     nested_cleanup_probe_t &probe)
{
    nested_child_cleanup_t cleanup{parent_value, probe};
    auto pending = source.task ();
    co_await pending;
    co_return 7;
}

zlink::framework::task_t<void>
cleanup_nested_parent (zlink::framework::task_completion_source_t<void> &source,
                       nested_cleanup_probe_t &probe)
{
    auto parent_value = std::make_unique<int> (0);
    auto child = cleanup_nested_child (source, *parent_value, probe);
    zlink::framework::detail::observe_task_completion (
      child, [&probe] (const zlink::framework::result_t<void> &result) {
          if (result && probe.child_finished)
              ++probe.parent_observations;
          probe.parent_observed.set_value ();
      });
    co_await child;
    probe.child_before_parent = probe.child_finished && *parent_value == 42;
}

bool verify_nested_cancellation_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> source;
    nested_cleanup_probe_t probe;
    auto observed = probe.parent_observed.get_future ();
    auto parent = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return cleanup_nested_parent (source, probe);
    }();
    owner.request_stop ();
    const auto result = parent.result ();
    const bool original_pending = !source.task ().result_for (std::chrono::milliseconds::zero ());
    const bool first = source.complete (result_t<void>::success ());
    const bool duplicate = source.complete (result_t<void>::success ());
    observed.get ();
    return result && probe.child_before_parent && probe.parent_observations == 1 && original_pending
           && first && !duplicate;
}

// An explicit co_return publishes only after the child's locals are destroyed.
zlink::framework::task_t<void>
return_nested_parent (zlink::framework::task_completion_source_t<void> &void_source,
                      zlink::framework::task_completion_source_t<void> &typed_source,
                      nested_cleanup_probe_t &probe)
{
    auto void_value = std::make_unique<int> (0);
    auto void_child = cleanup_nested_child (void_source, *void_value, probe);
    co_await void_child;
    const bool void_ordered = probe.child_finished && *void_value == 42;
    probe.child_finished = false;
    auto typed_value = std::make_unique<int> (0);
    auto typed_child = return_nested_child (typed_source, *typed_value, probe);
    const auto returned = co_await typed_child;
    probe.child_before_parent =
      void_ordered && probe.child_finished && *typed_value == 42 && returned == 7;
}

bool verify_nested_return_lifetime ()
{
    using namespace zlink::framework;
    task_completion_source_t<void> void_source;
    task_completion_source_t<void> typed_source;
    nested_cleanup_probe_t probe;
    auto parent = return_nested_parent (void_source, typed_source, probe);
    const bool void_completed = void_source.complete (result_t<void>::success ());
    const bool typed_completed = typed_source.complete (result_t<void>::success ());
    const auto result = parent.result ();
    return void_completed && typed_completed && result && probe.child_before_parent;
}

bool verify_void_completion_rejection_race (bool cancel_before_rejection)
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> trigger;
    // cancellation-and-shutdown §5.1: only the application-source waiter may
    // finish independently when its delivery owner shuts down.
    auto producer = trigger.task ();
    std::promise<void> submit_entered, allow_rejection;
    auto entered = submit_entered.get_future ();
    auto reject = allow_rejection.get_future ();
    std::promise<void> producer_observed;
    auto observed = producer_observed.get_future ();
    bool producer_succeeded = false;
    detail::observe_task_completion (producer, [&] (const result_t<void> &result) {
        producer_succeeded = static_cast<bool> (result);
        producer_observed.set_value ();
    });
    auto rejected_view =
      detail::with_task_resume_scheduler (std::move (producer), [&] (std::function<void ()>) {
          submit_entered.set_value ();
          reject.wait ();
          throw framework_exception_t (framework_error_kind_t::shutting_down,
                                       "executor stopped after owner cancellation");
      });
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_void_task (std::move (rejected_view));
    }();
    auto outside = observe_void_task (trigger.task ());
    bool first = false, duplicate = true;
    std::exception_ptr failure;
    std::thread completing ([&] {
        try {
            first = trigger.complete (result_t<void>::success ());
            duplicate = trigger.complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
    });
    entered.wait ();
    if (cancel_before_rejection)
        owner.request_stop ();
    allow_rejection.set_value ();
    completing.join ();
    if (!cancel_before_rejection) {
        if (owned.result_for (std::chrono::milliseconds (0)))
            return false;
        owner.request_stop ();
    }
    const auto cancelled = owned.result ();
    observed.get ();
    return !failure && first && !duplicate && !cancelled
           && cancelled.error_kind () == framework_error_kind_t::shutting_down && producer_succeeded
           && outside.result ();
}

bool verify_rejection_retain_cancellation_race ()
{
    using namespace zlink::framework;
    for (int i = 0; i != 32; ++i) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        std::barrier rejection (2);
        auto view =
          detail::with_task_resume_scheduler (source.task (), [&] (std::function<void ()>) {
              rejection.arrive_and_wait ();
              throw framework_exception_t (framework_error_kind_t::shutting_down,
                                           "executor rejection races owner cancellation");
          });
        auto owned = [&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            return observe_void_task (std::move (view));
        }();
        auto outside = observe_void_task (source.task ());
        std::atomic_int terminal_calls{0};
        std::promise<void> terminal_delivered;
        auto terminal_ready = terminal_delivered.get_future ();
        detail::observe_task_completion (owned, [&] (const result_t<void> &) {
            ++terminal_calls;
            terminal_delivered.set_value ();
        });
        std::thread cancellation ([&] {
            rejection.arrive_and_wait ();
            owner.request_stop ();
        });
        bool first = false;
        std::exception_ptr failure;
        try {
            first = source.complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
        cancellation.join ();
        const auto cancelled = owned.result ();
        terminal_ready.get ();
        if (failure || !first || source.complete (result_t<void>::success ()) || cancelled
            || cancelled.error_kind () != framework_error_kind_t::shutting_down
            || terminal_calls != 1 || !outside.result () || !source.task ().result ())
            return false;
    }
    return true;
}

/* Cancellation §5.1: the owner ends a wait whose resume was rejected. A stop
 * request whose owner callback has not run yet still leaves the wait to that
 * callback, so the registration must outlive the rejected delivery. The extra
 * stop callback is registered after the wait's callback and holds the stop
 * request open between setting the stop flag and running the wait's callback. */
bool verify_rejection_after_owner_stop_request ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> source;
    auto view = detail::with_task_resume_scheduler (source.task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "executor rejection after owner stop request");
    });
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_void_task (std::move (view));
    }();
    std::promise<void> stop_entered, delivery_finished;
    auto entered = stop_entered.get_future ();
    auto finished = delivery_finished.get_future ();
    std::stop_callback hold_stop (owner.get_token (), [&] {
        stop_entered.set_value ();
        finished.wait ();
    });
    std::thread stopping ([&] { owner.request_stop (); });
    entered.wait ();
    const bool first = source.complete (result_t<void>::success ());
    delivery_finished.set_value ();
    stopping.join ();
    // Every stop callback has returned, so the owner has ended the wait.
    const auto cancelled = owned.result_for (std::chrono::milliseconds (0));
    if (!cancelled)
        std::cerr << "owner stop request lost a rejected wait\n";
    return first && cancelled && !*cancelled
           && cancelled->error_kind () == framework_error_kind_t::shutting_down;
}

enum class delivery_defect_t
{
    pending_observer,
    ready_observer,
    terminal_observer,
    scheduled_observer,
    scheduled_after_delivery,
    owned_observer,
    cancellation_observer,
    queue_cancellation,
    reservation_transfer,
    native_continuation,
    count
};
constexpr int delivery_defect_exit = 86;

void run_delivery_defect (delivery_defect_t defect)
{
    using namespace zlink::framework;
    std::set_terminate ([] { std::_Exit (delivery_defect_exit); });
    const auto terminate_handler = std::get_terminate ();
    if (defect == delivery_defect_t::native_continuation) {
        runtime::coroutine_executor_t executor (1);
        executor.post_native_continuation ([terminate_handler] {
            // MSVC keeps the terminate handler per thread.
            std::set_terminate (terminate_handler);
            throw std::logic_error ("native continuation delivery defect");
        });
        executor.drain ();
        return;
    }
    if (defect == delivery_defect_t::queue_cancellation
        || defect == delivery_defect_t::reservation_transfer) {
        runtime::offload_executor_t executor;
        runtime::serial_execution_queue_t queue (executor);
        if (defect == delivery_defect_t::reservation_transfer) {
            runtime::serial_work_options_t options;
            options.transfer_owner_reservation = [] {
                throw std::logic_error ("reservation transfer defect");
            };
            queue.try_post_async (
              "reservation-defect", [] (auto complete) { complete ([] {}); }, options);
        } else {
            std::promise<void> started;
            auto running = started.get_future ();
            runtime::serial_execution_queue_t::async_completion_t finish;
            const auto submission = queue.try_post_cancellable_async (
              "cancellation-defect",
              [&] (auto complete) {
                  finish = std::move (complete);
                  started.set_value ();
              },
              [] { throw std::logic_error ("cancellation delivery defect"); });
            if (!submission)
                return;
            running.get ();
            queue.cancel_submission (submission.value ());
            finish ([] {});
        }
        return;
    }
    task_completion_source_t<void> source;
    auto task = source.task ();
    std::stop_source owner;
    const detail::ambient_context_scope_t scope (
      nullptr, nullptr,
      defect == delivery_defect_t::owned_observer
          || defect == delivery_defect_t::cancellation_observer
          || defect == delivery_defect_t::scheduled_after_delivery
        ? owner.get_token ()
        : std::stop_token{});
    const auto callback = [terminate_handler] (const result_t<void> &) {
        // MSVC keeps the terminate handler per thread.
        std::set_terminate (terminate_handler);
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "internal observer defect");
    };
    switch (defect) {
        case delivery_defect_t::ready_observer:
            source.complete (result_t<void>::success ());
            detail::observe_task_completion (task, callback);
            break;
        case delivery_defect_t::terminal_observer:
            detail::observe_task_terminal (task, callback);
            break;
        case delivery_defect_t::scheduled_observer:
            task =
              detail::with_task_resume_scheduler (std::move (task), [] (std::function<void ()>) {
                  throw std::logic_error ("internal scheduler defect");
              });
            detail::observe_task_completion (task, [] (const result_t<void> &) {});
            break;
        case delivery_defect_t::scheduled_after_delivery:
            task = detail::with_task_resume_scheduler (
              std::move (task), [] (std::function<void ()> work) {
                  work ();
                  throw framework_exception_t (framework_error_kind_t::shutting_down,
                                               "scheduler failed after delivery");
              });
            {
                auto observed = observe_void_task (std::move (task));
                source.complete (result_t<void>::success ());
                (void) observed;
                return;
            }
        default:
            detail::observe_task_completion (task, callback);
            break;
    }
    if (defect == delivery_defect_t::cancellation_observer)
        owner.request_stop ();
    else
        source.complete (result_t<void>::success ());
    if (defect == delivery_defect_t::pending_observer
        || defect == delivery_defect_t::owned_observer)
        boost::asio::system_executor{}.context ().join ();
}

bool verify_delivery_defects (const char *executable)
{
    for (int defect = 0; defect < static_cast<int> (delivery_defect_t::count); ++defect) {
        const auto command =
          std::string ("\"") + executable + "\" --delivery-defect " + std::to_string (defect);
        const auto status = std::system (command.c_str ());
#ifdef _WIN32
        if (status != delivery_defect_exit)
#else
        if (status != delivery_defect_exit * 256)
#endif
            return false;
    }
    return true;
}

} // namespace

namespace
{

template <typename T> bool has_standard_cancellation (const zlink::framework::result_t<T> &result)
{
    using namespace zlink::framework;
    if (result || result.error () != nullptr || !result.exception ())
        return false;
    try {
        (void) result.error_kind ();
        return false;
    }
    catch (const framework_exception_t &error) {
        if (error.kind () != framework_error_kind_t::invalid_operation)
            return false;
    }
    try {
        result.value ();
        return false;
    }
    catch (const std::system_error &error) {
        return error.code () == std::errc::operation_canceled;
    }
}

zlink::framework::task_t<int> throw_standard_cancellation ()
{
    throw std::system_error (std::make_error_code (std::errc::operation_canceled));
    co_return 0;
}

bool verify_cancellation_contract ()
{
    using namespace zlink::framework;
    const auto original = detail::make_cancellation_exception ("request cancelled");
    try {
        std::rethrow_exception (original);
        return false;
    }
    catch (const std::system_error &error) {
        if (!detail::is_cancellation_exception (error))
            return false;
    }
    const auto cancelled = detail::result_access_t::failure<int> (original);
    if (cancelled.exception () != original)
        return false;
    if (detail::is_cancellation_exception (
          std::system_error (std::make_error_code (std::errc::timed_out))))
        return false;
    for (const auto state :
         {detail::boundary_error_t::timed_out, detail::boundary_error_t::shutdown,
          detail::boundary_error_t::disconnected, detail::boundary_error_t::closed,
          detail::boundary_error_t::stale_generation}) {
        auto error = detail::make_boundary_exception (state, "boundary failure");
        if (detail::is_cancellation_exception (error))
            return false;
        error = detail::with_error_origin (std::move (error), detail::error_origin_t::framework);
        const auto pointer = std::make_exception_ptr (error);
        const auto boundary = detail::result_access_t::failure<int> (pointer);
        if (boundary.exception () != pointer || !boundary.error ()
            || boundary.error ()->kind () != error.kind ()
            || boundary.error ()->code () != error.code ()
            || detail::boundary_state (*boundary.error ()) != state
            || detail::error_origin (*boundary.error ()) != detail::error_origin_t::framework)
            return false;
    }
    if (!has_standard_cancellation (cancelled))
        return false;
    auto mutable_cancelled = cancelled;
    try {
        mutable_cancelled.value ();
        return false;
    }
    catch (const std::system_error &error) {
        if (error.code () != std::errc::operation_canceled)
            return false;
    }
    const auto propagated = detail::propagate_failure<void> (cancelled, "must retain cancellation");
    if (!has_standard_cancellation (propagated)
        || propagated.exception () != cancelled.exception ())
        return false;
    request_call_t<int> request (cancelled);
    send_call_t send (propagated);
    send_call_t submit ("test.cancelled", [] (const auto &, const auto &) {
        return detail::result_access_t::failure<void> (
          detail::make_cancellation_exception ("submit cancelled"));
    });
    if (!has_standard_cancellation (request.async ().result ())
        || !has_standard_cancellation (send.async ().result ())
        || !has_standard_cancellation (submit.async ().result ())
        || !has_standard_cancellation (throw_standard_cancellation ().result ()))
        return false;
    auto awaiting = request.async ();
    if (!has_standard_cancellation (await_shared (awaiting, 0).result ()))
        return false;
    const auto success = result_t<int>::success (1);
    if (success.error () || success.exception ())
        return false;
    try {
        (void) success.error_kind ();
        return false;
    }
    catch (const framework_exception_t &error) {
        if (error.kind () != framework_error_kind_t::invalid_operation)
            return false;
    }
    const auto failure = result_t<int>::failure (framework_error_kind_t::invalid_operation,
                                                 "invalid invocation is not cancellation");
    return failure.error () && failure.exception ()
           && failure.error_kind () == framework_error_kind_t::invalid_operation;
}

struct yield_resume_probe_t
{
    std::atomic_bool running{false};
    std::atomic_bool overlapped{false};
    std::atomic_bool ran_after_await{false};
    std::atomic_bool resumed_in_open_turn{false};
    std::atomic_int failure_kind{-1};
    std::string failure_message;
    std::promise<void> done;
};

zlink::framework::task_t<void>
yielding_lifecycle_handler (zlink::framework::request_call_t<int> call,
                            zlink::framework::runtime::serial_execution_queue_t &queue,
                            yield_resume_probe_t &probe)
{
    using namespace zlink::framework;
    try {
        (void) co_await call.yield ();
    }
    catch (const framework_exception_t &error) {
        probe.failure_kind.store (static_cast<int> (error.kind ()));
        probe.done.set_value ();
        co_return;
    }
    probe.ran_after_await.store (true);
    const auto turn = detail::capture_current_serial_turn ();
    probe.resumed_in_open_turn.store (turn && !turn->released ());
    probe.running.store (true);
    (void) queue.try_post ("yield-probe-application",
                           [&probe] { probe.overlapped.store (probe.running.load ()); });
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    probe.running.store (false);
    probe.done.set_value ();
}

enum class yield_follow_up_t
{
    second_yield,
    defer,
};

class pending_yield_call_t
    : public zlink::framework::detail::call_facade_t<pending_yield_call_t, int>
{
  public:
    explicit pending_yield_call_t (zlink::framework::task_t<int> pending) :
        call_facade_t (std::move (pending))
    {
    }
};

void record_after_await (yield_resume_probe_t &probe,
                         zlink::framework::runtime::serial_execution_queue_t &queue)
{
    probe.ran_after_await.store (true);
    const auto turn = zlink::framework::detail::capture_current_serial_turn ();
    probe.resumed_in_open_turn.store (turn && !turn->released ());
    probe.running.store (true);
    (void) queue.try_post ("yield-probe-application",
                           [&probe] { probe.overlapped.store (probe.running.load ()); });
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    probe.running.store (false);
}

/* After a Yield resumes, the handler runs in the new turn of its queue, so a
 * second Yield or a Defer in that continuation acts on that turn. */
zlink::framework::task_t<void>
yield_then_follow_up (zlink::framework::request_call_t<int> first,
                      zlink::framework::request_call_t<int> second,
                      yield_follow_up_t follow_up,
                      std::atomic_bool &deferred_ran,
                      zlink::framework::runtime::serial_execution_queue_t &queue,
                      yield_resume_probe_t &probe)
{
    using namespace zlink::framework;
    try {
        (void) co_await first.yield ();
        if (follow_up == yield_follow_up_t::second_yield) {
            (void) co_await second.yield ();
        } else {
            const auto deferred = detail::defer_current_serial_turn ([&deferred_ran] {
                deferred_ran.store (true);
                return ::zlink::framework::task_t<void> (
                  ::zlink::framework::result_t<void>::success ());
            });
            if (!deferred)
                throw framework_exception_t (deferred.error_kind (), deferred.error ()
                                                                       ? deferred.error ()->what ()
                                                                       : "defer failed");
        }
    }
    catch (const framework_exception_t &error) {
        probe.failure_message = error.what ();
        probe.failure_kind.store (static_cast<int> (error.kind ()));
        co_return;
    }
    record_after_await (probe, queue);
}

zlink::framework::task_t<void>
await_facade_yield (pending_yield_call_t call,
                    zlink::framework::runtime::serial_execution_queue_t &queue,
                    yield_resume_probe_t &probe)
{
    using namespace zlink::framework;
    try {
        (void) co_await call.yield ();
    }
    catch (const framework_exception_t &error) {
        probe.failure_message = error.what ();
        probe.failure_kind.store (static_cast<int> (error.kind ()));
        co_return;
    }
    record_after_await (probe, queue);
}

bool wait_ready (std::future<void> &future)
{
    return future.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
}

// Posts a sibling to the handler's queue and reports whether it ran, that is,
// whether the handler released the shared gate while it waits.
bool sibling_runs (zlink::framework::runtime::serial_execution_queue_t &queue)
{
    auto ran = std::make_shared<std::promise<void>> ();
    auto future = ran->get_future ();
    if (!queue.try_post ("yield-sibling", [ran] { ran->set_value (); }))
        return false;
    return wait_ready (future);
}

bool report_yield_probe (const char *name, const yield_resume_probe_t &probe)
{
    const bool passed = probe.failure_kind.load () == -1 && probe.ran_after_await.load ()
                        && probe.resumed_in_open_turn.load () && !probe.overlapped.load ();
    if (!passed)
        std::cerr << name << " kind=" << probe.failure_kind.load () << " message=["
                  << probe.failure_message << "] after_await=" << probe.ran_after_await.load ()
                  << " open_turn=" << probe.resumed_in_open_turn.load ()
                  << " overlapped=" << probe.overlapped.load () << std::endl;
    return passed;
}

/* Handler turn and execution gate §3·§4: the continuation of a Yield is a new
 * turn of the same queue. A second Yield releases that turn and resumes in
 * another new turn; a Defer settles at that turn's terminal. */
bool verify_yield_continuation_keeps_new_turn (yield_follow_up_t follow_up)
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             runtime::serial_execution_queue_t::error_handler_t{},
                                             runtime::serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> first_reply, second_reply;
    std::promise<void> first_submitted, second_submitted;
    auto first_entered = first_submitted.get_future ();
    auto second_entered = second_submitted.get_future ();
    const auto submit_with = [] (task_completion_source_t<int> &reply,
                                 std::promise<void> &entered) {
        return [&reply, &entered] (const std::string &, std::chrono::milliseconds,
                                   const request_call_t<int>::metadata_map_t &) {
            entered.set_value ();
            auto admitted = std::make_shared<task_completion_source_t<void>> ();
            admitted->complete (result_t<void>::success ());
            return request_task_t<int> (reply.task (), std::move (admitted));
        };
    };
    request_call_t<int> first ("test.yield.first", submit_with (first_reply, first_submitted));
    request_call_t<int> second ("test.yield.second", submit_with (second_reply, second_submitted));
    yield_resume_probe_t probe;
    auto done = probe.done.get_future ();
    std::atomic_bool deferred_ran{false};
    std::optional<task_t<void>> handler;
    if (!queue.try_post_async ("yield-follow-up", [&] (auto complete) {
            handler.emplace (
              yield_then_follow_up (first, second, follow_up, deferred_ran, queue, probe));
            detail::observe_task_completion (*handler, [&probe, complete] (const auto &) mutable {
                probe.done.set_value ();
                complete ([] {});
            });
        }))
        return false;
    const char *name = follow_up == yield_follow_up_t::second_yield ? "second yield after resume"
                                                                    : "defer after yield resume";
    if (!wait_ready (first_entered) || !sibling_runs (queue)) {
        (void) report_yield_probe (name, probe);
        return false;
    }
    first_reply.complete (result_t<int>::success (1));
    if (follow_up == yield_follow_up_t::second_yield) {
        if (!wait_ready (second_entered) || !sibling_runs (queue)) {
            (void) wait_ready (done);
            (void) report_yield_probe (name, probe);
            return false;
        }
        second_reply.complete (result_t<int>::success (2));
    }
    if (!wait_ready (done)) {
        (void) report_yield_probe (name, probe);
        return false;
    }
    queue.drain ();
    const bool deferred = follow_up != yield_follow_up_t::defer || deferred_ran.load ();
    if (!deferred)
        std::cerr << name << " did not run the deferred work" << std::endl;
    return report_yield_probe (name, probe) && deferred;
}

/* A call facade Yield follows the same rule: the handler releases the shared
 * gate where it suspends and resumes in a new turn of the same queue. */
bool verify_call_facade_yield_releases_gate ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             runtime::serial_execution_queue_t::error_handler_t{},
                                             runtime::serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> reply;
    yield_resume_probe_t probe;
    auto done = probe.done.get_future ();
    std::optional<task_t<void>> handler;
    if (!queue.try_post_async ("facade-yield", [&] (auto complete) {
            handler.emplace (
              await_facade_yield (pending_yield_call_t (reply.task ()), queue, probe));
            detail::observe_task_completion (*handler, [&probe, complete] (const auto &) mutable {
                probe.done.set_value ();
                complete ([] {});
            });
        }))
        return false;
    const bool released = sibling_runs (queue);
    reply.complete (result_t<int>::success (3));
    const bool finished = wait_ready (done);
    queue.drain ();
    if (!released)
        std::cerr << "call facade yield kept the shared gate while waiting" << std::endl;
    return released && finished && report_yield_probe ("call facade yield", probe);
}

enum class yield_reply_order_t
{
    after_suspend,
    after_close,
    before_suspend,
};

/* Handler turn and execution gate §3·§4: a Yield releases the shared gate where
 * the handler suspends, its continuation resumes in a new turn of the same
 * Spot queue, and a Spot that closes during the wait ends the handler with a
 * failure instead of resuming it. A reply that completes before the handler
 * suspends leaves the turn unreleased, so the handler continues in that turn. */
bool verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t order)
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             runtime::serial_execution_queue_t::error_handler_t{},
                                             runtime::serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> reply;
    std::promise<void> suspended;
    yield_resume_probe_t probe;
    std::optional<task_t<void>> handler;
    request_call_t<int> call (
      "test.yield.leave", [&] (const std::string &, std::chrono::milliseconds,
                               const request_call_t<int>::metadata_map_t &) {
          if (order == yield_reply_order_t::before_suspend)
              reply.complete (result_t<int>::success (1));
          auto admitted = std::make_shared<task_completion_source_t<void>> ();
          admitted->complete (result_t<void>::success ());
          return request_task_t<int> (reply.task (), std::move (admitted));
      });
    queue.post_async (
      "yielding-leave",
      [&] (auto complete) {
          handler.emplace (yielding_lifecycle_handler (call, queue, probe));
          // The handler has returned from its first suspension or finished.
          suspended.set_value ();
          complete ([] {});
      },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
    if (suspended.get_future ().wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    if (order == yield_reply_order_t::after_close)
        queue.close ();
    if (order != yield_reply_order_t::before_suspend)
        reply.complete (result_t<int>::success (1));
    if (probe.done.get_future ().wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    queue.drain ();
    if (order == yield_reply_order_t::after_close) {
        if (probe.ran_after_await.load ()
            || probe.failure_kind.load ()
                 != static_cast<int> (framework_error_kind_t::shutting_down))
            std::cerr << "closed Spot resumed its yielded handler after_await="
                      << probe.ran_after_await.load () << " kind=" << probe.failure_kind.load ()
                      << '\n';
        return !probe.ran_after_await.load ()
               && probe.failure_kind.load ()
                    == static_cast<int> (framework_error_kind_t::shutting_down);
    }
    // A reply that is ready before the suspension leaves the turn unreleased.
    const bool kept_turn =
      order != yield_reply_order_t::before_suspend || probe.resumed_in_open_turn.load ();
    if (!probe.ran_after_await.load () || !kept_turn || probe.overlapped.load ())
        std::cerr << "yield continuation left its Spot turn order=" << static_cast<int> (order)
                  << " after_await=" << probe.ran_after_await.load ()
                  << " open_turn=" << probe.resumed_in_open_turn.load ()
                  << " overlapped=" << probe.overlapped.load () << '\n';
    return probe.ran_after_await.load () && kept_turn && !probe.overlapped.load ();
}

} // namespace

int main (int argc, char **argv)
{
    if (!verify_released_turn_wrapper_handoff ())
        return 39;
    for (const bool stopped_host : {false, true})
        for (const auto scope :
             {completion_scope_probe_t::serial_turn, completion_scope_probe_t::application_job,
              completion_scope_probe_t::wait_owner, completion_scope_probe_t::infrastructure})
            if (!verify_completion_scope (stopped_host, scope))
                return 40;
    for (const auto scope :
         {completion_scope_probe_t::serial_turn, completion_scope_probe_t::application_job,
          completion_scope_probe_t::wait_owner, completion_scope_probe_t::infrastructure})
        if (!verify_completion_scope (false, scope, true))
            return 40;
    for (const bool with_job : {false, true})
        for (const bool infrastructure_completer : {false, true})
            if (!verify_owned_completion_turn (with_job, infrastructure_completer))
                return 41;
    const bool lookup_isolated = verify_lookup_admission_isolation ();
    const bool reply_admits = verify_reply_success_completes_admission ();
    if (!lookup_isolated || !reply_admits)
        return 34;
    if (!verify_request_admission_stages ())
        return 33;
    if (!verify_cancellation_contract ())
        return 90;

    // This test drives runtime parts without a host.
    zlink::framework::runtime::install_host_context_hooks ();
    if (argc == 2 && std::string_view (argv[1]) == "--serial-terminal-adapters") {
        bool passed = true;
        const auto check = [&] (const char *name, bool result) {
            std::cout << "serial-adapter case=" << name << " passed=" << result << '\n';
            passed = result && passed;
        };
        check ("second-yield",
               verify_yield_continuation_keeps_new_turn (yield_follow_up_t::second_yield));
        check ("defer", verify_yield_continuation_keeps_new_turn (yield_follow_up_t::defer));
        check ("facade", verify_call_facade_yield_releases_gate ());
        check ("after-suspend",
               verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::after_suspend));
        check ("after-close",
               verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::after_close));
        check ("before-suspend",
               verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::before_suspend));
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (argc == 3 && std::string (argv[1]) == "--delivery-defect") {
        run_delivery_defect (static_cast<delivery_defect_t> (std::atoi (argv[2])));
        return 1;
    }
    if (!verify_delivery_defects (argv[0]))
        return 30;
    zlink::framework::task_completion_source_t<int> shared_source;
    auto shared = shared_source.task ();
    auto first_waiter = await_shared (shared, 1);
    auto second_waiter = await_shared (shared, 2);
    std::thread shared_completion (
      [&] { shared_source.complete (zlink::framework::result_t<int>::success (40)); });
    shared_completion.join ();
    if (first_waiter.result ().value () != 41 || second_waiter.result ().value () != 42) {
        return 1;
    }

    zlink::framework::task_completion_source_t<int> completion;
    auto first_complete_wins = completion.task ();
    int callback_count = 0;
    int callback_value = 0;
    std::promise<void> callback_delivered;
    auto delivered = callback_delivered.get_future ();
    zlink::framework::detail::observe_task_completion (
      first_complete_wins, [&callback_count, &callback_value,
                            &callback_delivered] (const zlink::framework::result_t<int> &result) {
          ++callback_count;
          callback_value = result.value ();
          callback_delivered.set_value ();
      });
    const bool first_completion =
      completion.complete (zlink::framework::result_t<int>::success (100));
    const bool duplicate_completion =
      completion.complete (zlink::framework::result_t<int>::success (200));
    delivered.get ();
    if (!first_completion || duplicate_completion || first_complete_wins.result ().value () != 100
        || callback_count != 1 || callback_value != 100) {
        return 2;
    }

    zlink::framework::request_call_t<int> call (zlink::framework::detail::boundary_failure<int> (
      zlink::framework::detail::boundary_error_t::timed_out, "timeout"));
    auto coroutine_result = call.async ().result ();
    if ((coroutine_result.error () != nullptr
         && zlink::framework::detail::boundary_state (*coroutine_result.error ())
              != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 3;
    }

    const auto preserved_failure = await_timeout ().result ();
    if (preserved_failure
        || (preserved_failure.error () != nullptr
            && zlink::framework::detail::boundary_state (*preserved_failure.error ())
                 != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 4;
    }

    zlink::framework::request_call_t<int> shutdown_call (
      zlink::framework::detail::boundary_failure<int> (
        zlink::framework::detail::boundary_error_t::shutdown, "shutdown"));
    const auto shutdown_result = shutdown_call.async ().result ();
    if (shutdown_result || shutdown_result.error () == nullptr
        || zlink::framework::detail::boundary_state (*shutdown_result.error ())
             != zlink::framework::detail::boundary_error_t::shutdown) {
        return 5;
    }

    std::deque<std::function<void ()>> rescheduled_work;
    auto immediate_task =
      zlink::framework::task_t<int> (zlink::framework::result_t<int>::success (500));
    auto rescheduled_task = zlink::framework::detail::with_task_resume_scheduler (
      std::move (immediate_task), [&rescheduled_work] (std::function<void ()> work) {
          rescheduled_work.push_back (std::move (work));
      });
    int rescheduled_callback_count = 0;
    zlink::framework::detail::observe_task_completion (
      rescheduled_task,
      [&rescheduled_callback_count] (const zlink::framework::result_t<int> &result) {
          if (result.value () == 500) {
              ++rescheduled_callback_count;
          }
      });
    if (rescheduled_callback_count != 0 || rescheduled_work.size () != 1) {
        return 8;
    }
    rescheduled_work.front () ();
    rescheduled_work.pop_front ();
    if (rescheduled_callback_count != 1 || rescheduled_task.result ().value () != 500) {
        return 9;
    }

    bool one_way_invoked = false;
    zlink::framework::send_call_t accepted (
      "test.command", [&one_way_invoked] (const std::string &,
                                          const zlink::framework::send_call_t::metadata_map_t &) {
          one_way_invoked = true;
          return zlink::framework::result_t<void>::success ();
      });
    const auto accepted_result = accepted.async ().result ();
    if (!accepted_result || !one_way_invoked) {
        return 10;
    }

    zlink::framework::send_call_t timed_out (zlink::framework::detail::boundary_failure<void> (
      zlink::framework::detail::boundary_error_t::timed_out, "send timed out"));
    const auto timed_out_result = timed_out.async ().result ();
    if (timed_out_result
        || timed_out_result.error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded) {
        return 11;
    }

    zlink::framework::send_call_t disconnected (zlink::framework::detail::boundary_failure<void> (
      zlink::framework::detail::boundary_error_t::disconnected, "route unavailable"));
    const auto disconnected_result = disconnected.async ().result ();
    if (disconnected_result
        || disconnected_result.error_kind ()
             != zlink::framework::framework_error_kind_t::unavailable) {
        return 12;
    }

    std::mutex publish_mutex;
    std::condition_variable publish_changed;
    bool publish_started = false;
    bool release_target = false;
    zlink::framework::publish_call_t blocked_publish (
      [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
          std::unique_lock lock (publish_mutex);
          publish_started = true;
          publish_changed.notify_all ();
          publish_changed.wait (lock, [&] { return release_target; });
          return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "target failed after handoff");
      });
    const auto blocked_terminal = blocked_publish.async ().result ();
    if (!blocked_terminal) {
        return 13;
    }
    {
        std::unique_lock lock (publish_mutex);
        if (!publish_changed.wait_for (lock, std::chrono::seconds (1),
                                       [&] { return publish_started; })) {
            return 14;
        }
        release_target = true;
    }
    publish_changed.notify_all ();

    std::mutex failed_mutex;
    std::condition_variable failed_changed;
    bool failed_target_ran = false;
    zlink::framework::publish_call_t failed_publish (
      [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
          {
              std::lock_guard lock (failed_mutex);
              failed_target_ran = true;
          }
          failed_changed.notify_all ();
          return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "post-start failure is internal");
      });
    const auto failed_terminal = failed_publish.async ().result ();
    if (!failed_terminal) {
        return 15;
    }
    {
        std::unique_lock lock (failed_mutex);
        if (!failed_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return failed_target_ran; })) {
            return 16;
        }
    }

    const zlink::framework::detail::ambient_context_hooks_t test_ambient_hooks{
      &capture_test_ambient, &enter_test_ambient};
    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    zlink::framework::detail::set_ambient_context_hooks (&test_ambient_hooks);
    native_await_ambient_value = 42;
    auto native_state = std::make_shared<native_async_state_t> ();
    auto serial_turn = std::make_shared<test_serial_turn_t> ();
    std::atomic<int> observed_ambient{0};
    std::atomic<bool> observed_serial_turn{false};
    std::atomic<bool> serial_resumed_inside_completion{true};
    auto native_task = [&] {
        zlink::framework::detail::serial_turn_scope_t scope (serial_turn);
        return await_native_binding_result (native_state, serial_turn, observed_ambient,
                                            observed_serial_turn, serial_resumed_inside_completion);
    }();
    std::thread completion_thread ([native_state] {
        native_await_ambient_value = 999;
        inside_native_completion = true;
        native_state->complete (700);
        inside_native_completion = false;
    });
    completion_thread.join ();
    if (native_task.result ().value () != 700
        || observed_ambient.load (std::memory_order_acquire) != 42
        || !observed_serial_turn.load (std::memory_order_acquire)
        || serial_resumed_inside_completion.load (std::memory_order_acquire)
        || serial_turn->released ()) {
        return 17;
    }
    zlink::framework::detail::set_ambient_context_hooks (nullptr);

    auto unowned_native_state = std::make_shared<native_async_state_t> ();
    std::atomic<bool> resumed_inside_completion{true};
    auto unowned_native_task =
      await_native_binding_outside_completion (unowned_native_state, resumed_inside_completion);
    std::thread unowned_completion_thread ([unowned_native_state] {
        inside_native_completion = true;
        unowned_native_state->complete (701);
        inside_native_completion = false;
    });
    unowned_completion_thread.join ();
    if (unowned_native_task.result ().value () != 701
        || resumed_inside_completion.load (std::memory_order_acquire)) {
        return 18;
    }

    /* Regression pin for the request_erased retry loop
     * (actor_client.cpp): the admission-retry delay used to
     * std::this_thread::sleep_for on the coroutine's own thread. It now
     * co_awaits detail::delay, which must (a) return control to the caller
     * without blocking anywhere near the requested duration, since the
     * suspend happens inside the coroutine machinery rather than the
     * executing thread, and (b) still resume the coroutine, with the
     * correct value, once the duration has actually elapsed. */
    const auto delay_call_start = std::chrono::steady_clock::now ();
    std::mutex delay_mutex;
    zlink::framework::detail::delay_owner_t delays (delay_mutex);
    auto delayed_retry = await_async_delay (delays, std::chrono::milliseconds (150), 77);
    const auto delay_call_returned = std::chrono::steady_clock::now ();
    if (delay_call_returned - delay_call_start >= std::chrono::milliseconds (100)) {
        return 19;
    }
    if (delayed_retry.result ().value () != 77) {
        return 20;
    }
    if (std::chrono::steady_clock::now () - delay_call_start < std::chrono::milliseconds (140)) {
        return 21;
    }

    auto abandoned_scheduler =
      zlink::framework::detail::capture_runtime_native_continuation_scheduler ();
    std::atomic<bool> abandoned_shutdown_done{false};
    std::thread abandoned_shutdown ([&] {
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        abandoned_shutdown_done.store (true, std::memory_order_release);
    });
    abandoned_shutdown.join ();
    if (!abandoned_shutdown_done.load (std::memory_order_acquire)) {
        return 22;
    }
    bool rejected_after_shutdown = false;
    bool rejected_work_ran = false;
    try {
        abandoned_scheduler ([&] { rejected_work_ran = true; });
    }
    catch (const zlink::framework::framework_exception_t &error) {
        rejected_after_shutdown =
          error.kind () == zlink::framework::framework_error_kind_t::shutting_down;
    }
    abandoned_scheduler = {};
    if (!rejected_after_shutdown || rejected_work_ran) {
        return 23;
    }

    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    auto queued_scheduler =
      zlink::framework::detail::capture_runtime_native_continuation_scheduler ();
    std::mutex queued_mutex;
    std::condition_variable queued_changed;
    bool queued_started = false;
    bool release_queued = false;
    queued_scheduler ([&] {
        std::unique_lock lock (queued_mutex);
        queued_started = true;
        queued_changed.notify_all ();
        queued_changed.wait (lock, [&] { return release_queued; });
    });
    queued_scheduler = {};
    {
        std::unique_lock lock (queued_mutex);
        if (!queued_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return queued_started; })) {
            release_queued = true;
            queued_changed.notify_all ();
            zlink::framework::runtime::shutdown_handler_coroutine_executor ();
            return 24;
        }
    }
    std::atomic<bool> queued_shutdown_done{false};
    std::promise<void> queued_shutdown_started;
    auto shutdown_entered = queued_shutdown_started.get_future ();
    std::thread queued_shutdown ([&] {
        queued_shutdown_started.set_value ();
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        queued_shutdown_done.store (true, std::memory_order_release);
    });
    shutdown_entered.wait ();
    if (queued_shutdown_done.load (std::memory_order_acquire)) {
        {
            std::lock_guard lock (queued_mutex);
            release_queued = true;
        }
        queued_changed.notify_all ();
        queued_shutdown.join ();
        return 25;
    }
    {
        std::lock_guard lock (queued_mutex);
        release_queued = true;
    }
    queued_changed.notify_all ();
    queued_shutdown.join ();
    if (!queued_shutdown_done.load (std::memory_order_acquire)) {
        return 26;
    }

    zlink::framework::task_completion_source_t<int> observed_completion;
    auto observed_task = observed_completion.task ();
    if (observed_task.result_for (std::chrono::milliseconds (0)))
        return 27;
    if (!observed_completion.complete (zlink::framework::result_t<int>::success (123))
        || observed_completion.complete (zlink::framework::result_t<int>::success (456)))
        return 28;
    const auto observed_result = observed_task.result_for (std::chrono::milliseconds (0));
    if (!observed_result || observed_result->value () != 123)
        return 29;

    if (!verify_observer_lifetime () || !verify_released_observer_owner ()
        || !verify_registration_completion_cancellation_race ()
        || !verify_nested_cancellation_lifetime () || !verify_nested_return_lifetime ()
        || !verify_void_completion_rejection_race (true)
        || !verify_void_completion_rejection_race (false) || !verify_rejected_delivery_lifetime ()
        || !verify_rejected_ready_registration () || !verify_rejection_retain_cancellation_race ()
        || !verify_rejection_after_owner_stop_request () || !verify_cancelled_registration_lease ()
        || !verify_stopped_registration_state_lifetime ()
        || !verify_ownerless_wait_after_context_stop ())
        return 30;

    zlink::framework::runtime::configure_handler_coroutine_executor (2);
    const bool yield_kept_turn =
      verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::after_suspend);
    const bool closed_yield_failed =
      verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::after_close);
    const bool ready_reply_kept_turn =
      verify_yield_continuation_owned_by_spot_queue (yield_reply_order_t::before_suspend);
    const bool second_yield_released =
      verify_yield_continuation_keeps_new_turn (yield_follow_up_t::second_yield);
    const bool defer_after_yield =
      verify_yield_continuation_keeps_new_turn (yield_follow_up_t::defer);
    const bool facade_yield_released = verify_call_facade_yield_releases_gate ();
    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    if (!yield_kept_turn)
        return 31;
    if (!closed_yield_failed)
        return 32;
    if (!ready_reply_kept_turn)
        return 35;
    if (!second_yield_released)
        return 36;
    if (!defer_after_yield)
        return 37;
    if (!facade_yield_released)
        return 38;
    return 0;
}
