/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>
#include "../support/location_runtime_outage_fixture.hpp"

namespace zlink::framework::runtime
{
namespace
{
TEST_P (StoreOutageRetention, ConfirmedActorLossPerActorDefault)
{
    registered_owner (false, false, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossPerActorRetention)
{
    registered_owner (false, true, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossSpotWideDefault)
{
    registered_owner (true, false, 1);
}

TEST_P (StoreOutageRetention, ConfirmedActorLossSpotWideRetention)
{
    registered_owner (true, true, 1);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossPerActorDefault)
{
    registered_owner (false, false, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossPerActorRetention)
{
    registered_owner (false, true, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossSpotWideDefault)
{
    registered_owner (true, false, 2);
}

TEST_P (StoreOutageRetention, ConfirmedSpotLossSpotWideRetention)
{
    registered_owner (true, true, 2);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossPerActorDefault)
{
    registered_owner (false, false, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossPerActorRetention)
{
    registered_owner (false, true, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossSpotWideDefault)
{
    registered_owner (true, false, 3);
}

TEST_P (StoreOutageRetention, ConfirmedActorOwnerGenerationLossSpotWideRetention)
{
    registered_owner (true, true, 3);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossPerActorDefault)
{
    registered_owner (false, false, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossPerActorRetention)
{
    registered_owner (false, true, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossSpotWideDefault)
{
    registered_owner (true, false, 4);
}

TEST_P (StoreOutageRetention, ConfirmedSpotOwnerGenerationLossSpotWideRetention)
{
    registered_owner (true, true, 4);
}

TEST_P (StoreOutageRetention, PendingStopAndOwnershipLossHaveDifferentResults)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true})
        for (auto status : {status_t::stopped, status_t::ownership_lost}) {
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            auto gate = shared ? spot : actor;
            int handlers = 0, completed = 0;
            serial_work_options_t pending;
            pending.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (status == status_t::stopped ? framework_error_kind_t::shutting_down
                                                       : framework_error_kind_t::not_found,
                           result.error_kind ());
                ++completed;
            };
            ASSERT_TRUE (gate->try_post ("pending", [&] { ++handlers; }, pending));
            gate->cancel_pending (status);
            scheduler.run_all ();
            EXPECT_EQ (0, handlers);
            EXPECT_EQ (1, completed);
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
        }
}

TEST_P (StoreOutageRetention, TerminalCauseSurvivesParentDiscardAndLaterPolicy)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true})
        for (bool stopped_first : {false, true}) {
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            int handlers = 0, completed = 0;
            serial_work_options_t pending;
            pending.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (stopped_first ? framework_error_kind_t::shutting_down
                                         : framework_error_kind_t::not_found,
                           result.error_kind ());
                ++completed;
            };
            ASSERT_TRUE (actor->try_post ("pending", [&] { ++handlers; }, pending));
            if (stopped_first)
                actor->cancel_pending ();
            ASSERT_TRUE (spot->fence_execution ());
            // A subsequent retention/Ready publication cannot replace the terminal fence.
            spot->set_execution_policy ([] { return true; });
            EXPECT_EQ (status_t::ownership_lost, spot->execution_status ());
            EXPECT_FALSE (spot->execution_allowed ());
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
            EXPECT_EQ (0, handlers);
            EXPECT_EQ (1, completed);
        }
}

TEST_P (StoreOutageRetention, RecoveryMustWaitForNormalExpiryBeforeNewClaim)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "normal-expiry-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto old_token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    ASSERT_LT (clock_now.load (), initial->last_successful_lease->started_at
                                    + options.owner_lease_ttl + options.owner_lease_fencing_margin);
    store ().release_owner_lease (old_token).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    // Approved D17: Missing alone cannot authorize a claim before lifetime + margin.
    const auto after = lease_access::snapshot (runtime)->token;
    ASSERT_TRUE (after);
    EXPECT_EQ (old_token.lease_generation, after->lease_generation);
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryCanClaimAfterNormalExpiryAndExactMissing)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "expired-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto old_token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin);
    store ().release_owner_lease (old_token).result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (old_token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto after = lease_access::snapshot (runtime)->token;
    ASSERT_TRUE (after);
    EXPECT_NE (after->lease_generation, old_token.lease_generation);
}


TEST_P (StoreOutageRetention, RecoveryMissingBeforeExpiryBoundaryKeepsOriginalToken)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "boundary-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    store ().release_owner_lease (token).result ().value ();
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin - 1ms);
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
    EXPECT_FALSE (episode->discarded.load ());
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryDifferentTokenDiscardsWithoutAdoptingIt)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "changed-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    store ().release_owner_lease (token).result ().value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      store ().claim_owner_lease (token.owner_id, options.owner_lease_ttl).result ().value ());
    ASSERT_NE (token.lease_generation, replacement.token.lease_generation);
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (episode->discarded.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->healthy);
    const auto exact =
      std::get<owner_lease_found_t> (store ().read_owner_lease (token.owner_id).result ().value ());
    EXPECT_EQ (replacement.token.lease_generation, exact.token.lease_generation);
}

TEST_P (StoreOutageRetention, RecoveryFirstCommunicationFailureDiscardsExactDifferentToken)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "changed-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    store ().release_owner_lease (token).result ().value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      store ().claim_owner_lease (token.owner_id, options.owner_lease_ttl).result ().value ());
    ASSERT_NE (token.lease_generation, replacement.token.lease_generation);
    provider.fail_next_read = memory.fail_renew = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    provider.fail_next_read = memory.fail_renew = false;
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (episode->discarded.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->healthy);
    const auto exact =
      std::get<owner_lease_found_t> (store ().read_owner_lease (token.owner_id).result ().value ());
    EXPECT_EQ (replacement.token.lease_generation, exact.token.lease_generation);
}

TEST_P (StoreOutageRetention, RecoveryFailedExactReadDoesNotAuthorizeClaim)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "read-failure-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    const auto token = *initial->token;
    advance (5s);
    lease_access::communication_failure (runtime);
    store ().release_owner_lease (token).result ().value ();
    move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
             + options.owner_lease_fencing_margin);
    provider.fail = memory.fail = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    provider.fail = memory.fail = false;
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
    lease_access::heartbeat (runtime, clock_now.load ());
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_NE (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeStillClaimsImmediately)
{
    registered_owner (false, false, -60);
}

TEST_P (StoreOutageRetention, NonRetainedSpotTimerCannotEnterOrStartAfterLeaseDeadline)
{
    location_runtime_t runtime (store (), options, "timer-outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);

    auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("timer-outage");
    const auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    location_lifecycle_t lifecycle (runtime);
    detail::spot_node_runtime_t (node).bind_location_lifecycle (lifecycle);

    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->execution_mode = user_spot_execution_mode_t::per_actor;
    serializer_registry_t serializers;
    context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    context->channel_runtime->serializers = &serializers;
    context->channel_runtime->dispatch.message_flow (message_flow_log_mode_t::normal);
    const auto executor = context->ensure_spot_serial_executor ();
    ASSERT_TRUE (executor);
    // No steady-Ready confirmation, as with a timer whose activation is not Store-backed.
    const auto queue = executor->timer_queue ("lease");
    auto timer = std::make_shared<detail::timer_state_t> ();
    timer->name = "lease";
    timer->period = 1ms;
    timer->serial_queue = queue;
    const auto handler = std::make_shared<int> (0);
    timer->handler_instance = handler;
    int calls = 0;
    timer->handler_invoker = [&] (void *, void *, serializer_registry_t &, const timer_tick_t &) {
        ++calls;
        return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
    };

    detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
    scheduler->run_all ();
    ASSERT_EQ (1, calls) << "healthy control";
    detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
    ASSERT_TRUE (timer->pending_fire);

    advance (5s);
    provider.fail = memory.fail = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto lease = lease_access::snapshot (runtime);
    ASSERT_TRUE (lease->outage);
    ASSERT_TRUE (lease->admission_deadline);
    move_to (*lease->admission_deadline + 1ms);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_FALSE (runtime.owner_lease_usable ());
    scheduler->run_all ();
    EXPECT_EQ (1, calls) << "a callback accepted before expiry must not start after expiry";

    const auto pre_expiry_claims = context->callback_depth;
    detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
    EXPECT_EQ (pre_expiry_claims, context->callback_depth) << "a new tick must not claim admission";
    scheduler->run_all ();
    EXPECT_EQ (1, calls) << "a new tick must not run after expiry";
}

TEST_P (StoreOutageRetention, NormallyExpiredOwnerRepublishesBeforeAdmission)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    test::manual_internal_scheduler_t scheduler;
    options.owner_lease_ttl = 500ms;
    options.owner_lease_fencing_margin = 5ms;
    location_runtime_t runtime (store (), options, "normal-expiry-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto old = *runtime.current_owner_token ();
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "normal-expiry";
    descriptor.rid = zlink::routing_id_t::from ("expiry-node");
    descriptor.endpoint = "tcp://127.0.0.1:39000";
    descriptor.security_identity = "default";
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.owner_id = old.owner_id;
    descriptor.lease_generation = old.lease_generation;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_role = object_role_t::server;
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::snapshot, true, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 1}};
    descriptor.capacity.actors.limit = 1;
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
    ASSERT_EQ (location_write_status_t::stored,
               store ()
                 .update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);

    task_completion_source_t<void> submitted;
    auto release = std::make_shared<task_completion_source_t<void>> ();
    runtime
      .set_descriptor_publisher ([&] (auto intent, auto, auto) {
          EXPECT_EQ (location_write_intent_t::renew, intent);
          submitted.complete (result_t<void>::success ());
          return continue_task<bool> (release->task (), [&] {
              auto next = descriptor;
              ++next.descriptor_revision;
              next.lease_generation = lease_access::snapshot (runtime)->token->lease_generation;
              auto write = store ().update_mesh_node (next, location_write_intent_t::renew);
              return detail::project_task_terminal<bool> (
                write, [] (const result_t<location_write_result_t> &write) {
                    return result_t<bool>::success (
                      write && write.value ().status == location_write_status_t::stored);
                });
          });
      })
      .result ()
      .value ();
    auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
    auto fresh = std::make_shared<serial_execution_queue_t> (scheduler);
    actor->set_execution_policy (runtime.execution_policy ());
    fresh->set_execution_policy (runtime.execution_policy ());
    runtime
      .on_execution_discard ([actor] (auto policy) { actor->cancel_pending (policy (*actor)); })
      .result ()
      .value ();
    std::function<void ()> finish_actor;
    auto stale = location_write_status_t::stored;
    ASSERT_TRUE (actor->try_post_async ("active-before-normal-expiry", [&] (auto complete) {
        finish_actor = [&, complete = std::move (complete)] () mutable {
            auto mutation = descriptor;
            mutation.descriptor_revision += 2;
            stale = store ()
                      .update_mesh_node (mutation, location_write_intent_t::renew)
                      .result ()
                      .value ()
                      .status;
            complete ([] {});
        };
    }));
    scheduler.run_all ();
    ASSERT_TRUE (finish_actor);
    const auto first = lease_access::snapshot (runtime);
    std::this_thread::sleep_until (*first->renewed_at + options.owner_lease_ttl);
    advance (options.owner_lease_ttl + options.owner_lease_fencing_margin);
    auto renewal = lease_access::heartbeat_async (runtime, clock_now.load ());
    const bool attempted = bool (submitted.task ().result_for (auto_connect_wait));
    EXPECT_TRUE (attempted);
    const auto next = lease_access::snapshot (runtime);
    ASSERT_TRUE (next->token);
    EXPECT_EQ (old.owner_id, next->token->owner_id);
    EXPECT_NE (old.lease_generation, next->token->lease_generation);
    EXPECT_FALSE (next->outage);
    EXPECT_FALSE (runtime.owner_lease_usable ());
    EXPECT_FALSE (runtime.current_owner_token ());
    EXPECT_FALSE (fresh->try_post ("message-before-publication", [] {}));
    object_reserve_request_t actor_request;
    actor_request.key = {placement_object_kind_t::actor, "fresh-actor"};
    actor_request.intent.stable_type = "player";
    actor_request.target = {descriptor.mesh_name,
                            node_rid_t::from_string (descriptor.rid.to_string ()),
                            descriptor.lifecycle_generation, *next->token};
    actor_request.creating_payload = {std::byte{0}};
    actor_request.capacity_bundle.actor_slots = 1;
    auto spot_request = actor_request;
    spot_request.key = {placement_object_kind_t::user_spot, "fresh-spot"};
    spot_request.intent.stable_type = "room";
    spot_request.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
    for (const auto &request : {actor_request, spot_request}) {
        const auto blocked = store ().reserve (request).result ();
        EXPECT_FALSE (blocked);
        if (!blocked)
            EXPECT_EQ (framework_error_kind_t::unavailable, blocked.error_kind ());
    }
    EXPECT_EQ (status_t::ownership_lost, actor->execution_status ());

    release->complete (result_t<void>::success ());
    ASSERT_TRUE (renewal.result_for (auto_connect_wait));
    renewal.result ().value ();
    auto visible = store ().list_mesh_nodes (descriptor.mesh_name).result ().value ().items;
    ASSERT_EQ (1u, visible.size ());
    EXPECT_EQ (next->token->lease_generation, visible.front ().lease_generation);
    EXPECT_GT (visible.front ().descriptor_revision, descriptor.descriptor_revision);
    EXPECT_TRUE (runtime.owner_lease_usable ());
    for (const auto &request : {actor_request, spot_request}) {
        const auto accepted = store ().reserve (request).result ();
        EXPECT_TRUE (accepted);
        if (accepted)
            EXPECT_TRUE (std::holds_alternative<object_reserved_t> (accepted.value ()));
    }
    EXPECT_TRUE (fresh->try_post ("message-after-publication", [] {}));
    EXPECT_EQ (status_t::ownership_lost, actor->execution_status ());
    EXPECT_TRUE (finish_actor);
    std::exchange (finish_actor, {}) ();
    EXPECT_EQ (location_write_status_t::ignored_stale, stale);
    scheduler.run_all ();
    actor->cancel_pending ();
    fresh->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeDiscardsWithMainApi)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    location_runtime_t runtime (store (), options, "stale-outside-discard-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;

    auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> (
      "stale-outside-discard-node");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->serial_executor = scheduler;
    context->spot_id = spot_id_t ("stale-outside-discard-spot");
    context->spot_name = "room";
    context->mesh_name = "stale-outside-discard-node";
    context->node_rid = node_rid_t::from_string ("stale-outside-discard-node");
    context->execution_mode = user_spot_execution_mode_t::per_actor;
    context->object_generation = 1;
    context->authority_owner_generation = 1;
    context->actor_count = 1;
    auto close_finished = std::make_shared<task_completion_source_t<bool>> ();
    context->merged_close_results.push_back (
      [close_finished] (result_t<bool> result) { close_finished->complete (std::move (result)); });
    node->spot_contexts_by_id.emplace ("stale-outside-discard-spot",
                                       detail::spot_context_access_t::create (context));
    node->actor_types_by_id["early"] = "player";
    node->actor_spot_ids["player:early"] = context->spot_id;
    node->actor_generations["player:early"] = 1;
    node->actor_instances["player:early"] = std::make_shared<int> (0);

    detail::spot_node_runtime_t owner (node);
    location_lifecycle_t lifecycle (runtime);
    owner.bind_location_lifecycle (lifecycle);
    runtime
      .on_execution_discard ([&] (auto policy) {
          // Keep the callback void for origin/main; the branch also returns a task here.
          static_cast<void> (owner.request_location_execution_discard (std::move (policy)));
      })
      .result ()
      .value ();

    auto executor = context->ensure_spot_serial_executor ();
    auto spot = executor->spot_queue ();
    auto actor = executor->actor_executor ("player:early")->queue ();
    spot->confirm_execution_ready (clock_now.load ());
    actor->confirm_execution_ready (clock_now.load ());
    scheduler->run_all ();
    ASSERT_EQ (status_t::allowed, actor->execution_status ());

    // A spot timer keeps its own queue; the discard must stop it as well.
    auto timer_queue = std::make_shared<serial_execution_queue_t> (*scheduler);
    timer_queue->attach_spot_gate (spot);
    timer_queue->set_execution_policy (runtime.execution_policy ());
    timer_queue->confirm_execution_ready (clock_now.load ());
    serializer_registry_t serializers;
    context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    context->channel_runtime->serializers = &serializers;
    auto timer = std::make_shared<detail::timer_state_t> ();
    timer->name = "stale-outside-discard-timer";
    timer->serial_queue = timer_queue;
    auto handler = std::make_shared<int> (0);
    timer->handler_instance = handler;
    int actor_calls = 0, timer_calls = 0;
    timer->handler_invoker = [&] (void *, void *, serializer_registry_t &, const timer_tick_t &) {
        ++timer_calls;
        return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
    };
    context->timers.push_back (timer);
    scheduler->run_all ();

    ASSERT_TRUE (actor->try_post ("before-stale", [&] { ++actor_calls; }));
    detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
    move_to (*lease_access::snapshot (runtime)->admission_deadline + 1ms);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_EQ (status_t::stopped, actor->execution_status ());

    store ().release_owner_lease (token).result ().value ();
    auto renewal = lease_access::heartbeat_async (runtime, clock_now.load ());
    ASSERT_TRUE (renewal.result_for (auto_connect_wait));
    renewal.result ().value ();
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_NE (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);

    // A lane fence alone does not wait for offload cleanup, including Location release.
    // Main never starts this Close, so keep the wait bounded and continue to teardown.
    node->lane.run_sync ([] {});
    auto closed = close_finished->task ();
    const bool close_completed = scheduler->run_until (closed, auto_connect_wait);
    EXPECT_TRUE (close_completed);
    if (close_completed) {
        EXPECT_TRUE (closed.result ().value ());
        EXPECT_TRUE (timer->disposed.load ());
    }
    scheduler->run_all ();
    EXPECT_EQ (0, actor_calls);
    EXPECT_EQ (0, timer_calls);
    EXPECT_EQ (status_t::ownership_lost, actor->execution_status ());
    EXPECT_EQ (status_t::ownership_lost, spot->execution_status ());
    EXPECT_EQ (status_t::ownership_lost, timer_queue->execution_status ());

    runtime.stop ();
    std::vector<task_t<void>> completions;
    executor->close (&completions);
    for (auto &completion : completions) {
        ASSERT_TRUE (scheduler->run_until (completion, auto_connect_wait));
        completion.result ().value ();
    }
    scheduler->retire ();
    context->node.reset ();
    node->spot_contexts_by_id.clear ();
}

TEST_P (StoreOutageRetention, DiscardRequestPostsCloseBeforeFollowingLaneTurn)
{
    auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("discard-post");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->serial_executor = scheduler;
    context->spot_id = spot_id_t ("discard-post-spot");
    context->spot_name = "room";
    context->mesh_name = "discard-post";
    context->node_rid = node_rid_t::from_string ("discard-post-node");
    node->spot_contexts_by_id.emplace ("discard-post-spot",
                                       detail::spot_context_access_t::create (context));
    auto gate = context->ensure_spot_serial_executor ()->spot_queue ();
    auto timer_queue = std::make_shared<serial_execution_queue_t> (*scheduler);
    timer_queue->attach_spot_gate (gate);
    std::atomic<bool> admission_open = true;
    timer_queue->set_execution_policy ([&] { return admission_open.load (); });
    auto timer = std::make_shared<detail::timer_state_t> ();
    timer->name = "discard-post-timer";
    timer->serial_queue = timer_queue;
    context->timers.push_back (timer);
    scheduler->run_all ();
    int timer_calls = 0;
    ASSERT_TRUE (timer_queue->try_post ("before-discard", [&] { ++timer_calls; }));
    admission_open = false;
    auto finished = std::make_shared<task_completion_source_t<bool>> ();
    context->merged_close_results.push_back (
      [finished] (result_t<bool> result) { finished->complete (std::move (result)); });
    std::latch entered (1), release (1);
    auto held = std::async (std::launch::async, [&] {
        node->lane.run_sync ([&] {
            entered.count_down ();
            release.wait ();
        });
    });
    entered.wait ();
    detail::spot_node_runtime_t owner (node);
    auto applied = owner.request_location_execution_discard (
      [] (const auto &) { return serial_execution_queue_t::execution_status_t::ownership_lost; });
    EXPECT_FALSE (applied.await_ready ());
    EXPECT_FALSE (gate->closed ());
    // This turn is already queued before preparation can resume off the busy lane.
    // It must observe the actual close, rather than only a preparation lookup.
    EXPECT_TRUE (node->lane.try_post ([&] {
        EXPECT_TRUE (gate->closed ());
        EXPECT_TRUE (timer_queue->closed ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   gate->execution_status ());
    }));
    release.count_down ();
    held.get ();
    node->lane.run_sync ([] {});
    ASSERT_TRUE (applied.result_for (auto_connect_wait));
    EXPECT_TRUE (applied.result ());
    EXPECT_FALSE (finished->task ().await_ready ());
    EXPECT_FALSE (timer->disposed.load ());
    admission_open = true;
    auto closed = finished->task ();
    ASSERT_TRUE (scheduler->run_until (closed, auto_connect_wait));
    EXPECT_TRUE (closed.result ().value ());
    EXPECT_EQ (0, timer_calls);
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               timer_queue->execution_status ());
    scheduler->retire ();
    context->node.reset ();
    node->spot_contexts_by_id.clear ();
}

TEST_P (StoreOutageRetention, DiscardRequestRejectsClosedLane)
{
    auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("discard-closed");
    node->lane.close ();
    auto applied = detail::spot_node_runtime_t (node).request_location_execution_discard (
      [] (const auto &) { return serial_execution_queue_t::execution_status_t::ownership_lost; });
    ASSERT_TRUE (applied.result_for (auto_connect_wait));
    const auto result = applied.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeDiscardCanEnterLeaseLane)
{
    location_runtime_t runtime (store (), options, "discard-lane-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    unsigned discards = 0;
    runtime
      .on_execution_discard ([&] (auto) {
          return lease_access::lane (runtime).run_task ([&] {
              ++discards;
              EXPECT_FALSE (lease_access::snapshot (runtime)->token);
              EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
          });
      })
      .result ()
      .value ();
    store ().release_owner_lease (token).result ().value ();
    lease_access::heartbeat (runtime, clock_now.load ());
    EXPECT_EQ (1u, discards);
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_NE (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeDiscardsSpotWideBeforeClaim)
{
    registered_owner (true, false, -60);
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeDiscardsRetainedPerActorBeforeClaim)
{
    registered_owner (false, true, -60);
}

TEST_P (StoreOutageRetention, StaleOutsideEpisodeDiscardsRetainedSpotWideBeforeClaim)
{
    registered_owner (true, true, -60);
}

TEST_P (StoreOutageRetention, DiscardedCurrentOwnerCleansAuthorityWithoutReclaimWait)
{
    registered_owner (false, false, -61);
}

TEST_P (StoreOutageRetention, StaleRenewStartedBeforeOutageCannotClaimEarly)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "pending-stale-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    advance (5s);
    provider.hold_read = memory.hold_renew = true;
    lease_access::heartbeat (runtime, clock_now.load (), [&] {
        lease_access::communication_failure (runtime);
        provider.hold_read = memory.hold_renew = false;
        provider.resume.complete (result_t<void>::success ());
        memory.resume.complete (result_t<void>::success ());
    });
    ASSERT_TRUE (lease_access::snapshot (runtime)->token);
    EXPECT_EQ (token.lease_generation, lease_access::snapshot (runtime)->token->lease_generation);
    ASSERT_TRUE (lease_access::snapshot (runtime)->outage);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage->discarded.load ());
    EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
      store ().read_owner_lease (token.owner_id).result ().value ()));
}

TEST_P (StoreOutageRetention, RecoveryExactLeaseMustBeExpiredAsWellAsNormalLifetimeElapsed)
{
    for (bool expired : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (store (), options, expired ? "expired-exact" : "live-exact");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        const auto initial = lease_access::snapshot (runtime);
        const auto token = *initial->token;
        auto exact = std::get<owner_lease_found_t> (
          store ().read_owner_lease (token.owner_id).result ().value ());
        const auto lifetime = expired ? 0ms : 1ms;
        exact.lease_expires_at = exact.store_now + lifetime;
        std::optional<store_found_t> raw;
        if (GetParam ()) {
            raw = std::get<store_found_t> (
              provider.inner.read ({std::string ("owner-lease") + '\0' + token.owner_id})
                .result ()
                .value ());
            raw->value.expires_at = raw->value.store_now + lifetime;
        }
        advance (5s);
        lease_access::communication_failure (runtime);
        store ().release_owner_lease (token).result ().value ();
        move_to (initial->last_successful_lease->started_at + options.owner_lease_ttl
                 + options.owner_lease_fencing_margin);
        if (GetParam ())
            provider.injected_owner_reads = {store_missing_t{exact.store_now}, *raw};
        else
            memory.injected_owner_reads = {exact};
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_EQ (expired, token.lease_generation
                              != lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_TRUE (provider.injected_owner_reads.empty ());
        EXPECT_TRUE (memory.injected_owner_reads.empty ());
        const auto actual = store ().read_owner_lease (token.owner_id).result ().value ();
        EXPECT_EQ (!expired, std::holds_alternative<owner_lease_missing_t> (actual));
    }
}
} // namespace
} // namespace zlink::framework::runtime
