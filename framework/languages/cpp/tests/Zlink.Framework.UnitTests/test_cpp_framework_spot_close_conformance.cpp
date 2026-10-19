/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
// Runs framework/runtime/conformance/spot-close-v1.json against an in-process
// app with a Location Store (Spot address messaging §7, §9; gate §7).

#include "../support/committed_authority_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ZLINK_SPOT_CLOSE_CONFORMANCE_PATH
#error "Spot Close conformance fixture path is required"
#endif

namespace
{

// Spot timer §1: a native expiry must remain queued while creation is preparing.
// A FIFO worker marker follows the expiry's runnable queue consumer. All fixtures
// using this marker configure one worker, so observation does not stop the worker.
void observe_scheduled_expiry (
  const std::shared_ptr<zlink::framework::runtime::offload_executor_t> &executor)
{
    auto marker = std::make_shared<std::promise<void>> ();
    auto observed = marker->get_future ();
    if (!executor->try_submit_internal ([marker] { marker->set_value (); }))
        throw std::runtime_error ("expiry observation worker rejected the marker");
    observed.get ();
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
    ASSERT_TRUE (owner.restore_spot_relocation_state (frozen, target));
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
    const std::shared_ptr<void> release_on_exit (
      nullptr, [&] (void *) { resume.complete (result_t<void>::success ()); });
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
    node->lane.run ([&] { state->timers = {running, pending}; }).get ();
    auto marker = std::make_shared<std::promise<void>> ();
    auto application_drained = marker->get_future ();
    std::shared_ptr<void> cleanup_on_exit (nullptr, [&] (void *) {
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
    cancellation_started.get ();
    auto drained = node->lane.run ([&] { return state->preparation_callback_terminal; }).get ();
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

using namespace std::chrono_literals;
namespace zf = zlink::framework;

constexpr const char *mesh_name = "spot-close-mesh";
constexpr const char *node_name = "spot-close-node";

// ---------------------------------------------------------------------------
// Observation shared by the application types of one scenario.

struct observation_t
{
    std::mutex mutex;
    std::vector<std::string> order;
    std::vector<std::string> diagnostics;
    std::vector<std::string> notes;
    int on_closing_calls = 0;
    int handler_calls = 0;
    int factory_calls = 0;
    int joins_admitted = 0;
    bool on_closing_throws = false;
    // OnClosing waits until the scenario releases it, so the act observes a
    // Spot whose Close has committed Closing.
    bool on_closing_blocks = false;
    std::promise<void> closing_entered;
    zf::task_completion_source_t<void> closing_released;
    std::optional<std::promise<void>> queued_request_admitted;
    bool room_timer = false;
    std::optional<zf::timer_t> timer_resource;
    int timer_calls = 0;
    int timer_calls_on_closing = 0;
    // Both source handlers register their Actor barriers before either ends.
    std::atomic_bool cross_join_active{false};
    std::promise<void> cross_a_reserved;
    std::promise<void> cross_b_reserved;
    std::promise<void> cross_a_initial;
    std::promise<void> cross_b_initial;
    std::promise<void> cross_a_completed;
    std::promise<void> cross_b_completed;
    std::promise<void> cross_a_left;
    std::promise<void> cross_b_left;
    std::promise<void> cross_sources_released;
    std::shared_future<void> cross_sources_release = cross_sources_released.get_future ().share ();
    std::string handler_mode;
    std::string watched_spot;
    std::shared_ptr<zf::detail::spot_context_state_t> room_state;
    std::shared_ptr<zf::detail::spot_context_state_t> session_state;
    std::shared_ptr<zf::detail::timer_state_t> preparation_timer;
    bool prepare_timer = false;
    bool initialized = false;
    std::size_t prepared_authority_puts = 0;
    bool preparation_timer_closes = false;
    std::optional<zf::task_t<bool>> preparation_timer_close;

    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        order.push_back (std::move (event));
    }
};

observation_t *observed = nullptr;

observation_t &current ()
{
    return *observed;
}

void install_preparation_timer (const std::shared_ptr<zf::detail::spot_context_state_t> &state)
{
    if (!current ().prepare_timer)
        return;
    auto timer = std::make_shared<zf::detail::timer_state_t> ();
    timer->name = "prepared-expiry";
    timer->period = std::chrono::hours (1);
    timer->handler_instance = state->spot_instance;
    timer->handler_invoker = [weak_state = std::weak_ptr (state)] (void *, void *,
                                                                   zf::serializer_registry_t &,
                                                                   const zf::timer_tick_t &) {
        bool closes = false;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().timer_calls;
            closes = current ().preparation_timer_closes;
        }
        if (closes) {
            auto closing = zf::detail::spot_context_access_t::create (weak_state.lock ()).close ();
            std::lock_guard lock (current ().mutex);
            current ().preparation_timer_close.emplace (std::move (closing));
        }
        return zf::task_t<zlink::message_t> (
          zf::result_t<zlink::message_t>::success (zlink::message_t{}));
    };
    if (state->execution_mode == zf::user_spot_execution_mode_t::per_actor)
        timer->serial_queue = state->spot_serial_executor->timer_queue (timer->name);
    state->node->lane.run ([&] { state->timers.push_back (timer); }).get ();
    std::lock_guard lock (current ().mutex);
    current ().preparation_timer = std::move (timer);
    current ().initialized = true;
}

// ---------------------------------------------------------------------------
// Messages.

struct close_probe_request_t
{
    static constexpr const char *packet_name = "spot-close-probe-request";
    int value{};
};

struct close_probe_reply_t
{
    static constexpr const char *packet_name = "spot-close-probe-reply";
    int value{};
};

struct join_room_t
{
    static constexpr const char *packet_name = "spot-close-join-room";
    std::string room;
};

void to_json (nlohmann::json &json, const close_probe_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
void from_json (const nlohmann::json &json, close_probe_request_t &value)
{
    value.value = json.at ("value").get<int> ();
}
void to_json (nlohmann::json &json, const close_probe_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
void from_json (const nlohmann::json &json, close_probe_reply_t &value)
{
    value.value = json.at ("value").get<int> ();
}
void to_json (nlohmann::json &json, const join_room_t &value)
{
    json = nlohmann::json{{"room", value.room}};
}
void from_json (const nlohmann::json &json, join_room_t &value)
{
    value.room = json.at ("room").get<std::string> ();
}

// ---------------------------------------------------------------------------
// Application types.

class member_actor_t final : public zf::actor_t
{
  public:
    explicit member_actor_t (zf::actor_context_t context) : _context (std::move (context)) {}
    zf::actor_context_t &context () noexcept override { return _context; }
    const zf::actor_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_join_completed (const zf::actor_join_completion_t &completion) override
    {
        std::string note = "actorJoin:";
        if (std::holds_alternative<zf::actor_join_accepted_t> (completion))
            note += "accepted";
        else if (std::holds_alternative<zf::actor_join_rejected_t> (completion))
            note += "rejected";
        else
            note += "failed:"
                    + std::to_string (
                      static_cast<int> (std::get<zf::actor_join_failed_t> (completion).error_kind));
        std::lock_guard lock (current ().mutex);
        current ().notes.push_back (std::move (note));
        const auto id = _context.actor_id ().value ();
        if (current ().cross_join_active.load ()) {
            current ().order.push_back ("joinReply:" + std::string (id));
            (id == "cross-a" ? current ().cross_a_completed : current ().cross_b_completed)
              .set_value ();
        } else if (id == "cross-a" || id == "cross-b") {
            (id == "cross-a" ? current ().cross_a_initial : current ().cross_b_initial)
              .set_value ();
        }
        co_return;
    }

  private:
    zf::actor_context_t _context;
};

class member_actor_factory_t final : public zf::actor_factory_t<member_actor_t>
{
  public:
    zf::task_t<std::shared_ptr<member_actor_t>> create (zf::actor_context_t context,
                                                        std::stop_token) override
    {
        co_return std::make_shared<member_actor_t> (std::move (context));
    }
};

class close_entry_spot_t final : public zf::entry_spot_t<member_actor_t>
{
  public:
    explicit close_entry_spot_t (zf::entry_spot_context_t context) : _context (std::move (context))
    {
    }
    zf::entry_spot_context_t &context () noexcept override { return _context; }
    const zf::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&close_entry_spot_t::join_room> (
          join_room_t::packet_name);
    }

    void join_room (member_actor_t &actor, zf::message_context_t &, const join_room_t &request)
    {
        actor.context ().join_spot (zf::spot_id_t (request.room)).defer ();
    }

    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

  private:
    zf::entry_spot_context_t _context;
};

class close_room_spot_t;

struct close_room_timer_handler_t
{
    zf::task_t<void> handle (close_room_spot_t &, const zf::timer_tick_t &)
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().timer_calls;
            current ().order.push_back ("timer");
        }
        co_return;
    }
};

class close_other_room_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit close_other_room_spot_t (zf::spot_context_t context) : _context (std::move (context))
    {
    }
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

  private:
    zf::spot_context_t _context;
};

class close_room_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit close_room_spot_t (zf::spot_context_t context) : _context (std::move (context))
    {
        current ().room_state = zf::detail::spot_context_access_t::state (_context);
    }
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_initialize () override
    {
        install_preparation_timer (zf::detail::spot_context_access_t::state (_context));
        co_return;
    }
    void configure () override
    {
        _context.handlers ().add_handler<&close_room_spot_t::probe> (
          close_probe_request_t::packet_name);
        // An Actor handler registers this Spot's Join admission for member_actor_t.
        _context.handlers ().add_actor_send<&close_room_spot_t::member_ping> (
          join_room_t::packet_name);
        bool timer = false;
        {
            std::lock_guard lock (current ().mutex);
            timer = current ().room_timer;
        }
        if (timer) {
            auto resource = _context.add_timer<close_room_timer_handler_t> ("close-timer", 5ms);
            std::lock_guard lock (current ().mutex);
            current ().timer_resource = std::move (resource);
        }
    }

    close_probe_reply_t probe (const close_probe_request_t &request)
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().handler_calls;
        }
        return close_probe_reply_t{request.value + 1};
    }

    // A member asked to join another room moves there (cross-join scenario).
    void member_ping (member_actor_t &actor, zf::message_context_t &, const join_room_t &request)
    {
        if (request.room != _context.spot_id ()) {
            actor.context ().join_spot (zf::spot_id_t (request.room)).defer ();
            if (current ().cross_join_active.load ()) {
                auto &reserved = actor.context ().actor_id ().value () == "cross-a"
                                   ? current ().cross_a_reserved
                                   : current ().cross_b_reserved;
                reserved.set_value ();
                current ().cross_sources_release.wait ();
            }
        }
    }

    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().joins_admitted;
        }
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &actor) override
    {
        current ().record ("joinCompleted");
        current ().record ("joinStarted:" + _context.spot_id ());
        if (current ().cross_join_active.load ())
            current ().record ("joinedDone:" + std::string (actor.context ().actor_id ().value ()));
        co_return;
    }
    zf::task_t<void> on_leave_actor (member_actor_t &actor) override
    {
        current ().record ("leaveCompleted:" + _context.spot_id ());
        if (current ().cross_join_active.load ()) {
            const auto id = actor.context ().actor_id ().value ();
            if (id == "cross-a")
                current ().cross_a_left.set_value ();
            else if (id == "cross-b")
                current ().cross_b_left.set_value ();
        }
        co_return;
    }

    zf::task_t<void> on_closing (const zf::spot_closing_context_t &, std::stop_token) override
    {
        bool fail = false;
        bool blocks = false;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().on_closing_calls;
            current ().order.push_back ("onClosing");
            current ().timer_calls_on_closing = current ().timer_calls;
            fail = current ().on_closing_throws;
            blocks = current ().on_closing_blocks;
        }
        if (blocks) {
            current ().closing_entered.set_value ();
            co_await current ().closing_released.task ();
        }
        if (fail)
            throw std::runtime_error ("fixture OnClosing failure");
        co_return;
    }

  private:
    zf::spot_context_t _context;
};

class close_session_spot_t final : public zf::instance_spot_t
{
  public:
    explicit close_session_spot_t (zf::instance_spot_context_t context) :
        _context (std::move (context))
    {
        current ().session_state = zf::detail::spot_context_access_t::state (_context);
    }
    zf::instance_spot_context_t &context () noexcept override { return _context; }
    const zf::instance_spot_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_initialize () override
    {
        install_preparation_timer (zf::detail::spot_context_access_t::state (_context));
        co_return;
    }
    void configure () override
    {
        _context.handlers ().add_handler<&close_session_spot_t::probe> ();
    }

    close_probe_reply_t probe (const close_probe_request_t &request)
    {
        std::string mode;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().handler_calls;
            mode = request.value == 0 ? std::string{} : current ().handler_mode;
        }
        if (mode == "closeTwiceThenComplete") {
            _context.close ();
            _context.close ();
            current ().record ("handlerReturned");
        } else if (mode == "closeThenThrow") {
            _context.close ();
            current ().record ("handlerThrew");
            throw std::runtime_error ("fixture handler failure after Close");
        }
        return close_probe_reply_t{request.value + 1};
    }

    zf::task_t<void> on_closing (const zf::spot_closing_context_t &, std::stop_token) override
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().on_closing_calls;
            current ().order.push_back ("onClosing");
        }
        co_return;
    }

  private:
    zf::instance_spot_context_t _context;
};

// ---------------------------------------------------------------------------
// Location Store with one-shot write faults on a watched Spot authority row.

class close_fault_store_t final : public zf::location_store_t
{
  public:
    explicit close_fault_store_t (std::shared_ptr<zf::runtime::in_memory_location_store_t> inner) :
        _inner (std::move (inner))
    {
    }

    std::atomic_bool fail_closing_commit_once{false};
    std::atomic_bool fail_authority_release_once{false};
    // The next Closing commit stays pending until release_held_commit.
    std::atomic_bool hold_closing_commit_once{false};
    std::atomic_bool hold_authority_release_once{false};
    // Zero disables this fixture; Ready/cursor/pointer are prepared puts 1/2/3.
    std::size_t hold_prepared_authority_put = 0;
    std::atomic_bool owner_lease_released{false};
    std::promise<void> commit_held;

    // Completes the held Closing commit: applied, or an authority version conflict.
    void release_held_commit (bool conflict)
    {
        std::optional<zf::store_write_request_t> request;
        std::optional<zf::task_completion_source_t<zf::store_write_result_t>> completion;
        {
            std::lock_guard lock (_held_mutex);
            request = std::move (_held_request);
            completion = std::move (_held_completion);
            _held_request.reset ();
            _held_completion.reset ();
        }
        if (!completion)
            return;
        if (conflict) {
            completion->complete (conflict_after_authority_change (std::move (*request)).result ());
            return;
        }
        completion->complete (_inner->write (std::move (*request)).result ());
    }

    zf::task_t<zf::store_read_result_t> read (zf::store_key_t key) override
    {
        return _inner->read (std::move (key));
    }

    zf::task_t<zf::store_write_result_t> write (zf::store_write_request_t request) override
    {
        bool watched_put = false;
        bool watched_delete = false;
        bool owner_delete = false;
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<zf::store_put_t> (&mutation))
                watched_put = watched_put || is_watched (put->key);
            if (const auto *erase = std::get_if<zf::store_delete_t> (&mutation)) {
                watched_delete = watched_delete || is_watched (erase->key);
                owner_delete =
                  owner_delete || erase->key.value.starts_with (std::string ("owner-lease\0", 12));
            }
        }
        bool hold_prepared = false;
        if (watched_put) {
            std::lock_guard lock (current ().mutex);
            if (current ().initialized) {
                ++current ().prepared_authority_puts;
                hold_prepared = current ().prepared_authority_puts == hold_prepared_authority_put;
            }
        }
        if (hold_prepared || (watched_put && hold_closing_commit_once.exchange (false))
            || (watched_delete && hold_authority_release_once.exchange (false))) {
            zf::task_completion_source_t<zf::store_write_result_t> completion;
            auto held = completion.task ();
            {
                std::lock_guard lock (_held_mutex);
                _held_request = std::move (request);
                _held_completion = std::move (completion);
            }
            commit_held.set_value ();
            co_return co_await held;
        }
        if (watched_put && fail_closing_commit_once.exchange (false))
            co_return co_await conflict_after_authority_change (std::move (request));
        if (watched_delete && fail_authority_release_once.exchange (false)) {
            co_return zf::store_write_result_t{zf::store_write_conflict_t{}};
        }
        auto result = co_await _inner->write (std::move (request));
        if (owner_delete && std::holds_alternative<zf::store_write_applied_t> (result))
            owner_lease_released.store (true);
        if (watched_delete && std::holds_alternative<zf::store_write_applied_t> (result))
            current ().record ("authorityReleased");
        co_return result;
    }

    zf::task_t<zf::store_scan_result_t> scan (zf::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

  private:
    zf::task_t<zf::store_write_result_t>
    conflict_after_authority_change (zf::store_write_request_t request)
    {
        for (const auto &mutation : request.mutations) {
            const auto &key = std::visit (
              [] (const auto &value) -> const zf::store_key_t & { return value.key; }, mutation);
            if (!is_watched (key))
                continue;
            const auto current = std::get<zf::store_found_t> (co_await _inner->read (key));
            zf::store_write_request_t concurrent{
              {}, {zf::store_put_t{key, current.value.bytes, std::nullopt}}};
            const auto rewritten = co_await _inner->write (std::move (concurrent));
            const auto *applied = std::get_if<zf::store_write_applied_t> (&rewritten);
            if (!applied || applied->put_versions.size () != 1)
                throw std::runtime_error ("fixture authority rewrite failed");
            EXPECT_NE (current.value.version.value, applied->put_versions.front ().version.value);
            const auto result = co_await _inner->write (std::move (request));
            EXPECT_TRUE (std::holds_alternative<zf::store_write_conflict_t> (result));
            co_return result;
        }
        throw std::runtime_error ("fixture authority mutation missing");
    }

    bool is_watched (const zf::store_key_t &key) const
    {
        std::string suffix ("\0spot\0", 6);
        {
            std::lock_guard lock (current ().mutex);
            if (current ().watched_spot.empty ())
                return false;
            suffix += current ().watched_spot;
        }
        return key.value.size () >= suffix.size ()
               && key.value.compare (key.value.size () - suffix.size (), suffix.size (), suffix)
                    == 0;
    }

    std::shared_ptr<zf::runtime::in_memory_location_store_t> _inner;
    std::mutex _held_mutex;
    std::optional<zf::store_write_request_t> _held_request;
    std::optional<zf::task_completion_source_t<zf::store_write_result_t>> _held_completion;
};

// ---------------------------------------------------------------------------
// Scenario runner.

std::string error_name (zf::framework_error_kind_t kind)
{
    switch (kind) {
        case zf::framework_error_kind_t::not_found:
            return "NotFound";
        case zf::framework_error_kind_t::rejected:
            return "Rejected";
        case zf::framework_error_kind_t::unavailable:
            return "Unavailable";
        case zf::framework_error_kind_t::shutting_down:
            return "ShuttingDown";
        case zf::framework_error_kind_t::invalid_operation:
            return "InvalidOperation";
        default:
            return "Error" + std::to_string (static_cast<int> (kind));
    }
}

template <typename T> nlohmann::json close_result (const zf::result_t<T> &result)
{
    if (result)
        return result.value ();
    return error_name (result.error_kind ());
}

bool wait_until (const std::function<bool ()> &condition,
                 std::chrono::milliseconds limit = std::chrono::milliseconds (3000))
{
    const auto deadline = std::chrono::steady_clock::now () + limit;
    while (!condition ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::sleep_for (1ms);
    }
    return true;
}

class scenario_client_t final : public zf::hosted_service_t
{
  public:
    scenario_client_t (zf::app_t &app, const nlohmann::json &scenario) :
        _app (&app), _scenario (scenario)
    {
    }

    zf::task_t<void> start (zf::service_provider_t &services) override
    {
        if (_scenario.at ("act") == "shutdownDuringClose") {
            // Let host startup finish before requesting its terminal result.
            _worker = std::thread ([this, services] () mutable {
                (void) wait_until ([&] { return _app->is_ready (); });
                execute (services);
            });
        } else {
            execute (services);
        }
        co_return;
    }

    void join ()
    {
        if (_worker.joinable ())
            _worker.join ();
    }

    void execute (zf::service_provider_t &services)
    {
        try {
            run (services);
        }
        catch (const std::exception &error) {
            failure = error.what ();
        }
        {
            // Observed before the app stops: shutdown cleanup is not part of the act.
            std::lock_guard lock (current ().mutex);
            actual["onClosingCalls"] = current ().on_closing_calls;
            actual["handlerCalls"] = current ().handler_calls;
            actual["factoryCalls"] = current ().factory_calls;
            if (current ().timer_resource) {
                actual["timerDisposedAfterClose"] = current ().timer_resource->is_disposed ();
                actual["timerCallsAfterOnClosing"] =
                  current ().timer_calls - current ().timer_calls_on_closing;
            }
            actual["order"] = current ().order;
            actual["diagnostics"] = current ().diagnostics;
            actual["notes"] = current ().notes;
            actual["joinsAdmitted"] = current ().joins_admitted;
        }
        if (!stopping)
            _app->stop ();
    }

    void stop () noexcept override {}

    nlohmann::json actual = nlohmann::json::object ();
    std::string failure;
    bool stopping = false;

  private:
    std::thread _worker;
    std::string authority_state (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &locations = services.get_required<zf::location_repository_t> ();
        const auto read =
          locations.read_authority (zf::runtime::spot_authority_key (spot_id)).result ().value ();
        const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&read);
        if (!snapshot)
            return "Missing";
        if (const auto user = zf::runtime::decode_direct_user_spot_authority_payload (
              std::span<const std::byte> (snapshot->payload))) {
            switch (user->state) {
                case zf::runtime::user_spot_authority_state_t::creating:
                    return "Creating";
                case zf::runtime::user_spot_authority_state_t::ready:
                    return "Ready";
                case zf::runtime::user_spot_authority_state_t::closing:
                    return "Closing";
            }
        }
        const std::string_view instance_closing = "zlink:instance-spot:closing:v1\n";
        if (snapshot->payload.size () >= instance_closing.size ()
            && std::equal (instance_closing.begin (), instance_closing.end (),
                           snapshot->payload.begin (), [] (char left, std::byte right) {
                               return static_cast<unsigned char> (left)
                                      == std::to_integer<unsigned char> (right);
                           }))
            return "Closing";
        return "Ready";
    }

    zf::spot_ref_t create_room (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        const auto created =
          manager.get_or_create (zf::spot_id_t (spot_id), "room").timeout (3s).async ().result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "User Spot create failed");
        const auto found = manager.find (zf::spot_id_t (spot_id)).result ();
        if (!found || !found.value ())
            throw std::runtime_error ("User Spot find failed");
        return *found.value ();
    }

    zf::result_t<bool> manager_close (zf::service_provider_t &services, const zf::spot_ref_t &ref)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        return manager.close (ref).result ();
    }

    zf::result_t<close_probe_reply_t>
    direct_request (const std::string &spot_id, zf::service_provider_t &services, int value)
    {
        auto route = _app->advanced ().zlink ().route_client (
          services.get_required<zf::serializer_registry_t> ());
        return route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{value})
          .timeout (3s)
          .async<close_probe_reply_t> ()
          .result ();
    }

    void join_member (zf::service_provider_t &services,
                      const std::string &room,
                      const std::string &actor_id)
    {
        auto &actors = services.get_required<zf::actor_manager_t> ();
        const auto created = actors.get_or_create (zf::actor_id_t (actor_id), "member")
                               .timeout (3s)
                               .async ()
                               .result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "member Actor create failed");
        auto &client = services.get_required<zf::actor_client_t> ();
        const auto sent =
          client.send (zf::actor_id_t (actor_id), join_room_t{room}).async ().result ();
        if (!sent)
            throw std::runtime_error (sent.error () ? sent.error ()->what ()
                                                    : "member Join request failed");
    }

    void run (zf::service_provider_t &services)
    {
        const auto name = _scenario.at ("name").get<std::string> ();
        const auto &given = _scenario.at ("given");
        const auto act = _scenario.at ("act").get<std::string> ();
        const auto spot_id = "close-" + name;
        {
            std::lock_guard lock (current ().mutex);
            current ().watched_spot = spot_id;
            current ().on_closing_throws = given.value ("onClosing", "") == "throws";
            current ().on_closing_blocks = given.value ("onClosing", "") == "blocks";
            current ().handler_mode = given.value ("handler", "");
            current ().room_timer = given.value ("roomTimer", false);
            current ().prepare_timer = act == "timerDuringPreparedAuthorityWrite";
            current ().preparation_timer_closes = given.value ("timerCloses", false);
        }
        auto &store = *_store;

        if (act == "timerDuringPreparedAuthorityWrite") {
            const bool instance = given.value ("instance", false);
            const auto stage = given.at ("heldStage").get<std::string> ();
            store.hold_prepared_authority_put = stage == "ready" ? 1 : stage == "cursor" ? 2 : 3;
            auto held = store.commit_held.get_future ();
            auto creating = std::async (std::launch::async, [&] {
                if (!instance) {
                    auto &manager = services.get_required<zf::spot_manager_t> ();
                    const auto result = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                          .timeout (3s)
                                          .async ()
                                          .result ();
                    return bool (result);
                }
                auto route = _app->advanced ().zlink ().route_client (
                  services.get_required<zf::serializer_registry_t> ());
                return bool (
                  route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{0})
                    .instance_spot ("session")
                    .timeout (3s)
                    .async<close_probe_reply_t> ()
                    .result ());
            });
            const std::shared_ptr<void> release_on_exit (
              nullptr, [&] (void *) { store.release_held_commit (false); });
            held.get ();
            const auto state = instance ? current ().session_state : current ().room_state;
            const auto timer = current ().preparation_timer;
            const auto queue = state->serial_queue;
            zf::detail::timer_runtime_t::post_fire_count (state, timer, timer->serial_queue, 1);
            observe_scheduled_expiry (state->serial_executor);
            {
                std::lock_guard lock (current ().mutex);
                actual["timerCallsWhileHeld"] = current ().timer_calls;
                actual["handlerCallsWhileHeld"] = current ().handler_calls;
                if (given.value ("timerCloses", false) && stage != "ready") {
                    actual["timerCloseRequestedWhileHeld"] =
                      bool (current ().preparation_timer_close);
                    actual["timerCloseCompletedWhileHeld"] =
                      current ().preparation_timer_close
                      && current ().preparation_timer_close->await_ready ();
                }
            }
            store.release_held_commit (given.value ("terminalConflict", false));
            actual["created"] = creating.get ();
            queue->drain ();
            if (timer->serial_queue)
                timer->serial_queue->drain ();
            std::optional<zf::task_t<bool>> timer_close;
            {
                std::lock_guard lock (current ().mutex);
                actual["timerCallsAfterPublication"] = current ().timer_calls;
                timer_close = std::move (current ().preparation_timer_close);
            }
            if (timer_close)
                actual["timerCloseResult"] = close_result (timer_close->result ());
            return;
        }

        if (given.contains ("handler")) {
            // Instance Spot: the handler turn calls context Close itself.
            auto route = _app->advanced ().zlink ().route_client (
              services.get_required<zf::serializer_registry_t> ());
            const auto activated =
              route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{0})
                .instance_spot ("session")
                .timeout (3s)
                .async<close_probe_reply_t> ()
                .result ();
            if (!activated)
                throw std::runtime_error ("Instance Spot activation failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
            }
            if (act != "directRequestWithoutInstanceIntent")
                throw std::runtime_error ("unsupported act for a handler scenario: " + act);
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? "handlerReply" : "handlerFailure";
            (void) wait_until ([&] { return authority_state (services, spot_id) == "Missing"; });
            actual["authority"] = authority_state (services, spot_id);
            return;
        }

        const auto authority = given.at ("authority").get<std::string> ();
        std::optional<zf::spot_ref_t> ref;
        if (authority != "Missing")
            ref = create_room (services, spot_id);
        if (act == "shutdownDuringClose") {
            // Hold the fenced release after local teardown has marked the Spot closed.
            // Shutdown must retain the authority until this existing Close settles.
            const bool expires = given.value ("deadlineExpires", false);
            if (expires)
                store.hold_closing_commit_once.store (true);
            else
                store.hold_authority_release_once.store (true);
            auto held = store.commit_held.get_future ();
            // Exercise the node's explicit Close directly: manager request accounting
            // would make the earlier host completion barrier mask this ordering bug.
            auto runtime =
              zf::detail::spot_node_runtime_t::from (_app->advanced ().zlink (), mesh_name);
            auto closing = runtime->close_spot (zf::spot_id_t (spot_id));
            if (held.wait_for (3s) != std::future_status::ready) {
                store.release_held_commit (false);
                throw std::runtime_error ("Close did not reach authority release");
            }
            auto shutdown = _app->shutdown (expires ? 100ms : 3s);
            stopping = true;
            if (!expires) {
                const auto state = current ().room_state;
                auto owner = state->lane_owner.lock ();
                if (!owner)
                    owner = state->node;
                if (!wait_until ([&] {
                        return store.owner_lease_released.load ()
                               || owner->lane
                                    .run ([&] { return !state->merged_close_results.empty (); })
                                    .get ();
                    })) {
                    store.release_held_commit (false);
                    throw std::runtime_error ("shutdown did not reach the Close barrier");
                }
            }
            auto terminal = shutdown.result_for (expires ? 2s : 0ms);
            actual["completedBeforeRelease"] = terminal.has_value ();
            actual["authorityBeforeRelease"] = authority_state (services, spot_id);
            actual["ownerDeletedBeforeRelease"] = store.owner_lease_released.load ();
            store.release_held_commit (false);
            const auto close_result = closing.result ();
            actual["closeSucceeded"] = close_result && close_result.value ();
            if (!terminal)
                terminal = shutdown.result_for (3s);
            if (!terminal)
                throw std::runtime_error ("shutdown exceeded its bounded teardown");
            const auto result = terminal->value ();
            actual["deadlineExceeded"] =
              result.reason == zf::termination_reason_t::deadline_exceeded;
            actual["stopped"] = result.outcome == zf::termination_outcome_t::stopped;
            return;
        }
        if (authority == "Closing") {
            // A Close whose authority release fails leaves Closing committed.
            store.fail_authority_release_once.store (true);
            (void) manager_close (services, *ref);
            if (authority_state (services, spot_id) != "Closing")
                throw std::runtime_error ("could not establish a Closing authority");
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        if (given.value ("members", 0) > 0) {
            join_member (services, spot_id, "member-" + name);
            if (!wait_until ([&] {
                    std::lock_guard lock (current ().mutex);
                    return std::count (current ().order.begin (), current ().order.end (),
                                       "joinCompleted")
                           > 0;
                }))
                throw std::runtime_error ("member Join did not complete");
        }
        if (given.value ("closingCommit", "") == "ownerFenceMismatch")
            store.fail_closing_commit_once.store (true);
        if (given.value ("failOnce", "") == "authorityReleased")
            store.fail_authority_release_once.store (true);

        if (act == "crossJoins") {
            // Two members move between two rooms in opposite directions at
            // the same time; each Join also leaves the other room.
            const auto room_a = spot_id;
            const auto room_b = spot_id + "-b";
            (void) create_room (services, room_b);
            // Membership §7: completion follows target OnJoinedActor; source
            // OnLeaveActor is a one-way notification with its own terminal.
            const auto joins_completed = [&] {
                std::lock_guard lock (current ().mutex);
                return std::count (current ().notes.begin (), current ().notes.end (),
                                   "actorJoin:accepted");
            };
            auto initial_a = current ().cross_a_initial.get_future ();
            auto initial_b = current ().cross_b_initial.get_future ();
            join_member (services, room_a, "cross-a");
            if (initial_a.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("member cross-a did not join room A");
            join_member (services, room_b, "cross-b");
            if (initial_b.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("member cross-b did not join room B");
            auto completed_a = current ().cross_a_completed.get_future ();
            auto completed_b = current ().cross_b_completed.get_future ();
            auto left_a = current ().cross_a_left.get_future ();
            auto left_b = current ().cross_b_left.get_future ();
            auto source_a_reserved = current ().cross_a_reserved.get_future ();
            auto source_b_reserved = current ().cross_b_reserved.get_future ();
            current ().cross_join_active.store (true);
            auto &client = services.get_required<zf::actor_client_t> ();
            auto to_b = std::async (std::launch::async, [&] {
                return client.send (zf::actor_id_t ("cross-a"), join_room_t{room_b})
                  .async ()
                  .result ();
            });
            auto to_a = std::async (std::launch::async, [&] {
                return client.send (zf::actor_id_t ("cross-b"), join_room_t{room_a})
                  .async ()
                  .result ();
            });
            const auto registration_deadline = std::chrono::steady_clock::now () + 8000ms;
            const bool both_reserved =
              source_a_reserved.wait_until (registration_deadline) == std::future_status::ready
              && source_b_reserved.wait_until (registration_deadline) == std::future_status::ready;
            current ().cross_sources_released.set_value ();
            (void) to_b.get ();
            (void) to_a.get ();
            if (!both_reserved)
                throw std::runtime_error ("both source handlers must register their Join barrier");
            const bool completed =
              completed_a.wait_until (registration_deadline) == std::future_status::ready
              && completed_b.wait_until (registration_deadline) == std::future_status::ready
              && joins_completed () >= 4;
            const bool sources_left =
              left_a.wait_until (registration_deadline) == std::future_status::ready
              && left_b.wait_until (registration_deadline) == std::future_status::ready;
            bool replies_after_joined = true;
            int checked_replies = 0;
            int checked_leaves = 0;
            {
                std::lock_guard lock (current ().mutex);
                std::set<std::string> joined;
                for (const auto &event : current ().order) {
                    if (event.starts_with ("leaveCompleted:"))
                        ++checked_leaves;
                    if (event.starts_with ("joinedDone:")) {
                        joined.insert (event.substr (11));
                    } else if (event.starts_with ("joinReply:")) {
                        replies_after_joined &= joined.contains (event.substr (10));
                        ++checked_replies;
                    }
                }
            }
            actual["crossJoins"] = completed && sources_left && replies_after_joined
                                       && checked_replies == 2 && checked_leaves == 2
                                     ? "completed"
                                     : "stalled";
            current ().cross_join_active.store (false);
            return;
        }
        if (act == "getOrCreateDuringClose") {
            auto &manager = services.get_required<zf::spot_manager_t> ();
            auto entered = current ().closing_entered.get_future ();
            auto closing =
              std::async (std::launch::async, [&] { return manager_close (services, *ref); });
            const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
                current ().closing_released.complete (zf::result_t<void>::success ());
            });
            if (entered.wait_for (3s) != std::future_status::ready)
                throw std::runtime_error ("OnClosing did not start");
            const auto mismatch = manager.get_or_create (zf::spot_id_t (spot_id), "other-room")
                                    .timeout (3s)
                                    .async ()
                                    .result ();
            EXPECT_FALSE (mismatch);
            EXPECT_EQ (mismatch.error_kind (), zf::framework_error_kind_t::type_mismatch);
            auto pending = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                             .timeout (given.value ("deadlineExpires", false) ? 100ms : 3s)
                             .async ();
            if (given.value ("deadlineExpires", false)) {
                const auto result = pending.result ();
                actual["deadlineExceeded"] =
                  !result && result.error_kind () == zf::framework_error_kind_t::deadline_exceeded;
            } else {
                actual["completedBeforeRelease"] = pending.await_ready ();
                current ().closing_released.complete (zf::result_t<void>::success ());
                const auto closed = closing.get ();
                actual["closeSucceeded"] = closed && closed.value ();
                const auto result = pending.result ();
                actual["created"] =
                  result && result.value ().state == zf::spot_create_state_t::created;
                actual["newIncarnation"] =
                  result && result.value ().spot.object_generation () != ref->object_generation ();
                return;
            }
            current ().closing_released.complete (zf::result_t<void>::success ());
            const auto closed = closing.get ();
            actual["closeSucceeded"] = closed && closed.value ();
            return;
        }
        if (act == "directRequestDuringClose") {
            // The first request installs the Ready route in the source cache,
            // so the second one reaches the owner while its Close runs.
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
                current ().factory_calls = 0;
            }
            auto entered = current ().closing_entered.get_future ();
            std::thread closing ([&] { (void) manager_close (services, *ref); });
            if (entered.wait_for (3s) != std::future_status::ready) {
                current ().closing_released.complete (zf::result_t<void>::success ());
                closing.join ();
                throw std::runtime_error ("OnClosing did not start");
            }
            const auto authority_during = authority_state (services, spot_id);
            const auto reply = direct_request (spot_id, services, 1);
            current ().closing_released.complete (zf::result_t<void>::success ());
            closing.join ();
            actual["authorityDuringClose"] = authority_during;
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["authority"] = authority_state (services, spot_id);
            return;
        }
        if (act == "directRequestWithoutInstanceIntent") {
            if (given.at ("runtime") == "Draining") {
                stopping = true;
                (void) _app->shutdown (5s);
                (void) wait_until ([&] {
                    return _app->runtime_state () == zf::framework_runtime_state_t::draining;
                });
            }
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["creationIntent"] = false;
            return;
        }
        if (act == "directRequestDuringHeldClosingCommit") {
            // The request reaches the owner while the Closing commit is pending.
            // The lifecycle item holds application execution until the CAS result.
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            store.hold_closing_commit_once.store (true);
            auto held = store.commit_held.get_future ();
            std::optional<zf::result_t<bool>> closed;
            std::thread closing ([&] { closed.emplace (manager_close (services, *ref)); });
            if (held.wait_for (3s) != std::future_status::ready) {
                store.hold_closing_commit_once.store (false);
                closing.join ();
                throw std::runtime_error ("the Closing commit was not held");
            }
            std::future<void> admitted;
            {
                std::lock_guard lock (current ().mutex);
                current ().queued_request_admitted.emplace ();
                admitted = current ().queued_request_admitted->get_future ();
            }
            auto pending = std::async (std::launch::async,
                                       [&] { return direct_request (spot_id, services, 1); });
            if (admitted.wait_for (3s) != std::future_status::ready) {
                store.release_held_commit (true);
                closing.join ();
                (void) pending.get ();
                throw std::runtime_error ("queued request admission was not observed");
            }
            {
                std::lock_guard lock (current ().mutex);
                actual["handlerCallsBeforeCommit"] = current ().handler_calls;
            }
            store.release_held_commit (given.value ("closingCommit", "") == "heldThenConflict");
            const auto reply = pending.get ();
            closing.join ();
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["closeResult"] = close_result (*closed);
            actual["authority"] = authority_state (services, spot_id);
            if (authority_state (services, spot_id) == "Ready") {
                const auto admitted = direct_request (spot_id, services, 5);
                actual["admission"] = admitted ? "open" : "sealed";
            }
            return;
        }
        if (given.value ("closeGeneration", "") == "previous") {
            // A predecessor generation exists once the same Spot ID is closed
            // and created again; the setup Close is not part of the observation.
            if (manager_close (services, *ref).value () != true)
                throw std::runtime_error ("the setup Close did not close the first generation");
            ref = create_room (services, spot_id);
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        const auto close_ref = [&] {
            if (ref && given.value ("closeGeneration", "") == "previous") {
                if (ref->object_generation () <= 1)
                    throw std::runtime_error (
                      "the scenario needs a SpotRef whose generation has a predecessor");
                return zf::spot_ref_t (ref->spot_id (), ref->object_generation () - 1,
                                       std::string (ref->mesh_name ()), ref->node_rid ());
            }
            if (ref)
                return *ref;
            return zf::spot_ref_t (zf::spot_id_t (spot_id), 1, mesh_name,
                                   zf::node_rid_t::from_string (node_name));
        }();
        if (act == "managerClose") {
            {
                std::lock_guard lock (current ().mutex);
                current ().factory_calls = 0;
            }
            actual["result"] = close_result (manager_close (services, close_ref));
            if (given.value ("members", 0) > 0) {
                auto &manager = services.get_required<zf::spot_manager_t> ();
                const auto existing = manager.get_or_create (zf::spot_id_t (spot_id), "room")
                                        .timeout (3s)
                                        .async ()
                                        .result ();
                ASSERT_TRUE (existing);
                EXPECT_EQ (existing.value ().state, zf::spot_create_state_t::existing);
                EXPECT_EQ (existing.value ().spot.object_generation (),
                           close_ref.object_generation ());
            }
        } else if (act == "managerCloseThenManagerCloseAgain") {
            const auto first = manager_close (services, close_ref);
            actual["results"] = nlohmann::json::array (
              {first ? nlohmann::json (first.value ()) : nlohmann::json ("failure")});
            actual["authorityAfterFailure"] = authority_state (services, spot_id);
            actual["results"].push_back (close_result (manager_close (services, close_ref)));
        } else if (act == "joinAcceptedThenManagerClose") {
            const auto admitted_before = [&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted;
            }();
            std::thread join ([&] { join_member (services, spot_id, "member-" + name); });
            const bool admitted = wait_until ([&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted > admitted_before;
            });
            const auto closed = manager_close (services, close_ref);
            current ().record ("closeCompleted");
            join.join ();
            if (!admitted)
                throw std::runtime_error ("the Join was not accepted before Close");
            actual["result"] = close_result (closed);
        } else {
            throw std::runtime_error ("unsupported act: " + act);
        }
        actual["authority"] = authority_state (services, spot_id);
        if (ref && authority_state (services, spot_id) == "Ready") {
            const auto admitted = direct_request (spot_id, services, 5);
            actual["admission"] = admitted ? "open" : "sealed";
        }
    }

  public:
    std::shared_ptr<close_fault_store_t> _store;

  private:
    zf::app_t *_app;
    nlohmann::json _scenario;
};

const nlohmann::json &spot_close_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error ("Spot Close conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

const std::set<std::string> &known_scenarios ()
{
    static const std::set<std::string> names{
      "close-absent-incarnation-is-false",
      "close-other-generation-is-invalid-operation",
      "close-with-membership-is-false-and-keeps-authority",
      "close-after-accepted-join-observes-its-membership",
      "failure-before-closing-commit-keeps-authority",
      "on-closing-failure-is-diagnostic-and-cleanup-continues"};
    return names;
}

nlohmann::json run_scenario (const nlohmann::json &scenario, std::string &failure)
{
    observation_t observation;
    observed = &observation;

    const auto observe_diagnostics = [&observation] (const zf::message_flow_event_t &event) {
        if (event.outcome == zf::message_flow_outcome_t::admitted
            && event.packet_name == std::optional<std::string> (close_probe_request_t::packet_name)
            && event.detail_stage == std::optional<std::string> ("invoke_erased.post_serial")) {
            std::lock_guard lock (observation.mutex);
            if (observation.queued_request_admitted) {
                observation.queued_request_admitted->set_value ();
                observation.queued_request_admitted.reset ();
            }
        }
        if (event.result == zf::message_flow_result_t::failed) {
            std::string note =
              "flow:" + event.packet_name.value_or ("") + ":" + event.detail_result.value_or ("");
            if (event.exception) {
                try {
                    std::rethrow_exception (event.exception);
                }
                catch (const std::exception &error) {
                    note += std::string (":") + error.what ();
                }
                catch (...) {
                    note += ":non-standard-exception";
                }
            }
            std::lock_guard lock (observation.mutex);
            observation.notes.push_back (std::move (note));
        }
        if (event.packet_name == std::optional<std::string> ("spot_close")
            && event.detail_result == std::optional<std::string> ("on_closing_failed")) {
            std::lock_guard lock (observation.mutex);
            observation.diagnostics.push_back ("onClosingFailed");
        }
    };

    auto inner = std::make_shared<zf::runtime::in_memory_location_store_t> ();
    auto store = std::make_shared<close_fault_store_t> (inner);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    app.logging ().use_file ("spot-close.flow").set_min_level (zf::log_level_t::debug);
    app.add_zlink_framework ([&] (zf::zlink_framework_options_t &options) {
        if (scenario.at ("act") == "timerDuringPreparedAuthorityWrite")
            options.worker ().min_threads (1).max_threads (1);
        options.configure_dispatch ().message_flow (zf::message_flow_log_mode_t::detailed);
        zf::detail::dispatch_options_access_t::set_observer_for_tests (
          options.configure_dispatch (), observe_diagnostics);
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        options.add_route_mesh (mesh_name)
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (std::string (node_name)))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<close_entry_spot_t> ([] (zf::entry_spot_context_t context) {
              return std::make_shared<close_entry_spot_t> (std::move (context));
          })
          .add_spot_factory<close_room_spot_t> (
            "room",
            [] (zf::spot_context_t context) {
                {
                    std::lock_guard lock (current ().mutex);
                    ++current ().factory_calls;
                }
                return std::make_shared<close_room_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_spot_factory<close_other_room_spot_t> (
            "other-room",
            [] (zf::spot_context_t context) {
                return std::make_shared<close_other_room_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_instance_spot_factory<close_session_spot_t> (
            "session",
            [] (zf::instance_spot_context_t context) {
                {
                    std::lock_guard lock (current ().mutex);
                    ++current ().factory_calls;
                }
                return std::make_shared<close_session_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<member_actor_t, member_actor_factory_t> (
            "member", std::make_shared<member_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto client = std::make_unique<scenario_client_t> (app, scenario);
    client->_store = store;
    auto *runner = client.get ();
    app.add_hosted_service (std::move (client));
    (void) app.run (0, nullptr);
    runner->join ();

    failure = runner->failure;
    auto actual = runner->actual;
    observed = nullptr;
    return actual;
}

void run_and_check (const nlohmann::json &scenario)
{
    const auto name = scenario.at ("name").get<std::string> ();
    SCOPED_TRACE (name);
    std::string failure;
    const auto actual = run_scenario (scenario, failure);
    std::cout << "spot-close scenario " << name << ": " << actual.dump () << std::endl;
    if (!failure.empty ()) {
        ADD_FAILURE () << failure << ": " << actual.dump ();
        return;
    }
    const auto &expect = scenario.at ("expect");
    for (const auto &[key, value] : expect.items ()) {
        if (key == "order") {
            // The expected events appear in this relative order.
            std::vector<std::string> seen;
            for (const auto &event : actual.at ("order"))
                if (std::find (value.begin (), value.end (), event) != value.end ())
                    seen.push_back (event.get<std::string> ());
            EXPECT_EQ (value.get<std::vector<std::string>> (), seen) << actual.dump ();
            continue;
        }
        if (!actual.contains (key)) {
            ADD_FAILURE () << key << " not observed: " << actual.dump ();
            continue;
        }
        EXPECT_EQ (value, actual.at (key)) << key << ": " << actual.dump ();
    }
}

TEST (ZLinkFrameworkSpotCloseConformance, GetOrCreateWaitsForClosing)
{
    run_and_check (nlohmann::json{{"name", "get-or-create-closing"},
                                  {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}}},
                                  {"act", "getOrCreateDuringClose"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"closeSucceeded", true},
                                    {"created", true},
                                    {"newIncarnation", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, GetOrCreateClosingDeadline)
{
    run_and_check (nlohmann::json{
      {"name", "get-or-create-closing-deadline"},
      {"given", {{"authority", "Ready"}, {"onClosing", "blocks"}, {"deadlineExpires", true}}},
      {"act", "getOrCreateDuringClose"},
      {"expect", {{"deadlineExceeded", true}, {"closeSucceeded", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownJoinsExplicitCloseBeforeOwnerCleanup)
{
    run_and_check (nlohmann::json{{"name", "shutdown-joins-close"},
                                  {"given", {{"authority", "Ready"}}},
                                  {"act", "shutdownDuringClose"},
                                  {"expect",
                                   {{"completedBeforeRelease", false},
                                    {"authorityBeforeRelease", "Closing"},
                                    {"ownerDeletedBeforeRelease", false},
                                    {"closeSucceeded", true},
                                    {"deadlineExceeded", false},
                                    {"stopped", true}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, ShutdownCloseJoinUsesDrainDeadline)
{
    run_and_check (nlohmann::json{
      {"name", "shutdown-close-deadline"},
      {"given", {{"authority", "Ready"}, {"deadlineExpires", true}}},
      {"act", "shutdownDuringClose"},
      {"expect",
       {{"completedBeforeRelease", true}, {"deadlineExceeded", true}, {"stopped", false}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, RunsEveryFixtureScenario)
{
    const auto &fixture = spot_close_fixture ();
    ASSERT_EQ ("zlink.framework.spot-close", fixture.at ("fixture").get<std::string> ());
    ASSERT_EQ (1, fixture.at ("version").get<int> ());
    const auto &invariants = fixture.at ("invariants");
    EXPECT_TRUE (invariants.at ("contextCloseReturnsValue").get<bool> ());
    static_assert (
      std::is_same_v<decltype (std::declval<zf::instance_spot_context_t &> ().close ()),
                     zf::task_t<bool>>);

    for (const auto &scenario : fixture.at ("scenarios")) {
        const auto name = scenario.at ("name").get<std::string> ();
        ASSERT_TRUE (known_scenarios ().contains (name)) << "unknown scenario " << name;
        run_and_check (scenario);
    }
}

// Closing requests without activation intent never execute the old handler.
TEST (ZLinkFrameworkSpotCloseConformance, ClosingWithoutIntentReturnsNotFound)
{
    const auto &branches = spot_close_fixture ().at ("closeBranches");
    const auto branch = std::find_if (branches.begin (), branches.end (), [] (const auto &item) {
        return item.at ("name") == "closing-message-without-intent";
    });
    ASSERT_NE (branch, branches.end ());
    ASSERT_FALSE (branch->at ("given").at ("messageIntent").get<bool> ());
    ASSERT_EQ ("Closing", branch->at ("given").at ("authority"));
    ASSERT_EQ (0, branch->at ("expect").at ("newHandlerCalls"));
    auto scenario = nlohmann::json{
      {"name", branch->at ("name")},
      {"given", {{"runtime", "Ready"}, {"authority", "Ready"}, {"onClosing", "blocks"}}},
      {"act", "directRequestDuringClose"},
      {"expect",
       {{"authorityDuringClose", "Closing"},
        {"result", branch->at ("expect").at ("messageTerminal")},
        {"handlerCalls", branch->at ("expect").at ("oldHandlerCalls")},
        {"factoryCalls", branch->at ("expect").at ("factoryCalls")}}}};
    run_and_check (scenario);
}

TEST (ZLinkFrameworkSpotCloseConformance, NoPendingIntentReleasesAuthority)
{
    const auto &branches = spot_close_fixture ().at ("closeBranches");
    const auto branch = std::find_if (branches.begin (), branches.end (), [] (const auto &item) {
        return item.at ("name") == "release-without-pending-intent";
    });
    ASSERT_NE (branch, branches.end ());
    ASSERT_EQ ("Serving", branch->at ("given").at ("host"));
    ASSERT_FALSE (branch->at ("given").at ("pendingIntent").get<bool> ());
    ASSERT_EQ (0, branch->at ("expect").at ("newHandlerCalls"));
    run_and_check (nlohmann::json{{"name", branch->at ("name")},
                                  {"given", {{"runtime", "Ready"}, {"authority", "Ready"}}},
                                  {"act", "managerClose"},
                                  {"expect",
                                   {{"result", true},
                                    {"authority", branch->at ("expect").at ("authority")},
                                    {"handlerCalls", branch->at ("expect").at ("oldHandlerCalls")},
                                    {"factoryCalls", branch->at ("expect").at ("factoryCalls")},
                                    {"onClosingCalls", 1},
                                    {"order", branch->at ("expect").at ("order")}}}});
}

TEST (ZLinkFrameworkSpotCloseConformance, CloseCancelsTimerWithoutExecutingAfterOnClosing)
{
    const auto &fixture = spot_close_fixture ();
    ASSERT_TRUE (fixture.at ("invariants").at ("unexecutedTimersCancelled").get<bool> ());
    run_and_check (nlohmann::json{{"name", "close-cancels-timer-without-old-tick-after-on-closing"},
                                  {"given",
                                   {{"runtime", "Ready"},
                                    {"authority", "Ready"},
                                    {"roomTimer", true},
                                    {"onClosing", "blocks"}}},
                                  {"act", "directRequestDuringClose"},
                                  {"expect",
                                   {{"result", "NotFound"},
                                    {"authority", "Missing"},
                                    {"timerDisposedAfterClose", true},
                                    {"timerCallsAfterOnClosing", 0},
                                    {"onClosingCalls", 1}}}});
}

// The Store write entry controls the CAS boundary; admission evidence controls
// the queued request boundary without a test sleep.
TEST (ZLinkFrameworkSpotCloseConformance, QueuedRequestWaitsForClosingCommit)
{
    const auto scenarios = nlohmann::json::parse (R"json([
      {"name": "queued-request-after-closing-conflict-executes",
       "given": {"runtime": "Ready", "authority": "Ready", "closingCommit": "heldThenConflict"},
       "act": "directRequestDuringHeldClosingCommit",
       "expect": {"handlerCallsBeforeCommit": 1, "result": "handlerReply", "closeResult": "Unavailable",
                  "authority": "Ready", "admission": "open"}},
      {"name": "queued-request-after-closing-success-is-not-found",
       "given": {"runtime": "Ready", "authority": "Ready", "closingCommit": "held"},
       "act": "directRequestDuringHeldClosingCommit",
       "expect": {"handlerCallsBeforeCommit": 1, "result": "NotFound", "closeResult": true,
                  "authority": "Missing", "handlerCalls": 1}}
    ])json");
    for (const auto &scenario : scenarios)
        run_and_check (scenario);
}

// Two Actor Joins that move members between two User Spots in opposite
// directions at the same time complete (Spot-Actor membership: Join on the
// lifecycle lane; handler turn and execution gate §7).
TEST (ZLinkFrameworkSpotCloseConformance, OppositeJoinsBetweenTwoSpotsComplete)
{
    run_and_check (nlohmann::json::parse (R"json(
      {"name": "opposite-joins-complete",
       "given": {"runtime": "Ready", "authority": "Ready"},
       "act": "crossJoins",
       "expect": {"crossJoins": "completed"}})json"));
}

TEST (ZLinkFrameworkSpotCloseConformance, TimerWaitsForReadyStorePublication)
{
    for (const bool instance : {false, true}) {
        run_and_check (nlohmann::json{
          {"name", instance ? "instance-timer-ready-publication" : "user-timer-ready-publication"},
          {"given", {{"authority", "Missing"}, {"instance", instance}, {"heldStage", "ready"}}},
          {"act", "timerDuringPreparedAuthorityWrite"},
          {"expect",
           {{"timerCallsWhileHeld", 0},
            {"handlerCallsWhileHeld", 0},
            {"timerCallsAfterPublication", 1},
            {"created", true}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance, InstanceReadyTimerRunsWhileDurableTerminalDefersClose)
{
    for (const auto *stage : {"cursor", "pointer"}) {
        run_and_check (
          nlohmann::json{{"name", std::string ("instance-timer-first-terminal-") + stage},
                         {"given",
                          {{"authority", "Missing"},
                           {"instance", true},
                           {"heldStage", stage},
                           {"timerCloses", true}}},
                         {"act", "timerDuringPreparedAuthorityWrite"},
                         {"expect",
                          {{"timerCallsWhileHeld", 1},
                           {"handlerCallsWhileHeld", 1},
                           {"timerCloseRequestedWhileHeld", true},
                           {"timerCloseCompletedWhileHeld", false},
                           {"timerCloseResult", true},
                           {"timerCallsAfterPublication", 1},
                           {"created", true}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance,
      InstanceDurableTerminalFailureDiscardsPreparationAndCloseIsFalse)
{
    for (const auto *stage : {"cursor", "pointer"}) {
        run_and_check (
          nlohmann::json{{"name", std::string ("instance-timer-terminal-failure-") + stage},
                         {"given",
                          {{"authority", "Missing"},
                           {"instance", true},
                           {"heldStage", stage},
                           {"timerCloses", true},
                           {"terminalConflict", true}}},
                         {"act", "timerDuringPreparedAuthorityWrite"},
                         {"expect",
                          {{"timerCallsWhileHeld", 1},
                           {"handlerCallsWhileHeld", 1},
                           {"timerCloseRequestedWhileHeld", true},
                           {"timerCloseCompletedWhileHeld", false},
                           {"timerCloseResult", false},
                           {"created", false},
                           {"onClosingCalls", 0}}}});
    }
}

TEST (ZLinkFrameworkSpotCloseConformance, TimerCloseSucceedsAfterReadyPublication)
{
    run_and_check (nlohmann::json{
      {"name", "timer-close-after-ready-publication"},
      {"given", {{"authority", "Missing"}, {"heldStage", "ready"}, {"timerCloses", true}}},
      {"act", "timerDuringPreparedAuthorityWrite"},
      {"expect",
       {{"timerCallsWhileHeld", 0},
        {"timerCallsAfterPublication", 1},
        {"timerCloseResult", true},
        {"created", true}}}});
}

} // namespace
