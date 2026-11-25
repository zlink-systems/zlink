/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test::execution
{
bool verify_durable_head_owner_transitions ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    const auto jobs = std::make_shared<application_job_queue_t> (
      application_job_queue_configuration_t{application_job_queue_profile_t::balanced, 1u, 1, 1});
    test::manual_internal_scheduler_t worker;
    auto source = std::make_shared<serial_execution_queue_t> (worker);
    auto target = std::make_shared<serial_execution_queue_t> (worker);
    auto held = jobs->try_reserve_supply ();
    if (!held)
        return false;
    held->mark_queued ();
    auto record = std::make_shared<serial_application_record_t> ();
    record->payload ().emplace ();
    record->admission.jobs = jobs;
    std::atomic_int started{0}, cancelled{0}, native_returns{0};
    std::promise<void> handler_done;
    auto handler_terminal = handler_done.get_future ();
    record->transferred = [&] { ++native_returns; };
    const auto prefix = [&] {
        serial_prefix_work_t work;
        work.name = "durable-owner-transition";
        work.options.application_record = record;
        work.work = [&, record] (auto complete) {
            record->admission.enter_handler ();
            ++started;
            complete ([&] { handler_done.set_value (); });
        };
        work.cancel = [&] { ++cancelled; };
        return std::vector<serial_prefix_work_t>{std::move (work)};
    };
    bool passed = source->adopt_application_prefix (prefix).result ().value ();
    std::cout << "head-owner checkpoint waiters=" << jobs->snapshot ().capacity_waiters
              << " started=" << started << " passed_before=" << passed << '\n';
    passed = passed && jobs->snapshot ().capacity_waiters == 1 && started == 0;
    // Keep the source grant queued until ownership has moved to target.
    held.reset ();
    auto exported = source->take_application_suffix ().result ().value ();
    passed = passed && exported.size () == 1 && exported.front () == record;
    passed = target->adopt_application_prefix (prefix).result ().value () && passed;
    std::cout << "head-owner checkpoint waiters=" << jobs->snapshot ().capacity_waiters
              << " started=" << started << " passed_before=" << passed << '\n';
    passed = passed && jobs->snapshot ().capacity_waiters == 1 && started == 0;
    worker.run_all ();
    const bool reached_terminal = started == 1;
    if (!reached_terminal) {
        jobs->stop ();
        target->cancel_pending ();
        worker.run_all ();
    }
    passed = passed && reached_terminal;
    worker.run_all ();
    target->drain ();
    source->drain ();
    passed = passed && started == 1 && cancelled == 0 && native_returns == 1
             && jobs->snapshot ().permits_in_use == 0 && jobs->snapshot ().capacity_waiters == 0;
    std::cout << "head-owner terminal=" << reached_terminal << " started=" << started
              << " cancelled=" << cancelled << " native_returns=" << native_returns
              << " permits=" << jobs->snapshot ().permits_in_use
              << " waiters=" << jobs->snapshot ().capacity_waiters << " passed=" << passed << '\n';
    jobs->stop ();
    source->cancel_pending ();
    target->cancel_pending ();
    worker.drain ();
    return passed;
}

bool verify_durable_head_rejection_and_close ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    bool passed = true;
    for (const bool stopped_jobs : {false, true}) {
        auto jobs =
          std::make_shared<application_job_queue_t> (application_job_queue_configuration_t{
            application_job_queue_profile_t::balanced, 1u, 1, 1});
        test::manual_internal_scheduler_t worker;
        auto queue = std::make_shared<serial_execution_queue_t> (worker);
        if (stopped_jobs)
            jobs->stop ();
        else
            worker.retire ();
        std::atomic_int started{0}, cancelled{0}, initial_returns{0};
        bool terminal = false;
        auto record = std::make_shared<serial_application_record_t> ();
        record->payload ().emplace ();
        record->admission.jobs = jobs;
        record->admission.retain_initial ([&] { ++initial_returns; });
        const auto adopted = queue
                               ->adopt_application_prefix ([&] {
                                   serial_prefix_work_t work;
                                   work.name = "durable-rejected";
                                   work.options.application_record = record;
                                   work.work = [&] (auto complete) {
                                       ++started;
                                       complete ([] {});
                                   };
                                   work.cancel = [&] {
                                       ++cancelled;
                                       terminal = true;
                                   };
                                   return std::vector<serial_prefix_work_t>{std::move (work)};
                               })
                               .result ()
                               .value ();
        worker.run_all ();
        const bool settled = terminal;
        if (!settled)
            queue->cancel_pending ();
        passed = passed && settled && (stopped_jobs || !adopted) && started == 0 && cancelled == 1
                 && initial_returns == 1 && queue->pending_count () == 0
                 && jobs->snapshot ().capacity_waiters == 0;
        if (!stopped_jobs)
            passed = passed && !queue->reserve_barrier_next ("retired-worker");
        std::cout << "head-rejection stopped_jobs=" << stopped_jobs << " adopted=" << adopted
                  << " settled=" << settled << " started=" << started << " cancelled=" << cancelled
                  << " initial_returns=" << initial_returns
                  << " pending=" << queue->pending_count ()
                  << " waiters=" << jobs->snapshot ().capacity_waiters << " passed=" << passed
                  << '\n';
        jobs->stop ();
        queue->cancel_pending ();
        worker.drain ();
    }
    // Isolated coordinator close must cancel its permit waiter before its
    // scheduler is retired; the waiter otherwise retains the Actor owner.
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    state_lane_t lane (*scheduler);
    auto spot_queue = std::make_shared<serial_execution_queue_t> (
      *scheduler, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    detail::spot_serial_executor_t serial (scheduler, lane, serial_lane_policy_t::spot_wide (),
                                           spot_queue);
    auto jobs = std::make_shared<application_job_queue_t> (
      application_job_queue_configuration_t{application_job_queue_profile_t::balanced, 1u, 1, 1});
    auto held = jobs->try_reserve_supply ();
    held->mark_queued ();
    auto record = std::make_shared<serial_application_record_t> ();
    record->payload ().emplace ();
    record->admission.jobs = jobs;
    auto queue = serial.actor_executor ("waiting-actor")->queue ();
    bool cancelled = false;
    auto adoption = queue->adopt_application_prefix ([&] {
        serial_prefix_work_t work;
        work.name = "close-waiting-actor";
        work.options.application_record = record;
        work.work = [] (auto complete) { complete ([] {}); };
        work.cancel = [&] { cancelled = true; };
        return std::vector<serial_prefix_work_t>{std::move (work)};
    });
    scheduler->run_all ();
    adoption.result ().value ();
    serial.close ();
    scheduler->run_all ();
    const bool settled = cancelled;
    if (!settled) {
        jobs->stop ();
        queue->cancel_pending ();
    }
    scheduler->run_all ();
    queue->drain ();
    const auto close_waiters_at_check = jobs->snapshot ().capacity_waiters;
    const auto close_pending_at_check = queue->pending_count ();
    passed = passed && settled && close_waiters_at_check == 0 && close_pending_at_check == 0;
    held.reset ();
    jobs->stop ();
    std::cout << "head-close settled=" << settled << " waiters_at_check=" << close_waiters_at_check
              << " pending_at_check=" << close_pending_at_check
              << " pending=" << queue->pending_count ()
              << " waiters=" << jobs->snapshot ().capacity_waiters
              << " permits=" << jobs->snapshot ().permits_in_use << " passed=" << passed << '\n';
    scheduler->run_all ();
    scheduler->drain ();
    return passed && jobs->snapshot ().permits_in_use == 0;
}
bool verify_accepted_continuation_survives_shutdown (bool close_before_resume)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    std::shared_ptr<fw::detail::serial_turn_t> accepted;
    bool released = false, owned = false;
    rt::serial_execution_queue_t::async_completion_t finish;
    int callbacks = 0, fresh_callbacks = 0;
    queue->post_async (
      "accepted-before-shutdown",
      [&] (auto complete) {
          accepted = fw::detail::capture_current_serial_turn ();
          finish = std::move (complete);
          released = accepted && accepted->release ();
      },
      rt::serial_work_options_t{rt::serial_work_lane_t::lifecycle});
    worker.run_all ();
    if (close_before_resume)
        queue->close ();
    if (accepted) {
        accepted->resume_scheduler () ([&] {
            ++callbacks;
            const auto turn = fw::detail::capture_current_serial_turn ();
            owned = turn && !turn->released () && turn->belongs_to (queue.get ());
            // This is the handler's final segment, so deliver its actual
            // task result separately from the resume work's completion.
            finish ([] {});
        });
    }
    queue->cancel_pending ();
    const bool fresh_rejected = !queue->try_post ("fresh-after-close", [&] { ++fresh_callbacks; });
    worker.run_all ();
    const bool passed = released && owned && callbacks == 1 && fresh_rejected
                        && fresh_callbacks == 0 && queue->pending_count () == 0;
    finish = {};
    accepted.reset ();
    queue->drain ();
    worker.drain ();
    std::cout << "accepted-continuation-shutdown close_before_resume=" << close_before_resume
              << " callbacks=" << callbacks << " owned=" << owned
              << " fresh_rejected=" << fresh_rejected << " passed=" << passed << '\n';
    return passed;
}

bool verify_direct_continuation_after_close (bool shared)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    std::shared_ptr<fw::detail::serial_turn_t> accepted;
    rt::serial_execution_queue_t::async_completion_t finish;
    int executions = 0, settlements = 0, fresh = 0;
    queue->post_async ("accepted-source", [&] (auto complete) {
        accepted = fw::detail::capture_current_serial_turn ();
        finish = std::move (complete);
    });
    worker.run_all ();
    queue->close ();
    const bool continued = accepted
                           && queue->try_post_continuation (
                             "accepted-after-close",
                             [&] (auto complete) {
                                 ++executions;
                                 complete ([&] { ++settlements; });
                             },
                             accepted);
    const bool rejected = !queue->try_post ("new-after-close", [&] { ++fresh; });
    // Shutdown cancellation preserves the already accepted chain item.
    queue->cancel_pending ();
    if (finish)
        finish ([] {});
    worker.run_all ();
    const auto pending = queue->pending_count ();
    const bool passed =
      continued && rejected && executions == 1 && settlements == 1 && fresh == 0 && pending == 0;
    finish = {};
    accepted.reset ();
    queue->drain ();
    worker.drain ();
    std::cout << "direct-continuation-after-close shared=" << shared << " executions=" << executions
              << " settlements=" << settlements << " pending=" << pending
              << " fresh_rejected=" << rejected << " passed=" << passed << '\n';
    return passed;
}

bool verify_durable_tail_across_controls_and_export ()
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
    rt::serial_execution_queue_t::async_completion_t finish;
    queue->post_async ("held-before-durable-prefix",
                       [&] (auto complete) { finish = std::move (complete); });
    worker.run_all ();
    int releases = 0, cancelled = 0, controls = 0, handlers = 0;
    const auto make_record = [&] {
        auto record = std::make_shared<rt::serial_application_record_t> ();
        record->admission.retain_initial ([&] { ++releases; });
        return record;
    };
    const auto work = [&] (const auto &record) {
        return [&, record] (auto complete) {
            record->admission.enter_handler ();
            ++handlers;
            complete ([] {});
        };
    };
    auto a = make_record ();
    a->admission.take_over ();
    const bool accepted_a = queue->try_post_async (
      "durable-prefix", work (a), rt::serial_work_options_t{.application_record = a});
    constexpr int control_count = 64;
    for (int index = 0; index != control_count; ++index)
        queue->post ("recordless-control", [&] { ++controls; });
    auto b = make_record ();
    const auto b_submission = queue->try_post_cancellable_async (
      "cancelled-durable-tail", work (b), [&] { ++cancelled; },
      rt::serial_work_options_t{.application_record = b});
    const bool inherited_b = b->admission.durable ();
    const auto cancellation =
      b_submission ? queue->cancel_submission (b_submission.value ()).result ().value ()
                   : rt::serial_cancel_submission_outcome_t::already_terminal;
    auto c = make_record ();
    const bool accepted_c = queue->try_post_async (
      "durable-after-tail-cancel", work (c), rt::serial_work_options_t{.application_record = c});
    const bool inherited_c = c->admission.durable ();
    auto exported = queue->take_application_suffix ().result ();
    const bool exact_export = exported && exported.value ().size () == 2
                              && exported.value ()[0] == a && exported.value ()[1] == c;
    auto d = make_record ();
    const bool accepted_d = queue->try_post_async (
      "ordinary-after-export", work (d), rt::serial_work_options_t{.application_record = d});
    const bool ordinary_d = !d->admission.durable ();
    if (finish)
        finish ([] {});
    worker.run_all ();
    const bool passed = accepted_a && b_submission && inherited_b
                        && cancellation == rt::serial_cancel_submission_outcome_t::queued_cancelled
                        && accepted_c && inherited_c && exact_export && accepted_d && ordinary_d
                        && cancelled == 1 && controls == control_count && handlers == 1
                        && releases == 4 && queue->pending_count () == 0;
    queue->close ();
    queue->drain ();
    worker.drain ();
    std::cout << "durable-tail-controls-export inherited_b=" << inherited_b
              << " inherited_c=" << inherited_c << " exact_export=" << exact_export
              << " ordinary_d=" << ordinary_d << " releases=" << releases
              << " handlers=" << handlers << " passed=" << passed << '\n';
    return passed;
}

bool verify_inactive_barrier_and_cancel_reentry (bool shared)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    std::vector<std::string> events;
    auto inactive = queue->reserve_deferred_barrier_next ("index-inactive-barrier");
    queue->post ("index-before-activation", [&] { events.emplace_back ("ordinary"); });
    worker.run_all ();
    bool passed = inactive && events == std::vector<std::string>{"ordinary"};
    fw::detail::deferred_barrier_t::async_completion_t settle;
    if (inactive) {
        const auto activated = inactive.value ()->activate_async ([&] (auto complete) {
            events.emplace_back ("barrier");
            settle = std::move (complete);
        });
        passed = passed && bool (activated);
    }
    queue->post ("index-after-activation", [&] { events.emplace_back ("suffix"); });
    worker.run_all ();
    passed = passed && settle && events == std::vector<std::string>{"ordinary", "barrier"};
    if (settle)
        settle (fw::result_t<void>::success ());
    worker.run_all ();
    auto cancelled = queue->reserve_deferred_barrier_next ("index-cancel-reentry", [&] {
        // cancel() changes its own phase before this callback, and notifies
        // the queue afterward. The FIFO index must not copy that phase.
        queue->post ("index-from-cancel", [&] { events.emplace_back ("cancel-callback"); });
    });
    worker.run_all ();
    if (cancelled)
        cancelled.value ()->cancel ();
    worker.run_all ();
    passed =
      passed && cancelled && cancelled.value ()->terminal ().await_ready ()
      && !cancelled.value ()->terminal ().result ()
      && events == std::vector<std::string>{"ordinary", "barrier", "suffix", "cancel-callback"}
      && queue->pending_count () == 0;
    queue->close ();
    queue->drain ();
    worker.drain ();
    std::cout << "fifo-index-inactive-cancel-reentry shared=" << shared << " passed=" << passed
              << '\n';
    return passed;
}

bool verify_fifo_indexes_across_shutdown (bool shared)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    const auto policy =
      shared ? rt::serial_lane_policy_t::spot_wide () : rt::serial_lane_policy_t::per_actor_spot ();
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, policy);
    rt::serial_execution_queue_t::async_completion_t finish;
    queue->post_async ("index-blocker", [&] (auto complete) { finish = std::move (complete); });
    worker.run_all ();
    int handlers = 0, cancelled = 0, controls = 0, barrier_runs = 0;
    const auto record = [] { return std::make_shared<rt::serial_application_record_t> (); };
    const auto work = [&] (const auto &accepted) {
        return [&, accepted] (auto complete) {
            accepted->admission.enter_handler ();
            ++handlers;
            complete ([] {});
        };
    };
    auto first = record ();
    first->admission.take_over ();
    queue->post_async ("index-durable-first", work (first),
                       rt::serial_work_options_t{.application_record = first});
    auto removed = record ();
    const auto submission = queue->try_post_cancellable_async (
      "index-cancel-tail", work (removed), [&] { ++cancelled; },
      rt::serial_work_options_t{.application_record = removed});
    worker.run_all ();
    bool passed = submission && removed->admission.durable ();
    if (submission) {
        auto cancellation = queue->cancel_submission (submission.value ());
        worker.run_until (cancellation);
        passed = passed && cancellation.result ()
                 && cancellation.result ().value ()
                      == rt::serial_cancel_submission_outcome_t::queued_cancelled;
    }
    auto barrier = queue->reserve_deferred_barrier_next ("index-shutdown-barrier");
    fw::detail::deferred_barrier_t::async_completion_t settle;
    if (barrier) {
        const auto activated = barrier.value ()->activate_async ([&] (auto complete) {
            ++barrier_runs;
            settle = std::move (complete);
        });
        passed = passed && bool (activated);
    } else {
        passed = false;
    }
    queue->post ("index-recordless-control", [&] { ++controls; });
    auto last = record ();
    queue->post_async ("index-durable-after-cancel", work (last),
                       rt::serial_work_options_t{.application_record = last});
    worker.run_all ();
    passed = passed && last->admission.durable ();
    queue->close ();
    passed = passed && !queue->try_post ("index-closed-rejection", [&] { ++controls; });
    if (finish)
        finish ([] {});
    worker.run_all ();
    passed = passed && barrier_runs == 1 && handlers == 0 && controls == 0 && bool (settle);
    if (settle)
        settle (fw::result_t<void>::success ());
    worker.run_all ();
    passed = passed && handlers == 2 && cancelled == 1 && controls == 1
             && queue->pending_count () == 0 && barrier
             && barrier.value ()->terminal ().await_ready ();
    queue->drain ();

    auto cancelled_queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, policy);
    rt::serial_execution_queue_t::async_completion_t release;
    cancelled_queue->post_async ("index-cancel-blocker",
                                 [&] (auto complete) { release = std::move (complete); });
    worker.run_all ();
    auto pending = cancelled_queue->reserve_deferred_barrier_next ("index-cancelled-barrier");
    auto cancelled_record = record ();
    cancelled_record->admission.take_over ();
    cancelled_queue->post_async ("index-cancelled-record", work (cancelled_record),
                                 rt::serial_work_options_t{.application_record = cancelled_record});
    worker.run_all ();
    cancelled_queue->cancel_pending ();
    worker.run_all ();
    if (release)
        release ([] {});
    worker.run_all ();
    passed = passed && pending && pending.value ()->terminal ().await_ready ()
             && !pending.value ()->terminal ().result () && cancelled_queue->pending_count () == 0
             && handlers == 2;
    cancelled_queue->drain ();
    worker.drain ();
    std::cout << "fifo-index-shutdown shared=" << shared << " handlers=" << handlers
              << " cancelled=" << cancelled << " barrier_runs=" << barrier_runs
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_idle_mailbox_terminal (bool shared, mailbox_shutdown_t shutdown)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    auto gate = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{},
      shared ? rt::serial_lane_policy_t::spot_wide ()
             : rt::serial_lane_policy_t::per_actor_spot ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (gate);
    fw::task_completion_source_t<void> terminal;
    int cancelled = 0, next = 0;
    const auto retained = actor->retain_mailbox_until (terminal.task (), [&] {
        ++cancelled;
        terminal.complete (fw::result_t<void>::failure (fw::framework_error_kind_t::shutting_down,
                                                        "mailbox test cancelled"));
    });
    worker.run_all ();
    actor->post ("after-idle-mailbox-terminal", [&] { ++next; });
    worker.run_all ();
    const bool held = next == 0 && actor->pending_count () == 1;
    if (shutdown == mailbox_shutdown_t::close)
        actor->close ();
    else if (shutdown == mailbox_shutdown_t::cancel)
        actor->cancel_pending ();
    worker.run_all ();
    terminal.complete (fw::result_t<void>::success ());
    worker.run_all ();
    const auto pending = actor->pending_count ();
    // An idle fake turn has no lane message to decrement. drain() also waits
    // for _active==0, proving its sole release_mailbox -> complete_one release.
    actor->close ();
    gate->close ();
    actor->drain ();
    gate->drain ();
    worker.drain ();
    const bool passed = retained && held && pending == 0
                        && next == (shutdown == mailbox_shutdown_t::cancel ? 0 : 1)
                        && cancelled == (shutdown == mailbox_shutdown_t::cancel ? 1 : 0);
    std::cout << "idle-mailbox-terminal shared=" << shared
              << " shutdown=" << static_cast<int> (shutdown) << " held=" << held << " next=" << next
              << " cancelled=" << cancelled << " pending=" << pending
              << " drained_active_zero=true passed=" << passed << '\n';
    return passed;
}

// Execution gate §7 and relocation flow §4.2 include the final terminal of
// handlers that have yielded the Spot gate before the seal publication.
bool verify_seal_waits_for_yielded_handler_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    std::shared_ptr<detail::serial_turn_t> handler;
    bool finished = false;
    queue->post_async ("before-seal",
                       [&] (auto) { handler = detail::capture_current_serial_turn (); });
    worker.run_all ();
    auto early = queue->observe_handler_terminal ();
    worker.run_all ();
    auto early_terminal = early.value ()->run ();
    const bool waited_before_yield = !early_terminal.await_ready ();
    early.value ()->cancel ();
    worker.run_all ();
    const bool released = handler->release ();
    worker.run_all ();
    auto observed = queue->observe_handler_terminal ();
    worker.run_all ();
    auto terminal = observed.value ()->run ();
    const bool waited_after_yield = !terminal.await_ready ();
    // Keep observation pending while the accepted continuation runs.
    handler->resume_scheduler () ([&] {
        finished = true;
        handler->finish_handler ([] {}, true);
    });
    worker.run_all ();
    if (finished)
        worker.run_until (terminal);
    const bool waited_through_terminal = finished && terminal.await_ready ();
    observed.value ()->cancel ();
    worker.run_all ();
    handler.reset ();
    queue->close ();
    worker.run_all ();
    queue->drain ();
    worker.drain ();
    const bool passed =
      waited_before_yield && released && waited_after_yield && waited_through_terminal;
    std::cout << "seal-yield before=" << waited_before_yield << " released=" << released
              << " after=" << waited_after_yield << " terminal=" << waited_through_terminal
              << " finished=" << finished << " passed=" << passed << '\n';
    return passed;
}

// A resume record can finish its execution segment while its handler still
// awaits another result. Relocation must observe the handler's final result.
bool verify_intermediate_resume_keeps_handler_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    std::shared_ptr<detail::serial_turn_t> handler;
    serial_execution_queue_t::async_completion_t finish;
    bool continued = false, finished = false;
    queue->post_async ("intermediate-handler", [&] (auto complete) {
        handler = detail::capture_current_serial_turn ();
        finish = std::move (complete);
    });
    worker.run_all ();
    handler->release ();
    worker.run_all ();
    handler->resume_scheduler () ([&] { continued = true; });
    worker.run_all ();
    auto observed = queue->observe_handler_terminal ();
    worker.run_all ();
    auto terminal = observed.value ()->run ();
    const bool premature = terminal.await_ready ();
    finish ([&] { finished = true; });
    worker.run_all ();
    if (finished)
        worker.run_until (terminal);
    const bool passed = continued && !premature && finished && terminal.await_ready ();
    observed.value ()->cancel ();
    worker.run_all ();
    handler.reset ();
    finish = {};
    queue->close ();
    worker.run_all ();
    queue->drain ();
    worker.drain ();
    std::cout << "intermediate-terminal continued=" << continued << " premature=" << premature
              << " finished=" << finished << " passed=" << passed << '\n';
    return passed;
}
}
