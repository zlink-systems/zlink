/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/channels/call.hpp>

#include <gtest/gtest.h>

#include "runtime_context_hooks_environment.hpp"

#include <chrono>

namespace
{

using namespace zlink::framework;

TEST (ZLinkFrameworkSharedSerialTurn, ActorJoinRegistersAcrossLibraryBoundary)
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    int activated = 0;
    queue.run ("deferred-actor-join-handler", [&] {
        EXPECT_NO_THROW (
          actor_join_call_t ([&] (std::chrono::milliseconds) { ++activated; }).defer ());
        EXPECT_EQ (0, activated);
    });
    EXPECT_EQ (1, activated);
}

TEST (ZLinkFrameworkSharedSerialTurn, ApplicationJobContextReachesRuntimeLibrary)
{
    int job = 0;
    const auto previous = detail::application_job_context_t::exchange (&job);
    EXPECT_THROW (detail::ensure_blocking_submit_allowed (), framework_exception_t);
    EXPECT_EQ (&job, detail::application_job_context_t::current ());
    detail::application_job_context_t::exchange (previous);
}

TEST (ZLinkFrameworkSharedSerialTurn, SerialResumeFailureUsesRuntimeLibrary)
{
    detail::set_serial_resume_failure (framework_error_kind_t::shutting_down, "stopping");
    const auto failure = detail::take_serial_resume_failure ();
    ASSERT_TRUE (failure.has_value ());
    EXPECT_EQ (framework_error_kind_t::shutting_down, failure->kind);
    EXPECT_EQ ("stopping", failure->message);
    EXPECT_FALSE (detail::take_serial_resume_failure ().has_value ());
}

TEST (ZLinkFrameworkSharedSerialTurn, AmbientFlowHooksReachExecutable)
{
    EXPECT_NE (nullptr, detail::current_ambient_context_hooks ());
}

} // namespace

#ifdef __linux__
#include <fstream>
#include <future>
#include <sys/syscall.h>
#include <unistd.h>

TEST (ZLinkFrameworkSharedSerialTurn, PublicationRollbackWakesDrain)
{
    EXPECT_EXIT (
      {
          using namespace zlink::framework::runtime;
          struct rejecting_executor_t : offload_executor_t
          {
              std::promise<void> entered;
              std::promise<void> reject;
              rejecting_executor_t () : offload_executor_t (1) {}
              bool try_submit_internal (std::function<void ()>) override
              {
                  entered.set_value ();
                  reject.get_future ().wait ();
                  return false;
              }
          } executor;
          serial_execution_queue_t queue (executor, {}, {}, serial_lane_policy_t::entry_spot ());
          auto producer =
            std::async (std::launch::async, [&] { return queue.try_post ("rollback", [] {}); });
          executor.entered.get_future ().wait ();
          std::promise<long> tid;
          auto drainer = std::async (std::launch::async, [&] {
              tid.set_value (syscall (SYS_gettid));
              queue.drain ();
          });
          const auto waiter_tid = tid.get_future ().get ();
          bool parked = false;
          const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
          while (std::chrono::steady_clock::now () < deadline) {
              std::ifstream file ("/proc/self/task/" + std::to_string (waiter_tid) + "/wchan");
              std::string name;
              file >> name;
              if (name == "futex_wait_queue" || name == "futex_wait" || name == "futex_do_wait") {
                  parked = true;
                  break;
              }
              std::this_thread::yield ();
          }
          executor.reject.set_value ();
          const bool accepted = producer.get ();
          const bool returned =
            drainer.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
          std::cerr << "parked=" << parked << " accepted=" << accepted
                    << " pending=" << queue.pending_count () << " returned=" << returned << '\n';
          std::_Exit (parked && !accepted && queue.pending_count () == 0 && returned ? 0 : 1);
      },
      ::testing::ExitedWithCode (0), "parked=1 accepted=0 pending=0 returned=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, AcceptedPublicationWakesDrain)
{
    EXPECT_EXIT (
      {
          using namespace zlink::framework::runtime;
          offload_executor_t executor (1);
          serial_execution_queue_t queue (executor, {}, {}, serial_lane_policy_t::entry_spot ());
          std::promise<void> entered;
          std::promise<void> release;
          queue.post ("accepted", [&] {
              entered.set_value ();
              release.get_future ().wait ();
          });
          entered.get_future ().wait ();
          auto drainer = std::async (std::launch::async, [&] { queue.drain (); });
          release.set_value ();
          const bool returned =
            drainer.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
          std::cerr << "accepted pending=" << queue.pending_count () << " returned=" << returned
                    << '\n';
          std::_Exit (queue.pending_count () == 0 && returned ? 0 : 1);
      },
      ::testing::ExitedWithCode (0), "accepted pending=0 returned=1");
}
#endif

#include "runtime/spots/spot_runtime.hpp"

namespace
{
void verify_creation_shutdown (bool published, bool pending)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("pending-shutdown");
    auto worker = std::make_shared<offload_executor_t> (1);
    auto context = std::make_shared<detail::spot_context_state_t> ();
    node->worker_executor = worker;
    context->serial_executor = worker;
    context->serial_queue = std::make_shared<serial_execution_queue_t> (
      *worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    auto queue = context->serial_queue;
    auto reserved = queue->reserve_barrier_next ("spot-preparation");
    if (!reserved)
        std::_Exit (2);
    auto barrier = reserved.value ();
    auto phase = barrier->run ([] { return task_t<void> (result_t<void>::success ()); });
    phase.result ().value ();
    std::promise<void> observed;
    queue->notify_control ("observe-suspension", [&] { observed.set_value (); });
    observed.get_future ().wait ();
    if (published)
        node->spot_contexts_by_id.emplace ("creation",
                                           detail::spot_context_access_t::create (context));
    if (pending) {
        detail::spot_node_builder_state_t::pending_spot_creation_t creation;
        creation.spot_name = "pending";
        creation.reservation = 1;
        creation.context = context;
        node->pending_spot_creations_by_id.emplace ("creation", std::move (creation));
    }
    int visits = 0;
    detail::visit_spot_node_dispatch_queues (*node, [&] (auto &visited) {
        if (&visited == queue.get ())
            ++visits;
    });
    detail::drain_spot_node_executors (*node);
    barrier->cancel ();
    std::cerr << "published=" << published
              << " after_node_drain pending=" << queue->pending_count ()
              << " terminal=" << barrier->terminal ().await_ready () << " visits=" << visits
              << std::endl;
    std::_Exit (
      queue->pending_count () == 0 && barrier->terminal ().await_ready () && visits == 1 ? 0 : 1);
}
}

TEST (ZLinkFrameworkSharedSerialTurn, PublishedPreparationDrainsBeforeExecutorStops)
{
    EXPECT_EXIT (verify_creation_shutdown (true, false), ::testing::ExitedWithCode (0),
                 "published=1 after_node_drain pending=0 terminal=1 visits=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, PendingPreparationDrainsBeforeExecutorStops)
{
    EXPECT_EXIT (verify_creation_shutdown (false, true), ::testing::ExitedWithCode (0),
                 "published=0 after_node_drain pending=0 terminal=1 visits=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, CreationInBothTablesDrainsOnce)
{
    EXPECT_EXIT (verify_creation_shutdown (true, true), ::testing::ExitedWithCode (0),
                 "published=1 after_node_drain pending=0 terminal=1 visits=1");
}

namespace
{
void verify_pending_creation_terminal_precedes_worker_stop ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("pending-terminal");
    auto worker = std::make_shared<offload_executor_t> (1);
    auto context = std::make_shared<detail::spot_context_state_t> ();
    node->worker_executor = worker;
    context->serial_executor = worker;
    context->serial_queue = std::make_shared<serial_execution_queue_t> (
      *worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    auto reserved = context->serial_queue->reserve_barrier_next ("spot-preparation");
    if (!reserved)
        std::_Exit (2);
    auto barrier = reserved.value ();
    barrier->run ([] { return task_t<void> (result_t<void>::success ()); }).result ().value ();
    std::promise<void> creation;
    detail::spot_node_builder_state_t::pending_spot_creation_t pending;
    pending.future = creation.get_future ().share ();
    pending.context = context;
    node->pending_spot_creations_by_id.emplace ("creation", std::move (pending));
    auto stopped =
      std::async (std::launch::async, [&] { detail::drain_spot_node_executors (*node); });
    auto terminal = barrier->terminal ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!terminal.await_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool kept_worker = terminal.await_ready () && worker->try_submit_internal ([] {});
    creation.set_value ();
    const bool drained = stopped.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    const bool stopped_worker = drained && !worker->try_submit_internal ([] {});
    std::cerr << "creation_terminal_order kept_worker=" << kept_worker << " drained=" << drained
              << " stopped_worker=" << stopped_worker << std::endl;
    std::_Exit (kept_worker && drained && stopped_worker ? 0 : 1);
}
}

TEST (ZLinkFrameworkSharedSerialTurn, PendingCreationTerminalPrecedesWorkerStop)
{
    EXPECT_EXIT (verify_pending_creation_terminal_precedes_worker_stop (),
                 ::testing::ExitedWithCode (0),
                 "creation_terminal_order kept_worker=1 drained=1 stopped_worker=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, StoppingNodeRejectsCreationBeforeAllocatingWorkers)
{
    EXPECT_EXIT (
      {
          using namespace zlink::framework;
          auto node = std::make_shared<detail::spot_node_builder_state_t> ("stopped-creation");
          node->stopping.store (true);
          node->spot_factories.emplace ("pending", std::type_index (typeid (int)));
          detail::spot_node_runtime_t runtime (node);
          bool rejected = false;
          try {
              (void) runtime.get_or_create_spot ("pending", spot_id_t ("creation"));
          }
          catch (const framework_exception_t &error) {
              rejected = error.kind () == framework_error_kind_t::shutting_down;
          }
          std::cerr << "stopping_creation rejected=" << rejected
                    << " worker_allocated=" << bool (node->worker_executor) << std::endl;
          std::_Exit (rejected && !node->worker_executor ? 0 : 1);
      },
      ::testing::ExitedWithCode (0), "stopping_creation rejected=1 worker_allocated=0");
}

namespace
{
void verify_cleanup_shutdown_worker_lifetime ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("cleanup-shutdown");
    auto worker = std::make_shared<offload_executor_t> (1);
    node->worker_executor = worker;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = spot_id_t ("creation");
    context->spot_name = "pending";
    context->serial_executor = worker;
    auto queue = std::make_shared<serial_execution_queue_t> (
      *worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    context->serial_queue = queue;
    context->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
      worker, node->lane, serial_lane_policy_t::spot_wide (), queue);
    auto preparation = context->spot_serial_executor->begin_preparation ();
    preparation->lifecycle->run ([] { return task_t<void> (result_t<void>::success ()); })
      .result ()
      .value ();
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto entering = entered.get_future ();
    context->spot_instance = std::shared_ptr<void> (new int (1), [&] (void *p) {
        entered.set_value ();
        released.wait ();
        delete static_cast<int *> (p);
    });
    node->spot_contexts_by_id.emplace ("creation", detail::spot_context_access_t::create (context));
    auto cleanup = context->complete_preparation (false);
    if (entering.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        std::_Exit (2);
    auto stopped =
      std::async (std::launch::async, [&] { detail::drain_spot_node_executors (*node); });
    const bool drained_early =
      stopped.wait_for (std::chrono::milliseconds (100)) == std::future_status::ready;
    const bool worker_kept = worker->try_submit_internal ([] {});
    release.set_value ();
    const bool drained = stopped.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    std::cerr << "cleanup_shutdown drained_early=" << drained_early
              << " worker_kept=" << worker_kept << " drained=" << drained
              << " pending=" << queue->pending_count ()
              << " terminal=" << preparation->lifecycle->terminal ().await_ready () << std::endl;
    std::_Exit (!drained_early && worker_kept && drained && queue->pending_count () == 0
                    && preparation->lifecycle->terminal ().await_ready ()
                  ? 0
                  : 1);
}

}

TEST (ZLinkFrameworkSharedSerialTurn, CleanupRemainsOwnedUntilPreparationTerminal)
{
    EXPECT_EXIT (verify_cleanup_shutdown_worker_lifetime (), ::testing::ExitedWithCode (0),
                 "cleanup_shutdown drained_early=0 worker_kept=1 drained=1 pending=0 terminal=1");
}

namespace
{
void verify_discard_terminal_context_release (bool prepared)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("discard-terminal");
    auto worker = std::make_shared<offload_executor_t> (1);
    node->worker_executor = worker;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = spot_id_t ("creation");
    context->spot_name = "pending";
    context->serial_executor = worker;
    context->serial_queue = std::make_shared<serial_execution_queue_t> (*worker);
    if (prepared) {
        context->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
          worker, node->lane, serial_lane_policy_t::spot_wide (), context->serial_queue);
        auto preparation = context->spot_serial_executor->begin_preparation ();
        preparation->lifecycle->run ([] { return task_t<void> (result_t<void>::success ()); })
          .result ()
          .value ();
        context->complete_preparation (true).result ().value ();
    }
    node->spot_contexts_by_id.emplace ("creation", detail::spot_context_access_t::create (context));
    context->discard_preparation ({}).result ().value ();
    const bool removed = node->lane.run ([&] { return node->spot_contexts_by_id.empty (); }).get ();
    detail::drain_spot_node_executors (*node);
    std::cerr << "discard_terminal prepared=" << prepared << " removed=" << removed << std::endl;
    std::_Exit (removed ? 0 : 1);
}
}

TEST (ZLinkFrameworkSharedSerialTurn, DiscardAfterPreparationTerminalReleasesContext)
{
    EXPECT_EXIT (verify_discard_terminal_context_release (true), ::testing::ExitedWithCode (0),
                 "discard_terminal prepared=1 removed=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, DiscardWithoutPreparationReleasesContext)
{
    EXPECT_EXIT (verify_discard_terminal_context_release (false), ::testing::ExitedWithCode (0),
                 "discard_terminal prepared=0 removed=1");
}

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "../support/telemetry_observation.hpp"

namespace
{
void verify_accepted_activation_shutdown ()
{
#ifdef __linux__
    alarm (5);
#endif
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("activation-shutdown");
    auto worker = std::make_shared<offload_executor_t> (1);
    node->worker_executor = worker;
    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->node = node;
    state->lane_owner = node;
    state->spot_id = spot_id_t ("instance");
    state->spot_name = "pending";
    state->spot_instance = std::make_shared<int> (1);
    state->serial_executor = worker;
    state->serial_queue = std::make_shared<serial_execution_queue_t> (*worker);
    state->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
      worker, node->lane, serial_lane_policy_t::spot_wide (), state->serial_queue);
    node->snapshot.instance_spot_names.push_back ("pending");
    node->spot_contexts_by_id.emplace ("instance", detail::spot_context_access_t::create (state));
    std::promise<void> worker_entered, release_worker;
    auto worker_released = release_worker.get_future ().share ();
    state->serial_queue->post ("hold-handler", [&] {
        worker_entered.set_value ();
        worker_released.wait ();
    });
    worker_entered.get_future ().wait ();
    std::atomic_bool handler_invoked{false};
    std::atomic_int handler_terminals{0};
    std::atomic_bool handler_succeeded{false};
    std::future<void> stopped;
    const auto coordinator = state->spot_serial_executor;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    bool retained = false;
    node->dispatch.message_flow (message_flow_log_mode_t::normal);
    auto observations = std::make_shared<
      test::telemetry_observations_t<message_flow_event_t>> ();
    detail::dispatch_options_access_t::set_observer_for_tests (
      node->dispatch, [observations] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::received)
              observations->record (event);
      });
    service_provider_t services;
    serializer_registry_t serializers;
    std::function<void ()> accepted_terminal;
    detail::spot_node_runtime_t runtime (node);
    const auto result =
      runtime
        .dispatch_instance_activation (spot_id_t ("instance"), "probe", "application/json", {}, {},
                                       false, "diagnostic", services, serializers, [] {}, {}, {},
                                       &accepted_terminal, {})
        .result ();
    // No handler is registered; its typed rejection must still release admission.
    const bool rejected = !result && result.error_kind () == framework_error_kind_t::not_found;
    if (!accepted_terminal)
        std::_Exit (2);
    test::acknowledge_delivery (deadline);
    if (observations->snapshot ().size () != 1)
        std::_Exit (5);
    // The accepted activation remains owned until its explicit terminal below.
    if (!state->admit (true))
        std::_Exit (3);
    serial_work_options_t options;
    options.completion = [state, &handler_terminals,
                          &handler_succeeded] (result_t<void> terminal) {
        if (!terminal) {
            ++handler_terminals;
            handler_succeeded = false;
            state->leave_callback ();
        }
    };
    if (!state->try_post_serial_async (
          "accepted-handler",
          [&] (auto resume) {
              handler_invoked = true;
              resume ([state, &handler_terminals, &handler_succeeded] {
                  ++handler_terminals;
                  handler_succeeded = true;
                  state->leave_callback ();
              });
          },
          std::move (options)))
        std::_Exit (4);
    stopped = std::async (std::launch::async,
                          [&] { detail::drain_spot_node_executors (*node, deadline); });
    const bool drained_early =
      stopped.wait_for (std::chrono::milliseconds (100)) == std::future_status::ready;
    retained =
      !drained_early && worker->try_submit_internal ([] {}) && handler_terminals == 0;
    release_worker.set_value ();
    std::cerr << "accepted_activation drained_early=" << drained_early
              << " retained=" << retained << std::endl;
    accepted_terminal ();
    const bool drained = stopped.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    const bool released =
      drained
      && node->lane
           .run ([&] {
               return state->spot_serial_executor == coordinator && state->callback_depth == 0;
           })
           .get ();
    const bool sealed = !state->admit (true);
    const bool stopped_worker = drained && !worker->try_submit_internal ([] {});
    std::cerr << "activation_terminal rejected=" << rejected << " drained=" << drained
              << " released=" << released << " stopped_worker=" << stopped_worker << std::endl;
    std::_Exit (retained && rejected && drained && released && sealed && stopped_worker
                    && handler_invoked && handler_succeeded && handler_terminals == 1
                    && std::chrono::steady_clock::now () < deadline
                  ? 0
                  : 1);
}
}

TEST (ZLinkFrameworkSharedSerialTurn,
      AcceptedActivationDrainsBeforeHostDeadlineAndRetainsCoordinator)
{
    EXPECT_EXIT (verify_accepted_activation_shutdown (), ::testing::ExitedWithCode (0),
                 "activation_terminal rejected=1 drained=1 released=1 stopped_worker=1");
}

TEST (ZLinkFrameworkSharedSerialTurn, RemovedAsyncItemsCompleteExactlyOnce)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    for (const auto lane : {serial_work_lane_t::application, serial_work_lane_t::lifecycle})
        for (unsigned owners = 0; owners != 4; ++owners) {
            offload_executor_t worker (1);
            auto queue = std::make_shared<serial_execution_queue_t> (worker);
            std::promise<void> entered, release;
            auto released = release.get_future ().share ();
            ASSERT_TRUE (queue->try_post_async ("blocker", [&] (auto resume) {
                entered.set_value ();
                released.wait ();
                resume ([] {});
            }));
            entered.get_future ().wait ();
            std::atomic_int completions{0}, retirements{0}, cancellations{0};
            std::atomic_bool invoked{false};
            std::atomic_bool shutting_down{false};
            serial_work_options_t options;
            options.lane = lane;
            if (owners & 1) {
                options.application_record = std::make_shared<serial_application_record_t> ();
                options.application_record->retire_invocation = [&] (result_t<void>) {
                    ++retirements;
                };
            }
            options.completion = [&] (result_t<void> terminal) {
                ++completions;
                shutting_down =
                  !terminal && terminal.error_kind () == framework_error_kind_t::shutting_down;
            };
            serial_execution_queue_t::async_work_t handler = [&] (auto resume) {
                invoked = true;
                resume ([] {});
            };
            const bool accepted = owners & 2
                                    ? static_cast<bool> (queue->try_post_cancellable_async (
                                        "removed-handler", std::move (handler),
                                        [&] { ++cancellations; }, std::move (options)))
                                    : queue->try_post_async ("removed-handler", std::move (handler),
                                                             std::move (options));
            queue->cancel_pending ();
            queue->cancel_pending ();
            release.set_value ();
            queue->drain ();
            EXPECT_TRUE (accepted);
            EXPECT_FALSE (invoked);
            EXPECT_TRUE (shutting_down);
            EXPECT_EQ (1, completions);
            EXPECT_EQ (owners & 2 ? 1 : 0, cancellations);
            EXPECT_EQ ((owners & 1) && !(owners & 2) ? 1 : 0, retirements);
        }
}

TEST (ZLinkFrameworkSharedSerialTurn, HostDeadlineCancelsAcceptedQueuedHandler)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("deadline-shutdown");
    auto worker = std::make_shared<offload_executor_t> (1);
    node->worker_executor = worker;
    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->node = node;
    state->lane_owner = node;
    state->serial_executor = worker;
    state->serial_queue = std::make_shared<serial_execution_queue_t> (*worker);
    state->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
      worker, node->lane, serial_lane_policy_t::spot_wide (), state->serial_queue);
    node->spot_contexts_by_id.emplace ("instance", detail::spot_context_access_t::create (state));
    std::promise<void> entered, release, terminal;
    auto released = release.get_future ().share ();
    state->serial_queue->post ("hold-worker", [&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    EXPECT_TRUE (state->admit (true));
    std::atomic_int completions{0};
    std::atomic_bool cancelled{false};
    std::atomic_bool invoked{false};
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (100);
    serial_work_options_t options;
    options.completion = [state, &completions, &cancelled, &terminal,
                          deadline] (result_t<void> result) {
        cancelled = !result && result.error_kind () == framework_error_kind_t::shutting_down
                    && std::chrono::steady_clock::now () >= deadline;
        ++completions;
        state->leave_callback ();
        terminal.set_value ();
    };
    EXPECT_TRUE (state->try_post_serial_async (
      "accepted-handler",
      [&] (auto resume) {
          invoked = true;
          resume ([] {});
      },
      std::move (options)));
    auto stopping =
      std::async (std::launch::async, [&] { detail::drain_spot_node_executors (*node, deadline); });
    const bool settled =
      terminal.get_future ().wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release.set_value ();
    EXPECT_TRUE (settled);
    EXPECT_EQ (std::future_status::ready, stopping.wait_for (std::chrono::seconds (2)));
    stopping.get ();
    EXPECT_FALSE (invoked);
    EXPECT_TRUE (cancelled);
    EXPECT_EQ (1, completions);
    EXPECT_NE (nullptr, state->spot_serial_executor);
    node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
}

TEST (ZLinkFrameworkSharedSerialTurn, StartingWorkDiscardsPendingCompletion)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    offload_executor_t worker (1);
    auto queue = std::make_shared<serial_execution_queue_t> (worker);
    auto obligation = std::make_shared<int> (0);
    std::weak_ptr<int> observed = obligation;
    serial_execution_queue_t::async_completion_t retained_resume;
    std::atomic_int completions{0};
    serial_work_options_t options;
    options.completion = [obligation = std::move (obligation),
                          &completions] (result_t<void> terminal) {
        EXPECT_FALSE (terminal);
        ++completions;
    };
    EXPECT_TRUE (queue->try_post_async (
      "completed-handler",
      [&] (auto resume) {
          retained_resume = resume;
          resume ([] {});
      },
      std::move (options)));
    queue->drain ();
    EXPECT_EQ (0, completions);
    EXPECT_TRUE (observed.expired ());
    retained_resume ([] {});
    EXPECT_EQ (0, completions);
}

TEST (ZLinkFrameworkSharedSerialTurn, ActiveFailureRemainsOwnedByHandlerTerminal)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    offload_executor_t worker (1);
    std::atomic_int errors{0}, cancellations{0}, completions{0};
    auto queue = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{},
      [&] (const std::string &, const std::exception_ptr &) { ++errors; });
    serial_work_options_t options;
    options.completion = [&] (result_t<void> terminal) {
        EXPECT_FALSE (terminal);
        ++completions;
    };
    EXPECT_TRUE (queue->try_post_cancellable_async (
      "failed-active-handler", [] (auto) { throw std::runtime_error ("handler failed"); },
      [&] { ++cancellations; }, std::move (options)));
    queue->drain ();
    EXPECT_EQ (1, errors);
    EXPECT_EQ (0, completions);
    EXPECT_EQ (0, cancellations);
}
