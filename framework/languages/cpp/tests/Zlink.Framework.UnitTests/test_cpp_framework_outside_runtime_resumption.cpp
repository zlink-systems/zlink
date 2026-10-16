/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

// Submit and completion §11: a wait started outside a runtime execution
// context has no lane to return to. The resumption the Framework delivers
// still starts in a new turn - a completion dispatcher turn - and never on
// the call stack that decided the completion. A synchronous blocking wait
// returns on the waiting thread, and an infrastructure wait keeps resuming
// where its Store step completes (execution gate §13).

#include <zlink/framework/contracts/dispatch/task.hpp>

#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/foundation/operation_registry.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace
{

thread_local bool inside_completion = false;
constexpr std::chrono::seconds completion_observation_timeout{5};

struct resumption_t
{
    std::atomic<bool> resumed_inside_completion{true};
    std::atomic<bool> resumed_on_dispatcher{false};
    std::atomic<std::thread::id> resumed_on{};
};

zlink::framework::task_t<int> await_from_application (zlink::framework::task_t<int> &pending,
                                                      resumption_t &observed)
{
    const int value = co_await pending;
    observed.resumed_inside_completion.store (inside_completion);
    observed.resumed_on_dispatcher.store (zlink::framework::detail::current_completion_owner ()
                                          != nullptr);
    observed.resumed_on.store (std::this_thread::get_id ());
    co_return value;
}

std::thread::id complete_on_other_thread (zlink::framework::task_completion_source_t<int> &source,
                                          int value,
                                          bool infrastructure = false)
{
    std::thread completer ([&source, value, infrastructure] {
        const bool previous =
          zlink::framework::detail::exchange_infrastructure_wait (infrastructure);
        inside_completion = true;
        source.complete (zlink::framework::result_t<int>::success (value));
        inside_completion = false;
        zlink::framework::detail::exchange_infrastructure_wait (previous);
    });
    const auto id = completer.get_id ();
    completer.join ();
    return id;
}

// The application thread starts the wait; another thread decides the result.
int expect_dispatcher_turn (int value, int code, bool infrastructure_completion = false)
{
    zlink::framework::task_completion_source_t<int> source;
    auto pending = source.task ();
    resumption_t observed;
    auto caller = await_from_application (pending, observed);
    const auto completer = complete_on_other_thread (source, value, infrastructure_completion);

    // Blocking wait: language contract, returns on this thread.
    const auto waiter = std::this_thread::get_id ();
    if (caller.result ().value () != value || std::this_thread::get_id () != waiter)
        return code;
    if (observed.resumed_inside_completion.load () || observed.resumed_on.load () == completer)
        return code + 1;
    if (!observed.resumed_on_dispatcher.load ())
        return code + 2;
    return 0;
}

int expect_native_handoff (bool stop_host, bool infrastructure, int code)
{
    if (stop_host)
        zlink::framework::runtime::configure_handler_coroutine_executor (1);
    const bool previous = zlink::framework::detail::exchange_infrastructure_wait (infrastructure);
    auto scheduler = zlink::framework::detail::capture_native_continuation_scheduler ();
    zlink::framework::detail::exchange_infrastructure_wait (previous);
    if (stop_host)
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    std::promise<int> finished;
    auto released = std::make_shared<std::promise<void>> ();
    auto release = released->get_future ();
    auto payload = std::shared_ptr<int> (new int (1770), [released] (int *value) {
        delete value;
        released->set_value ();
    });
    auto done = finished.get_future ();
    std::atomic<bool> inline_resume{false};
    std::atomic<bool> failed{false};
    std::thread completer ([&] {
        inside_completion = true;
        try {
            scheduler ([&, payload] {
                inline_resume.store (inside_completion);
                finished.set_value (*payload);
            });
        }
        catch (const zlink::framework::framework_exception_t &) {
            failed.store (true);
            finished.set_value (0);
        }
        inside_completion = false;
    });
    completer.join ();
    payload.reset ();
    const auto value = done.get ();
    if (failed.load () || value != 1770 || inline_resume.load () != infrastructure)
        return code;
    return release.wait_for (completion_observation_timeout) == std::future_status::ready
             ? 0
             : code + 1;
}

int expect_stopped_host_observer_lifetime ()
{
    using namespace zlink::framework;
    auto finished = std::make_shared<std::promise<int>> ();
    auto done = finished->get_future ();
    auto released = std::make_shared<std::promise<void>> ();
    auto release = released->get_future ();
    runtime::configure_handler_coroutine_executor (1);
    {
        task_completion_source_t<std::shared_ptr<int>> source;
        auto pending = source.task ();
        detail::observe_task_completion (
          pending, [finished] (const auto &result) { finished->set_value (*result.value ()); });
        runtime::shutdown_handler_coroutine_executor ();
        auto payload = std::shared_ptr<int> (new int (1770), [released] (int *value) {
            delete value;
            released->set_value ();
        });
        source.complete (result_t<std::shared_ptr<int>>::success (std::move (payload)));
    }
    // No source or task owner remains. The queued registration alone keeps
    // the terminal result alive until its weak observer has consumed it.
    if (done.wait_for (completion_observation_timeout) != std::future_status::ready)
        return 92;
    if (done.get () != 1770)
        return 93;
    return release.wait_for (completion_observation_timeout) == std::future_status::ready ? 0 : 94;
}

int expect_independent_completions ()
{
    std::promise<void> entered, release, first_finished, second_finished;
    auto first_entered = entered.get_future ();
    auto release_first = release.get_future ().share ();
    auto first_done = first_finished.get_future ();
    auto second_done = second_finished.get_future ();
    auto scheduler = zlink::framework::detail::capture_native_continuation_scheduler ();
    std::thread first ([&] {
        scheduler ([&] {
            entered.set_value ();
            release_first.wait ();
            first_finished.set_value ();
        });
    });
    first_entered.wait ();
    std::thread second ([&] { scheduler ([&] { second_finished.set_value (); }); });
    // This bound only identifies a broken independence contract. Release and
    // join still finish the test's children if a callback was serialized.
    const bool independent =
      second_done.wait_for (completion_observation_timeout) == std::future_status::ready;
    release.set_value ();
    first.join ();
    second.join ();
    first_done.wait ();
    second_done.wait ();
    return independent ? 0 : 80;
}

int expect_independent_registry_completions ()
{
    namespace foundation = zlink::framework::runtime::foundation;
    foundation::operation_registry_t registry;
    std::promise<void> entered, release, second_finished;
    auto first_entered = entered.get_future ();
    auto release_first = release.get_future ().share ();
    auto second_done = second_finished.get_future ();
    const auto deadline = foundation::operation_registry_t::clock_t::time_point::max ();
    registry.register_operation (foundation::call_id_t{1770, 1}, deadline,
                                 [&] (foundation::operation_terminal_t, std::vector<std::uint8_t>) {
                                     entered.set_value ();
                                     release_first.wait ();
                                 });
    registry.register_operation (foundation::call_id_t{1770, 2}, deadline,
                                 [&] (foundation::operation_terminal_t, std::vector<std::uint8_t>) {
                                     second_finished.set_value ();
                                 });
    registry.complete (foundation::call_id_t{1770, 1}, {});
    first_entered.wait ();
    registry.complete (foundation::call_id_t{1770, 2}, {});
    const bool independent =
      second_done.wait_for (completion_observation_timeout) == std::future_status::ready;
    release.set_value ();
    registry.shutdown ();
    second_done.get ();
    return independent ? 0 : 85;
}

int expect_registry_callback_final_owner ()
{
    namespace foundation = zlink::framework::runtime::foundation;
    auto registry = std::make_shared<foundation::operation_registry_t> ();
    auto finished = std::make_shared<std::promise<void>> ();
    auto done = finished->get_future ();
    registry->register_operation (
      foundation::call_id_t{1770, 3}, foundation::operation_registry_t::clock_t::time_point::max (),
      [&, finished] (foundation::operation_terminal_t, std::vector<std::uint8_t>) {
          registry.reset ();
          finished->set_value ();
      });
    // The final owner is released inside its own callback. Shutdown must not
    // wait for this callback's reservation to be released by its destructor.
    registry->complete (foundation::call_id_t{1770, 3}, {});
    done.get ();
    return registry ? 87 : 0;
}

int expect_existing_completion_turn ()
{
    zlink::framework::task_completion_source_t<int> source;
    auto pending = source.task ();
    resumption_t observed;
    auto caller = await_from_application (pending, observed);
    auto scheduler = zlink::framework::detail::capture_native_continuation_scheduler ();
    std::promise<void> completed;
    auto done = completed.get_future ();
    scheduler ([&] {
        inside_completion = true;
        source.complete (zlink::framework::result_t<int>::success (91));
        inside_completion = false;
        completed.set_value ();
    });
    done.get ();
    if (caller.result ().value () != 91)
        return 90;
    // A wrapper already delivered in a valid completion turn propagates in
    // that turn. The thread-local marker also detects a second pool hop.
    return observed.resumed_inside_completion.load () && observed.resumed_on_dispatcher.load ()
             ? 0
             : 91;
}

} // namespace

int main ()
{
    int failures = 0;
    auto check = [&] (const char *name, int result) {
        std::cout << name << " result=" << result << std::endl;
        failures += result != 0;
    };
    // No runtime hooks at all: no host has ever run in this process.
    check ("no-runtime-hooks", expect_dispatcher_turn (11, 10));
    check ("application-wait-infrastructure-completion", expect_dispatcher_turn (12, 15, true));
    check ("native-no-runtime-hooks", expect_native_handoff (false, false, 50));

    // Runtime hooks without an executor owner: the host is not running.
    zlink::framework::runtime::install_host_context_hooks ();
    check ("host-without-owner", expect_dispatcher_turn (21, 20));

    // The host scheduler was captured, then the host stopped before the
    // result arrived. No wait owner can cancel this wait.
    {
        zlink::framework::runtime::configure_handler_coroutine_executor (1);
        zlink::framework::task_completion_source_t<int> source;
        auto pending = source.task ();
        resumption_t observed;
        auto caller = await_from_application (pending, observed);
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        const auto completer = complete_on_other_thread (source, 31);
        check ("stopped-host-result", caller.result ().value () == 31 ? 0 : 30);
        check ("stopped-host-new-turn", observed.resumed_inside_completion.load ()
                                            || observed.resumed_on.load () == completer
                                          ? 31
                                          : 0);
        check ("stopped-host-dispatcher", observed.resumed_on_dispatcher.load () ? 0 : 32);
    }

    // Execution gate §13: an infrastructure wait resumes on the thread that
    // completes the Store step, not behind application work.
    {
        zlink::framework::task_completion_source_t<int> source;
        auto pending = source.task ();
        resumption_t observed;
        const bool previous = zlink::framework::detail::exchange_infrastructure_wait (true);
        auto caller = await_from_application (pending, observed);
        zlink::framework::detail::exchange_infrastructure_wait (previous);
        const auto completer = complete_on_other_thread (source, 41);
        check ("infrastructure-result", caller.result ().value () == 41 ? 0 : 40);
        check ("infrastructure-inline", !observed.resumed_inside_completion.load ()
                                            || observed.resumed_on.load () != completer
                                          ? 41
                                          : 0);
    }
    check ("stopped-host-observer-lifetime", expect_stopped_host_observer_lifetime ());
    check ("native-stopped-host", expect_native_handoff (true, false, 60));
    check ("native-infrastructure", expect_native_handoff (false, true, 70));
    check ("independent-completions", expect_independent_completions ());
    check ("independent-registry-completions", expect_independent_registry_completions ());
    check ("registry-callback-final-owner", expect_registry_callback_final_owner ());
    check ("existing-completion-turn", expect_existing_completion_turn ());
    return failures == 0 ? 0 : 1;
}
