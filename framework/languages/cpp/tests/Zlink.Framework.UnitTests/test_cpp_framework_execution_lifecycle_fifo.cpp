/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/execution_cases.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{
bool verify_spot_close_waits_for_earlier_lifecycle_join ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("close-after-join");
    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    auto state = std::make_shared<spot_context_state_t> ();
    state->node = node;
    state->spot_id = "close-after-join-spot";
    state->spot_name = "close-after-join-player";
    state->spot_instance = std::make_shared<int> (1);
    state->serial_executor = executor;
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    node->spot_contexts_by_id.emplace (state->spot_id, spot_context_access_t::create (state));
    node->spot_names_by_id.emplace (state->spot_id, state->spot_name);
    node->spot_ids_by_name.emplace (state->spot_name, state->spot_id);
    int closing_calls = 0;
    state->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &, std::stop_token) {
        ++closing_calls;
        return task_t<void> (result_t<void>::success ());
    };

    auto barrier = state->serial_queue->reserve_barrier_next ("older-lifecycle-turn");
    if (!barrier)
        return false;
    bool join_completed = false;
    if (!state->serial_queue->try_post (
          "accepted-join",
          [&] {
              state->actor_count = 1;
              join_completed = true;
          },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}))
        return false;
    auto close = std::async (std::launch::async, [node, state] {
        return spot_node_runtime_t (node).close_spot (spot_id_t (state->spot_id)).result ();
    });
    const bool completed_before_join =
      close.wait_for (std::chrono::milliseconds (50)) == std::future_status::ready;
    (void) barrier.value ()->activate ([] {});
    const auto result = close.get ();
    state->drain_serial ();
    const auto passed = !completed_before_join && join_completed && result && !result.value ()
                        && closing_calls == 0 && !state->closed;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

bool verify_suspended_lifecycle_keeps_fifo_and_allows_application ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_t queue (executor, serial_execution_queue_options_t{});
    std::mutex gate;
    std::condition_variable changed;
    bool started = false;
    bool application_ran = false;
    std::atomic_bool later_lifecycle_ran{false};
    serial_execution_queue_t::async_completion_t finish;
    queue.post_async (
      "suspended-lifecycle",
      [&] (auto complete) {
          std::lock_guard lock (gate);
          finish = std::move (complete);
          started = true;
          changed.notify_all ();
      },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    // Completes the suspended turn and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return started; });
        }
        finish ([] {});
        queue.drain ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return started; }))
            return false;
    }
    queue.post (
      "later-lifecycle", [&] { later_lifecycle_ran.store (true); },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    queue.post ("eligible-application", [&] {
        std::lock_guard lock (gate);
        application_ran = true;
        changed.notify_all ();
    });
    bool application_progressed = false;
    {
        std::unique_lock lock (gate);
        application_progressed =
          changed.wait_for (lock, std::chrono::seconds (1), [&] { return application_ran; });
    }
    const bool lifecycle_waited = !later_lifecycle_ran.load ();
    finish ([] {});
    queue.drain ();
    return application_progressed && lifecycle_waited && later_lifecycle_ran.load ();
}

bool verify_suspended_lifecycle_resumes_at_original_fifo_head ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_t queue (executor, serial_execution_queue_options_t{});
    std::mutex gate;
    std::condition_variable changed;
    bool started = false;
    std::vector<int> order;
    detail::task_scheduler_t resume;
    serial_execution_queue_t::async_completion_t finish;
    queue.post_async (
      "yielding-lifecycle",
      [&] (auto complete) {
          std::lock_guard lock (gate);
          resume = detail::capture_current_serial_turn ()->resume_scheduler ();
          finish = std::move (complete);
          started = true;
          changed.notify_all ();
      },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    // Completes the suspended turn and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return started; });
        }
        finish ([] {});
        queue.drain ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return started; }))
            return false;
    }
    queue.post (
      "later-lifecycle", [&] { order.push_back (3); },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    resume ([&] {
        order.push_back (1);
        resume ([&] {
            order.push_back (2);
            finish ([] {});
        });
    });
    queue.drain ();
    return order == std::vector<int>{1, 2, 3};
}

bool verify_ready_lifecycle_continuation_pays_burst_debt ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_options_t options;
    options.lifecycle_burst_limit = 1;
    serial_execution_queue_t queue (executor, options);
    std::mutex gate;
    std::condition_variable changed;
    bool blocker_started = false;
    bool release_blocker = false;
    std::vector<int> order;
    queue.post ("application-blocker", [&] {
        std::unique_lock lock (gate);
        blocker_started = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release_blocker; });
    });
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::lock_guard lock (gate);
            release_blocker = true;
        }
        changed.notify_all ();
        queue.drain ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return blocker_started; }))
            return false;
    }
    queue.post_async (
      "yielding-lifecycle",
      [&] (auto complete) {
          auto resume = detail::capture_current_serial_turn ()->resume_scheduler ();
          resume ([&, complete = std::move (complete)] () mutable {
              order.push_back (2);
              complete ([] {});
          });
      },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    queue.post ("eligible-application", [&] { order.push_back (1); });
    queue.post (
      "later-lifecycle", [&] { order.push_back (3); },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    {
        std::lock_guard lock (gate);
        release_blocker = true;
    }
    changed.notify_all ();
    queue.drain ();
    return order == std::vector<int>{1, 2, 3};
}

bool verify_suspended_lifecycle_cancellation_releases_fifo ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_t queue (executor, serial_execution_queue_options_t{});
    std::mutex gate;
    std::condition_variable changed;
    bool started = false;
    bool application_ran = false;
    std::atomic_int cancelled{0};
    std::atomic_int later_runs{0};
    serial_execution_queue_t::async_completion_t finish;
    const auto submission = queue.try_post_cancellable_async (
      "cancellable-lifecycle",
      [&] (auto complete) {
          std::lock_guard lock (gate);
          finish = std::move (complete);
          started = true;
          changed.notify_all ();
      },
      [&] {
          ++cancelled;
          finish ([] {});
      },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    if (!submission)
        return false;
    // Completes the suspended turn and drains before the locals above are destroyed.
    const auto drain_suspended = [&] {
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return started; });
        }
        finish ([] {});
        queue.drain ();
    };
    std::optional<scope_exit_t> drain_on_exit (std::in_place, drain_suspended);
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return started; }))
            return false;
    }
    queue.post (
      "later-lifecycle", [&] { ++later_runs; },
      serial_work_options_t{serial_work_lane_t::lifecycle});
    queue.post ("application-progress", [&] {
        std::lock_guard lock (gate);
        application_ran = true;
        changed.notify_all ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return application_ran; })) {
            finish ([] {});
            return false;
        }
    }
    const auto outcome = queue.cancel_submission (submission.value ()).result ().value ();
    queue.drain ();
    if (outcome != serial_cancel_submission_outcome_t::active_cancel_requested
        || cancelled.load () != 1 || later_runs.load () != 1 || queue.pending_count () != 0)
        return false;

    drain_on_exit.reset ();
    started = false;
    application_ran = false;
    const auto normal = queue.try_post_cancellable_async (
      "noncancelled-lifecycle",
      [&] (auto complete) {
          std::lock_guard lock (gate);
          finish = std::move (complete);
          started = true;
          changed.notify_all ();
      },
      [&] { ++cancelled; }, serial_work_options_t{serial_work_lane_t::lifecycle});
    if (!normal)
        return false;
    drain_on_exit.emplace (drain_suspended);
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return started; }))
            return false;
    }
    queue.post ("noncancelled-application", [&] {
        std::lock_guard lock (gate);
        application_ran = true;
        changed.notify_all ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return application_ran; })) {
            finish ([] {});
            return false;
        }
    }
    finish ([] {});
    queue.drain ();
    return cancelled.load () == 1 && queue.pending_count () == 0;
}

bool verify_delayed_barrier_activation_uses_retained_turn ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    // Declared before the queue so queued work never outlives them.
    std::mutex gate;
    std::condition_variable changed;
    bool application_ran = false;
    bool ran_on_retained_turn = false;
    offload_executor_t executor (2);
    serial_execution_queue_t queue (executor, serial_execution_queue_options_t{});
    auto reserved = queue.reserve_barrier_next ("delayed-lifecycle-activation");
    if (!reserved)
        return false;
    queue.post ("application-while-barrier-reached", [&] {
        std::lock_guard lock (gate);
        application_ran = true;
        changed.notify_all ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return application_ran; })) {
            reserved.value ()->cancel ();
            return false;
        }
    }
    const auto activated = reserved.value ()->activate_async ([&] (auto release) {
        const auto turn = detail::capture_current_serial_turn ();
        ran_on_retained_turn = turn && turn->belongs_to (&queue);
        release (result_t<void>::success ());
    });
    if (!activated) {
        reserved.value ()->cancel ();
        return false;
    }
    queue.drain ();
    return ran_on_retained_turn;
}
bool verify_cancellable_serial_submission_lifecycle ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    // If the executor rejects the drain job, enqueue rollback removes exactly
    // one item and restores its accounting before the next admission.
    {
        offload_executor_t executor (1);
        executor.drain ();
        serial_execution_queue_options_t options;
        serial_execution_queue_t queue (executor, options);
        const serial_work_options_t work_options{
          serial_work_lane_t::application, serial_execution_queue_t::fixed_work_byte_cost + 37};
        std::atomic_int work_runs = 0;
        std::atomic_int cancel_calls = 0;

        const bool first_accepted =
          queue.try_post ("rejected-drain-job", [&] { ++work_runs; }, work_options);
        const bool first_rollback_clean =
          queue.pending_count (serial_work_lane_t::application) == 0 && queue.pending_bytes () == 0;
        const auto second = queue.try_post_cancellable_async (
          "rejected-drain-job-after-rollback",
          [&] (auto complete) {
              ++work_runs;
              complete ([] {});
          },
          [&] { ++cancel_calls; }, work_options);
        const bool second_rollback_clean =
          queue.pending_count (serial_work_lane_t::application) == 0 && queue.pending_bytes () == 0;
        queue.drain ();
        if (first_accepted || !first_rollback_clean || second
            || second.error_kind () != framework_error_kind_t::shutting_down
            || !second_rollback_clean || work_runs.load () != 0 || cancel_calls.load () != 0) {
            return false;
        }
    }

    // A queued cancellation removes its accounting before the stop callback
    // runs, and its replacement is accepted immediately.
    {
        // Declared before the executor so the blocking job never outlives them.
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        offload_executor_t executor (1);
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (lock, std::chrono::seconds (1),
                                          [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_options_t options;
        serial_execution_queue_t queue (executor, options);
        std::atomic_int cancelled = 0;
        std::atomic_int cancelled_work_runs = 0;
        std::atomic_int replacement_runs = 0;
        const auto submission = queue.try_post_cancellable_async (
          "queued-cancellable",
          [&] (auto complete) {
              ++cancelled_work_runs;
              complete ([] {});
          },
          [&] { ++cancelled; });
        const bool accepted = submission.has_value ();
        const auto outcome = accepted
                               ? queue.cancel_submission (submission.value ()).result ().value ()
                               : serial_cancel_submission_outcome_t::already_terminal;
        const bool replacement_accepted =
          queue.try_post ("replacement-after-unlink", [&] { ++replacement_runs; });

        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!accepted || outcome != serial_cancel_submission_outcome_t::queued_cancelled
            || !replacement_accepted || cancelled.load () != 1 || cancelled_work_runs.load () != 0
            || replacement_runs.load () != 1 || queue.pending_count () != 0) {
            return false;
        }
    }

    // An active cancellation requests cooperative stop. The active turn and
    // its accounting remains until work acknowledges completion.
    {
        offload_executor_t executor (1);
        serial_execution_queue_options_t options;
        serial_execution_queue_t queue (executor, options);
        std::mutex gate;
        std::condition_variable changed;
        std::optional<serial_execution_queue_t::async_completion_t> acknowledge;
        bool entered = false;
        bool follower_ran = false;
        std::atomic_int stop_requests = 0;
        // Acknowledges an active turn left by a failed check, then drains before the locals above
        // are destroyed.
        scope_exit_t drain_on_exit ([&] {
            if (queue.pending_count () != 0) {
                serial_execution_queue_t::async_completion_t finish;
                {
                    std::unique_lock lock (gate);
                    changed.wait (lock, [&] { return entered; });
                    finish = std::move (*acknowledge);
                }
                if (finish)
                    finish ([] {});
            }
            queue.drain ();
        });
        const auto submission = queue.try_post_cancellable_async (
          "active-cancellable",
          [&] (auto complete) {
              std::lock_guard lock (gate);
              entered = true;
              acknowledge.emplace (std::move (complete));
              changed.notify_all ();
          },
          [&] {
              ++stop_requests;
              changed.notify_all ();
          });
        if (!submission) {
            return false;
        }
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                queue.cancel_pending ();
                return false;
            }
        }
        if (!queue.try_post ("follower-after-cancel", [&] {
                std::lock_guard lock (gate);
                follower_ran = true;
                changed.notify_all ();
            })) {
            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*acknowledge);
            }
            finish ([] {});
            queue.drain ();
            return false;
        }

        const auto first_cancel = queue.cancel_submission (submission.value ()).result ().value ();
        const auto repeated_cancel =
          queue.cancel_submission (submission.value ()).result ().value ();
        bool follower_ran_before_ack = false;
        {
            std::unique_lock lock (gate);
            follower_ran_before_ack =
              changed.wait_for (lock, std::chrono::milliseconds (50), [&] { return follower_ran; });
        }
        serial_execution_queue_t::async_completion_t finish;
        {
            std::lock_guard lock (gate);
            finish = std::move (*acknowledge);
        }
        finish ([] {});
        queue.drain ();
        if (first_cancel != serial_cancel_submission_outcome_t::active_cancel_requested
            || repeated_cancel != serial_cancel_submission_outcome_t::active_cancel_requested
            || follower_ran_before_ack || !follower_ran || stop_requests.load () != 1
            || queue.cancel_submission (submission.value ()).result ().value ()
                 != serial_cancel_submission_outcome_t::already_terminal) {
            return false;
        }
    }

    // Shutdown removes queued followers but does not consume a generic async
    // turn's completion. The original work remains the only owner that can
    // acknowledge and release its active reservation.
    {
        offload_executor_t executor (1);
        serial_execution_queue_t queue (executor);
        std::mutex gate;
        std::condition_variable changed;
        std::optional<serial_execution_queue_t::async_completion_t> acknowledge;
        bool entered = false;
        std::atomic_int follower_runs = 0;
        std::atomic_int acknowledged_completions = 0;
        if (!queue.try_post_async ("generic-active-during-cancel-pending", [&] (auto complete) {
                std::lock_guard lock (gate);
                acknowledge.emplace (std::move (complete));
                entered = true;
                changed.notify_all ();
            })) {
            return false;
        }
        // Acknowledges the active turn and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            serial_execution_queue_t::async_completion_t finish;
            {
                std::unique_lock lock (gate);
                changed.wait (lock, [&] { return entered; });
                if (acknowledge)
                    finish = std::move (*acknowledge);
            }
            if (finish)
                finish ([] {});
            queue.drain ();
        });
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                return false;
            }
        }
        if (!queue.try_post ("generic-follower-during-cancel-pending", [&] { ++follower_runs; })) {
            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*acknowledge);
            }
            finish ([] {});
            queue.drain ();
            return false;
        }

        queue.cancel_pending ();
        const bool active_reservation_retained = queue.pending_count () == 1;
        serial_execution_queue_t::async_completion_t finish;
        {
            std::lock_guard lock (gate);
            finish = std::move (*acknowledge);
        }
        finish ([&] { ++acknowledged_completions; });
        queue.drain ();
        if (!active_reservation_retained || !queue.closed () || follower_runs.load () != 0
            || acknowledged_completions.load () != 1 || queue.pending_count () != 0) {
            return false;
        }
    }

    // Queue shutdown cancels queued cancellable submissions and makes their
    // identifiers terminal without running their work.
    {
        // Declared before the executor so the blocking job never outlives them.
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        offload_executor_t executor (1);
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (lock, std::chrono::seconds (1),
                                          [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_t queue (executor);
        std::atomic_int cancelled = 0;
        std::atomic_int work_runs = 0;
        const auto submission = queue.try_post_cancellable_async (
          "cancelled-by-shutdown",
          [&] (auto complete) {
              ++work_runs;
              complete ([] {});
          },
          [&] { ++cancelled; });
        if (!submission) {
            {
                std::lock_guard lock (worker_gate);
                release_worker = true;
            }
            worker_changed.notify_all ();
            return false;
        }
        queue.cancel_pending ();
        const auto terminal = queue.cancel_submission (submission.value ()).result ().value ();
        const bool rejected_after_close = !queue.try_post ("closed", [] {});
        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!queue.closed () || !rejected_after_close
            || terminal != serial_cancel_submission_outcome_t::already_terminal
            || cancelled.load () != 1 || work_runs.load () != 0) {
            return false;
        }
    }

    // Queued deferred barriers receive cancellation before shutdown removes
    // their work, so activation becomes terminal and drain still converges.
    {
        // Declared before the executor so the blocking job never outlives them.
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        offload_executor_t executor (1);
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (lock, std::chrono::seconds (1),
                                          [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_t queue (executor);
        auto join_barrier = queue.reserve_barrier_next ("queued-join-barrier-cancellation");
        auto handoff_barrier =
          queue.reserve_barrier_next ("queued-handoff-barrier-cancellation", true);
        queue.cancel_pending ();
        const auto join_activation =
          join_barrier ? join_barrier.value ()->activate ([] {}) : result_t<void>::success ();
        const auto handoff_activation =
          handoff_barrier ? handoff_barrier.value ()->activate ([] {}) : result_t<void>::success ();
        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!join_barrier || !handoff_barrier || join_activation || handoff_activation
            || join_activation.error_kind () != framework_error_kind_t::invalid_operation
            || handoff_activation.error_kind () != framework_error_kind_t::invalid_operation
            || queue.pending_count () != 0) {
            return false;
        }
    }

    // A reservation between projection and queue preparation must retain the
    // packet. Once the move closes, the still-active barrier orders new work.
    {
        offload_executor_t executor (1);
        serial_execution_queue_t queue (executor);
        zlink::framework::detail::actor_transfer_coordinator_t transfers;
        using namespace zlink::framework::detail;
        const std::string key = "player:atomic-admission";
        handoff_packet_t packet;
        packet.packet_name = "AtomicProbe";
        packet.is_request = true;
        packet.metadata.emplace ("__zlink.actorRequestId", "atomic-request");
        auto initial = transfers.admit_dispatch_packet (key, nullptr, true, packet);
        if (initial.append_result != handoff_append_result_t::not_moving || !initial.released_packet
            || initial.queue_submission)
            return false;
        if (!transfers.try_reserve_source (key, "atomic-transfer"))
            return false;
        auto barrier = queue.reserve_barrier_next ("atomic-admission", true);
        if (!barrier)
            return false;
        // Ingress enters the hold only after the source is sealed; reservation
        // alone still admits ordinary work behind the inactive barrier.
        if (!transfers.try_begin_source_remote (key, "atomic-transfer"))
            return false;
        std::atomic_int calls{0};
        std::atomic_int runs{0};
        auto submit = [&] {
            ++calls;
            return queue.try_post ("ordinary-ingress", [&] { ++runs; });
        };
        const auto parked = transfers.admit_dispatch_packet (
          key, nullptr, true, std::move (*initial.released_packet), submit);
        const auto duplicate = transfers.admit_dispatch_packet (key, nullptr, true, packet, submit);
        auto backlog = transfers.cancel_move (key);
        const auto admitted = transfers.admit_dispatch_packet (key, nullptr, true, packet, submit);
        // One worker executes this sentinel after the barrier's queue job.
        // The active barrier still prevents its ordinary follower from running.
        std::promise<void> worker_reached_barrier;
        auto reached = worker_reached_barrier.get_future ();
        executor.submit ([&] { worker_reached_barrier.set_value (); });
        reached.get ();
        const bool ordered = calls == 1 && runs == 0;
        barrier.value ()->cancel ();
        queue.drain ();
        queue.close ();
        const auto closed = transfers.admit_dispatch_packet (key, nullptr, true, packet, submit);
        const runtime::protocol::actor_route_fence_t source_fence{
          "atomic-admission", 1, zlink::routing_id_t::from ("source").to_bytes (), 1, 1, 1};
        auto target_fence = source_fence;
        target_fence.target_node_routing_id = zlink::routing_id_t::from ("target").to_bytes ();
        target_fence.authority_owner_generation = 2;
        const auto target_actor = actor_ref_access_t::make (node_rid_t::from_string ("target"),
                                                            "player", "atomic-admission", 1);
        transfers.activate_message_follow (
          key, source_fence, target_actor,
          spot_route_t{node_rid_t::from_string ("target"), spot_id_t ("target-spot"), "game"},
          target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
          "atomic-transfer");
        // A successful commit that already closed the move must forward the
        // original operation rather than submitting to the prepared old queue.
        const auto followed =
          transfers.admit_dispatch_packet (key, nullptr, true, packet, submit, &source_fence);
        const auto fresh_followed = transfers.admit_dispatch_packet (
          key, nullptr, true, packet, submit, nullptr, source_fence.object_generation);
        const auto returned = transfers.admit_dispatch_packet (
          key, nullptr, true, packet, submit, &target_fence, target_fence.object_generation);

        if (parked.append_result != handoff_append_result_t::appended || parked.queue_submission
            || parked.released_packet
            || duplicate.append_result != handoff_append_result_t::duplicate_request
            || duplicate.queue_submission || backlog.size () != 1
            || admitted.queue_submission != std::optional<bool>{true} || admitted.released_packet
            || !ordered || runs != 1 || closed.queue_submission != std::optional<bool>{false}
            || calls != 3 || followed.queue_submission
            || followed.message_follow_source != source_fence || !followed.released_packet
            || followed.released_packet->metadata != packet.metadata
            || fresh_followed.queue_submission
            || fresh_followed.message_follow_source != source_fence
            || returned.message_follow_source
            || returned.queue_submission != std::optional<bool>{false}) {
            std::cerr << "sealed source atomic-admission lifecycle failed" << std::endl;
            return false;
        }
    }

    // Completing and then throwing reports the late exception without a
    // second queue release. Each work callback receives only its own turn.
    {
        offload_executor_t executor (1);
        std::atomic_int errors = 0;
        std::atomic_int completions = 0;
        std::atomic_int followers = 0;
        std::atomic_bool first_had_turn = false;
        std::atomic_bool completion_had_turn = false;
        std::atomic_bool follower_had_turn = false;
        serial_execution_queue_t queue (
          executor, {}, [&] (const std::string &name, const std::exception_ptr &error) {
              if (name != "throw-after-complete" || !error)
                  return;
              try {
                  std::rethrow_exception (error);
              }
              catch (const std::runtime_error &failure) {
                  if (std::string_view (failure.what ()) == "throw-after-complete") {
                      ++errors;
                  }
              }
          });
        if (!queue.try_post_async ("throw-after-complete",
                                   [&] (auto complete) {
                                       first_had_turn =
                                         static_cast<bool> (detail::capture_current_serial_turn ());
                                       complete ([&] {
                                           completion_had_turn = static_cast<bool> (
                                             detail::capture_current_serial_turn ());
                                           ++completions;
                                       });
                                       throw std::runtime_error ("throw-after-complete");
                                   })
            || !queue.try_post ("after-throw", [&] {
                   follower_had_turn = static_cast<bool> (detail::capture_current_serial_turn ());
                   ++followers;
               })) {
            queue.cancel_pending ();
            return false;
        }
        queue.drain ();
        if (errors.load () != 1 || completions.load () != 1 || followers.load () != 1
            || !first_had_turn.load () || completion_had_turn.load () || !follower_had_turn.load ()
            || detail::capture_current_serial_turn () || queue.pending_count () != 0) {
            return false;
        }
    }

    return true;
}
// Handler turn and execution gate §5, §7: work a handler defers to its
// terminal keeps the lifecycle FIFO position accepted at registration. It
// never runs ahead of lifecycle work accepted before it.
int verify_deferred_work_keeps_lifecycle_fifo_position ()
{
    using namespace zlink::framework::runtime;
    offload_executor_t executor (1);
    serial_execution_queue_t queue (executor);
    std::mutex events_mutex;
    std::vector<std::string> events;
    const auto record = [&] (std::string event) {
        std::lock_guard lock (events_mutex);
        events.push_back (std::move (event));
    };
    const auto deferred_phase = [] {
        const auto turn = zlink::framework::detail::capture_current_serial_turn ();
        return turn && turn->is_after_active_phase ();
    };
    const serial_work_options_t lifecycle{serial_work_lane_t::lifecycle};

    queue.run ("handler", [&] {
        queue.post ("older-lifecycle", [&] { record ("older-lifecycle"); }, lifecycle);
        if (!zlink::framework::detail::defer_current_serial_turn ([&] {
                record (deferred_phase () ? "join" : "join-in-handler-phase");
                return ::zlink::framework::task_t<void> (
                  ::zlink::framework::result_t<void>::success ());
            }))
            throw std::runtime_error ("join was not deferred");
        if (!queue.try_post_deferred ("cleanup", [&] { record ("cleanup"); }))
            throw std::runtime_error ("cleanup was not deferred");
        queue.post ("newer-lifecycle", [&] { record ("newer-lifecycle"); }, lifecycle);
        record ("handler");
    });
    queue.drain ();
    {
        std::lock_guard lock (events_mutex);
        if (events
            != std::vector<std::string>{"handler", "older-lifecycle", "join", "cleanup",
                                        "newer-lifecycle"}) {
            for (const auto &event : events)
                std::cerr << "deferred order: " << event << '\n';
            return 1;
        }
        events.clear ();
    }

    queue.run ("failed-handler", [&] {
        queue.post ("older-lifecycle", [&] { record ("older-lifecycle"); }, lifecycle);
        if (!zlink::framework::detail::defer_current_serial_turn (
              [&] {
                  record ("join-must-not-run");
                  return ::zlink::framework::task_t<void> (
                    ::zlink::framework::result_t<void>::success ());
              },
              [&] { record ("join-discarded"); }))
            throw std::runtime_error ("join was not deferred");
        if (!queue.try_post_deferred ("cleanup", [&] { record ("cleanup"); }))
            throw std::runtime_error ("cleanup was not deferred");
        throw std::runtime_error ("handler failed after defer");
    });
    queue.drain ();
    {
        std::lock_guard lock (events_mutex);
        if (events != std::vector<std::string>{"join-discarded", "older-lifecycle", "cleanup"}) {
            for (const auto &event : events)
                std::cerr << "failed deferred order: " << event << '\n';
            return 2;
        }
    }
    return 0;
}

// Handler turn and execution gate §5: a Join deferred before a Yield activates
// when the last awaited continuation ends normally, not at the Yield, and is
// discarded when the handler fails after the Yield.
int verify_deferred_join_waits_for_handler_terminal_across_yield ()
{
    using namespace zlink::framework::runtime;
    offload_executor_t executor (1);
    serial_execution_queue_t queue (executor);
    std::mutex events_mutex;
    std::vector<std::string> events;
    const auto record = [&] (std::string event) {
        std::lock_guard lock (events_mutex);
        events.push_back (std::move (event));
    };
    const auto yielding_handler = [&] (bool fail_after_yield) {
        queue.post_async ("yielding-handler", [&, fail_after_yield] (auto complete) {
            if (!zlink::framework::detail::defer_current_serial_turn (
                  [&] {
                      record ("join");
                      return ::zlink::framework::task_t<void> (
                        ::zlink::framework::result_t<void>::success ());
                  },
                  [&] { record ("join-discarded"); }))
                throw std::runtime_error ("join was not deferred");
            auto turn = zlink::framework::detail::capture_current_serial_turn ();
            auto resume = zlink::framework::detail::release_serial_turn_for_resume (*turn);
            if (!resume)
                throw std::runtime_error ("turn was not released");
            record ("yield");
            // Deliver the actual handler result after its final continuation,
            // as the dispatcher does for a coroutine that awaited Yield.
            resume ([&, complete, fail_after_yield] {
                record ("continuation");
                complete ([fail_after_yield] {
                    if (fail_after_yield)
                        throw std::runtime_error ("handler failed after Yield");
                });
            });
        });
        queue.drain ();
    };
    yielding_handler (false);
    {
        std::lock_guard lock (events_mutex);
        if (events != std::vector<std::string>{"yield", "continuation", "join"}) {
            for (const auto &event : events)
                std::cerr << "yield deferred order: " << event << '\n';
            return 3;
        }
        events.clear ();
    }
    yielding_handler (true);
    {
        std::lock_guard lock (events_mutex);
        if (events != std::vector<std::string>{"yield", "continuation", "join-discarded"}) {
            for (const auto &event : events)
                std::cerr << "failed yield deferred order: " << event << '\n';
            return 4;
        }
    }
    return 0;
}
bool verify_deferred_actual_terminal (bool fail_first, bool fail_handler, bool flat)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      flat ? rt::serial_lane_policy_t::per_actor_spot () : rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::actor_delivery ());
    actor->attach_spot_gate (root);
    fw::task_completion_source_t<void> first, second;
    rt::serial_execution_queue_t::async_completion_t handler;
    int activated = 0, cancelled = 0, suffix = 0, reply = 0, cleanup = 0;
    actor->post_async ("terminal-handler", [&] (auto complete) {
        handler = std::move (complete);
        const auto turn = fw::detail::capture_current_serial_turn ();
        if (!turn->defer (
              [&] {
                  ++activated;
                  return first.task ();
              },
              [&] { ++cancelled; })
            || !turn->defer (
              [&] {
                  ++activated;
                  return second.task ();
              },
              [&] { ++cancelled; })
            || !(flat ? actor : root)->try_post_deferred ("failure-cleanup", [&] { ++cleanup; }))
            throw std::runtime_error ("deferred registration failed");
    });
    actor->post ("terminal-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler && activated == 0 && suffix == 0;
    handler ([&] {
        ++reply;
        if (fail_handler)
            throw std::runtime_error ("handler failed");
    });
    worker.run_all ();
    const auto before = activated;
    const auto suffix_before = suffix;
    passed = passed && reply == 1;
    if (fail_handler)
        passed = passed && activated == 0 && cancelled == 2 && suffix == 1 && cleanup == 1;
    else
        passed = passed && activated == 1 && suffix == 0 && cleanup == 0;
    first.complete (fail_first ? fw::result_t<void>::failure (
                                   fw::framework_error_kind_t::internal_failure, "first failed")
                               : fw::result_t<void>::success ());
    worker.run_all ();
    if (!fail_handler)
        passed = passed && activated == 2 && suffix == 0 && cleanup == 0;
    second.complete (fw::result_t<void>::success ());
    worker.run_all ();
    passed = passed && suffix == 1 && cleanup == 1 && reply == 1;
    const auto root_pending = root->pending_count ();
    const auto actor_pending = actor->pending_count ();
    passed = passed && root_pending == 0 && actor_pending == 0;
    actor->close ();
    root->close ();
    worker.run_all ();
    worker.drain ();
    std::cout << "deferred-actual flat=" << flat << " fail_first=" << fail_first
              << " fail_handler=" << fail_handler << " activated_before=" << before
              << " suffix_before=" << suffix_before << " activated=" << activated
              << " cancelled=" << cancelled << " suffix=" << suffix << " reply=" << reply
              << " cleanup=" << cleanup << " root_pending=" << root_pending
              << " actor_pending=" << actor_pending << " passed=" << passed << '\n';
    return passed;
}

bool verify_readiness_before_deferred_join ()
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::actor_delivery ());
    actor->attach_spot_gate (root);
    std::shared_ptr<fw::detail::serial_turn_t> handler_turn;
    rt::serial_execution_queue_t::async_completion_t handler;
    fw::task_completion_source_t<void> joined;
    int capture = 0, join_started = 0, suffix = 0, reply = 0;
    actor->post_async ("readiness-handler", [&] (auto complete) {
        handler_turn = fw::detail::capture_current_serial_turn ();
        handler = std::move (complete);
    });
    actor->post ("readiness-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler_turn && handler_turn->release ();
    auto readiness = root->reserve_barrier_next (
      "before-deferred-join", false,
      [actor] () -> fw::task_t<std::vector<std::shared_ptr<rt::serial_execution_queue_t>>> {
          co_return std::vector<std::shared_ptr<rt::serial_execution_queue_t>>{actor};
      });
    auto captured = readiness.value ()->run ([&] () -> fw::task_t<void> {
        ++capture;
        co_return;
    });
    worker.run_all ();
    passed = passed && capture == 0;
    handler_turn->resume_scheduler () ([&] {
        const auto turn = fw::detail::capture_current_serial_turn ();
        if (!turn->defer (
              [&] {
                  ++join_started;
                  return joined.task ();
              },
              [&] {
                  joined.complete (fw::result_t<void>::failure (
                    fw::framework_error_kind_t::shutting_down, "Join cancelled"));
              }))
            throw std::runtime_error ("join registration failed");
        handler ([&] { ++reply; });
    });
    worker.run_all ();
    const auto capture_before = capture;
    const auto join_before = join_started;
    const auto suffix_before = suffix;
    passed = passed && capture == 1 && join_started == 0 && suffix == 0;
    readiness.value ()->cancel ();
    worker.run_all ();
    passed = passed && join_started == 1 && suffix == 0;
    joined.complete (fw::result_t<void>::success ());
    worker.run_all ();
    passed = passed && reply == 1 && suffix == 1;
    const auto root_pending = root->pending_count ();
    const auto actor_pending = actor->pending_count ();
    passed = passed && root_pending == 0 && actor_pending == 0;
    actor->close ();
    root->close ();
    worker.run_all ();
    worker.drain ();
    std::cout << "readiness-before-join capture_before=" << capture_before
              << " join_before=" << join_before << " suffix_before=" << suffix_before
              << " capture=" << capture << " join=" << join_started << " suffix=" << suffix
              << " reply=" << reply << " root_pending=" << root_pending
              << " actor_pending=" << actor_pending << " passed=" << passed << '\n';
    return passed;
}
zlink::framework::task_t<void> deferred_owner_task (zlink::framework::task_t<void> source)
{
    co_await source;
}

bool verify_deferred_burst_terminal (bool flat, bool fail_first)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    const rt::serial_execution_queue_options_t options;
    const auto count = options.lifecycle_burst_limit + 1;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, options, rt::serial_execution_queue_t::error_handler_t{},
      flat ? rt::serial_lane_policy_t::per_actor_spot () : rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (root);
    std::vector<fw::task_completion_source_t<void>> sources (count);
    rt::serial_execution_queue_t::async_completion_t handler;
    std::size_t activated = 0;
    int suffix = 0, reply = 0, cancelled = 0;
    actor->post_async ("burst-handler", [&] (auto complete) {
        handler = std::move (complete);
        auto turn = fw::detail::capture_current_serial_turn ();
        for (std::size_t index = 0; index < count; ++index) {
            if (!turn->defer (
                  [&, index] {
                      ++activated;
                      return deferred_owner_task (sources[index].task ());
                  },
                  [&] { ++cancelled; }))
                throw std::runtime_error ("burst Defer registration failed");
        }
    });
    actor->post ("burst-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler && activated == 0 && suffix == 0;
    handler ([&] { ++reply; });
    worker.run_all ();
    passed = passed && reply == 1 && activated == 1 && suffix == 0;
    for (std::size_t index = 0; index < count; ++index) {
        passed = passed && activated == index + 1 && suffix == 0;
        sources[index].complete (
          fail_first && index == 0
            ? fw::result_t<void>::failure (fw::framework_error_kind_t::internal_failure,
                                           "burst first Defer failed")
            : fw::result_t<void>::success ());
        worker.run_all ();
        passed = passed && activated == std::min (index + 2, count)
                 && suffix == (index + 1 == count ? 1 : 0);
    }
    passed = passed && reply == 1 && cancelled == 0 && root->pending_count () == 0
             && actor->pending_count () == 0;
    // The fixture's completed callback retains the shared Actor wrapper.
    // Release that caller-owned handle before checking runtime ownership.
    handler = {};
    const std::weak_ptr<rt::serial_execution_queue_t> root_reference = root;
    const std::weak_ptr<rt::serial_execution_queue_t> actor_reference = actor;
    actor->close ();
    root->close ();
    worker.run_all ();
    actor.reset ();
    root.reset ();
    worker.run_all ();
    const bool references_released = root_reference.expired () && actor_reference.expired ();
    passed = passed && references_released;
    worker.drain ();
    std::cout << "deferred-burst flat=" << flat << " fail_first=" << fail_first
              << " count=" << count << " activated=" << activated << " reply=" << reply
              << " suffix=" << suffix << " cancelled=" << cancelled
              << " references_released=" << references_released << " passed=" << passed << '\n';
    return passed;
}


zlink::framework::task_t<zlink::framework::result_t<void>> deferred_open_gate_task (
  std::shared_ptr<zlink::framework::runtime::serial_execution_queue_t> target, int &adopted)
{
    const auto accepted = co_await target->adopt_application_prefix ([&adopted] {
        ++adopted;
        return std::vector<zlink::framework::runtime::serial_prefix_work_t>{};
    });
    co_return accepted
      ? zlink::framework::result_t<void>::success ()
      : zlink::framework::result_t<void>::failure (
          zlink::framework::framework_error_kind_t::shutting_down, "target closed");
}

bool verify_deferred_coroutine_gate_terminal (bool flat, bool fail, bool switch_gate)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    const auto policy =
      flat ? rt::serial_lane_policy_t::per_actor_spot () : rt::serial_lane_policy_t::spot_wide ();
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, policy);
    auto target_root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, policy);
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    auto target = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (root);
    target->attach_spot_gate (switch_gate ? target_root : root);
    fw::task_completion_source_t<void> source, barrier;
    rt::serial_execution_queue_t::async_completion_t handler;
    std::shared_ptr<fw::task_t<void>> join;
    std::shared_ptr<fw::task_t<fw::result_t<void>>> opened;
    int activated = 0, h = 0, suffix = 0, adopted = 0, finished = 0;
    actor->post_async ("coroutine-gate-handler", [&] (auto complete) {
        handler = std::move (complete);
        if (!fw::detail::defer_current_serial_turn ([&] {
                ++activated;
                join = std::make_shared<fw::task_t<void>> (deferred_owner_task (source.task ()));
                fw::detail::observe_task_terminal (*join, [&] (const fw::result_t<void> &result) {
                    opened = std::make_shared<fw::task_t<fw::result_t<void>>> (
                      deferred_open_gate_task (target, adopted));
                    fw::detail::observe_task_completion (*opened, [&,
                                                                   result] (const auto &settled) {
                        ++finished;
                        barrier.complete (result ? (settled
                                                      ? settled.value ()
                                                      : fw::detail::result_access_t::failure<void> (
                                                          *settled.error ()))
                                                 : result);
                    });
                });
                return barrier.task ();
            }))
            throw std::runtime_error ("Defer registration failed");
    });
    actor->post ("coroutine-gate-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler && activated == 0 && suffix == 0;
    handler ([&] { ++h; });
    worker.run_all ();
    passed = passed && h == 1 && activated == 1 && suffix == 0 && !barrier.task ().await_ready ();
    source.complete (fail ? fw::result_t<void>::failure (
                              fw::framework_error_kind_t::internal_failure, "source failed")
                          : fw::result_t<void>::success ());
    worker.run_all ();
    const bool join_done = join && join->await_ready ();
    const bool opened_done = opened && opened->await_ready ();
    const bool barrier_done = barrier.task ().await_ready ();
    const auto private_pending = actor->pending_count ();
    passed = passed && join_done && opened_done && barrier_done && finished == 1 && adopted == 1
             && suffix == 1 && private_pending == 0;
    std::cout << "coroutine-gate flat=" << flat << " fail=" << fail << " switch=" << switch_gate
              << " H=" << h << " join=" << join_done << " opened=" << opened_done
              << " barrier=" << barrier_done << " finished=" << finished << " adopted=" << adopted
              << " suffix=" << suffix << " private_pending=" << private_pending
              << " passed=" << passed << '\n';
    // Release the actual terminal even on a cyclic/failing input, then drain normally.
    source.complete (fw::result_t<void>::success ());
    barrier.complete (fw::result_t<void>::success ());
    worker.run_all ();
    actor->close ();
    target->close ();
    worker.run_all ();
    actor->drain ();
    target->drain ();
    root->close ();
    target_root->close ();
    worker.run_all ();
    root->drain ();
    target_root->drain ();
    worker.retire ();
    return passed;
}

bool verify_deferred_terminal_owner (bool flat, bool fail, bool active_application)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      flat ? rt::serial_lane_policy_t::per_actor_spot () : rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (root);
    fw::task_completion_source_t<void> source;
    rt::serial_execution_queue_t::async_completion_t handler, application;
    std::shared_ptr<fw::detail::serial_turn_t> application_turn;
    int activated = 0, reply = 0, suffix = 0, application_terminal = 0;
    actor->post_async ("deferred-owner-handler", [&] (auto complete) {
        handler = std::move (complete);
        if (!fw::detail::defer_current_serial_turn ([&] {
                ++activated;
                return deferred_owner_task (source.task ());
            }))
            throw std::runtime_error ("Defer owner registration failed");
    });
    actor->post ("deferred-owner-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler && activated == 0 && suffix == 0;
    handler ([&] { ++reply; });
    worker.run_all ();
    passed = passed && reply == 1 && activated == 1 && suffix == 0;
    if (active_application) {
        root->post_async ("application-keeps-claim", [&] (auto complete) {
            application = std::move (complete);
            application_turn = fw::detail::capture_current_serial_turn ();
        });
        worker.run_all ();
        passed = passed && application && application_turn && !application_turn->released ();
    }
    source.complete (fail ? fw::result_t<void>::failure (
                              fw::framework_error_kind_t::internal_failure, "Defer failed")
                          : fw::result_t<void>::success ());
    const auto suffix_before_owner = suffix;
    const auto mailbox_before_owner = actor->pending_count ();
    passed = passed && suffix_before_owner == 0 && mailbox_before_owner == 2;
    auto mailbox_after_owner_attempt = mailbox_before_owner;
    auto application_pending = root->pending_count (rt::serial_work_lane_t::application);
    bool application_claim_retained = !active_application || !application_turn->released ();
    if (active_application) {
        worker.run_all ();
        mailbox_after_owner_attempt = actor->pending_count ();
        application_pending = root->pending_count (rt::serial_work_lane_t::application);
        application_claim_retained = !application_turn->released ();
        passed = passed && application_claim_retained && application_terminal == 0
                 && root->pending_count (rt::serial_work_lane_t::application) == 1;
        // Execution gate §2: PerActor has an independent Actor gate; only
        // SpotWide completion must wait for this retained Spot application.
        if (flat)
            passed = passed && actor->pending_count () == 0 && suffix == 1
                     && root->pending_count (rt::serial_work_lane_t::lifecycle) == 0;
        else
            passed = passed && actor->pending_count () == 2 && suffix == 0
                     && root->pending_count (rt::serial_work_lane_t::lifecycle) == 1;
    }
    // Keep the failure observation before releasing the other real H.
    if (application)
        application ([&] { ++application_terminal; });
    worker.run_all ();
    passed = passed && reply == 1 && activated == 1 && suffix == 1
             && application_terminal == (active_application ? 1 : 0) && root->pending_count () == 0
             && actor->pending_count () == 0;
    handler = {};
    application = {};
    application_turn.reset ();
    actor->close ();
    root->close ();
    worker.run_all ();
    actor->drain ();
    root->drain ();
    worker.drain ();
    std::cout << "deferred-terminal-owner flat=" << flat << " fail=" << fail
              << " active_application=" << active_application
              << " suffix_before_owner=" << suffix_before_owner
              << " mailbox_before_owner=" << mailbox_before_owner
              << " mailbox_after_owner_attempt=" << mailbox_after_owner_attempt
              << " application_pending=" << application_pending
              << " application_claim_retained=" << application_claim_retained << " reply=" << reply
              << " suffix=" << suffix << " application_terminal=" << application_terminal
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_generic_defer_registered_after_yield ()
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    const auto make_actor = [&] {
        auto actor = std::make_shared<rt::serial_execution_queue_t> (
          worker, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{},
          rt::serial_lane_policy_t::actor_delivery ());
        actor->attach_spot_gate (root);
        return actor;
    };
    auto actor = make_actor ();
    auto other = make_actor ();
    fw::task_completion_source_t<void> terminal;
    std::shared_ptr<fw::detail::serial_turn_t> handler_turn;
    rt::serial_execution_queue_t::async_completion_t handler;
    bool registered = false, resumed = false, handler_finished = false;
    int activated = 0, suffix = 0, other_lifecycle = 0;
    actor->post_async ("after-yield-source", [&] (auto complete) {
        handler_turn = fw::detail::capture_current_serial_turn ();
        handler = std::move (complete);
    });
    actor->post ("after-yield-suffix", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler_turn && handler && activated == 0 && suffix == 0;
    const bool released = handler_turn && handler_turn->release ();
    passed = passed && released;
    worker.run_all ();
    if (released) {
        handler_turn->resume_scheduler () ([&] {
            resumed = true;
            // This is the first deferred registration for the handler chain.
            registered = static_cast<bool> (fw::detail::defer_current_serial_turn ([&] {
                ++activated;
                return terminal.task ();
            }));
            if (handler) {
                handler_finished = true;
                handler ([] {});
            }
        });
        worker.run_all ();
    }
    // Release the original handler even if the registration/release path failed.
    if (handler && !handler_finished) {
        handler_finished = true;
        handler ([] {});
        worker.run_all ();
    }
    const auto suffix_before_terminal = suffix;
    passed = passed && resumed && registered && activated == 1 && suffix == 0;
    other->post (
      "other-actor-lifecycle", [&] { ++other_lifecycle; },
      rt::serial_work_options_t{rt::serial_work_lane_t::lifecycle});
    worker.run_all ();
    passed = passed && other_lifecycle == 1 && suffix == 0;
    terminal.complete (fw::result_t<void>::success ());
    worker.run_all ();
    const auto root_pending = root->pending_count ();
    const auto actor_pending = actor->pending_count ();
    const auto other_pending = other->pending_count ();
    passed = passed && suffix == 1 && activated == 1 && root_pending == 0 && actor_pending == 0
             && other_pending == 0;
    actor->close ();
    other->close ();
    root->close ();
    worker.run_all ();
    worker.drain ();
    std::cout << "generic-defer-after-yield resumed=" << resumed << " registered=" << registered
              << " activated=" << activated << " suffix_before_terminal=" << suffix_before_terminal
              << " suffix=" << suffix << " other_lifecycle=" << other_lifecycle
              << " root_pending=" << root_pending << " actor_pending=" << actor_pending
              << " other_pending=" << other_pending << " passed=" << passed << '\n';
    return passed;
}

bool verify_deferred_settlement_terminal (bool fail_work,
                                          bool fail_settlement,
                                          bool cancel_pending = false)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    std::vector<std::string> reported_errors;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      [&] (const std::string &, const std::exception_ptr &error) {
          try {
              if (error)
                  std::rethrow_exception (error);
          }
          catch (const std::exception &failure) {
              reported_errors.emplace_back (failure.what ());
          }
      });
    fw::detail::deferred_barrier_t::async_completion_t work_done, settlement_done;
    int work_started = 0, settlement_started = 0, cancelled = 0, suffix = 0;
    auto reserved = queue->reserve_deferred_barrier_next (
      "controlled-settlement", [&] { ++cancelled; },
      [&] (auto complete) {
          ++settlement_started;
          settlement_done = std::move (complete);
      });
    bool passed = static_cast<bool> (reserved);
    if (reserved) {
        auto barrier = reserved.value ();
        auto terminal = barrier->terminal ();
        queue->post ("settlement-suffix", [&] { ++suffix; });
        if (cancel_pending) {
            barrier->cancel ();
            barrier->cancel ();
            worker.run_all ();
            passed = passed && terminal.await_ready ();
            if (terminal.await_ready ())
                passed =
                  passed && !terminal.result ()
                  && terminal.result ().error_kind () == fw::framework_error_kind_t::shutting_down;
            passed = passed && cancelled == 1 && work_started == 0 && settlement_started == 0
                     && suffix == 1;
        } else {
            const auto activated = barrier->activate_async ([&] (auto complete) {
                ++work_started;
                work_done = std::move (complete);
            });
            passed = passed && static_cast<bool> (activated);
            worker.run_all ();
            passed = passed && work_started == 1 && !terminal.await_ready () && suffix == 0;
            if (work_done)
                work_done (fail_work
                             ? fw::result_t<void>::failure (
                                 fw::framework_error_kind_t::internal_failure, "work failure")
                             : fw::result_t<void>::success ());
            worker.run_all ();
            const bool pending_at_settlement = !terminal.await_ready ();
            passed = passed && settlement_started == 1 && settlement_done && pending_at_settlement
                     && suffix == 0;
            if (settlement_done)
                settlement_done (fail_settlement
                                   ? fw::result_t<void>::failure (
                                       fw::framework_error_kind_t::rejected, "settlement failure")
                                   : fw::result_t<void>::success ());
            worker.run_all ();
            passed = passed && terminal.await_ready ();
            if (terminal.await_ready ()) {
                const auto &result = terminal.result ();
                if (fail_work || fail_settlement) {
                    const auto expected_kind = fail_work
                                                 ? fw::framework_error_kind_t::internal_failure
                                                 : fw::framework_error_kind_t::rejected;
                    const std::string_view expected_message =
                      fail_work ? "work failure" : "settlement failure";
                    passed = passed && !result && result.error_kind () == expected_kind
                             && result.error () && result.error ()->what () == expected_message;
                    passed = passed && reported_errors.size () == 1
                             && reported_errors.front () == expected_message;
                } else
                    passed = passed && result && reported_errors.empty ();
            }
            passed = passed && work_started == 1 && settlement_started == 1 && cancelled == 0
                     && suffix == 1;
        }
    }
    const auto pending = queue->pending_count ();
    passed = passed && pending == 0;
    queue->close ();
    worker.run_all ();
    worker.drain ();
    std::cout << "deferred-settlement fail_work=" << fail_work
              << " fail_settlement=" << fail_settlement << " cancel_pending=" << cancel_pending
              << " work_started=" << work_started << " settlement_started=" << settlement_started
              << " cancelled=" << cancelled << " suffix=" << suffix << " pending=" << pending
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_deferred_extra_boundaries ()
{
    bool passed = verify_generic_defer_registered_after_yield ();
    for (const bool fail_work : {false, true})
        for (const bool fail_settlement : {false, true})
            passed = verify_deferred_settlement_terminal (fail_work, fail_settlement) && passed;
    return verify_deferred_settlement_terminal (false, false, true) && passed;
}
bool verify_deferred_after_lifecycle_prefix (bool flat)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    const rt::serial_execution_queue_options_t options;
    const auto prefix_count = options.lifecycle_burst_limit + 1;
    auto root = std::make_shared<rt::serial_execution_queue_t> (
      worker, options, rt::serial_execution_queue_t::error_handler_t{},
      flat ? rt::serial_lane_policy_t::per_actor_spot () : rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (root);
    fw::task_completion_source_t<void> joined;
    rt::serial_execution_queue_t::async_completion_t handler;
    std::size_t prefix_finished = 0;
    int activated = 0, suffix = 0;
    bool registered = false;
    actor->post_async ("prefix-source-handler", [&] (auto complete) {
        handler = std::move (complete);
        // Infrastructure may accept Actor lifecycle controls while its
        // application handler is active, before that handler registers Join.
        for (std::size_t index = 0; index < prefix_count; ++index)
            actor->post (
              "earlier-lifecycle-control", [&] { ++prefix_finished; },
              rt::serial_work_options_t{rt::serial_work_lane_t::lifecycle});
        registered = static_cast<bool> (fw::detail::defer_current_serial_turn ([&] {
            ++activated;
            return joined.task ();
        }));
    });
    actor->post ("payload-after-source-handler", [&] { ++suffix; });
    worker.run_all ();
    bool passed = handler && registered && prefix_finished == 0 && activated == 0 && suffix == 0;
    if (handler)
        handler ([] {});
    worker.run_all ();
    const auto suffix_before_join = suffix;
    passed = passed && prefix_finished == prefix_count && activated == 1 && suffix == 0;
    joined.complete (fw::result_t<void>::success ());
    worker.run_all ();
    const auto root_pending = root->pending_count ();
    const auto actor_pending = actor->pending_count ();
    passed = passed && suffix == 1 && activated == 1 && root_pending == 0 && actor_pending == 0;
    handler = {};
    actor->close ();
    root->close ();
    worker.run_all ();
    actor->drain ();
    root->drain ();
    worker.drain ();
    std::cout << "deferred-after-lifecycle-prefix flat=" << flat << " prefix_count=" << prefix_count
              << " prefix_finished=" << prefix_finished << " activated=" << activated
              << " suffix_before_join=" << suffix_before_join << " suffix=" << suffix
              << " root_pending=" << root_pending << " actor_pending=" << actor_pending
              << " passed=" << passed << '\n';
    return passed;
}


bool verify_durable_policy_late_grant (bool transfer)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    const auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, 1u, 1, 1});
    fw::test::manual_internal_scheduler_t worker;
    auto source = std::make_shared<rt::serial_execution_queue_t> (worker);
    auto target = std::make_shared<rt::serial_execution_queue_t> (worker);
    auto held = jobs->try_reserve_supply ();
    if (!held)
        return false;
    held->mark_queued ();
    const auto make_record = [&] {
        auto record = std::make_shared<rt::serial_application_record_t> ();
        record->payload ().emplace ();
        record->admission.jobs = jobs;
        return record;
    };
    auto a = make_record ();
    auto b = make_record ();
    int started_a = 0, started_b = 0, cancelled = 0;
    const auto work = [&] (const std::shared_ptr<rt::serial_application_record_t> &record,
                           int &started) {
        rt::serial_prefix_work_t item;
        item.name = "durable-dynamic-policy";
        item.options.application_record = record;
        item.options.retained_message = record;
        item.work = [record, &started] (auto complete) {
            record->admission.enter_handler ();
            ++started;
            complete ([] {});
        };
        item.cancel = [&] { ++cancelled; };
        return item;
    };
    source->set_application_start_policy (
      [b] (const std::shared_ptr<const void> &message) { return message == b; });
    const auto accepted = source
                            ->adopt_application_prefix ([&] {
                                std::vector<rt::serial_prefix_work_t> prefix;
                                if (!transfer)
                                    prefix.push_back (work (a, started_a));
                                prefix.push_back (work (b, started_b));
                                return prefix;
                            })
                            .result ();
    bool passed = accepted && accepted.value () && started_a == 0 && started_b == 0
                  && jobs->snapshot ().capacity_waiters == 1;
    // The grant is delivered on the manual scheduler; keep it queued while
    // changing the selected record or transferring its actual accepted body.
    held.reset ();
    if (transfer) {
        auto exported = source->take_application_suffix ().result ();
        passed =
          passed && exported && exported.value ().size () == 1 && exported.value ().front () == b;
        auto adopted = target
                         ->adopt_application_prefix ([&] {
                             std::vector<rt::serial_prefix_work_t> prefix;
                             prefix.push_back (work (b, started_b));
                             return prefix;
                         })
                         .result ();
        passed = passed && adopted && adopted.value ();
    } else {
        // Opening preparation makes the earlier physical record A eligible.
        // B's pending grant must not strand B or consume A's only supply.
        source->set_application_start_policy ({});
    }
    worker.run_all ();
    const auto source_pending = source->pending_count ();
    const auto target_pending = target->pending_count ();
    const auto waiters = jobs->snapshot ().capacity_waiters;
    const auto permits = jobs->snapshot ().permits_in_use;
    passed = passed && started_a == (transfer ? 0 : 1) && started_b == 1 && cancelled == 0
             && source_pending == 0 && target_pending == 0 && waiters == 0 && permits == 0;
    // A red result can leave the stale durable waiter in source. Discarding
    // queued records is the normal owner cleanup and makes this test terminate.
    source->cancel_pending ();
    target->cancel_pending ();
    jobs->stop ();
    worker.run_all ();
    source->drain ();
    target->drain ();
    worker.drain ();
    std::cout << "durable-policy-late-grant transfer=" << transfer << " started_a=" << started_a
              << " started_b=" << started_b << " source_pending=" << source_pending
              << " target_pending=" << target_pending << " waiters=" << waiters
              << " permits=" << permits << " passed=" << passed << '\n';
    return passed;
}
}
