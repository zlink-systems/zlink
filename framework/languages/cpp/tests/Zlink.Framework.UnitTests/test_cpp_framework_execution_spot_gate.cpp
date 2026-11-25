/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/execution_wait.hpp"
#include "../support/execution_signal.hpp"
#include "../support/execution_serial_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/scope_exit.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{
bool verify_spot_wide_consumer_waits_for_shared_gate ()
{
    using namespace zlink::framework::runtime;

    // Declared before the fixture so the marker job never outlives it.
    serial_test_signal_t marker;
    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide (), {}, 1);
    serial_test_blocker_t owner;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        owner.release ();
        fixture.drain ();
    });
    if (!fixture.serial.execute_spot ("gate-owner", owner.work ()) || !owner.wait_for_entry ())
        return false;

    bool accepted = true;
    std::atomic_int started{0};
    for (const auto *actor : {"actor-a", "actor-b"}) {
        serial_work_options_t options;
        options.retained_message = std::make_shared<int> (7);
        accepted = fixture.serial.execute_actor (
                     actor, "waiting-actor",
                     [&] (auto complete) {
                         started.fetch_add (1, std::memory_order_release);
                         complete ([] {});
                     },
                     options)
                   && accepted;
    }
    // One worker makes this marker follow the existing independent Actor drains.
    // Admission must finish while the ordinary asynchronous Spot turn is held.
    const auto marked = fixture.worker->try_submit_internal ([&] { marker.set (); });
    const auto observed = marked && marker.wait_for ();
    std::size_t waiting_messages = 0;
    for (const auto *actor : {"actor-a", "actor-b"})
        waiting_messages += fixture.serial.actor_executor (actor)
                              ->queue ()
                              ->pending_messages ()
                              .result ()
                              .value ()
                              .size ();
    bool retained = waiting_messages == 2;
    const auto started_while_held = started.load (std::memory_order_acquire);
    owner.release ();
    fixture.spot_queue->drain ();
    for (const auto *actor : {"actor-a", "actor-b"}) {
        const auto queue = fixture.serial.actor_executor (actor)->queue ();
        queue->drain ();
        retained = queue->pending_messages ().result ().value ().empty () && retained;
    }
    const auto started_after_release = started.load (std::memory_order_acquire);
    std::cout << "SpotWide consumer probe: accepted=" << accepted << " marker=" << observed
              << " retained-while-held=" << waiting_messages
              << " handlers-while-held=" << started_while_held
              << " handlers-after-release=" << started_after_release << '\n';
    return accepted && observed && retained && started_while_held == 0
           && started_after_release == 2;
}

bool verify_spot_wide_cancel_removes_unclaimed_actor_head ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    std::atomic_int started{0};
    std::atomic_int cancelled{0};
    serial_test_blocker_t owner;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        owner.release ();
        fixture.drain ();
    });
    if (!fixture.serial.execute_spot ("cancel-gate-owner", owner.work ())
        || !owner.wait_for_entry ())
        return false;

    auto submitted = fixture.serial.execute_actor_cancellable (
      "actor-a", "cancel-before-claim",
      [&] (auto complete) {
          ++started;
          complete ([] {});
      },
      [&] { ++cancelled; });
    if (!submitted) {
        owner.release ();
        return false;
    }
    const auto outcome =
      submitted.value ().queue->cancel_submission (submitted.value ().id).result ().value ();
    owner.release ();
    fixture.spot_queue->drain ();
    submitted.value ().queue->drain ();
    return outcome == serial_cancel_submission_outcome_t::queued_cancelled && started.load () == 0
           && cancelled.load () == 1 && submitted.value ().queue->pending_count () == 0;
}

bool verify_spot_wide_control_returns_without_waiting_for_gate ()
{
    using namespace zlink::framework::runtime;
    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    // Releases the held gate and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::lock_guard lock (mutex);
            release = true;
        }
        changed.notify_all ();
        fixture.drain ();
    });
    if (!fixture.spot_queue->try_post ("held-physical-gate", [&] {
            std::unique_lock lock (mutex);
            entered = true;
            changed.notify_all ();
            changed.wait (lock, [&] { return release; });
        }))
        return false;
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
            release = true;
            changed.notify_all ();
            return false;
        }
    }
    auto actor = fixture.serial.actor_executor ("actor-a");
    auto submitted = actor->queue ()->try_post_cancellable_async (
      "control-cancel-target", [] (auto complete) { complete ([] {}); }, [] {});
    std::atomic_bool returned{false};
    std::atomic_bool cancelled{false};
    std::atomic_bool queried{false};
    std::thread caller ([&] {
        auto outcome = actor->queue ()->cancel_submission (submitted.value ());
        auto pending = actor->queue ()->pending_messages ();
        returned.store (true);
        [&]<typename T> (T value) {
            if constexpr (requires { value.result (); })
                cancelled.store (value.result ().value ()
                                 == serial_cancel_submission_outcome_t::queued_cancelled);
            else
                cancelled.store (value == serial_cancel_submission_outcome_t::queued_cancelled);
        }(std::move (outcome));
        const auto messages = pending.result ();
        queried.store (messages && messages.value ().empty ());
    });
    const bool nonblocking = wait_until ([&] { return returned.load (); });
    {
        std::lock_guard lock (mutex);
        release = true;
    }
    changed.notify_all ();
    caller.join ();
    std::cout << "control returned while physical gate held=" << nonblocking << '\n';
    return nonblocking && cancelled.load () && queried.load ();
}

bool verify_spot_wide_stop_settles_queued_actor_rejection ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    std::atomic_int started{0};
    std::atomic_int rejected{0};
    serial_test_blocker_t owner;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        owner.release ();
        fixture.drain ();
    });
    if (!fixture.serial.execute_spot ("stop-gate-owner", owner.work ()) || !owner.wait_for_entry ())
        return false;

    if (!fixture.serial.execute_actor (
          "actor-a", "stop-before-claim",
          [&] (auto complete) {
              ++started;
              complete ([] {});
          },
          {}, false, [&] { ++rejected; })) {
        owner.release ();
        return false;
    }
    fixture.spot_queue->cancel_pending ();
    owner.release ();
    fixture.spot_queue->drain ();
    fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
    return started.load () == 0 && rejected.load () == 1;
}

bool verify_spot_wide_rejects_actor_when_gate_scheduler_stops ()
{
    using namespace zlink::framework::runtime;

    class rejected_scheduler_t final : public offload_executor_t
    {
      public:
        rejected_scheduler_t () : offload_executor_t (1, "stopped-spot-gate") {}
        bool try_submit_internal (std::function<void ()>) override { return false; }
    } worker;
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::actor_delivery ());
    actor->attach_spot_gate (gate);
    const bool accepted =
      actor->try_post_async ("stopped-before-claim", [] (auto complete) { complete ([] {}); });
    const auto outcome = actor->cancel_submission (1).result ().value ();
    actor->cancel_pending ();
    gate->cancel_pending ();
    actor->drain ();
    gate->drain ();
    return !accepted && outcome == serial_cancel_submission_outcome_t::already_terminal
           && actor->pending_count () == 0 && gate->pending_count () == 0;
}

bool verify_spot_wide_actor_ready_lane_tracks_lifecycle ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    std::mutex order_mutex;
    std::vector<std::string> order;
    const auto record = [&] (std::string name) {
        std::lock_guard lock (order_mutex);
        order.push_back (std::move (name));
    };
    serial_test_blocker_t owner;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        owner.release ();
        fixture.drain ();
    });
    if (!fixture.serial.execute_spot ("lane-gate-owner", owner.work ()) || !owner.wait_for_entry ())
        return false;
    auto actor = fixture.serial.actor_executor ("actor-a");
    const bool accepted_application =
      fixture.serial.execute_actor ("actor-a", "queued-application", [&] (auto complete) {
          record ("application");
          complete ([] {});
      });
    if (!accepted_application || !wait_until ([&] {
            return fixture.spot_queue->pending_count (serial_work_lane_t::application) >= 2;
        })) {
        owner.release ();
        return false;
    }
    const bool accepted_lifecycle =
      actor->execute_lifecycle ("ready-lifecycle", [&] { record ("lifecycle"); });
    owner.release ();
    fixture.spot_queue->drain ();
    actor->queue ()->drain ();
    std::lock_guard lock (order_mutex);
    return accepted_application && accepted_lifecycle
           && order == std::vector<std::string>{"lifecycle", "application"};
}

bool verify_spot_wide_handoff_allows_spot_control ()
{
    using namespace zlink::framework::runtime;
    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    auto actor = fixture.serial.actor_executor ("actor-a");
    auto reserved = actor->queue ()->reserve_barrier_next ("handoff-control", true);
    if (!reserved)
        return false;
    serial_execution_queue_t::async_completion_t settle;
    std::mutex mutex;
    std::atomic_bool reached{false};
    std::atomic_bool control_ran{false};
    const auto activated = reserved.value ()->activate_async ([&] (auto complete) {
        std::lock_guard lock (mutex);
        settle = [complete = std::move (complete)] (auto finish) mutable {
            finish ();
            complete (zlink::framework::result_t<void>::success ());
        };
        reached.store (true);
    });
    // Settles the held barrier and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        if (activated) {
            while (!reached.load ())
                std::this_thread::yield ();
            std::lock_guard lock (mutex);
            if (settle) {
                settle ([] {});
                settle = nullptr;
            }
        }
        actor->queue ()->drain ();
        fixture.drain ();
    });
    if (!activated || !wait_until ([&] { return reached.load (); }))
        return false;
    const bool posted = fixture.serial.execute_spot ("handoff-peer-control", [&] (auto complete) {
        control_ran.store (true);
        complete ([] {});
    });
    const bool progressed = posted && wait_until ([&] { return control_ran.load (); });
    {
        std::lock_guard lock (mutex);
        settle ([] {});
        settle = nullptr;
    }
    actor->queue ()->drain ();
    fixture.spot_queue->drain ();
    return progressed;
}

bool verify_spot_wide_terminal_retains_gate_lifetime ()
{
    using namespace zlink::framework::runtime;
    // Declared before the executor so the settle job never outlives it.
    std::atomic_bool settled{false};
    offload_executor_t executor (1);
    auto queue = std::make_shared<serial_execution_queue_t> (
      executor, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    serial_test_blocker_t owner;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        owner.release ();
        if (queue)
            queue->drain ();
    });
    if (!queue->try_post_async ("lifetime-owner", owner.work ()) || !owner.wait_for_entry ())
        return false;
    if (!queue->try_post ("last-context-reference", [retained = queue] {}))
        return false;
    queue.reset ();
    owner.release ();
    if (!executor.try_submit_internal ([&] { settled.store (true); }))
        return false;
    return wait_until ([&] { return settled.load (); });
}
zlink::framework::task_t<void> await_lifecycle_terminal (
  std::shared_ptr<zlink::framework::task_completion_source_t<void>> terminal)
{
    co_await terminal->task ();
}

/* A lifecycle callback such as OnLeaveActor receives raw Spot and Actor
 * references. The work object that owns them must live until the callback
 * terminal, not only until its first suspension. */
bool verify_spot_serial_task_async_retains_work_until_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    struct actor_release_guard_t
    {
        std::shared_ptr<int> actor;
        std::promise<void> *released;

        actor_release_guard_t (std::shared_ptr<int> value, std::promise<void> &release_completed) :
            actor (std::move (value)), released (&release_completed)
        {
        }

        ~actor_release_guard_t ()
        {
            actor.reset ();
            released->set_value ();
        }
    };

    // Declared before the executor and owner so queued work never outlives them.
    std::promise<void> entered;
    std::promise<result_t<void>> settled;
    std::promise<void> released;
    const auto released_result = released.get_future ();
    auto executor = std::make_shared<runtime::offload_executor_t> (1, "spot-serial-retain");
    auto owner = std::make_shared<spot_context_state_t> ();
    owner->serial_executor = executor;
    owner->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    auto terminal = std::make_shared<task_completion_source_t<void>> ();
    // Completes the callback terminal so the queue can drain on early returns.
    scope_exit_t complete_terminal (
      [&] { (void) terminal->complete (result_t<void>::success ()); });
    auto actor_instance = std::make_shared<int> (7);
    const std::weak_ptr<int> weak_actor = actor_instance;
    auto work_guard =
      std::make_shared<actor_release_guard_t> (std::move (actor_instance), released);
    owner->run_serial_task_async (
      "retained-spot-lifecycle-leave",
      [work_guard = std::move (work_guard), terminal, &entered] {
          entered.set_value ();
          return await_lifecycle_terminal (terminal);
      },
      [&settled] (result_t<void> value) { settled.set_value (std::move (value)); });
    if (entered.get_future ().wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    auto settled_result = settled.get_future ();
    const bool retained_while_suspended = !weak_actor.expired ();
    terminal->complete (result_t<void>::success ());
    if (settled_result.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    const bool released_after_terminal =
      released_result.wait_for (std::chrono::seconds (5)) == std::future_status::ready
      && weak_actor.expired ();
    const bool succeeded = static_cast<bool> (settled_result.get ());
    owner->serial_queue->drain ();
    if (!retained_while_suspended || !succeeded || !released_after_terminal)
        std::cerr << "lifecycle work lifetime: retained=" << retained_while_suspended
                  << " succeeded=" << succeeded << " released=" << released_after_terminal << '\n';
    return retained_while_suspended && succeeded && released_after_terminal;
}

bool verify_spot_serial_task_async_shutdown_settlement ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // A queued lifecycle callback is removed by shutdown and settles its
    // observer exactly once without invoking application work.
    {
        // Declared before the executor so the blocking job and the callbacks never outlive them.
        std::mutex worker_mutex;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        std::mutex result_mutex;
        std::condition_variable result_changed;
        std::optional<result_t<void>> result;
        std::atomic_int work_calls = 0;
        std::atomic_int completion_calls = 0;
        auto executor =
          std::make_shared<runtime::offload_executor_t> (1, "spot-serial-queued-cancel");
        auto owner = std::make_shared<spot_context_state_t> ();
        owner->serial_executor = executor;
        owner->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *executor, runtime::serial_execution_queue_options_t{});
        auto queue = owner->serial_queue;
        // Releases the blocking worker job so the queue can drain on early returns.
        scope_exit_t release_worker_on_exit ([&] {
            {
                std::lock_guard lock (worker_mutex);
                release_worker = true;
            }
            worker_changed.notify_all ();
        });
        if (!executor->try_submit_internal ([&] {
                std::unique_lock lock (worker_mutex);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_mutex);
            if (!worker_changed.wait_for (lock, std::chrono::seconds (1),
                                          [&] { return worker_entered; })) {
                return false;
            }
        }
        owner->run_serial_task_async (
          "queued-spot-lifecycle-cancel",
          [&] () -> task_t<void> {
              ++work_calls;
              co_return;
          },
          [&] (result_t<void> value) {
              ++completion_calls;
              {
                  std::lock_guard lock (result_mutex);
                  result.emplace (std::move (value));
              }
              result_changed.notify_all ();
          });
        queue->cancel_pending ();
        {
            std::unique_lock lock (result_mutex);
            if (!result_changed.wait_for (lock, std::chrono::seconds (1),
                                          [&] { return result.has_value (); })) {
                return false;
            }
        }
        {
            std::lock_guard lock (worker_mutex);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue->drain ();
        if (*result || result->error_kind () != framework_error_kind_t::shutting_down
            || work_calls.load () != 0 || completion_calls.load () != 1) {
            return false;
        }
    }

    // Shutdown settles the owned waiter while preserving its pending source.
    // The terminal callback releases the owner before late source completion.
    {
        // Declared before the executor so queued work and callbacks never outlive them.
        std::mutex gate;
        std::condition_variable changed;
        bool entered = false;
        std::optional<result_t<void>> result;
        std::atomic_int completion_calls = 0;
        auto executor =
          std::make_shared<runtime::offload_executor_t> (1, "spot-serial-active-cancel");
        auto owner = std::make_shared<spot_context_state_t> ();
        owner->serial_executor = executor;
        owner->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *executor, runtime::serial_execution_queue_options_t{});
        auto queue = owner->serial_queue;
        auto callback_terminal = std::make_shared<task_completion_source_t<void>> ();
        auto source_task = callback_terminal->task ();
        // Completes the callback terminal so the queue can drain on early returns.
        scope_exit_t complete_terminal (
          [&] { (void) callback_terminal->complete (result_t<void>::success ()); });
        owner->run_serial_task_async (
          "active-spot-lifecycle-cancel",
          [callback_terminal, &gate, &changed, &entered] () -> task_t<void> {
              {
                  std::lock_guard lock (gate);
                  entered = true;
              }
              changed.notify_all ();
              co_await callback_terminal->task ();
          },
          [&] (result_t<void> value) {
              ++completion_calls;
              {
                  std::lock_guard lock (gate);
                  result.emplace (std::move (value));
              }
              changed.notify_all ();
          });
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                return false;
            }
        }
        std::weak_ptr<spot_context_state_t> weak_owner = owner;
        queue->cancel_pending ();
        owner.reset ();
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (lock, std::chrono::seconds (1),
                                   [&] { return result.has_value (); })) {
                return false;
            }
        }
        if (source_task.result_for (std::chrono::milliseconds (0)) || completion_calls.load () != 1
            || !callback_terminal->complete (result_t<void>::success ())
            || callback_terminal->complete (result_t<void>::success ())
            || !source_task.result_for (std::chrono::milliseconds (0))
            || completion_calls.load () != 1) {
            return false;
        }
        queue->drain ();
        queue.reset ();
        executor->drain ();
        if (*result || result->error_kind () != framework_error_kind_t::shutting_down
            || completion_calls.load () != 1 || !weak_owner.expired ()) {
            return false;
        }
    }
    return true;
}
// Relocation flow §4.2 cut S at a SpotWide gate: the started handler (A) stays, the imported
// unstarted record (B) and one published but not imported at the cut (P) leave once, and a
// later import does not queue P again. A record published after the cut (E) still runs.
bool verify_seal_cut_takes_gate_and_publications ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t worker;
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    const auto record = [] {
        auto value = std::make_shared<serial_application_record_t> ();
        value->payload ().emplace ();
        return value;
    };
    std::vector<std::string> ran;
    serial_execution_queue_t::async_completion_t finish_a;
    const auto post = [&] (std::string name, std::shared_ptr<serial_application_record_t> owner,
                           bool hold) {
        serial_work_options_t options;
        options.application_record = std::move (owner);
        return gate->try_post_async (
          name,
          [&, name, hold] (serial_execution_queue_t::async_completion_t complete) {
              ran.push_back (name);
              if (hold)
                  finish_a = std::move (complete);
              else
                  complete ([] {});
          },
          std::move (options));
    };
    const auto a = record (), b = record (), p = record (), e = record ();
    bool passed = post ("a", a, true);
    worker.run_all ();
    passed = passed && ran == std::vector<std::string>{"a"} && finish_a && post ("b", b, false);
    worker.run_all ();
    bool sealed = false;
    auto cut = gate->take_application_suffix ([&] { sealed = true; },
                                              [&] (const std::function<void ()> &step) {
                                                  passed = post ("p", p, false) && passed;
                                                  step ();
                                                  return true;
                                              });
    worker.run_all ();
    std::vector<std::shared_ptr<serial_application_record_t>> exported;
    if (cut.await_ready () && cut.result ())
        exported = cut.result ().value ();
    passed = passed && sealed && exported.size () == 2 && exported[0] == b && exported[1] == p;
    passed = post ("e", e, false) && passed;
    finish_a ([] {});
    worker.run_all ();
    gate->drain ();
    worker.run_all ();
    std::cout << "seal-cut-gate exported=" << exported.size () << " sealed=" << sealed
              << " ran=" << ran.size () << " pending=" << gate->pending_count () << '\n';
    return passed && ran == std::vector<std::string>{"a", "e"} && gate->pending_count () == 0;
}

bool verify_cancelled_barrier_settles_running_phase (bool shared,
                                                     bool cancel_queue,
                                                     bool reached_first)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    auto reserved = queue->reserve_barrier_next ("cancel-running-phase");
    if (!reserved)
        return false;
    const auto barrier = reserved.value ();
    if (reached_first)
        worker.run_all ();
    fw::task_completion_source_t<void> release;
    bool entered = false;
    auto phase = barrier->run ([&] {
        entered = true;
        return release.task ();
    });
    int phase_completions = 0;
    fw::detail::observe_task_terminal (phase,
                                       [&] (const fw::result_t<void> &) { ++phase_completions; });
    worker.run_all ();
    const bool phase_started = entered && !phase.await_ready ();
    if (cancel_queue)
        queue->cancel_pending ();
    else
        barrier->cancel ();
    worker.run_all ();
    auto terminal = barrier->terminal ();
    const bool terminal_ready = terminal.await_ready ();
    const bool phase_ready = phase.await_ready ();
    const bool phase_failed =
      phase_ready && !phase.result ()
      && phase.result ().error_kind () == fw::framework_error_kind_t::shutting_down;
    const bool terminal_failed = terminal_ready && !terminal.result ();
    release.complete (fw::result_t<void>::success ());
    worker.run_all ();
    queue->close ();
    queue->drain ();
    worker.retire ();
    const bool passed = phase_started && terminal_ready && phase_failed && phase_completions == 1;
    std::cout << "cancel-running-barrier-phase shared=" << shared
              << " cancel_queue=" << cancel_queue << " reached_first=" << reached_first
              << " entered=" << entered << " terminal_ready=" << terminal_ready
              << " terminal_failed=" << terminal_failed << " phase_ready=" << phase_ready
              << " phase_failed=" << phase_failed << " phase_completions=" << phase_completions
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_barrier_phase_completion_allows_reentrant_phase (bool shared)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    auto reserved = queue->reserve_barrier_next ("reentrant-running-phase");
    if (!reserved)
        return false;
    const auto barrier = reserved.value ();
    worker.run_all ();
    fw::task_completion_source_t<void> release;
    int phase_runs = 0;
    bool activated = false;
    std::optional<fw::task_t<void>> second;
    auto first = barrier->run ([&] {
        ++phase_runs;
        return release.task ();
    });
    fw::detail::observe_task_terminal (first, [&] (const fw::result_t<void> &result) {
        if (!result)
            return;
        second = barrier->run ([&] {
            ++phase_runs;
            return fw::task_t<void> (fw::result_t<void>::success ());
        });
        fw::detail::observe_task_terminal (*second, [&] (const fw::result_t<void> &result) {
            if (result)
                (void) barrier->activate ([&] { activated = true; });
        });
    });
    worker.run_all ();
    const bool held = phase_runs == 1 && !first.await_ready ();
    release.complete (fw::result_t<void>::success ());
    worker.run_all ();
    const bool passed = held && first.await_ready () && first.result () && second
                        && second->await_ready () && second->result () && phase_runs == 2
                        && activated && barrier->terminal ().await_ready ()
                        && barrier->terminal ().result ();
    barrier->cancel ();
    worker.run_all ();
    queue->close ();
    queue->drain ();
    worker.retire ();
    std::cout << "reentrant-barrier-phase shared=" << shared << " runs=" << phase_runs
              << " activated=" << activated << " passed=" << passed << '\n';
    return passed;
}

bool verify_nested_barrier_phases (bool shared, bool cancel)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    auto reserved = queue->reserve_barrier_next ("nested-running-phase");
    if (!reserved)
        return false;
    const auto barrier = reserved.value ();
    fw::task_completion_source_t<void> release;
    bool outer_entered = false, inner_entered = false, activated = false;
    int outer_completions = 0, inner_completions = 0;
    std::optional<fw::task_t<void>> inner;
    auto outer = barrier->run ([&] () -> fw::task_t<void> {
        outer_entered = true;
        inner = barrier->run ([&] {
            inner_entered = true;
            return release.task ();
        });
        co_await *inner;
    });
    fw::detail::observe_task_terminal (outer, [&] (const fw::result_t<void> &result) {
        ++outer_completions;
        if (!cancel && result)
            (void) barrier->activate ([&] { activated = true; });
    });
    worker.run_all ();
    if (inner)
        fw::detail::observe_task_terminal (
          *inner, [&] (const fw::result_t<void> &) { ++inner_completions; });
    const bool nested =
      outer_entered && inner_entered && inner && !inner->await_ready () && !outer.await_ready ();
    bool settled = false;
    if (cancel) {
        queue->cancel_pending ();
        worker.run_all ();
        settled = outer.await_ready () && !outer.result ()
                  && outer.result ().error_kind () == fw::framework_error_kind_t::shutting_down
                  && inner && inner->await_ready () && !inner->result ()
                  && inner->result ().error_kind () == fw::framework_error_kind_t::shutting_down;
    }
    release.complete (fw::result_t<void>::success ());
    worker.run_all ();
    if (!cancel)
        settled = outer.await_ready () && outer.result () && inner && inner->await_ready ()
                  && inner->result () && activated && barrier->terminal ().await_ready ()
                  && barrier->terminal ().result ();
    const bool passed = nested && settled && outer_completions == 1 && inner_completions == 1;
    barrier->cancel ();
    worker.run_all ();
    queue->close ();
    queue->drain ();
    worker.retire ();
    std::cout << "nested-barrier-phases shared=" << shared << " cancel=" << cancel
              << " outer_entered=" << outer_entered << " inner_entered=" << inner_entered
              << " outer_completions=" << outer_completions
              << " inner_completions=" << inner_completions << " settled=" << settled
              << " passed=" << passed << '\n';
    return passed;
}
}
