/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/request_admission_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/execution_cases.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::test::execution
{
bool verify_relocation_ready_keeps_registering_turn_across_state_lane ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    runtime::offload_executor_t executor (2, "relocation-ready-turn");
    auto node = std::make_shared<spot_node_builder_state_t> ("relocation-ready-turn");
    auto owner = std::make_shared<spot_context_state_t> ();
    owner->node = node;
    owner->relocation_coordination_mode = spot_relocation_coordination_mode_t::application_signaled;
    owner->relocation_boundary_active = true;
    owner->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    runtime::serial_execution_queue_t other_queue (executor);

    // Keep the state lane claimed by another application's turn while the
    // registering handler submits its state mutations to that lane.
    std::atomic_bool keep_lane_claimed{true};
    std::atomic_bool awaiting_record{false};
    std::atomic_bool record_queued{false};
    std::promise<void> lane_claimed;
    auto claimed = lane_claimed.get_future ();
    bool announced = false;
    std::function<void ()> keep_draining;
    keep_draining = [&] {
        if (!keep_lane_claimed.load (std::memory_order_acquire))
            return;
        record_queued.store (false, std::memory_order_release);
        awaiting_record.store (true, std::memory_order_release);
        if (!announced) {
            announced = true;
            lane_claimed.set_value ();
        }
        while (keep_lane_claimed.load (std::memory_order_acquire)
               && !record_queued.load (std::memory_order_acquire))
            std::this_thread::yield ();
    };
    std::thread producer ([&] {
        while (keep_lane_claimed.load (std::memory_order_acquire)) {
            if (!awaiting_record.exchange (false, std::memory_order_acq_rel)) {
                std::this_thread::yield ();
                continue;
            }
            node->lane.try_post (keep_draining);
            record_queued.store (true, std::memory_order_release);
        }
    });
    other_queue.post_async ("claim-state-lane", [&] (auto complete) {
        node->lane.run (keep_draining).get ();
        complete ([] {});
    });
    if (claimed.wait_for (std::chrono::seconds (1)) != std::future_status::ready) {
        keep_lane_claimed.store (false, std::memory_order_release);
        producer.join ();
        other_queue.drain ();
        // Drain queued copies of keep_draining before its captures go out of scope.
        node->lane.run ([] {}).get ();
        return false;
    }

    bool registered = false;
    bool registering_turn_retained = false;
    bool same_turn_rejected = false;
    std::promise<void> registration_finished;
    auto finished = registration_finished.get_future ();
    owner->serial_queue->post ("register-relocation-ready", [&] {
        const auto registering_turn = capture_current_serial_turn ();
        try {
            owner->defer_relocation_ready ();
            registered = true;
            registering_turn_retained = owner->relocation_ready_turn.lock () == registering_turn;
            owner->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            same_turn_rejected =
              registered && error.kind () == framework_error_kind_t::invalid_operation;
        }
        registration_finished.set_value ();
    });
    finished.wait ();
    keep_lane_claimed.store (false, std::memory_order_release);
    producer.join ();
    other_queue.drain ();
    // Drain queued copies of keep_draining before its captures go out of scope.
    node->lane.run ([] {}).get ();
    owner->complete_relocation_ready (spot_relocation_ready_outcome_t::continued);
    owner->serial_queue->drain ();

    bool next_turn_allowed = false;
    owner->serial_queue->run ("next-application-turn", [&] {
        owner->ensure_relocation_turn_open ();
        next_turn_allowed = true;
    });
    const bool passed =
      registered && registering_turn_retained && same_turn_rejected && next_turn_allowed;
    if (!passed) {
        std::cerr << "relocation readiness: registered=" << registered
                  << " registering-turn-retained=" << registering_turn_retained
                  << " same-turn-rejected=" << same_turn_rejected
                  << " next-turn-allowed=" << next_turn_allowed << '\n';
    }
    return passed;
}
bool verify_relocation_readiness_root_rejection ()
{
    namespace fw = zlink::framework;
    namespace detail = zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    auto mesh =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("readiness-rejection");
    auto node = mesh->spot_state;
    node->worker_executor = std::make_shared<rt::offload_executor_t> (2, "readiness-rejection");
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->spot_id = fw::spot_id_t ("rejected-source");
    spot->spot_instance = std::make_shared<int> (1);
    spot->serial_executor = node->worker_executor;
    spot->relocation_coordination_mode =
      fw::spot_relocation_coordination_mode_t::application_signaled;
    spot->relocation_boundary_active = true;
    const auto coordinator = spot->ensure_spot_serial_executor ();
    spot->serial_queue = coordinator->spot_queue ();
    node->actor_spot_ids.emplace ("probe:member", spot->spot_id);
    const auto actor = coordinator->actor_executor ("probe:member")->queue ();
    std::promise<void> finished;
    bool rejected = false;
    spot->serial_queue->post ("reject-readiness-root", [&] {
        spot->serial_queue->close ();
        try {
            spot->defer_relocation_ready ();
        }
        catch (const fw::framework_exception_t &error) {
            rejected = error.kind () == fw::framework_error_kind_t::shutting_down;
        }
        finished.set_value ();
    });
    finished.get_future ().wait ();
    spot->serial_queue->drain ();
    actor->drain ();
    coordinator->close ();
    node->worker_executor->drain ();
    std::cout << "readiness-root-rejection rejected=" << rejected
              << " deferred=" << spot->relocation_ready_deferred
              << " holder=" << bool (spot->relocation_ready_barrier)
              << " pending=" << actor->pending_count () << '\n';
    return rejected && !spot->relocation_ready_deferred && !spot->relocation_ready_barrier
           && actor->pending_count () == 0;
}

bool verify_relocation_participant_resources (bool fail_reserve, bool abort)
{
    namespace fw = zlink::framework;
    namespace detail = zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    auto mesh =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("readiness-resources");
    auto node = mesh->spot_state;
    node->worker_executor = std::make_shared<rt::offload_executor_t> (2, "readiness-resources");
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->spot_id = fw::spot_id_t ("resource-source");
    spot->spot_instance = std::make_shared<int> (1);
    spot->serial_executor = node->worker_executor;
    spot->relocation_coordination_mode =
      fw::spot_relocation_coordination_mode_t::application_signaled;
    spot->relocation_boundary_active = true;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    const auto coordinator = spot->ensure_spot_serial_executor ();
    spot->serial_queue = coordinator->spot_queue ();
    std::vector<std::shared_ptr<rt::serial_execution_queue_t>> actors;
    for (const auto &key : {"probe:first", "probe:second"}) {
        node->actor_spot_ids.emplace (key, spot->spot_id);
        actors.push_back (coordinator->actor_executor (key)->queue ());
    }
    std::atomic_int completions{0};
    spot->lifecycle.on_relocation_ready_completed =
      [&] (void *, const fw::spot_relocation_ready_completion_t &completion) {
          if (completion.outcome == fw::spot_relocation_ready_outcome_t::continued)
              ++completions;
      };
    std::promise<void> handler_entered;
    detail::task_scheduler_t resume;
    rt::serial_execution_queue_t::async_completion_t terminal;
    if (fail_reserve) {
        actors[1]->close ();
    } else {
        actors[1]->post_async ("participant-resource-handler", [&] (auto complete) {
            terminal = std::move (complete);
            const auto turn = detail::capture_current_serial_turn ();
            resume = turn->resume_scheduler ();
            turn->release ();
            handler_entered.set_value ();
        });
        handler_entered.get_future ().wait ();
    }
    std::promise<void> registered;
    spot->serial_queue->post ("resource-readiness", [&] {
        spot->defer_relocation_ready ();
        registered.set_value ();
    });
    registered.get_future ().wait ();
    std::atomic_bool captured{false};
    auto capture = spot->relocation_ready_barrier->run ([&] () -> fw::task_t<void> {
        captured.store (true);
        co_return;
    });
    if (!fail_reserve) {
        std::promise<void> acquisition_checkpoint;
        spot->serial_queue->post ("participant-acquisition-checkpoint",
                                  [&] { acquisition_checkpoint.set_value (); });
        acquisition_checkpoint.get_future ().wait ();
        if (abort)
            spot->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
        else
            actors[1]->cancel_pending ();
    }
    const auto result = capture.result ();
    std::size_t pending_before_continued = 0;
    int continued_before = completions.load ();
    bool next_before_continued = false;
    std::atomic_bool next_started{false};
    std::promise<void> next_terminal;
    auto next_completed = next_terminal.get_future ();
    if (!abort) {
        std::promise<void> released_head_checkpoint;
        // The participant head was connected before acquisition failed. This
        // later root FIFO item observes its terminal if cancellation released it.
        spot->serial_queue->post ("failed-participant-release-checkpoint",
                                  [&] { released_head_checkpoint.set_value (); });
        released_head_checkpoint.get_future ().wait ();
        pending_before_continued = actors[0]->pending_count ();
        actors[0]->post ("after-failed-participant-acquisition", [&] {
            next_started.store (true);
            next_terminal.set_value ();
        });
        // A retained boundary is already a deterministic failure. Release it
        // through Continued instead of waiting on work behind that boundary.
        if (pending_before_continued == 0)
            next_completed.wait ();
        next_before_continued = next_started.load ();
        spot->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
    }
    if (!fail_reserve)
        resume ([&] { terminal ([] {}); });
    for (const auto &actor : actors)
        actor->drain ();
    spot->serial_queue->drain ();
    coordinator->close ();
    node->worker_executor->drain ();
    std::cout << "readiness-resources reserve_failure=" << fail_reserve << " abort=" << abort
              << " capture_started=" << captured.load () << " result=" << bool (result)
              << " error=" << (result ? 0 : static_cast<int> (result.error_kind ()))
              << " pending_before_continued=" << pending_before_continued
              << " next_before_continued=" << next_before_continued
              << " continued_before=" << continued_before << " continued=" << completions.load ()
              << " pending_first=" << actors[0]->pending_count ()
              << " pending_second=" << actors[1]->pending_count () << '\n';
    const auto passed =
      !result && !captured.load ()
      && (abort
          || (pending_before_continued == 0 && next_before_continued && continued_before == 0))
      && completions.load () == 1 && actors[0]->pending_count () == 0
      && actors[1]->pending_count () == 0;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

bool verify_relocation_membership_pending_releases_root_turn ()
{
    namespace fw = zlink::framework;
    namespace detail = zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    auto mesh = fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (
      "readiness-node-pending");
    auto node = mesh->spot_state;
    node->worker_executor = std::make_shared<rt::offload_executor_t> (2, "readiness-node-pending");
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->spot_id = fw::spot_id_t ("node-pending-source");
    spot->spot_instance = std::make_shared<int> (1);
    spot->serial_executor = node->worker_executor;
    spot->relocation_coordination_mode =
      fw::spot_relocation_coordination_mode_t::application_signaled;
    spot->relocation_boundary_active = true;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    const auto coordinator = spot->ensure_spot_serial_executor ();
    spot->serial_queue = coordinator->spot_queue ();
    std::promise<void> node_entered;
    auto node_entered_future = node_entered.get_future ();
    std::promise<void> node_release;
    auto node_release_future = node_release.get_future ();
    std::promise<void> node_terminal;
    auto node_terminal_future = node_terminal.get_future ();
    std::promise<void> registered;
    std::atomic_bool captured{false};
    std::atomic_int continued{0};
    spot->lifecycle.on_relocation_ready_completed =
      [&] (void *, const fw::spot_relocation_ready_completion_t &completion) {
          if (completion.outcome == fw::spot_relocation_ready_outcome_t::continued)
              ++continued;
      };
    spot->serial_queue->post ("register-before-node-hold", [&] {
        spot->defer_relocation_ready ();
        // Registration has completed every synchronous state capture. Hold the
        // node lane before this root turn ends and its lifecycle provider starts.
        node->lane.try_post ([&] {
            node_entered.set_value ();
            node_release_future.wait ();
            node_terminal.set_value ();
        });
        node_entered_future.wait ();
        registered.set_value ();
    });
    registered.get_future ().wait ();
    auto capture = spot->relocation_ready_barrier->run ([&] () -> fw::task_t<void> {
        captured.store (true);
        co_return;
    });
    std::promise<void> root_checkpoint;
    bool captured_while_node_pending = false;
    spot->serial_queue->post ("root-progress-with-node-pending", [&] {
        captured_while_node_pending = captured.load ();
        root_checkpoint.set_value ();
    });
    root_checkpoint.get_future ().wait ();
    node_release.set_value ();
    node_terminal_future.wait ();
    const auto result = capture.result ();
    spot->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
    spot->serial_queue->drain ();
    node->lane.run ([] {}).get ();
    coordinator->close ();
    node->worker_executor->drain ();
    std::cout << "readiness-node-pending root_checkpoint=1 captured_while_pending="
              << captured_while_node_pending << " captured=" << captured.load ()
              << " result=" << bool (result) << " continued=" << continued.load ()
              << " pending=" << spot->serial_queue->pending_count () << '\n';
    const auto passed = !captured_while_node_pending && captured.load () && result
                        && continued.load () == 1 && spot->serial_queue->pending_count () == 0;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
bool verify_teardown_observes_actor_handoff_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "source-terminal-drain");
    auto workers = std::make_shared<runtime::offload_executor_t> (1);
    // The fixture owns this executor through cleanup, including a failing snapshot.
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->spot_id = "source-terminal-spot";
    context->serial_executor = workers;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *workers, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));
    auto executor = context->ensure_spot_serial_executor ();
    auto actor = executor->actor_executor ("source-terminal-actor");
    task_completion_source_t<void> source_terminal;
    task_completion_source_t<void> entered;
    task_completion_source_t<bool> first_event;
    deferred_barrier_t::async_completion_t finish_source = [&] (result_t<void> result) {
        source_terminal.complete (std::move (result));
    };
    const auto retained =
      actor->queue ()->reserve_deferred_barrier_next ("source-handoff-terminal");
    if (!retained)
        return false;
    const auto activated = retained.value ()->activate_async ([&] (auto complete) {
        auto pending = std::make_shared<task_t<void>> (source_terminal.task ());
        observe_task_terminal (*pending,
                               [pending, complete = std::move (complete)] (
                                 const result_t<void> &result) mutable { complete (result); });
    });
    if (!activated)
        return false;
    actor->queue ()->notify_control ("source-terminal-published",
                                     [&] { entered.complete (result_t<void>::success ()); });
    entered.task ().result ().value ();
    std::atomic_bool sentinel_ran{false};
    const auto sentinel = actor->queue ()->try_post_cancellable_async (
      "pending-after-source",
      [&] (auto complete) {
          sentinel_ran.store (true, std::memory_order_release);
          complete ([] {});
      },
      [&] { first_event.complete (result_t<bool>::success (true)); });
    const bool pending = sentinel && actor->queue ()->pending_count () >= 2
                         && !sentinel_ran.load (std::memory_order_acquire);
    if (!pending) {
        actor->queue ()->cancel_pending ();
        finish_source (result_t<void>::success ());
        drain_spot_node_executors (*node);
        actor->queue ()->drain ();
        workers->drain ();
        std::cerr << "Actor source teardown prerequisite pending=0\n";
        return false;
    }
    std::thread teardown ([&] {
        drain_spot_node_executors (*node);
        first_event.complete (result_t<bool>::success (false));
    });
    const bool cancelled_before_teardown = first_event.task ().result ().value ();
    // The immutable result precedes fixture cleanup on the old implementation.
    if (!cancelled_before_teardown)
        actor->queue ()->cancel_pending ();
    finish_source (result_t<void>::success ());
    teardown.join ();
    actor->queue ()->drain ();
    workers->drain ();
    std::cerr << "Actor source teardown pending=" << pending
              << " cancelled-before-teardown=" << cancelled_before_teardown
              << " sentinel-ran=" << sentinel_ran.load () << '\n';
    const auto passed = pending && cancelled_before_teardown && !sentinel_ran.load ();
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}


bool verify_aggregate_capture_waits_for_participant_terminal ()
{
    namespace fw = zlink::framework;
    namespace detail = zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    namespace stateful = rt::stateful;
    auto mesh =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("aggregate-boundary");
    auto node = mesh->spot_state;
    node->worker_executor = std::make_shared<rt::offload_executor_t> (2, "aggregate-boundary");
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->spot_id = fw::spot_id_t ("aggregate-source");
    spot->spot_instance = std::make_shared<int> (0);
    spot->serial_executor = node->worker_executor;
    spot->relocation_coordination_mode =
      fw::spot_relocation_coordination_mode_t::application_signaled;
    spot->relocation_boundary_active = true;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    const auto coordinator = spot->ensure_spot_serial_executor ();
    spot->serial_queue = coordinator->spot_queue ();
    std::vector<std::shared_ptr<rt::serial_execution_queue_t>> actors;
    for (const auto &key : {"probe:actor", "probe:other"}) {
        node->actor_spot_ids.emplace (key, spot->spot_id);
        node->actor_generations.emplace (key, 1);
        actors.push_back (coordinator->actor_executor (key)->queue ());
    }
    // A departed Actor's retained executor is not an aggregate participant.
    coordinator->actor_executor ("probe:former")->queue ()->close ();
    std::vector<std::promise<void>> handler_entered (2);
    std::promise<void> ready_registered;
    std::promise<void> capture_admitted;
    std::atomic_int handler_terminal{0};
    std::promise<void> first_terminal;
    std::atomic_bool captured{false};
    std::atomic_bool captured_after_terminal{false};
    std::atomic_int terminals_at_capture{-1};
    std::vector<detail::task_scheduler_t> resume (2);
    std::vector<rt::serial_execution_queue_t::async_completion_t> terminal (2);
    for (std::size_t index = 0; index != actors.size (); ++index) {
        actors[index]->post_async ("aggregate-participant-handler", [&, index] (auto complete) {
            terminal[index] = std::move (complete);
            const auto turn = detail::capture_current_serial_turn ();
            resume[index] = turn->resume_scheduler ();
            (void) turn->release ();
            handler_entered[index].set_value ();
        });
        handler_entered[index].get_future ().wait ();
    }
    spot->serial_queue->post ("aggregate-ready", [&] {
        spot->defer_relocation_ready ();
        ready_registered.set_value ();
    });
    ready_registered.get_future ().wait ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"spot", "actor"},
                                            .weight = 100,
                                            .active_capacity = 100,
                                            .pending_capacity = 100}});
    const auto create = [&] (stateful::object_kind_t kind, const std::string &key,
                             const std::string &type) {
        const auto attempt = objects.begin_create (
          {.kind = kind, .key = key, .stable_type = type, .mesh_name = "mesh", .exclusive = true});
        objects.commit_create (attempt.attempt);
        return attempt.object;
    };
    const auto source = create (stateful::object_kind_t::user_spot, "aggregate-source", "spot");
    const auto participant = create (stateful::object_kind_t::actor, "actor", "actor");
    const auto other = create (stateful::object_kind_t::actor, "other", "actor");
    objects.configure_relocation_state (
      [&] (const auto &object, const auto &, auto)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          stateful::frozen_object_state_t state;
          if (object.kind == stateful::object_kind_t::user_spot) {
              // This is the same readiness phase used by Spot capture. Publish
              // admission after run() returns, so neither result relies on time.
              auto pending = spot->relocation_ready_barrier->run ([&] () -> fw::task_t<void> {
                  terminals_at_capture.store (handler_terminal.load ());
                  captured_after_terminal.store (terminals_at_capture.load () == 2);
                  captured.store (true);
                  co_return;
              });
              capture_admitted.set_value ();
              co_await pending;
          }
          co_return state;
      },
      [] (const auto &, const auto &, auto) -> zlink::framework::task_t<bool> { co_return true; });
    stateful::stateful_error_t error = stateful::stateful_error_t::conflict;
    std::atomic_bool membership_ran{false};
    detail::spot_node_runtime_t (node).dispatch_wire_actor_join_admission (
      spot->spot_id, {},
      [&] () -> fw::task_t<void> {
          membership_ran.store (true);
          co_return;
      },
      [] (fw::result_t<void>) {});
    std::thread capture ([&] {
        error = objects.try_seal_relocation_aggregate ({source, participant, other})
                  .result ()
                  .value ()
                  .error;
        // Red cleanup releases an early root hold before the queued continuation.
        spot->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
    });
    capture_admitted.get_future ().wait ();
    // Resume the existing handler through its own scheduler. A root readiness
    // hold installed too early would prevent this continuation from finishing.
    resume[0]([&] {
        ++handler_terminal;
        terminal[0]([] {});
        first_terminal.set_value ();
    });
    first_terminal.get_future ().wait ();
    std::promise<void> checkpoint;
    bool all_participants_required = false;
    bool checkpoint_capture = false;
    bool checkpoint_admission = false;
    spot->serial_queue->post ("aggregate-one-participant-terminal", [&] {
        checkpoint_capture = captured.load ();
        checkpoint_admission = membership_ran.load ();
        all_participants_required = !checkpoint_capture && !checkpoint_admission;
        checkpoint.set_value ();
    });
    checkpoint.get_future ().wait ();
    resume[1]([&] {
        ++handler_terminal;
        terminal[1]([] {});
    });
    capture.join ();
    spot->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
    for (const auto &actor : actors)
        actor->drain ();
    spot->serial_queue->drain ();
    coordinator->close ();
    node->worker_executor->drain ();
    std::cout << "aggregate-boundary terminals_at_capture=" << terminals_at_capture.load ()
              << " capture_started=" << captured.load ()
              << " checkpoint_capture=" << checkpoint_capture
              << " checkpoint_admission=" << checkpoint_admission
              << " final_admission=" << membership_ran.load ()
              << " native_seal_error=" << static_cast<int> (error) << '\n';
    const auto passed = all_participants_required && membership_ran.load () && captured.load ()
                        && captured_after_terminal.load ()
                        && error == stateful::stateful_error_t::none;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

bool verify_source_suffix_native_order_and_rollback ()
{
    using namespace zlink::framework::runtime;
    using namespace zlink::framework::runtime::stateful;
    stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .weight = 100,
                                            .active_capacity = 100,
                                            .pending_capacity = 100}});
    const auto created = objects.begin_create ({.kind = object_kind_t::actor,
                                                .key = "suffix-order",
                                                .stable_type = "actor",
                                                .mesh_name = "mesh",
                                                .exclusive = true});
    if (created.status != create_status_t::reserved) {
        std::cout << "suffix-native create_status=" << int (created.status) << '\n';
        return false;
    }
    const auto committed = objects.commit_create (created.attempt);
    if (committed != stateful_error_t::none) {
        std::cout << "suffix-native commit_error=" << int (committed) << '\n';
        return false;
    }
    const auto actor = created.object;
    auto original = std::make_shared<serial_application_record_t> ();
    original->admission.take_over ();
    objects.configure_relocation_state (
      [original] (const auto &, const auto &, auto)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t state;
          // Coordinator holds and exported suffixes need not form one sorted
          // range. Native sequence is the existing acceptance-order owner.
          state.pending_application.push_back ({4, {4, 4}});
          state.pending_application.push_back ({2, {2, 2}});
          state.pending_application.back ().accepted_owner = original;
          co_return state;
      },
      [] (const auto &, const auto &, auto) -> zlink::framework::task_t<bool> { co_return true; });
    const auto enqueued = objects.enqueue (actor, turn_domain_t::application, {3, {3, 3}});
    if (enqueued != stateful_error_t::none) {
        std::cout << "suffix-native enqueue_error=" << int (enqueued) << '\n';
        return false;
    }
    const auto sealed = objects.try_seal_relocation_aggregate ({actor}).result ().value ();
    if (sealed.error != stateful_error_t::none) {
        std::cout << "suffix-native seal_error=" << int (sealed.error) << '\n';
        return false;
    }
    bool passed = objects.abort_relocation_before_cutover (sealed.seal.token).result ().value ()
                  == stateful_error_t::none;
    std::cout << "suffix-native abort=" << passed
              << " pending=" << objects.pending (actor, turn_domain_t::application)
              << " bytes=" << objects.pending_bytes (actor, turn_domain_t::application)
              << " original_saved=" << original->saved_work () << '\n';
    passed = passed && objects.pending (actor, turn_domain_t::application) == 3
             && objects.pending_bytes (actor, turn_domain_t::application)
                  == 3 * (dispatch_limits::fixed_work_byte_cost + 2);
    for (const auto expected : {2u, 3u, 4u}) {
        const auto [error, turn] = objects.try_claim (actor, turn_domain_t::application);
        std::cout << "suffix-native claim expected=" << expected << " error=" << int (error)
                  << " sequence=" << (turn ? turn->sequence : 0)
                  << " original=" << (turn && turn->accepted_owner == original) << " saved="
                  << (turn && turn->accepted_owner && turn->accepted_owner->saved_work ())
                  << " passed_before=" << passed << '\n';
        passed = passed && error == stateful_error_t::none && turn && turn->sequence == expected;
        if (turn) {
            if (expected == 2)
                passed =
                  passed && turn->accepted_owner == original && turn->accepted_owner->saved_work ();
            (void) objects.complete_claim (actor, turn_domain_t::application);
        }
    }
    passed = passed && objects.pending (actor, turn_domain_t::application) == 0
             && objects.pending_bytes (actor, turn_domain_t::application) == 0;
    objects.configure_relocation_state (
      [original] (const auto &, const auto &, auto)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t state;
          // Exercise abort after suffix ownership moved, before frozen
          // materialization, using the existing 64 MiB capture limit.
          state.application_state.resize (64u * 1024u * 1024u + 1);
          state.pending_application.push_back ({5, {5, 5}});
          state.pending_application.back ().accepted_owner = original;
          co_return state;
      },
      [] (const auto &, const auto &, auto) -> zlink::framework::task_t<bool> { co_return true; });
    const auto failed = objects.try_seal_relocation_aggregate ({actor}).result ().value ();
    std::cout << "suffix-native failed_capture error=" << int (failed.error)
              << " error_expected=" << (failed.error == stateful_error_t::conflict)
              << " pending=" << objects.pending (actor, turn_domain_t::application)
              << " bytes=" << objects.pending_bytes (actor, turn_domain_t::application)
              << " passed_before=" << passed << '\n';
    passed = passed && failed.error == stateful_error_t::conflict
             && objects.pending (actor, turn_domain_t::application) == 1
             && objects.pending_bytes (actor, turn_domain_t::application)
                  == dispatch_limits::fixed_work_byte_cost + 2;
    const auto [error, restored] = objects.try_claim (actor, turn_domain_t::application);
    std::cout << "suffix-native restored error=" << int (error)
              << " sequence=" << (restored ? restored->sequence : 0)
              << " original=" << (restored && restored->accepted_owner == original) << " saved="
              << (restored && restored->accepted_owner && restored->accepted_owner->saved_work ())
              << " passed_before=" << passed << '\n';
    passed = passed && error == stateful_error_t::none && restored && restored->sequence == 5
             && restored->accepted_owner == original && restored->accepted_owner->saved_work ();
    if (restored)
        (void) objects.complete_claim (actor, turn_domain_t::application);
    std::cout << "suffix-native final pending="
              << objects.pending (actor, turn_domain_t::application)
              << " bytes=" << objects.pending_bytes (actor, turn_domain_t::application)
              << " passed=" << passed << '\n';
    return passed && objects.pending (actor, turn_domain_t::application) == 0
           && objects.pending_bytes (actor, turn_domain_t::application) == 0;
}
bool verify_public_local_member_defer (bool fail_first,
                                       bool other_handler,
                                       public_member_policy_t policy)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace detail = fw::detail;
    auto mesh =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("public-members");
    auto node = mesh->spot_state;
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    scheduler->publication_fence (fw::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    fw::service_collection_t services;
    node->root_services = services.build_provider ();
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("public-members-node");
    spot->spot_id = fw::spot_id_t ("source");
    spot->spot_instance = std::make_shared<int> (1);
    spot->serial_executor = scheduler;
    if (policy == public_member_policy_t::per_actor)
        spot->execution_mode = fw::user_spot_execution_mode_t::per_actor;
    if (policy == public_member_policy_t::entry)
        spot->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    auto serial = spot->ensure_spot_serial_executor ();
    auto root = serial->spot_queue ();
    auto a = serial->actor_executor ("probe:a")->queue ();
    auto b = serial->actor_executor ("probe:b")->queue ();
    node->actor_spot_ids.emplace ("probe:a", spot->spot_id);
    node->actor_spot_ids.emplace ("probe:b", spot->spot_id);
    auto ref_a = detail::actor_ref_access_t::make (spot->node_rid, "probe", "a", 1);
    auto ref_b = detail::actor_ref_access_t::make (spot->node_rid, "probe", "b", 1);
    detail::actor_gateway_runtime_t gateway;
    gateway.on_membership ([&] (const auto &) { return std::optional (spot->spot_id); });
    std::vector<std::shared_ptr<detail::deferred_barrier_t>> reservations;
    gateway.on_join_barrier ([&] (const auto &actor) {
        auto reserved = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
        if (reserved)
            reservations.push_back (reserved.value ());
        return reserved;
    });
    fw::task_completion_source_t<detail::actor_join_reply_t> first, second;
    int started_a = 0, started_b = 0, suffix_a = 0, suffix_b = 0, reply = 0;
    gateway.on_join_spot ([&] (const fw::actor_ref_t &actor, auto, const auto &, auto, auto, auto) {
        if (actor.actor_id ().value () == "a") {
            ++started_a;
            return first.task ();
        }
        ++started_b;
        return second.task ();
    });
    auto context_a = gateway.actor_context (ref_a);
    auto context_b = gateway.actor_context (ref_b);
    rt::serial_execution_queue_t::async_completion_t handler;
    bool b_head_connected = false, b_suffix_queued = false;
    if (other_handler) {
        a->post ("already-finished-handler", [] {});
        scheduler->run_all ();
    }
    auto caller = other_handler ? root : a;
    caller->post_async ("public-members-handler", [&] (auto complete) {
        handler = std::move (complete);
        b->post ("public-member-b-suffix", [&] { ++suffix_b; });
        b_head_connected = root->pending_count (rt::serial_work_lane_t::application) >= 2;
        b_suffix_queued = b->pending_count () == 1;
        context_a.join_spot (fw::spot_id_t ("target-a")).defer ();
        context_b.join_spot (fw::spot_id_t ("target-b")).defer ();
    });
    a->post ("public-member-a-suffix", [&] { ++suffix_a; });
    scheduler->run_all ();
    bool passed = handler && b_suffix_queued
                  && (policy != public_member_policy_t::spot_wide || b_head_connected)
                  && started_a == 0 && started_b == 0 && suffix_a == 0 && suffix_b == 0;
    handler ([&] { ++reply; });
    scheduler->run_all ();
    std::shared_ptr<rt::serial_handler_participant_t> late;
    int captured = 0;
    std::optional<fw::task_t<void>> late_capture;
    if (other_handler) {
        auto observed = a->observe_handler_terminal ();
        if (!observed)
            throw std::runtime_error ("late participant reservation failed");
        late = observed.value ();
        late_capture.emplace (late->run ());
        detail::observe_task_terminal (*late_capture, [&] (const fw::result_t<void> &terminal) {
            if (terminal)
                ++captured;
        });
        scheduler->run_all ();
        passed = passed && captured == 1;
    }
    std::cout << "public-member-before-sources policy=" << static_cast<int> (policy)
              << " fail_first=" << fail_first << " started_a=" << started_a
              << " started_b=" << started_b << " suffix_a=" << suffix_a << " suffix_b=" << suffix_b
              << " reply=" << reply << '\n';
    const auto captured_before_first = captured;
    const auto started_b_before_first = started_b;
    // Each Actor owns its lifecycle FIFO. Waiting for A's transport does not
    // delay B's Join; each suffix stays behind its own actual terminal.
    passed =
      passed && started_a == 1 && started_b == 1 && suffix_a == 0 && suffix_b == 0 && reply == 1;
    first.complete (fail_first
                      ? fw::result_t<detail::actor_join_reply_t>::failure (
                          fw::framework_error_kind_t::internal_failure, "first Join failed")
                      : fw::result_t<detail::actor_join_reply_t>::success ({0, ref_a, {}}));
    scheduler->run_all ();
    passed = passed && started_b == 1 && suffix_a == (other_handler ? 0 : 1) && suffix_b == 0;
    second.complete (fw::result_t<detail::actor_join_reply_t>::success ({0, ref_b, {}}));
    scheduler->run_all ();
    if (late) {
        passed = passed && suffix_a == 0;
        late->cancel ();
        scheduler->run_all ();
    }

    passed = passed && suffix_a == 1 && suffix_b == 1 && reply == 1 && root->pending_count () == 0
             && a->pending_count () == 0 && b->pending_count () == 0
             && !node->actor_transfer_coordinator->phase ("probe:a")
             && !node->actor_transfer_coordinator->phase ("probe:b");
    for (const auto &reservation : reservations)
        reservation->cancel ();
    scheduler->run_all ();
    serial->close ();
    scheduler->run_all ();
    scheduler->drain ();
    node->spot_contexts_by_id.clear ();
    spot->node.reset ();
    std::cout << "public-local-members policy=" << static_cast<int> (policy)
              << " captured_before_first=" << captured_before_first
              << " other_handler=" << other_handler << " captured=" << captured
              << " fail_first=" << fail_first << " b_head_connected=" << b_head_connected
              << " b_started_before_first=" << started_b_before_first << " started_a=" << started_a
              << " started_b=" << started_b << " suffix_a=" << suffix_a << " suffix_b=" << suffix_b
              << " reply=" << reply << " passed=" << passed << '\n';
    return passed;
}
class completion_callback_owner_probe_t final : public zlink::framework::actor_t
{
  public:
    explicit completion_callback_owner_probe_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }
    zlink::framework::task_t<void>
    on_join_completed (const zlink::framework::actor_join_completion_t &) override
    {
        namespace fw = zlink::framework;
        ++entered;
        callback_turn = fw::detail::capture_current_serial_turn ();
        if (yield_callback) {
            fw::request_call_t<int> call (
              "CompletionOwnerProbe", [source = reply] (const auto &, auto, const auto &) {
                  return fw::test::accepted_request_task (source->task ());
              });
            const auto value = co_await call.yield ();
            if (value != 7)
                throw std::runtime_error ("completion Yield reply mismatch");
        } else {
            co_await ordinary.task ();
        }
        ++returned;
    }

    bool yield_callback = false;
    int entered = 0, returned = 0;
    std::shared_ptr<zlink::framework::detail::serial_turn_t> callback_turn;
    zlink::framework::task_completion_source_t<void> ordinary;
    std::shared_ptr<zlink::framework::task_completion_source_t<int>> reply =
      std::make_shared<zlink::framework::task_completion_source_t<int>> ();

  private:
    zlink::framework::actor_context_t _context;
};

// policy: 0 = SpotWide, 1 = PerActor, 2 = Entry. Yield is tested only where allowed.
bool verify_real_join_completion_callback_owner (int policy,
                                                 bool yield_callback,
                                                 bool close_before_callback)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace detail = fw::detail;
    auto node = fw::test::runtime_failure_fixture<detail::spot_node_builder_state_t> (
      "join-completion-owner-node");
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    scheduler->publication_fence (fw::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("join-completion-owner-node");
    spot->spot_id = fw::spot_id_t ("join-completion-owner-spot");
    spot->spot_instance = std::make_shared<int> (1);
    if (policy == 1)
        spot->execution_mode = fw::user_spot_execution_mode_t::per_actor;
    if (policy == 2)
        spot->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    auto serial = spot->ensure_spot_serial_executor ();
    auto root = serial->spot_queue ();
    spot->serial_queue = root;
    const std::string key = "probe:completion-owner";
    auto actor_queue = serial->actor_executor (key)->queue ();
    auto other_queue = serial->actor_executor ("probe:other-owner")->queue ();
    const auto actor_ref =
      detail::actor_ref_access_t::make (spot->node_rid, "probe", "completion-owner", 1);
    detail::actor_gateway_runtime_t gateway;
    auto actor =
      std::make_shared<completion_callback_owner_probe_t> (gateway.actor_context (actor_ref));
    actor->yield_callback = yield_callback;
    node->actor_instances.emplace (key, actor);
    node->actor_generations.emplace (key, actor_ref.object_generation ());
    node->actor_spot_ids.emplace (key, spot->spot_id);
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (completion_callback_owner_probe_t));
    factory.on_join_completed = [] (void *instance, detail::actor_join_completion_outcome_t outcome,
                                    std::uint64_t high, std::uint64_t low,
                                    const fw::actor_ref_t *committed,
                                    const std::optional<fw::message_t> &reply,
                                    fw::framework_error_kind_t error, bool) {
        return static_cast<completion_callback_owner_probe_t *> (instance)->on_join_completed (
          detail::actor_join_completion_from_erased (outcome, high, low, committed, reply, error));
    };
    node->actor_factories.emplace ("probe", std::move (factory));
    auto reserved = actor_queue->reserve_deferred_barrier_next ("real-completion-source-barrier");
    bool passed = static_cast<bool> (reserved);
    int other_payload = 0, spot_payload = 0, same_actor_suffix = 0;
    if (reserved) {
        auto terminal = reserved.value ()->terminal ();
        auto activated = reserved.value ()->activate_async ([&] (auto complete) {
            if (close_before_callback)
                root->cancel_pending ();
            auto runtime = std::make_shared<detail::spot_node_runtime_t> (node);
            auto pending = std::make_shared<fw::task_t<fw::result_t<void>>> (
              runtime->deliver_actor_join_completion (
                actor_ref, fw::actor_join_accepted_t{11, 12, actor_ref, std::nullopt},
                spot->spot_id));
            detail::observe_task_completion (
              *pending, [runtime, pending, complete = std::move (complete)] (
                          const fw::result_t<fw::result_t<void>> &result) mutable {
                  complete (result ? result.value ()
                                   : detail::propagate_failure<void> (
                                       result, "completion callback delivery failed"));
              });
        });
        passed = passed && static_cast<bool> (activated);
        if (!close_before_callback)
            actor_queue->post ("same-actor-after-completion", [&] { ++same_actor_suffix; });
        scheduler->run_all ();
        passed = passed && actor->entered == 1 && actor->returned == 0 && !terminal.await_ready ();
        passed =
          passed && actor->callback_turn
          && actor->callback_turn->belongs_to (policy == 0 ? root.get () : actor_queue.get ());
        if (close_before_callback) {
            passed = passed && !root->try_post ("fresh-after-close", [&] { ++spot_payload; });
        } else {
            other_queue->post ("other-actor-during-completion", [&] { ++other_payload; });
            root->post ("spot-during-completion", [&] { ++spot_payload; });
        }
        scheduler->run_all ();
        const int expected_progress =
          !close_before_callback && (policy != 0 || yield_callback) ? 1 : 0;
        passed = passed && other_payload == expected_progress && spot_payload == expected_progress
                 && same_actor_suffix == 0;
        actor->ordinary.complete (fw::result_t<void>::success ());
        actor->reply->complete (fw::result_t<int>::success (7));
        scheduler->run_until (terminal);
        scheduler->run_all ();
        passed = passed && terminal.await_ready ();
        if (terminal.await_ready ())
            passed = passed && static_cast<bool> (terminal.result ());
        const int expected_payload = close_before_callback ? 0 : 1;
        passed = passed && actor->entered == 1 && actor->returned == 1
                 && other_payload == expected_payload && spot_payload == expected_payload
                 && same_actor_suffix == expected_payload;
    }
    const auto root_pending = root->pending_count ();
    const auto actor_pending = actor_queue->pending_count ();
    const auto other_pending = other_queue->pending_count ();
    passed = passed && root_pending == 0 && actor_pending == 0 && other_pending == 0;
    actor->callback_turn.reset ();
    serial->close ();
    scheduler->run_all ();
    actor_queue->drain ();
    other_queue->drain ();
    root->drain ();
    node->actor_factories.clear ();
    node->actor_instances.clear ();
    node->spot_contexts_by_id.clear ();
    spot->node.reset ();
    scheduler->drain ();
    std::cout << "real-join-completion-owner policy=" << policy << " yield=" << yield_callback
              << " close_before_callback=" << close_before_callback << " entered=" << actor->entered
              << " returned=" << actor->returned << " other_payload=" << other_payload
              << " spot_payload=" << spot_payload << " same_actor_suffix=" << same_actor_suffix
              << " root_pending=" << root_pending << " actor_pending=" << actor_pending
              << " other_pending=" << other_pending << " passed=" << passed << '\n';
    return passed;
}
int verify_actor_join_completion_observations ()
{
    auto completion_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("completion-node");
    // Shared with the observer, which may outlive this scope on a worker thread.
    struct completion_failure_log_t
    {
        std::mutex mutex;
        std::vector<zlink::framework::message_flow_event_t> events;
    };
    auto completion_failure_log = std::make_shared<completion_failure_log_t> ();
    // Reads copy the events under the mutex.
    const auto completion_failure_events = [&completion_failure_log] {
        std::lock_guard lock (completion_failure_log->mutex);
        return completion_failure_log->events;
    };
    auto completion_logs = std::make_shared<
      zlink::framework::test::telemetry_observations_t<zlink::framework::log_record_t>> ();
    zlink::framework::logging_builder_t completion_logging;
    completion_logging.use_provider ("completion-cause", [completion_logs] (const auto &record) {
        completion_logs->record (record);
    });
    zlink::framework::detail::dispatch_options_access_t::set_logger (
      completion_state->dispatch, completion_logging.create_logger ("completion-cause"));
    completion_state->dispatch.message_flow (zlink::framework::message_flow_log_mode_t::errors);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      completion_state->dispatch,
      [completion_failure_log] (const zlink::framework::message_flow_event_t &event) {
          if (event.packet_name == "JoinSpot" && event.error_reason.has_value ()) {
              std::lock_guard lock (completion_failure_log->mutex);
              completion_failure_log->events.push_back (event);
          }
      });
    zlink::framework::detail::spot_node_runtime_t completion_runtime (completion_state);
    const zlink::framework::actor_ref_t completion_actor =
      zlink::framework::detail::actor_ref_access_t::make (
        zlink::framework::node_rid_t::from_string ("completion-node"), "player", "completion-actor",
        7);
    const std::string completion_key = "player:completion-actor";
    auto completion_instance = std::make_shared<int> (42);
    completion_state->actor_instances.emplace (completion_key, completion_instance);
    completion_state->actor_generations.emplace (completion_key,
                                                 completion_actor.object_generation ());
    completion_state->actor_spot_ids.emplace (completion_key,
                                              zlink::framework::spot_id_t ("source-spot"));

    int completion_callback_count = 0;
    auto suspended_completion =
      std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    std::promise<void> suspended_completion_entered;
    bool fail_completion_once = false;
    auto completion_error_kind = zlink::framework::framework_error_kind_t::internal_failure;
    bool completion_retryable = true;
    std::vector<zlink::framework::detail::actor_join_completion_outcome_t> completion_outcomes;
    zlink::framework::detail::spot_node_builder_state_t::actor_factory_registration_t
      completion_factory;
    completion_factory.actor_type = std::type_index (typeid (int));
    completion_factory.on_join_completed =
      [&] (void *actor, zlink::framework::detail::actor_join_completion_outcome_t outcome,
           std::uint64_t operation_high, std::uint64_t, const zlink::framework::actor_ref_t *,
           const std::optional<zlink::framework::message_t> &,
           zlink::framework::framework_error_kind_t error_kind, bool retryable) {
          if (actor != completion_instance.get ()) {
              return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::not_configured,
                "completion callback received another Actor"));
          }
          ++completion_callback_count;
          completion_outcomes.push_back (outcome);
          completion_error_kind = error_kind;
          completion_retryable = retryable;
          if (operation_high == 97) {
              suspended_completion_entered.set_value ();
              return suspended_completion->task ();
          }
          if (fail_completion_once) {
              fail_completion_once = false;
              return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
                zlink::framework::framework_error_kind_t::internal_failure,
                "completion callback failed"));
          }
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
      };
    completion_state->actor_factories.emplace ("player", std::move (completion_factory));

    const zlink::framework::actor_join_completion_t suspended_result =
      zlink::framework::actor_join_rejected_t{97, 101, std::nullopt};
    auto entered = suspended_completion_entered.get_future ();
    auto delivery = std::async (std::launch::async, [&] {
        return completion_runtime.deliver_actor_join_completion (
          completion_actor, suspended_result, zlink::framework::spot_id_t ("source-spot"));
    });
    entered.wait ();
    const bool returned_before_callback =
      delivery.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    suspended_completion->complete (zlink::framework::result_t<void>::success ());
    if (!returned_before_callback || !delivery.get ().result ().value ()) {
        return 75;
    }
    completion_callback_count = 0;
    completion_outcomes.clear ();

    const auto first_operation = completion_runtime.actor_join_operation_id ("transfer-1");
    const auto repeated_operation = completion_runtime.actor_join_operation_id ("transfer-1");
    const auto second_operation = completion_runtime.actor_join_operation_id ("transfer-2");
    if ((first_operation.first == 0 && first_operation.second == 0)
        || first_operation != repeated_operation || first_operation == second_operation) {
        return 70;
    }

    const zlink::framework::actor_join_completion_t rejected_completion =
      zlink::framework::actor_join_rejected_t{
        first_operation.first, first_operation.second,
        zlink::framework::message_t::from (std::string ("rejected"))};
    if (!completion_runtime
           .deliver_actor_join_completion (completion_actor, rejected_completion,
                                           zlink::framework::spot_id_t ("source-spot"))
           .result ()
           .value ()
        || !completion_runtime
              .deliver_actor_join_completion (completion_actor, rejected_completion,
                                              zlink::framework::spot_id_t ("source-spot"))
              .result ()
              .value ()
        || completion_callback_count != 1
        || completion_outcomes
             != std::vector{zlink::framework::detail::actor_join_completion_outcome_t::rejected}) {
        return 71;
    }

    fail_completion_once = true;
    const zlink::framework::actor_join_completion_t failed_completion =
      zlink::framework::actor_join_failed_t{
        second_operation.first, second_operation.second,
        zlink::framework::framework_error_kind_t::internal_failure};
    const auto first_failed_delivery =
      completion_runtime
        .deliver_actor_join_completion (completion_actor, failed_completion,
                                        zlink::framework::spot_id_t ("source-spot"))
        .result ()
        .value ();
    const auto repeated_failed_delivery =
      completion_runtime
        .deliver_actor_join_completion (completion_actor, failed_completion,
                                        zlink::framework::spot_id_t ("source-spot"))
        .result ()
        .value ();
    zlink::framework::test::acknowledge_delivery ();
    if (first_failed_delivery || !repeated_failed_delivery || completion_callback_count != 3
        || completion_outcomes.back ()
             != zlink::framework::detail::actor_join_completion_outcome_t::failed
        || completion_error_kind != zlink::framework::framework_error_kind_t::internal_failure
        || completion_retryable || completion_failure_events ().size () != 2
        || completion_failure_events ().back ().actor_id != "completion-actor"
        || completion_failure_events ().back ().error_reason
             != zlink::framework::dispatch_error_reason_t::handler_exception
        || !completion_failure_events ().back ().exception) {
        return 72;
    }

    try {
        std::rethrow_exception (completion_failure_events ().back ().exception);
        return 73;
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () != zlink::framework::framework_error_kind_t::internal_failure)
            return 73;
    }

    const auto completion_records = completion_logs->snapshot ();
    if (std::none_of (
          completion_records.begin (), completion_records.end (), [] (const auto &record) {
              const auto has = [&] (std::string_view key, std::string_view value) {
                  return std::any_of (
                    record.fields.begin (), record.fields.end (),
                    [&] (const auto &field) { return field.key == key && field.value == value; });
              };
              return has (zlink::framework::detail::dispatch_event_field::packet, "JoinSpot")
                     && has (zlink::framework::detail::dispatch_event_field::actor,
                             "completion-actor")
                     && has (zlink::framework::detail::dispatch_event_field::error_type,
                             typeid (zlink::framework::framework_exception_t).name ())
                     && has (zlink::framework::detail::dispatch_event_field::error_message,
                             "deferred Actor Join failed");
          }))
        return 73;

    const auto successor_operation =
      completion_runtime.actor_join_operation_id ("transfer-successor");
    completion_state->actor_generations[completion_key] = completion_actor.object_generation () + 1;
    const zlink::framework::actor_join_completion_t successor_completion =
      zlink::framework::actor_join_rejected_t{
        successor_operation.first, successor_operation.second,
        zlink::framework::message_t::from (std::string ("successor"))};
    if (!completion_runtime
           .deliver_actor_join_completion (completion_actor, successor_completion,
                                           zlink::framework::spot_id_t ("source-spot"))
           .result ()
           .value ()
        || completion_callback_count != 4
        || completion_outcomes.back ()
             != zlink::framework::detail::actor_join_completion_outcome_t::rejected) {
        return 74;
    }

    return 0;
}

bool verify_bulk_deferred_actor_joins ()
{
    namespace fw = zlink::framework;
    fw::runtime::offload_executor_t deferred_executor (1);
    fw::runtime::serial_execution_queue_t deferred_queue (deferred_executor);
    fw::runtime::offload_executor_t target_executor (1);
    fw::runtime::serial_execution_queue_t target_queue (target_executor);
    fw::detail::actor_gateway_runtime_t actor_gateway;
    fw::serializer_registry_t actor_serializers;
    actor_gateway.bind_serializers (actor_serializers);
    const auto actor = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("barrier-node"), "player", "barrier-actor", 1);
    auto actor_context = actor_gateway.actor_context (actor);
    constexpr std::size_t bulk_join_count = 65;
    const std::string bulk_request (129u * 1024u, 'x');
    std::size_t completed_bulk_joins = 0;
    std::size_t completed_bulk_request_bytes = 0;
    std::set<std::string> completed_bulk_targets;
    std::vector<std::shared_ptr<zlink::framework::detail::deferred_barrier_t>> bulk_barriers;
    actor_gateway.on_join_barrier ([&] (const auto &) {
        auto reserved = target_queue.reserve_barrier_next ("bulk-join-barrier", true);
        if (reserved)
            bulk_barriers.push_back (reserved.value ());
        return reserved;
    });
    actor_gateway.on_join_spot (
      [&] (const auto &actor, const auto &spot_id, const auto &request, auto, auto,
           auto) -> zlink::framework::task_t<zlink::framework::detail::actor_join_reply_t> {
          ++completed_bulk_joins;
          completed_bulk_request_bytes += request.to_string ().size ();
          completed_bulk_targets.emplace (spot_id);
          co_return zlink::framework::result_t<
            zlink::framework::detail::actor_join_reply_t>::success ({1, actor, zlink::message_t{}});
      });
    // Defer starts each call's deadline. Prepare the distinct typed payloads
    // before the handler so later serialization cannot consume earlier calls' budgets.
    std::vector<fw::actor_join_call_t> calls;
    calls.reserve (bulk_join_count);
    for (std::size_t index = 0; index < bulk_join_count; ++index)
        calls.push_back (actor_context.join_spot ("bulk-target-" + std::to_string (index),
                                                  fw::message_t::from (bulk_request)));
    deferred_queue.run ("bulk-deferred-actor-joins", [&] {
        for (auto &call : calls)
            call.defer ();
    });
    bool passed = bulk_barriers.size () == bulk_join_count;
    for (const auto &barrier : bulk_barriers) {
        const auto terminal = barrier->terminal ().result ();
        if (!terminal)
            std::cerr << "bulk deferred join terminal failed: "
                      << (terminal.error () ? terminal.error ()->what () : "no error") << '\n';
        passed = static_cast<bool> (terminal) && passed;
    }
    target_queue.drain ();
    if (!passed || completed_bulk_joins != bulk_join_count
        || completed_bulk_targets.size () != bulk_join_count
        || completed_bulk_request_bytes <= 8u * 1024u * 1024u) {
        std::cerr << "bulk deferred joins completed=" << completed_bulk_joins
                  << " targets=" << completed_bulk_targets.size ()
                  << " bytes=" << completed_bulk_request_bytes << '\n';
        return false;
    }

    return true;
}
}
