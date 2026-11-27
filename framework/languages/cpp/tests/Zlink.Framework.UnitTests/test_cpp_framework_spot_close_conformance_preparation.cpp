/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/committed_authority_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../support/spot_close_preparation_fixture.hpp"

namespace
{
using namespace zlink::framework::test::spot_close;

TEST (LocalCallbackCause, UserSpotOnCreatePreservesEmptyApplicationException)
{
    namespace fw = zlink::framework;
    using namespace fw::detail;
    const auto marker = std::make_shared<const int> (369305);
    const auto original = std::make_exception_ptr (empty_callback_failure_t (marker));
    auto node = fw::test::runtime_failure_fixture<spot_node_builder_state_t> ("callback-cause");
    fw::serializer_registry_t serializers;
    node->channel_runtime = fw::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->spot_factories.emplace ("failing", std::type_index (typeid (int)));
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [] (fw::spot_context_t, fw::service_provider_t &) {
        return std::make_shared<int> (1);
    };
    lifecycle.on_create =
      [original] (void *, const zlink::message_t &,
                  fw::serializer_registry_t &) -> fw::task_t<fw::spot_create_response_t> {
        std::rethrow_exception (original);
        co_return fw::spot_create_response_t::accept ();
    };
    node->spot_lifecycles.emplace ("failing", std::move (lifecycle));
    spot_node_runtime_t owner (node);
    try {
        (void) owner.get_or_create_spot ("failing", fw::spot_id_t ("callback-cause"), {});
        FAIL () << "onCreate must fail";
    }
    catch (const empty_callback_failure_t &error) {
        EXPECT_EQ (marker, error.marker);
        EXPECT_STREQ ("", error.what ());
    }
}

TEST (ActorEntryPlacementRegression, ReadyContinuationAwaitsBusySpotInventory)
{
    namespace fw = zlink::framework;
    auto node = fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (
      "inventory-continuation");
    fw::detail::spot_node_runtime_t runtime (node);
    auto release_workers =
      fw::test::reserve_single_infrastructure_worker (*fw::runtime::infrastructure_executor ());
    std::promise<void> ready, occupied, release, yielded;
    auto ready_seen = ready.get_future ();
    auto occupied_seen = occupied.get_future ().share ();
    auto released = release.get_future ().share ();
    auto suspended = yielded.get_future ();
    std::optional<fw::task_t<std::size_t>> query;
    auto count_after_ready = [&] () -> fw::task_t<std::size_t> {
        (void) co_await runtime.relocation_ready_async ();
        ready.set_value ();
        occupied_seen.wait ();
        ::zlink::framework::runtime::infrastructure_executor ()->submit (
          [&] { yielded.set_value (); });
        co_return co_await runtime.active_user_spot_count_async ();
    };
    ::zlink::framework::runtime::infrastructure_executor ()->submit (
      [&] { query.emplace (count_after_ready ()); });
    ready_seen.get ();
    std::jthread holder ([&] {
        node->lane
          .run ([&] {
              occupied.set_value ();
              released.wait ();
          })
          .get ();
    });
    // The executor marker follows the inventory continuation after it yields.
    // A blocking query aborts before that marker can run.
    suspended.get ();
    release.set_value ();
    holder.join ();
    ASSERT_TRUE (query.has_value ());
    ASSERT_EQ (query->result ().value (), 0u);
}

void verify_timer_waits_for_creation (zlink::framework::user_spot_execution_mode_t mode,
                                      bool reject = false,
                                      bool expire_from_factory = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "timer-preparation-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1);
    serializer_registry_t serializers;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->snapshot.spot_execution_modes.emplace ("preparing", mode);
    node->spot_factories.emplace ("preparing", std::type_index (typeid (int)));
    std::shared_ptr<spot_context_state_t> context;
    std::promise<void> prepared;
    auto preparation_started = prepared.get_future ();
    task_completion_source_t<spot_create_response_t> release_preparation;
    const auto callbacks = std::make_shared<std::atomic_int> (0);
    auto timer = std::make_shared<timer_state_t> ();
    timer->name = "creation-expiry";
    timer->period = std::chrono::hours (1);
    timer->handler_invoker = [callbacks] (void *, void *, serializer_registry_t &,
                                          const timer_tick_t &) {
        ++*callbacks;
        return task_t<zlink::message_t> (result_t<zlink::message_t>::success (zlink::message_t{}));
    };
    const auto expire = [&] {
        if (mode == user_spot_execution_mode_t::per_actor)
            timer->serial_queue = context->spot_serial_executor->timer_queue (timer->name);
        node->lane.run ([&] { context->timers.push_back (timer); }).get ();
        timer_runtime_t::post_fire_count (context, timer, timer->serial_queue, 1);
    };
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [&] (spot_context_t value,
                                                  service_provider_t &) -> std::shared_ptr<void> {
        context = spot_context_access_t::state (value);
        auto instance = std::make_shared<int> (1);
        timer->handler_instance = instance;
        // The public factory adapter also calls Configure before it returns the
        // instance to create_spot_context, so spot_instance is still unset here.
        if (expire_from_factory)
            expire ();
        return instance;
    };
    lifecycle.on_create = [&] (void *, const zlink::message_t &, serializer_registry_t &) {
        prepared.set_value ();
        return release_preparation.task ();
    };
    node->spot_lifecycles.emplace ("preparing", std::move (lifecycle));
    spot_node_runtime_t owner (node);
    auto creating = std::async (std::launch::async, [&] {
        return owner.get_or_create_spot ("preparing", spot_id_t ("preparing-spot"), {});
    });
    const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
        release_preparation.complete (
          result_t<spot_create_response_t>::success (spot_create_response_t::accept ()));
    });
    preparation_started.get ();
    if (!expire_from_factory)
        expire ();
    observe_scheduled_expiry (node->worker_executor);
    const auto callbacks_before_ready = callbacks->load ();
    std::optional<task_t<bool>> early_close;
    if (reject)
        early_close.emplace (spot_context_access_t::create (context).close ());
    // Release and join before assertions, including the expected red outcome.
    release_preparation.complete (result_t<spot_create_response_t>::success (
      reject ? spot_create_response_t::reject () : spot_create_response_t::accept ()));
    auto created = creating.get ();
    context->serial_queue->drain ();
    if (timer->serial_queue)
        timer->serial_queue->drain ();
    const auto callbacks_after_creation = callbacks->load ();
    context->spot_serial_executor->close ();
    context->serial_queue->close ();
    node->worker_executor->drain ();
    node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    EXPECT_EQ (created.state,
               reject ? spot_create_state_t::rejected : spot_create_state_t::created);
    EXPECT_EQ (callbacks_before_ready, 0);
    EXPECT_EQ (callbacks_after_creation, reject ? 0 : 1);
    if (early_close) {
        const auto closed = early_close->result ();
        ASSERT_TRUE (closed);
        EXPECT_FALSE (closed.value ());
    }
}

void verify_cancelled_creation_callback_lifetime (
  const std::shared_ptr<zlink::framework::test::manual_internal_scheduler_t> &late_completion = {})
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "cancelled-creation-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1);
    serializer_registry_t serializers;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->spot_factories.emplace ("preparing", std::type_index (typeid (int)));
    std::shared_ptr<spot_context_state_t> context;
    std::weak_ptr<void> instance;
    auto instance_released = std::make_shared<task_completion_source_t<void>> ();
    auto released = instance_released->task ();
    std::promise<void> entered;
    auto started = entered.get_future ();
    task_completion_source_t<spot_create_response_t> release;
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [&] (spot_context_t value,
                                                  service_provider_t &) -> std::shared_ptr<void> {
        context = spot_context_access_t::state (value);
        auto created = std::shared_ptr<int> (new int (1), [instance_released] (int *value) {
            delete value;
            instance_released->complete (result_t<void>::success ());
        });
        instance = created;
        return created;
    };
    lifecycle.on_create = [&] (void *, const zlink::message_t &, serializer_registry_t &) {
        entered.set_value ();
        if (late_completion)
            return detail::with_task_resume_scheduler (
              release.task (), [late_completion] (std::function<void ()> work) {
                  if (!late_completion->try_submit_internal (std::move (work)))
                      throw std::runtime_error ("late creation completion scheduler stopped");
              });
        return release.task ();
    };
    node->spot_lifecycles.emplace ("preparing", std::move (lifecycle));
    spot_node_runtime_t owner (node);
    auto creating = std::async (std::launch::async, [&] {
        try {
            (void) owner.get_or_create_spot ("preparing", spot_id_t ("cancelled-creation"), {});
            return false;
        }
        catch (const framework_exception_t &error) {
            return error.kind () == framework_error_kind_t::shutting_down;
        }
    });
    started.get ();
    if (late_completion)
        observe_scheduled_expiry (node->worker_executor);
    const auto barrier = context->spot_serial_executor->preparation_lifecycle ();
    auto terminal = barrier->terminal ();
    std::atomic_int terminal_count{0};
    detail::observe_task_terminal (terminal, [&] (const result_t<void> &) { ++terminal_count; });
    barrier->cancel ();
    // Cancellation settles the caller first; the callback then completes into a returned frame.
    const bool cancelled = creating.get ();
    const bool callback_retained = !instance.expired ();
    release.complete (
      result_t<spot_create_response_t>::success (spot_create_response_t::accept ()));
    node->worker_executor->drain ();
    context->spot_serial_executor->close ();
    context->serial_queue->close ();
    node->worker_executor->drain ();
    node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    context.reset ();
    // The cancelled barrier terminal and worker drain do not join the callback frame.
    // Observe the instance's release before checking its weak reference.
    if (late_completion)
        late_completion->run_until (released);
    released.result ().value ();
    EXPECT_TRUE (cancelled);
    EXPECT_TRUE (callback_retained);
    EXPECT_EQ (terminal_count.load (), 1);
    EXPECT_TRUE (instance.expired ());
}

TEST (ZLinkFrameworkSpotCloseConformance, CancelledCreationRetainsCallbackUntilLateCompletion)
{
    verify_cancelled_creation_callback_lifetime ();
}

TEST (ZLinkFrameworkSpotCloseConformance, CancelledCreationAwaitsDeferredCallbackRelease)
{
    verify_cancelled_creation_callback_lifetime (
      std::make_shared<zlink::framework::test::manual_internal_scheduler_t> ());
}

TEST (ZLinkFrameworkSpotCloseConformance, SpotWideTimerWaitsForCreationPreparation)
{
    verify_timer_waits_for_creation (zlink::framework::user_spot_execution_mode_t::spot_wide);
}

TEST (ZLinkFrameworkSpotCloseConformance, PerActorTimerWaitsForCreationPreparation)
{
    verify_timer_waits_for_creation (zlink::framework::user_spot_execution_mode_t::per_actor);
}

TEST (ZLinkFrameworkSpotCloseConformance, PreparationOwnsInvokedCoroutineCallbackUntilTerminal)
{
    using namespace zlink::framework;
    struct observation_t
    {
        std::atomic<const void *> invoked{nullptr};
        std::atomic_bool alive{false};
        std::promise<void> entered;
        task_completion_source_t<void> release;
    };
    struct work_t
    {
        std::shared_ptr<observation_t> observation;
        ~work_t ()
        {
            if (observation->invoked.load () == this)
                observation->alive.store (false);
        }
        task_t<void> operator() () const
        {
            const auto state = observation;
            state->invoked.store (this);
            state->alive.store (true);
            state->entered.set_value ();
            co_await state->release.task ();
        }
    };
    auto worker = std::make_shared<runtime::offload_executor_t> (1);
    auto queue = std::make_shared<runtime::serial_execution_queue_t> (*worker);
    auto reserved = queue->reserve_barrier_next ("preparation-callback-lifetime");
    ASSERT_TRUE (reserved);
    const auto barrier = reserved.value ();
    auto state = std::make_shared<observation_t> ();
    auto entered = state->entered.get_future ();
    const auto cleanup = std::shared_ptr<void> (nullptr, [&] (void *) {
        state->release.complete (result_t<void>::success ());
        (void) barrier->activate ([] {});
        queue->drain ();
        queue->close ();
        worker->drain ();
    });
    auto pending = barrier->run (work_t{state});
    entered.get ();
    observe_scheduled_expiry (worker);
    EXPECT_TRUE (state->alive.load ());
    EXPECT_FALSE (pending.await_ready ());
    state->release.complete (result_t<void>::success ());
    EXPECT_TRUE (pending.result ());
    observe_scheduled_expiry (worker);
    EXPECT_FALSE (state->alive.load ());
}

TEST (ZLinkFrameworkSpotCloseConformance, BorrowedPreparationReleasesPerActorChildQueues)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "borrowed-preparation-node");
    auto worker = std::make_shared<runtime::offload_executor_t> (1);
    auto previous = std::make_shared<spot_serial_executor_t> (
      worker, node->lane, runtime::serial_lane_policy_t::per_actor_spot ());
    auto replacement = std::make_shared<spot_serial_executor_t> (
      worker, node->lane, runtime::serial_lane_policy_t::per_actor_spot (),
      previous->spot_queue ());
    auto operation = previous->begin_preparation ();
    std::shared_ptr<runtime::serial_execution_queue_t> timer_queue;
    std::atomic_int callbacks{0};
    const auto cleanup = std::shared_ptr<void> (nullptr, [&] (void *) {
        (void) previous->finish_preparation ().result ();
        if (timer_queue)
            timer_queue->drain ();
        replacement->close ();
        previous->close ();
        worker->drain ();
    });
    const auto attached = operation->lifecycle
                            ->run ([&] {
                                replacement->attach_preparation (operation);
                                timer_queue = replacement->timer_queue ("replacement-timer");
                                timer_queue->post ("replacement-expiry", [&] { ++callbacks; });
                                return task_t<void> (result_t<void>::success ());
                            })
                            .result ();
    ASSERT_TRUE (attached);
    observe_scheduled_expiry (worker);
    EXPECT_EQ (callbacks.load (), 0);
    const auto terminal = previous->finish_preparation ().result ();
    ASSERT_TRUE (terminal);
    node->lane.run ([] {}).get ();
    timer_queue->drain ();
    EXPECT_EQ (callbacks.load (), 1);
    EXPECT_FALSE (previous->preparation_operation ());
    EXPECT_FALSE (replacement->preparation_operation ());
}

TEST (ZLinkFrameworkSpotCloseConformance, RejectedCreationNeverTicksAndCloseIsFalse)
{
    verify_timer_waits_for_creation (zlink::framework::user_spot_execution_mode_t::spot_wide, true);
}

TEST (ZLinkFrameworkSpotCloseConformance, FactoryExpiryRemainsPendingUntilInstancePublication)
{
    verify_timer_waits_for_creation (zlink::framework::user_spot_execution_mode_t::spot_wide, false,
                                     true);
}

void verify_target_staging_commit (bool initialize_throws)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "timer-staging-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1);
    serializer_registry_t serializers;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->spot_factories.emplace ("staged", std::type_index (typeid (int)));
    node->spot_factory_relocations.emplace (
      "staged", factory_relocation_configuration_t{factory_relocation_kind_t::recreate});
    std::shared_ptr<spot_context_state_t> context;
    const auto initializes = std::make_shared<std::atomic_int> (0);
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [&] (spot_context_t value,
                                                  service_provider_t &) -> std::shared_ptr<void> {
        context = spot_context_access_t::state (value);
        return std::make_shared<int> (1);
    };
    lifecycle.on_initialize = [initializes, initialize_throws] (void *) -> task_t<void> {
        ++*initializes;
        if (initialize_throws)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "fixture target initialize failure");
        co_return;
    };
    node->spot_lifecycles.emplace ("staged", std::move (lifecycle));
    spot_node_runtime_t owner (node);
    runtime::stateful::frozen_object_state_t frozen;
    frozen.stable_type = "staged";
    runtime::stateful::object_ref_t target{.kind = runtime::stateful::object_kind_t::user_spot,
                                           .key = "staged-timer-spot",
                                           .object_generation = 1,
                                           .authority_owner_generation = 1};
    owner.bind_relocation_authority (
      std::make_shared<zlink::framework::test::committed_authority_fixture_t> (target));
    frozen.owner = target;
    ASSERT_TRUE (owner.restore_spot_relocation_state (frozen, target).result ().value ());
    const auto initializes_before_commit = initializes->load ();
    const auto callbacks = std::make_shared<std::atomic_int> (0);
    auto timer = std::make_shared<timer_state_t> ();
    timer->name = "staged-expiry";
    timer->period = std::chrono::hours (1);
    timer->handler_instance = context->spot_instance;
    timer->handler_invoker = [callbacks] (void *, void *, serializer_registry_t &,
                                          const timer_tick_t &) {
        ++*callbacks;
        return task_t<zlink::message_t> (result_t<zlink::message_t>::success (zlink::message_t{}));
    };
    node->lane.run ([&] { context->timers.push_back (timer); }).get ();
    timer_runtime_t::post_fire_count (context, timer, {}, 1);
    observe_scheduled_expiry (node->worker_executor);
    const auto callbacks_before_commit = callbacks->load ();
    const bool committed = owner.commit_relocation_materialization ({target});
    context->serial_queue->drain ();
    context->spot_serial_executor->close ();
    context->serial_queue->close ();
    node->worker_executor->drain ();
    node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    EXPECT_TRUE (committed);
    EXPECT_EQ (initializes_before_commit, 0);
    EXPECT_EQ (initializes->load (), 1);
    EXPECT_EQ (callbacks_before_commit, 0);
    if (!initialize_throws)
        EXPECT_EQ (callbacks->load (), 1);
}

TEST (ZLinkFrameworkSpotCloseConformance, FailedPreparationDrainsYieldedTimerAndAcceptedClaims)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "failed-preparation-drain");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1);
    serializer_registry_t serializers;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->spot_factories.emplace ("preparing", std::type_index (typeid (int)));
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [] (spot_context_t,
                                                 service_provider_t &) -> std::shared_ptr<void> {
        return std::make_shared<int> (1);
    };
    node->spot_lifecycles.emplace ("preparing", std::move (lifecycle));
    std::function<task_t<void> (bool)> terminal;
    auto created = spot_node_runtime_t (node).get_or_create_spot (
      "preparing", spot_id_t ("failed-preparation-drain"), {}, 1, {}, 1, &terminal);
    auto state = spot_context_access_t::state (created.context);
    const auto queue = state->serial_queue;
    EXPECT_TRUE (state->admit (true));
    std::shared_ptr<void> first_record_claim (nullptr,
                                              [state] (void *) { state->leave_callback (); });
    task_completion_source_t<void> resume;
    task_completion_source_t<void> proceed;
    std::promise<void> entered;
    auto callback_entered = entered.get_future ();
    const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
        proceed.complete (result_t<void>::success ());
        resume.complete (result_t<void>::success ());
    });
    std::promise<void> cancelling;
    auto cancellation_started = cancelling.get_future ();
    struct observed_timer_resource_t final : timer_resource_t
    {
        explicit observed_timer_resource_t (std::promise<void> &value) : observed (value) {}
        result_t<void> cancel () noexcept override
        {
            observed.set_value ();
            return result_t<void>::success ();
        }
        std::promise<void> &observed;
    };
    bool resumed_with_instance = false;
    int callbacks = 0;
    int pending_callbacks = 0;
    auto running = std::make_shared<timer_state_t> ();
    running->name = "yielded-preparation-timer";
    running->period = std::chrono::hours (1);
    running->handler_instance = state->spot_instance;
    running->native_timer = std::make_unique<observed_timer_resource_t> (cancelling);
    running->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                    const timer_tick_t &) -> task_t<zlink::message_t> {
        entered.set_value ();
        co_await proceed.task ();
        terminal (false);
        co_await detail::yield_call<void> ([&] { return resume.task (); });
        resumed_with_instance = bool (state->spot_instance);
        ++callbacks;
        co_return zlink::message_t{};
    };
    auto pending = std::make_shared<timer_state_t> ();
    pending->name = "pending-preparation-timer";
    pending->period = std::chrono::hours (1);
    pending->handler_instance = state->spot_instance;
    pending->handler_invoker = [&] (void *, void *, serializer_registry_t &, const timer_tick_t &) {
        ++pending_callbacks;
        return task_t<zlink::message_t> (result_t<zlink::message_t>::success (zlink::message_t{}));
    };
    node->lane
      .run ([&] {
          state->timers.push_back (running);
          state->timers.push_back (pending);
      })
      .get ();
    auto marker = std::make_shared<std::promise<void>> ();
    auto application_drained = marker->get_future ();
    std::shared_ptr<void> cleanup_on_exit (nullptr, [&] (void *) {
        proceed.complete (result_t<void>::success ());
        resume.complete (result_t<void>::success ());
        terminal (false);
        first_record_claim.reset ();
        queue->drain ();
        state->spot_serial_executor->close ();
        node->worker_executor->drain ();
        node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    });
    timer_runtime_t::post_fire_count (state, running, {}, 1);
    timer_runtime_t::post_fire_count (state, pending, {}, 1);
    state->spot_serial_executor->open_preparation_admission ();
    callback_entered.get ();
    state->spot_serial_executor->close_preparation_admission ();
    proceed.complete (result_t<void>::success ());
    cancellation_started.get ();
    auto drained = node->lane.run ([&] { return state->admitted_callback_terminal; }).get ();
    ASSERT_TRUE (drained);
    auto early_close = created.context.close ();
    resume.complete (result_t<void>::success ());
    ASSERT_TRUE (queue->try_post ("accepted-callback-marker", [marker] { marker->set_value (); }));
    application_drained.get ();
    EXPECT_TRUE (resumed_with_instance);
    EXPECT_EQ (callbacks, 1);
    EXPECT_EQ (pending_callbacks, 0);
    EXPECT_FALSE (drained->task ().await_ready ());
    EXPECT_FALSE (early_close.await_ready ());
    auto repeated_terminal = state->complete_preparation (false);
    EXPECT_FALSE (repeated_terminal.await_ready ());
    EXPECT_TRUE (node->lane.run ([&] { return bool (state->spot_instance); }).get ());
    first_record_claim.reset ();
    queue->drain ();
    EXPECT_TRUE (drained->task ().await_ready ());
    EXPECT_TRUE (repeated_terminal.result ());
    EXPECT_TRUE (early_close.result ());
    EXPECT_FALSE (early_close.result ().value ());
    EXPECT_FALSE (node->lane.run ([&] { return bool (state->spot_instance); }).get ());
    cleanup_on_exit.reset ();
}

TEST (ZLinkFrameworkSpotCloseConformance, RelocationTargetTimerWaitsForStagingCommit)
{
    verify_target_staging_commit (false);
}

TEST (ZLinkFrameworkSpotCloseConformance, TargetInitializeFailurePreservesConfirmedRelocation)
{
    verify_target_staging_commit (true);
}
} // namespace
