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
TEST (StoreOutageRetentionWait, BoundedTerminalWaitReportsMissingAndCompletedSignals)
{
    test::manual_internal_scheduler_t scheduler;
    task_completion_source_t<void> source;
    auto terminal = source.task ();
    EXPECT_FALSE (scheduler.run_until (terminal, 0ms));
    ASSERT_TRUE (
      scheduler.try_submit_internal ([&] { source.complete (result_t<void>::success ()); }));
    EXPECT_TRUE (scheduler.run_until (terminal, auto_connect_wait));
}
} // namespace

void StoreOutageRetention::discard_retained (bool shared, bool retained, bool rejoin)
{
    options.store_outage_retention = retained;
    location_runtime_t runtime (store (), options, "discard-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    location_lifecycle_t lifecycle (runtime);
    auto builder =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("discard-mesh");
    builder->routing_id = zlink::routing_id_t::from ("discard-node");
    builder->core_context = std::make_shared<zlink::context_t> ();
    auto node = builder->spot_state;
    node->snapshot.routing_id = builder->routing_id;
    node->snapshot.actor_types.push_back ("player");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    detail::spot_node_runtime_t owner (node);
    owner.bind_location_lifecycle (lifecycle);
    std::map<std::string, std::weak_ptr<void>> instances;
    int handlers = 0, completed = 0;
    std::map<std::string, int> starts;
    serial_work_options_t pending;
    std::shared_ptr<detail::timer_state_t> timer;
    detail::mesh_node_runtime_t mesh (builder);
    mesh.start ().result ().value ();
    service_collection_t services;
    auto gateway_state = std::make_shared<detail::actor_gateway_state_t> ();
    services.add_singleton<detail::actor_gateway_runtime_t> (
      std::make_unique<detail::actor_gateway_runtime_t> (gateway_state));
    auto service_provider = services.build_provider ();
    node->root_services = service_provider;
    auto &gateway = service_provider.get_required<detail::actor_gateway_runtime_t> ();
    std::vector<std::shared_ptr<detail::spot_context_state_t>> contexts;
    // Close settles on the offload worker after its lane turn erases the context entry.
    std::vector<std::shared_ptr<std::atomic<bool>>> close_settled;
    const auto add_context = [&] (std::string id) {
        auto context = std::make_shared<detail::spot_context_state_t> ();
        auto settled = std::make_shared<std::atomic<bool>> (false);
        context->merged_close_results.push_back (
          [settled] (result_t<bool>) mutable { settled->store (true); });
        close_settled.push_back (settled);
        context->node = node;
        context->lane_owner = node;
        context->serial_executor = scheduler;
        context->spot_id = spot_id_t (id);
        context->spot_name = "room";
        context->mesh_name = "discard-mesh";
        context->node_rid = node_rid_t::from_string ("discard-node");
        context->execution_mode =
          shared ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
        contexts.push_back (context);
        node->spot_contexts_by_id.emplace (id, detail::spot_context_access_t::create (context));
        return context;
    };
    auto spot = add_context ("retained-spot");
    auto spot_finished = std::make_shared<task_completion_source_t<bool>> ();
    spot->merged_close_results.push_back ([spot_finished] (result_t<bool> result) mutable {
        spot_finished->complete (std::move (result));
    });
    auto entry = add_context ("entry");
    entry->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    std::map<std::string, std::shared_ptr<serial_execution_queue_t>> gates;
    const auto add_actor = [&] (std::string id, auto context, bool ready) {
        auto key = "player:" + id;
        auto actor =
          ::zlink::framework::detail::actor_ref_access_t::make (context->node_rid, "player", id, 1);
        node->actor_types_by_id[id] = "player";
        node->actor_spot_ids[key] = context->spot_id;
        node->actor_generations[key] = 1;
        node->actor_instances[key] = std::make_shared<int> (1);
        instances[id] = node->actor_instances[key];
        ++context->actor_count;
        stateful::object_ref_t object{
          stateful::object_kind_t::actor, id, 1, 1, "discard-mesh", "discard-node"};
        auto creating = mesh.native_node ().create_reserved_actor_async ("player", object);
        EXPECT_TRUE (scheduler->run_until (creating));
        creating.result ().value ();
        EXPECT_TRUE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor, id));
        auto queue = context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
        if (ready)
            queue->confirm_execution_ready (clock_now.load ());
        gates.emplace (id, queue);
        gateway.bind_session_sink (actor,
                                   [] (std::string, stream_codec_t, const zlink::message_t &) {
                                       return task_t<void> (result_t<void>::success ());
                                   });
    };
    auto late_spot = add_context ("late-spot");
    auto creating_spot = add_context ("creating-spot");
    auto closing_spot = add_context ("closing-spot");
    auto relocating_spot = add_context ("relocating-spot");
    for (const auto &excluded : {closing_spot, relocating_spot}) {
        auto queue = excluded->ensure_spot_serial_executor ()->spot_queue ();
        queue->confirm_execution_ready (clock_now.load ());
        queue->invalidate_execution_ready ();
    }
    add_actor ("early-member", late_spot, true);
    add_actor ("other-member", late_spot, false);
    auto late_spot_gate = late_spot->ensure_spot_serial_executor ()->spot_queue ();
    auto spot_gate = spot->ensure_spot_serial_executor ()->spot_queue ();
    spot_gate->confirm_execution_ready (clock_now.load ());
    add_actor ("member", spot, true);
    add_actor ("late-member", spot, false);
    add_actor ("independent", entry, true);
    add_actor ("creating", entry, false);
    add_actor ("closing", entry, true);
    gates.at ("closing")->invalidate_execution_ready ();
    add_actor ("relocating", entry, true);
    gates.at ("relocating")->invalidate_execution_ready ();
    node->retiring_actor_keys.insert ("player:relocating");
    auto timer_queue = spot->ensure_spot_serial_executor ()->timer_queue ("pending-timer");
    timer = std::make_shared<detail::timer_state_t> ();
    timer->name = "pending-timer";
    timer->serial_queue = timer_queue;
    spot->timers.push_back (timer);
    scheduler->run_all ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    add_actor ("late", entry, true);
    late_spot_gate->confirm_execution_ready (clock_now.load ());
    provider.forbid_io = memory.forbid_io = true;
    const auto check_store_closed = [&] {
        const auto check = [] (const auto &result) {
            ASSERT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
        };
        check (store ().reserve ({}).result ());
        check (store ().read_authority (actor_authority_key ("rejoined-actor")).result ());
        check (store ().list_mesh_nodes ("discard-mesh").result ());
    };
    pending.completion = [&] (result_t<void> result) {
        EXPECT_FALSE (timer->disposed.load ());
        EXPECT_FALSE (result);
        if (!result)
            EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
        ++completed;
    };
    for (const auto &id : {"independent", "early-member", "member", "late-member"}) {
        if (rejoin && retained && std::string (id) == "member") {
            move_to (*lease_access::snapshot (runtime)->admission_deadline + 1ms);
            EXPECT_LT (clock_now.load (), episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            EXPECT_FALSE (episode->discarded.load ());
            check_store_closed ();
        }
        auto actor_pending = pending;
        actor_pending.completion = [&, id = std::string (id)] (result_t<void> result) {
            EXPECT_FALSE (instances.at (id).expired ()) << id;
            pending.completion (std::move (result));
        };
        const bool accepted = gates.at (id)->try_post (
          id,
          [&, id = std::string (id)] {
              ++starts[id];
              ++handlers;
          },
          actor_pending);
        EXPECT_EQ (!(rejoin && retained && std::string (id) == "late-member"), accepted) << id;
    }
    EXPECT_TRUE (spot_gate->try_post ("pending-spot", [&] { ++handlers; }, pending));
    EXPECT_TRUE (timer_queue->try_post ("pending-timer", [&] { ++handlers; }, pending));
    std::latch entered (1), release (1);
    auto held = std::async (std::launch::async, [&] {
        node->lane.run_sync ([&] {
            entered.count_down ();
            release.wait ();
        });
    });
    entered.wait ();
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
    if (rejoin)
        check_store_closed ();
    EXPECT_FALSE (spot_gate->execution_allowed ());
    EXPECT_FALSE (gates.at ("late-member")->execution_allowed ());
    EXPECT_FALSE (gates.at ("early-member")->execution_allowed ());
    EXPECT_FALSE (late_spot_gate->execution_allowed ());
    lease_access::lane (runtime).run_sync ([] {});
    scheduler->run_all ();
    EXPECT_EQ (0, handlers);
    for (const auto &id : {"member", "late-member", "independent", "early-member"})
        EXPECT_EQ (0, starts[id]) << id;
    EXPECT_EQ (0, completed);
    release.count_down ();
    held.get ();
    node->lane.run_sync ([] {});
    scheduler->run_all ();
    auto finished = spot_finished->task ();
    if (spot_gate->closed ()) {
        scheduler->run_until (finished);
        EXPECT_TRUE (finished.result ().value ());
    }
    for (const auto &excluded : {late_spot, creating_spot, closing_spot, relocating_spot}) {
        EXPECT_FALSE (excluded->closed.has_value ());
        EXPECT_FALSE (excluded->ensure_spot_serial_executor ()->spot_queue ()->closed ());
    }
    // Cleanup uses only local resources even when the Store is unavailable.
    EXPECT_TRUE (spot->closed.has_value ());
    EXPECT_TRUE (timer->disposed.load ());
    EXPECT_EQ (rejoin && retained ? 5 : 6, completed);
    EXPECT_EQ (0, handlers);
    EXPECT_TRUE (gates.at ("independent")->closed ());
    EXPECT_TRUE (gates.at ("member")->closed ());
    EXPECT_TRUE (gates.at ("late-member")->closed ());
    node->lane.run_sync ([&] {
        EXPECT_FALSE (node->spot_contexts_by_id.contains ("retained-spot"));
        for (const auto &id : {"independent", "member", "late-member", "early-member"})
            EXPECT_FALSE (node->actor_types_by_id.contains (id));
        for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
            EXPECT_TRUE (node->actor_types_by_id.contains (id));
    });
    for (const auto &id : {"independent", "member", "late-member", "early-member"})
        EXPECT_FALSE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor, id));
    gateway_state->sync ([&] {
        for (const auto &id : {"independent", "member", "late-member", "early-member"}) {
            EXPECT_FALSE (gateway_state->actors_by_id.contains (id));
            EXPECT_FALSE (gateway_state->bound_session_sinks.contains (id));
        }
        for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
            EXPECT_TRUE (gateway_state->actors_by_id.contains (id));
    });
    for (const auto &id : {"creating", "closing", "relocating", "late", "other-member"})
        EXPECT_FALSE (gates.at (id)->closed ());
    EXPECT_EQ (0, provider.forbidden_calls.load ());
    EXPECT_EQ (0, memory.forbidden_calls.load ());
    provider.forbid_io = memory.forbid_io = false;
    auto current = lease_access::snapshot (runtime);
    auto token = *current->token;
    if (rejoin) {
        move_to (std::max (clock_now.load () + 1ms, current->last_successful_lease->started_at
                                                      + options.owner_lease_ttl
                                                      + options.owner_lease_fencing_margin));
        check_store_closed ();
        // Release the fixture Store lease to model expiry without waiting on wall time.
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          store ().release_owner_lease (token).result ().value ()));
        lease_access::heartbeat (runtime, clock_now.load ());
        const auto recovered = lease_access::snapshot (runtime);
        ASSERT_TRUE (lease_access::usable_at (runtime, clock_now.load ()));
        EXPECT_NE (token.lease_generation, recovered->token->lease_generation);
        EXPECT_FALSE (recovered->outage);
        EXPECT_TRUE (episode->discarded.load ());

        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "discard-mesh";
        descriptor.rid = zlink::routing_id_t::from ("discard-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = recovered->token->owner_id;
        descriptor.lease_generation = recovered->token->lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.security_identity = "default";
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::snapshot, true, 0}};
        descriptor.capacity.actors.limit = 1;
        ASSERT_EQ (location_write_status_t::stored,
                   store ()
                     .update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, "rejoined-actor"};
        request.intent.stable_type = "player";
        request.target = {descriptor.mesh_name, node_rid_t::from_string ("discard-node"), 1,
                          *recovered->token};
        request.creating_payload = {std::byte{0}};
        request.capacity_bundle.actor_slots = 1;
        const auto reservation = store ().reserve (request).result ();
        EXPECT_TRUE (reservation);
        if (reservation)
            EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reservation.value ()));
        const auto authority =
          store ().read_authority (actor_authority_key ("rejoined-actor")).result ();
        EXPECT_TRUE (authority);
        if (authority)
            EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (authority.value ()));
        const auto nodes = store ().list_mesh_nodes ("discard-mesh").result ();
        EXPECT_TRUE (nodes);
        if (nodes)
            EXPECT_EQ (1u, nodes.value ().items.size ());
        try {
            const auto *reserved =
              reservation ? std::get_if<object_reserved_t> (&reservation.value ()) : nullptr;
            ASSERT_TRUE (reserved);
            const auto payload = encode_actor_authority_payload (actor_authority_payload_t{
              .state = actor_authority_state_t::ready,
              .stable_type = "player",
              .actor_id = "rejoined-actor",
              .current_spot_id = "entry",
              .current_spot_generation = 1,
              .current_spot_kind = actor_authority_spot_kind_t::entry,
              .owner_id = recovered->token->owner_id,
              .owner_lease_generation =
                static_cast<std::uint64_t> (recovered->token->lease_generation),
              .mesh_name = descriptor.mesh_name,
              .node_rid = entry->node_rid,
              .node_generation = 1});
            advance (1ms);
            add_actor ("rejoined-actor", entry, false);
            const auto published =
              store ().commit ({request.key, reserved->fence, payload}).result ().value ();
            EXPECT_TRUE (std::holds_alternative<object_committed_t> (published));
            gates.at ("rejoined-actor")->confirm_execution_ready (clock_now.load ());
            entry->ensure_spot_serial_executor ()->spot_queue ()->confirm_execution_ready (
              clock_now.load ());
            scheduler->run_all ();
            advance (1ms);
            lease_access::communication_failure (runtime);
            const auto second = lease_access::snapshot (runtime)->outage;
            EXPECT_NE (episode, second);
            if (second != episode) {
                EXPECT_EQ (clock_now.load (), second->observed_at);
                move_to (*recovered->admission_deadline + (retained ? 1ms : -1ms));
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                int second_calls = 0;
                EXPECT_TRUE (gates.at ("rejoined-actor")->try_post ("second-retained", [&] {
                    ++second_calls;
                }));
                scheduler->run_all ();
                EXPECT_EQ (1, second_calls);
                move_to (second->cutoff);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                EXPECT_TRUE (second->discarded.load ());
                const auto cleanup_deadline = std::chrono::steady_clock::now () + 5s;
                bool removed = false;
                while (std::chrono::steady_clock::now () < cleanup_deadline) {
                    scheduler->run_all ();
                    removed = node->lane.run_sync ([&] {
                        return !node->actor_types_by_id.contains ("rejoined-actor");
                    })
                              && !mesh.native_node ().objects ().find (
                                stateful::object_kind_t::actor, "rejoined-actor");
                    if (removed)
                        break;
                    std::this_thread::yield ();
                }
                EXPECT_TRUE (removed);
                EXPECT_FALSE (mesh.native_node ().objects ().find (stateful::object_kind_t::actor,
                                                                   "rejoined-actor"));
                EXPECT_FALSE (gates.at ("rejoined-actor")->try_post ("second-discarded", [&] {
                    ++second_calls;
                }));
                EXPECT_EQ (1, second_calls);
            }
        }
        catch (const std::exception &error) {
            ADD_FAILURE () << "Second object setup: " << error.what ();
        }
    } else {
        ++token.lease_generation;
        lease_access::accept (runtime, token, *current->renewed_at + 30s, *current->renewed_at,
                              clock_now.load (), false);
    }
    for (const auto &id : {"independent", "member", "late-member", "early-member"})
        EXPECT_FALSE (gates.at (id)->try_post ("after-new-claim", [&] { ++handlers; }));
    EXPECT_FALSE (spot_gate->try_post ("after-new-claim", [&] { ++handlers; }));
    EXPECT_FALSE (timer_queue->try_post ("after-new-claim", [&] { ++handlers; }));
    scheduler->run_all ();
    EXPECT_EQ (0, handlers);
    EXPECT_EQ (rejoin && retained ? 5 : 6, completed);
    for (const auto &[_, queue] : gates)
        queue->cancel_pending ();
    for (const auto &context : contexts) {
        context->ensure_spot_serial_executor ()->close ();
        context->node.reset ();
    }
    // Teardown waits for every started Close to settle, then clears the map on its lane.
    const auto closes_settled = [&] {
        return node->lane.run_sync ([&] {
            for (std::size_t i = 0; i < contexts.size (); ++i)
                if (contexts[i]->closed.has_value () && !close_settled[i]->load ())
                    return false;
            return true;
        });
    };
    const auto close_deadline = std::chrono::steady_clock::now () + 10s;
    for (;;) {
        scheduler->run_all ();
        if (closes_settled () || std::chrono::steady_clock::now () >= close_deadline)
            break;
        std::this_thread::yield ();
    }
    EXPECT_TRUE (closes_settled ());
    node->lane.run_sync ([&] { node->spot_contexts_by_id.clear (); });
    mesh.stop ();
}

namespace
{
TEST_P (StoreOutageRetention, CacheIntervalRefusesNewStoreWork)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "admission-owner-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto bound = std::make_shared<serial_execution_queue_t> (scheduler);
        bound->set_execution_policy (runtime.execution_policy ());
        bound->confirm_execution_ready (clock_now.load ());
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        int handlers = 0;
        for (const auto at :
             {*lease_access::snapshot (runtime)->admission_deadline, episode->cutoff}) {
            move_to (at);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (bound->try_post ("cached-bound-message", [&] { ++handlers; }));
            scheduler.run_all ();
            EXPECT_EQ (retained ? 1 : 0, handlers);
            provider.forbid_io = memory.forbid_io = true;
            const auto check = [] (const auto &result) {
                EXPECT_FALSE (result);
                if (!result) {
                    EXPECT_NE (nullptr, result.error ());
                    if (result.error ())
                        EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
                }
            };
            check (store ().read_authority ({"uncached-route"}).result ());
            check (store ().list_mesh_nodes ("uncached-mesh").result ());
            check (store ().prepare_aggregate ({}).result ());
            check (store ().reserve ({}).result ());
            check (store ().commit ({}).result ());
            check (store ().complete_creation ({}).result ());
            check (store ()
                     .compare_exchange_authority ({"relocation"}, "old", authority_retarget_t{})
                     .result ());
            EXPECT_EQ (0, provider.forbidden_calls.load ());
            EXPECT_EQ (0, memory.forbidden_calls.load ());
            provider.forbid_io = memory.forbid_io = false;
        }
        bound->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, AdmissionGuardCountsEveryMemoryStoreEntry)
{
    memory.forbid_io = true;
    EXPECT_FALSE (memory.list_mesh_nodes ("guard").result ());
    EXPECT_FALSE (memory.reserve ({}).result ());
    EXPECT_FALSE (memory.commit ({}).result ());
    EXPECT_FALSE (memory.complete_creation ({}).result ());
    EXPECT_FALSE (memory.prepare_aggregate ({}).result ());
    EXPECT_EQ (5, memory.forbidden_calls.load ());
}

TEST_P (StoreOutageRetention, ObjectCloseKeepsShuttingDownResultInBothExecutionModes)
{
    for (bool shared : {false, true}) {
        location_runtime_t runtime (store (), options, "close-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        int handlers = 0, completed = 0;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
            ++completed;
        };
        for (const auto &queue : {spot, actor}) {
            queue->set_execution_policy (runtime.execution_policy ());
            ASSERT_TRUE (queue->try_post ("before-close", [&] { ++handlers; }, pending));
        }
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (2, completed);
    }
}

TEST_P (StoreOutageRetention, RuntimeStopCancelsRetentionDeadlineAndKeepsShuttingDown)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options, "stop-retention-owner");
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            for (const auto &gate : {spot, actor}) {
                gate->set_execution_policy (runtime.execution_policy ());
                gate->confirm_execution_ready (clock_now.load ());
            }
            scheduler.run_all ();
            unsigned discards = 0;
            runtime.on_execution_discard ([&] (auto) { ++discards; }).result ().value ();
            advance (5s);
            lease_access::communication_failure (runtime);
            const auto episode = lease_access::snapshot (runtime)->outage;
            ASSERT_TRUE (episode);
            unsigned handlers = 0;
            std::array<std::optional<result_t<void>>, 2> outcomes;
            std::vector<task_t<void>> cancellations;
            serial_work_options_t pending;
            std::size_t index = 0;
            for (const auto &gate : {spot, actor}) {
                pending.completion = [&, current = index++] (result_t<void> result) {
                    outcomes[current] = std::move (result);
                };
                ASSERT_TRUE (gate->try_post ("stop-retained", [&] { ++handlers; }, pending));
            }
            runtime.stop ();
            // A heartbeat continuation accepted after stop cannot reopen this episode.
            const auto lease = lease_access::snapshot (runtime);
            lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s,
                                  *lease->renewed_at, clock_now.load (), true, episode);
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            for (const auto &gate : {actor, spot}) {
                EXPECT_EQ (status_t::stopped, gate->execution_status ());
                cancellations.push_back (gate->cancel_pending_async ());
            }
            for (auto &cancelled : cancellations) {
                scheduler.run_until (cancelled);
                cancelled.result ().value ();
            }
            scheduler.run_all ();
            EXPECT_EQ (0u, discards);
            EXPECT_EQ (0u, handlers);
            for (const auto &outcome : outcomes) {
                ASSERT_TRUE (outcome);
                EXPECT_FALSE (*outcome);
                if (!*outcome)
                    EXPECT_EQ (framework_error_kind_t::shutting_down, outcome->error_kind ());
            }
            store ().release_owner_lease (*lease->token).result ().value ();
        }
}

TEST_P (StoreOutageRetention, RuntimeStopSuppressesAlreadyDeliveredDiscardAndKeepsNotFound)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "queued-discard-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy (runtime.execution_policy ());
    gate->confirm_execution_ready (clock_now.load ());
    scheduler.run_all ();
    serial_execution_queue_t::execution_admission_t delivered;
    unsigned requests = 0;
    runtime
      .on_execution_discard ([&] (auto policy) {
          ++requests;
          delivered = std::move (policy);
      })
      .result ()
      .value ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_EQ (1u, requests);
    ASSERT_TRUE (delivered);
    ASSERT_EQ (status_t::ownership_lost, delivered (*gate));
    runtime.stop ();
    // The owner has received the request, but has not submitted object cleanup yet.
    EXPECT_EQ (status_t::allowed, delivered (*gate));
    EXPECT_EQ (status_t::ownership_lost, gate->execution_status ());
    std::optional<result_t<void>> outcome;
    serial_work_options_t pending;
    pending.completion = [&] (result_t<void> result) { outcome = std::move (result); };
    ASSERT_TRUE (gate->try_post ("discarded-before-stop", [] {}, pending));
    auto cancelled = gate->cancel_pending_async ();
    scheduler.run_until (cancelled);
    cancelled.result ().value ();
    scheduler.run_all ();
    ASSERT_TRUE (outcome);
    EXPECT_FALSE (*outcome);
    if (!*outcome)
        EXPECT_EQ (framework_error_kind_t::not_found, outcome->error_kind ());
    EXPECT_EQ (1u, requests);
    gate->cancel_pending ();
}

TEST_P (StoreOutageRetention, RuntimeStopDrainsLatePublicationWithoutRetentionDiscard)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "stop-publication-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    unsigned requests = 0;
    runtime.on_execution_discard ([&] (auto) { ++requests; }).result ().value ();
    advance (5s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    task_completion_source_t<location_write_result_t> provider_completion;
    auto publication = runtime.descriptor_write_confirmed (
      [&] { return provider_completion.task (); }, [&] { return clock_now.load (); });
    ASSERT_FALSE (publication.await_ready ());
    auto stopped = std::async (std::launch::async, [&] { runtime.stop (); });
    // Observation completes on cancellation; physical publication is still pending.
    const auto observed = publication.result ();
    provider_completion.complete (
      result_t<location_write_result_t>::success ({location_write_status_t::stored, 1, {}}));
    stopped.get ();
    EXPECT_FALSE (observed);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_EQ (0u, requests);
}

TEST_P (StoreOutageRetention, OutagePolicyReportsOwnershipLostAtCutoff)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    for (bool shared : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (store (), options, "cutoff-result-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        for (const auto &queue : {spot, actor}) {
            queue->set_execution_policy (runtime.execution_policy ());
            queue->confirm_execution_ready (clock_now.load ());
        }
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        int handlers = 0, completed = 0;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
            ++completed;
        };
        for (const auto &queue : {spot, actor})
            ASSERT_TRUE (queue->try_post ("before-cutoff", [&] { ++handlers; }, pending));
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        for (const auto &queue : {actor, spot}) {
            EXPECT_EQ (status_t::ownership_lost, queue->execution_status ());
            queue->cancel_pending ();
        }
        scheduler.run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (2, completed);
        store ().release_owner_lease (*lease_access::snapshot (runtime)->token).result ().value ();
    }
}

TEST_P (StoreOutageRetention, StoreCallGuardRejectsReadsAndWrites)
{
    provider.forbid_io = memory.forbid_io = true;
    EXPECT_FALSE (provider.read ({"guard"}).result ());
    EXPECT_FALSE (provider.write ({}).result ());
    EXPECT_FALSE (provider.scan ({}).result ());
    EXPECT_EQ (3, provider.forbidden_calls.load ());
    EXPECT_FALSE (memory.read_authority ({"guard"}).result ());
    EXPECT_FALSE (
      memory.compare_exchange_authority ({"guard"}, "v", authority_delete_t{}).result ());
    EXPECT_EQ (2, memory.forbidden_calls.load ());
}

TEST_P (StoreOutageRetention, DiscardRetainedPerActorRetention)
{
    discard_retained (false, true);
}
TEST_P (StoreOutageRetention, DiscardRetainedSpotWideRetention)
{
    discard_retained (true, true);
}
TEST_P (StoreOutageRetention, NewClaimReopensStoreAfterDiscardPerActor)
{
    discard_retained (false, true, true);
}
TEST_P (StoreOutageRetention, NewClaimReopensStoreAfterDiscardSpotWide)
{
    discard_retained (true, true, true);
}
TEST_P (StoreOutageRetention, SecondOutageDiscardsNewPerActorWithoutRetention)
{
    discard_retained (false, false, true);
}
TEST_P (StoreOutageRetention, SecondOutageDiscardsNewSpotWideWithoutRetention)
{
    discard_retained (true, false, true);
}
TEST_P (StoreOutageRetention, DiscardRetainedPerActorDefault)
{
    discard_retained (false, false);
}
TEST_P (StoreOutageRetention, DiscardRetainedSpotWideDefault)
{
    discard_retained (true, false);
}
} // namespace
} // namespace zlink::framework::runtime
