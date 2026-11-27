/* SPDX-License-Identifier: Elastic-2.0 */
#include "location_runtime_mesh_host_test_access.hpp"
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
#include "location_runtime_outage_fixture.hpp"

namespace zlink::framework::detail
{
struct spot_node_runtime_test_access_t
{
    static task_t<void> commit_membership (spot_node_runtime_t &owner,
                                           spot_context_t &context,
                                           const actor_ref_t &actor)
    {
        bool committed = false;
        spot_actor_admission_callbacks_t callbacks;
        co_await owner.commit_accepted_actor_join ("player:early", context, actor, typeid (int),
                                                   nullptr, callbacks, false, {}, {}, committed,
                                                   {});
        EXPECT_TRUE (committed);
    }
};
}

namespace zlink::framework::runtime
{
namespace host
{
struct public_host_runtime_test_access_t
{
    template <class Host, class Publication>
    static task_t<stateful::stateful_error_t> adopt (Host &host, const Publication &published)
    {
        if constexpr (requires {
                          host.advance_local_actor_authority (published.current->target,
                                                              published.current->ready);
                      })
            return host.advance_local_actor_authority (published.current->target,
                                                       published.current->ready);
        else
            return host.advance_local_actor_authority (published.current->target);
    }
};
}
} // namespace zlink::framework::runtime

namespace zlink::framework::runtime
{
void StoreOutageRetention::registered_owner (bool shared, bool retained, int loss)
{
    options.store_outage_retention = retained;
    location_runtime_t runtime (store (), options, "registered-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto token = *lease_access::snapshot (runtime)->token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "registered-mesh";
    descriptor.rid = zlink::routing_id_t::from ("registered-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = token.owner_id;
    descriptor.lease_generation = token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.security_identity = "default";
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::snapshot, true, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 10}};
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (location_write_status_t::stored,
               store ()
                 .update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    auto node =
      test::runtime_failure_fixture<detail::spot_node_builder_state_t> (descriptor.mesh_name);
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    const auto close_executor = [&] (const auto &executor) {
        std::vector<task_t<void>> completions;
        executor->close (&completions);
        for (auto &completion : completions)
            if (!scheduler->run_until (completion, auto_connect_wait))
                return false;
        scheduler->retire ();
        return true;
    };
    node->worker_executor = scheduler;
    mesh::service_node_descriptor_t native_descriptor;
    native_descriptor.mesh_name = descriptor.mesh_name;
    native_descriptor.node_routing_id = descriptor.rid.to_bytes ();
    native_descriptor.lifecycle_generation = 1;
    native_descriptor.descriptor_revision = 1;
    native_descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = mesh::raw_mesh_node_options_t{.descriptor = native_descriptor,
                                            .runtime_failures =
                                              std::make_shared<runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot", "player"}});
    node->native_node = native;
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->serial_executor = scheduler;
    context->spot_id = spot_id_t ("registered-spot");
    context->spot_name = "room";
    context->mesh_name = descriptor.mesh_name;
    context->node_rid = node_rid_t::from_string (descriptor.rid.to_string ());
    context->execution_mode =
      shared ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
    context->object_generation = 1;
    context->authority_owner_generation = 1;
    node->spot_contexts_by_id.emplace ("registered-spot",
                                       detail::spot_context_access_t::create (context));
    for (const auto &id : {"early", "late"}) {
        const std::string key = std::string ("player:") + id;
        node->actor_types_by_id[id] = "player";
        node->actor_spot_ids[key] = context->spot_id;
        node->actor_generations[key] = 1;
    }
    detail::spot_node_runtime_t owner (node);
    location_lifecycle_t lifecycle (runtime);
    owner.bind_location_lifecycle (lifecycle);
    native->configure_location_execution (
      runtime.execution_clock (),
      [&] (stateful::object_ref_t object) {
          return owner.prepare_store_ready (std::move (object));
      },
      runtime.bound_execution_owner ());
    std::map<std::string, authority_snapshot_t> ready_snapshots;
    const auto commit = [&] (placement_object_kind_t kind, std::string id, std::string type) {
        object_reserve_request_t request;
        request.key = {kind, id};
        request.intent.stable_type = type;
        request.target = {descriptor.mesh_name, context->node_rid, 1, token};
        request.creating_payload = {std::byte{0}};
        request.capacity_bundle.actor_slots = kind == placement_object_kind_t::actor ? 1 : 0;
        request.capacity_bundle.spot_slots = kind == placement_object_kind_t::user_spot ? 1 : 0;
        if (kind == placement_object_kind_t::user_spot)
            request.capacity_bundle.spot_type = spot_type_capacity_delta_t{kind, type, 1};
        const auto reserved = store ().reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (nullptr, reservation);
        if (!reservation)
            return;
        if (kind == placement_object_kind_t::actor) {
            const stateful::object_ref_t reserved_actor{
              stateful::object_kind_t::actor,
              id,
              reservation->fence.object_generation,
              reservation->fence.authority_owner_generation,
              descriptor.mesh_name,
              descriptor.rid.to_string ()};
            native->create_reserved_actor_async (type, reserved_actor).result ().value ();
            node->actor_generations["player:" + id] = reserved_actor.object_generation;
        }
        const auto ready_payload =
          kind == placement_object_kind_t::actor
            ? encode_actor_authority_payload (actor_authority_payload_t{
                .state = actor_authority_state_t::ready,
                .stable_type = type,
                .actor_id = id,
                .current_spot_id = std::string (context->spot_id),
                .current_spot_generation = 1,
                .owner_id = token.owner_id,
                .owner_lease_generation = static_cast<std::uint64_t> (token.lease_generation),
                .mesh_name = descriptor.mesh_name,
                .node_rid = context->node_rid,
                .node_generation = 1})
            : encode_user_spot_authority_payload (user_spot_authority_payload_t{
                .state = user_spot_authority_state_t::ready,
                .stable_type = type,
                .spot_id = id,
                .owner_id = token.owner_id,
                .owner_lease_generation = static_cast<std::uint64_t> (token.lease_generation),
                .mesh_name = descriptor.mesh_name,
                .node_rid = context->node_rid,
                .node_generation = 1});
        if (kind != placement_object_kind_t::actor)
            native->get_or_create_spot (id);
        const stateful::object_ref_t object{kind == placement_object_kind_t::actor
                                              ? stateful::object_kind_t::actor
                                              : stateful::object_kind_t::user_spot,
                                            id,
                                            reservation->fence.object_generation,
                                            reservation->fence.authority_owner_generation,
                                            descriptor.mesh_name,
                                            descriptor.rid.to_string ()};
        auto confirm = native->prepare_store_ready (object).result ().value ();
        if ((loss == -22 || loss == -23) && id == "early") {
            const auto lease = std::get<owner_lease_found_t> (
              store ().read_owner_lease (token.owner_id).result ().value ());
            creation_terminal_publication_t publication{
              {context->node_rid, 1, {18, 27}}, {std::byte{7}}, lease.store_now + 1h};
            object_complete_creation_request_t completion{
              request.key, reservation->fence,
              object_creation_completed_t{ready_payload, publication}};
            const auto completed = store ().complete_creation (completion).result ().value ();
            EXPECT_TRUE (std::holds_alternative<object_creation_completed_result_t> (completed));
            const auto replay = store ().complete_creation (completion).result ().value ();
            EXPECT_TRUE (
              std::holds_alternative<object_creation_already_completed_result_t> (replay));
            const auto snapshot = std::visit (
              [] (const auto &value) { return completion_ready_snapshot (value); }, replay);
            EXPECT_TRUE (snapshot);
            if (snapshot)
                ready_snapshots[id] = *snapshot;
            if (confirm)
                confirm (snapshot).result ().value ();
            return;
        }
        const auto committed =
          store ().commit ({request.key, reservation->fence, ready_payload}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (nullptr, ready);
        if (ready) {
            ready_snapshots[id] = ready->ready;
            if (confirm)
                confirm (ready->ready).result ().value ();
        }
    };
    commit (placement_object_kind_t::user_spot, "registered-spot", "room");
    commit (placement_object_kind_t::actor, "early", "player");
    if (loss == -60 || loss == -61) {
        using status_t = serial_execution_queue_t::execution_status_t;
        location_reclaim_test_access_t reclaim_clock;
        reclaim_clock.bind (store ());
        auto executor = context->ensure_spot_serial_executor ();
        auto spot = executor->spot_queue ();
        auto actor = executor->actor_executor ("player:early")->queue ();
        auto timer_queue = std::make_shared<serial_execution_queue_t> (*scheduler);
        timer_queue->attach_spot_gate (spot);
        timer_queue->set_execution_policy (runtime.execution_policy ());
        timer_queue->confirm_execution_ready (clock_now.load ());
        context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
        serializer_registry_t serializers;
        context->channel_runtime->serializers = &serializers;
        auto timer = std::make_shared<detail::timer_state_t> ();
        timer->name = "stale-timer";
        timer->serial_queue = timer_queue;
        auto handler = std::make_shared<int> (0);
        timer->handler_instance = handler;
        int timer_calls = 0, actor_calls = 0;
        timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                      const timer_tick_t &) {
            ++timer_calls;
            return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
        };
        context->timers.push_back (timer);
        scheduler->run_all ();
        task_completion_source_t<void> applied, acknowledged, terminal;
        runtime
          .on_execution_discard ([&] (auto policy) {
              return continue_task<void> (
                owner.request_location_execution_discard (std::move (policy)), [&] {
                    applied.complete (result_t<void>::success ());
                    return acknowledged.task ();
                });
          })
          .result ()
          .value ();
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        ASSERT_TRUE (actor->try_post ("before-stale", [&] { ++actor_calls; }, work));
        detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
        // Exercise a temporarily closed admission as well as an open gate.
        move_to (*lease_access::snapshot (runtime)->admission_deadline + 1ms);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        ASSERT_EQ (status_t::stopped, actor->execution_status ());
        std::optional<task_t<void>> renewal;
        if (loss == -60) {
            store ().release_owner_lease (token).result ().value ();
            renewal.emplace (lease_access::heartbeat_async (runtime, clock_now.load ()));
            ASSERT_TRUE (applied.task ().result_for (auto_connect_wait));
        } else {
            owner
              .request_location_execution_discard (
                [] (const auto &) { return status_t::ownership_lost; })
              .result ()
              .value ();
        }
        EXPECT_EQ (status_t::ownership_lost, actor->execution_status ());
        EXPECT_EQ (status_t::ownership_lost, spot->execution_status ());
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        if (renewal) {
            EXPECT_FALSE (lease_access::snapshot (runtime)->token);
            EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
              store ().read_owner_lease (token.owner_id).result ().value ()));
            EXPECT_FALSE (renewal->await_ready ());
            acknowledged.complete (result_t<void>::success ());
            renewal->result ().value ();
            ASSERT_TRUE (lease_access::snapshot (runtime)->token);
            EXPECT_NE (token.lease_generation,
                       lease_access::snapshot (runtime)->token->lease_generation);
        } else {
            const auto current = std::get<owner_lease_found_t> (
              store ().read_owner_lease (token.owner_id).result ().value ());
            lease_access::accept (runtime, token, current.lease_expires_at, current.store_now,
                                  clock_now.load (), true);
        }
        auto terminal_task = terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal_task, auto_connect_wait));
        scheduler->run_all ();
        EXPECT_FALSE (terminal.task ().result ());
        EXPECT_EQ (framework_error_kind_t::not_found, terminal.task ().result ().error_kind ());
        EXPECT_EQ (0, actor_calls);
        EXPECT_EQ (0, timer_calls);
        EXPECT_EQ (status_t::ownership_lost, actor->execution_status ());
        EXPECT_EQ (status_t::ownership_lost, timer_queue->execution_status ());
        actor->set_execution_policy (runtime.execution_policy ());
        actor->confirm_execution_ready (clock_now.load ());
        EXPECT_FALSE (actor->try_post ("after-new-claim", [&] { ++actor_calls; }));
        auto fresh = std::make_shared<serial_execution_queue_t> (*scheduler);
        fresh->set_execution_policy (runtime.execution_policy ());
        fresh->confirm_execution_ready (clock_now.load ());
        ASSERT_TRUE (fresh->try_post ("new-incarnation", [&] { ++actor_calls; }));
        scheduler->run_all ();
        EXPECT_EQ (1, actor_calls);
        fresh->cancel_pending ();
        // Local discard must settle before the repository starts release.
        // The reclaim clock is independent of the owner's admission clock.
        ASSERT_TRUE (lease_access::cleanup_submitted (runtime, auto_connect_wait,
                                                      [&] { scheduler->run_all (); }));
        if (loss == -61) {
            EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
              store ().read_authority_core (actor_authority_key ("early")).result ().value ()));
            EXPECT_EQ (0u, location_reclaim_test_access_t::observations (store ()));
        }
        if (loss == -60) {
            advance (1s);
            lease_access::heartbeat (runtime, clock_now.load ());
            ASSERT_TRUE (lease_access::cleanup_submitted (runtime, auto_connect_wait));
            EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (
              store ().read_authority_core (actor_authority_key ("early")).result ().value ()));
            reclaim_clock.advance (3 * options.owner_lease_fencing_margin - 1ms);
            advance (1s);
            lease_access::heartbeat (runtime, clock_now.load ());
            ASSERT_TRUE (lease_access::cleanup_submitted (runtime, auto_connect_wait));
            EXPECT_TRUE (std::holds_alternative<authority_snapshot_t> (
              store ().read_authority_core (actor_authority_key ("early")).result ().value ()));
            reclaim_clock.advance (1ms);
        }
        advance (1s);
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::cleanup_submitted (runtime, auto_connect_wait));
        EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
          store ().read_authority_core (actor_authority_key ("early")).result ().value ()));
        scheduler->run_all ();
        runtime.stop ();
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -55 || loss == -56 || loss == -57) {
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        if (loss == -55) {
            store ().release_owner_lease (token).result ().value ();
            move_to (episode->cutoff - options.owner_lease_renew_timeout);
            provider.hold_cas = memory.hold_cas = true;
        } else {
            advance (1s);
            memory.held_authority = actor_authority_key ("early").value;
            provider.held_authority = std::string ("authority\0actor\0early", 21);
        }
        int calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (gate->try_post ("stop-held-recovery", [&] { ++calls; }, work));
        auto renewal = lease_access::heartbeat_async (runtime, clock_now.load ());
        if (loss != -55)
            (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
              .result ()
              .value ();
        renewal.result ().value ();
        // The object lane has submitted every recovery before this fence returns.
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "early"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        EXPECT_GT (lease_access::pending_store_operations (runtime), 0u);
        if (loss == -55)
            EXPECT_GT (
              GetParam () ? provider.preserve_writes.load () : memory.preserve_writes.load (), 0u);
        scheduler->run_all ();
        if (loss == -57) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            auto completed = terminal.task ();
            ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
            EXPECT_FALSE (completed.result ());
            EXPECT_EQ (framework_error_kind_t::not_found, completed.result ().error_kind ());
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        }
        task_completion_source_t<void> stopped;
        const auto stop_complete = stopped.task ();
        std::latch stop_entered (1);
        std::thread stopper ([&] {
            stop_entered.count_down ();
            runtime.stop ();
            stopped.complete (result_t<void>::success ());
        });
        stop_entered.wait ();
        // Exchange of the existing drain list proves stop reached physical drain.
        const auto drain_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (lease_access::pending_store_operations (runtime) != 0
               && !stop_complete.await_ready ()
               && std::chrono::steady_clock::now () < drain_deadline)
            std::this_thread::yield ();
        EXPECT_EQ (0u, lease_access::pending_store_operations (runtime));
        EXPECT_FALSE (stop_complete.await_ready ());
        provider.cas_release.complete (result_t<void>::success ());
        memory.cas_release.complete (result_t<void>::success ());
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        stop_complete.result ().value ();
        stopper.join ();
        ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
        EXPECT_EQ (0, calls);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -24 || loss == -25) {
        commit (placement_object_kind_t::actor, "late", "player");
        commit (placement_object_kind_t::user_spot, "next-spot", "room");
        node->snapshot.routing_id = descriptor.rid;
        service_collection_t services;
        services.add_factory<location_repository_t> (
          [&] (service_provider_t &) {
              return std::shared_ptr<location_repository_t> (&store (), [] (auto *) {});
          },
          service_lifetime_t::singleton);
        node->root_services = services.build_provider ();
        auto next = std::make_shared<detail::spot_context_state_t> ();
        next->node = node;
        next->lane_owner = node;
        next->serial_executor = scheduler;
        next->spot_id = spot_id_t ("next-spot");
        next->spot_name = "room";
        next->mesh_name = descriptor.mesh_name;
        next->node_rid = context->node_rid;
        next->object_generation = ready_snapshots.at ("next-spot").object_generation;
        next->authority_owner_generation =
          ready_snapshots.at ("next-spot").authority_owner_generation;
        next->spot_instance = std::make_shared<int> (1);
        next->lifecycle_domain = detail::spot_lifecycle_domain_t::user ();
        node->spot_contexts_by_id.emplace ("next-spot",
                                           detail::spot_context_access_t::create (next));
        node->mesh_runtime_owned_native_actor_ids.insert ("early");
        const auto actor = detail::actor_ref_access_t::make (
          context->node_rid, "player", "early",
          native->objects ().find (stateful::object_kind_t::actor, "early")->object_generation);
        actor_location_t location;
        location.mesh_name = descriptor.mesh_name;
        location.actor_id = "early";
        location.actor_ref = actor;
        EXPECT_EQ (location_write_status_t::stored, lifecycle.claim_actor (location).status);
        const auto before = ready_snapshots.at ("early").store_version;
        detail::spot_node_runtime_test_access_t::commit_membership (
          owner, node->spot_contexts_by_id.at ("next-spot"), actor)
          .result ()
          .value ();
        const auto changed = std::get<authority_snapshot_t> (
          store ().read_authority (actor_authority_key ("early")).result ().value ());
        EXPECT_NE (before, changed.store_version);
        EXPECT_EQ ("next-spot",
                   decode_direct_actor_authority_payload (changed.payload)->current_spot_id);
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto gate =
          native->objects ()
            .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "early"))
            .result ()
            .value ();
        ASSERT_TRUE (gate);
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        advance (1s);
        if (loss == -25) {
            const auto superseding = store ()
                                       .compare_exchange_authority_core (
                                         actor_authority_key ("early"), changed.store_version,
                                         authority_put_t{changed.payload})
                                       .result ()
                                       .value ();
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (superseding));
        }
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        EXPECT_EQ (loss == -24 ? serial_execution_queue_t::execution_status_t::allowed
                               : serial_execution_queue_t::execution_status_t::ownership_lost,
                   gate->execution_status ());
        gate->cancel_pending ();
        context->ensure_spot_serial_executor ()->close ();
        next->ensure_spot_serial_executor ()->close ();
        scheduler->run_all ();
        context->node.reset ();
        next->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -20 || loss == -21 || loss == -22 || loss == -23) {
        commit (placement_object_kind_t::actor, "late", "player");
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        if (loss == -20 || loss == -21) {
            auto source = *native->objects ().find (stateful::object_kind_t::actor, "early");
            auto target = source;
            stateful::public_authority_store_adapter_t adapter (store ());
            const auto snapshot = ready_snapshots.at ("early");
            const auto published =
              adapter.publish (source, target, token, snapshot.allocation.target, "adopted", 7, {},
                               snapshot.payload, snapshot.store_version, {}, snapshot.owner);
            ASSERT_EQ (stateful::authority_publish_status_t::published, published.status);
            ASSERT_TRUE (published.current);
            EXPECT_EQ (stateful::stateful_error_t::none,
                       host::public_host_runtime_test_access_t::adopt (*native, published)
                         .result ()
                         .value ());
            const auto current =
              store ().read_authority (actor_authority_key ("early")).result ().value ();
            ready_snapshots["early"] = std::get<authority_snapshot_t> (current);
        }
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        advance (1s);
        if (loss == -21 || loss == -23) {
            const auto current =
              store ().read_authority_core (actor_authority_key ("early")).result ().value ();
            const auto snapshot = std::get<authority_snapshot_t> (current);
            const auto changed = store ()
                                   .compare_exchange_authority_core (
                                     actor_authority_key ("early"), snapshot.store_version,
                                     authority_put_t{snapshot.payload})
                                   .result ()
                                   .value ();
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (changed));
        }
        provider.authority_reads = memory.authority_reads = 0;
        int early_calls = 0, late_calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (early->try_post (
          "review-fence",
          [&] {
              ++early_calls;
              terminal.complete (result_t<void>::success ());
          },
          work));
        EXPECT_TRUE (late->try_post ("review-independent", [&] { ++late_calls; }));
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        auto terminal_task = terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal_task, auto_connect_wait));
        scheduler->run_all ();
        EXPECT_EQ (3u, GetParam () ? provider.authority_reads : memory.authority_reads);
        const bool matches = loss == -20 || loss == -22;
        EXPECT_EQ (matches, terminal.task ().result ().has_value ());
        if (!matches && !terminal.task ().result ())
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.task ().result ().error_kind ());
        EXPECT_EQ (matches ? 1 : 0, early_calls);
        EXPECT_EQ (1, late_calls);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -14 || loss == -15 || loss == -16) {
        commit (placement_object_kind_t::actor, "late", "player");
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover, monotonic_time observed) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        advance (1s);
        memory.held_authority = actor_authority_key ("early").value;
        provider.held_authority = std::string ("authority\0actor\0early", 21);
        if (loss == -16) {
            memory.failed_authority = actor_authority_key ("late").value;
            provider.failed_authority = std::string ("authority\0actor\0late", 20);
        }
        provider.authority_reads = memory.authority_reads = 0;
        int calls = 0;
        task_completion_source_t<void> terminal;
        serial_work_options_t work;
        work.completion = [&] (result_t<void> result) { terminal.complete (result); };
        EXPECT_TRUE (early->try_post (
          "held-review",
          [&] {
              ++calls;
              terminal.complete (result_t<void>::success ());
          },
          work));
        auto first = lease_access::heartbeat_async (runtime, clock_now.load ());
        (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
          .result ()
          .value ();
        // The heartbeat terminal precedes its object pass and Store submission fences.
        const bool first_completed = bool (first.result_for (auto_connect_wait));
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        const auto late_before = late->execution_status ();
        EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        advance (options.owner_lease_renew_interval);
        memory.failed_authority.clear ();
        provider.failed_authority.clear ();
        auto second = lease_access::heartbeat_async (runtime, clock_now.load ());
        const bool second_completed = bool (second.result_for (auto_connect_wait));
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        const auto reads = GetParam () ? provider.authority_reads : memory.authority_reads;
        scheduler->run_all ();
        EXPECT_EQ (0, calls);
        bool cutoff_terminal = false, cutoff_released = false;
        if (loss == -15) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            auto cutoff = terminal.task ();
            cutoff_terminal = scheduler->run_until (cutoff, auto_connect_wait);
            cutoff_released = !lease_access::snapshot (runtime)->outage;
        }
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        first.result ().value ();
        second.result ().value ();
        ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
        auto terminal_task = terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal_task, auto_connect_wait));
        scheduler->run_all ();
        EXPECT_TRUE (first_completed);
        EXPECT_TRUE (second_completed);
        EXPECT_EQ (loss == -16 ? 4u : 3u, reads);
        EXPECT_EQ (loss == -16 ? serial_execution_queue_t::execution_status_t::stopped
                               : serial_execution_queue_t::execution_status_t::allowed,
                   late_before);
        if (loss == -15) {
            EXPECT_TRUE (cutoff_terminal);
            EXPECT_TRUE (cutoff_released);
            EXPECT_FALSE (terminal.task ().result ());
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.task ().result ().error_kind ());
            EXPECT_EQ (0, calls);
        } else {
            EXPECT_TRUE (terminal.task ().result ());
            EXPECT_EQ (1, calls);
        }
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -13 || (loss <= -30 && loss >= -54)) {
        using status_t = serial_execution_queue_t::execution_status_t;
        commit (placement_object_kind_t::actor, "late", "player");
        unsigned recoveries = 0, publications = 0, second_recoveries = 0;
        auto second_owner = std::make_shared<stateful::stateful_object_runtime_t> ();
        if (loss == -47)
            runtime
              .on_execution_discard (
                [] (auto) {}, {},
                [second_owner, &second_recoveries] (
                  location_runtime_t::execution_recovery_t recover, monotonic_time observed_at) {
                    ++second_recoveries;
                    return stateful::stateful_object_runtime_t::recover_location_execution (
                      second_owner, std::move (recover), observed_at);
                })
              .result ()
              .value ();
        serializer_registry_t host_serializers;
        std::unique_ptr<mesh_node_host_service_t> mesh_host;
        if (loss == -48 || loss == -49) {
            auto mesh_state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> (
              descriptor.mesh_name);
            mesh_state->routing_id = descriptor.rid;
            mesh_state->core_context = std::make_shared<zlink::context_t> ();
            mesh_host = std::make_unique<mesh_node_host_service_t> (
              std::make_shared<runtime_failure_collector_t> (), std::vector{mesh_state},
              host_serializers);
            mesh_node_host_service_test_access_t::recovery_descriptors (*mesh_host, store (),
                                                                        runtime, descriptor);
            if (loss == -49) {
                ASSERT_EQ (location_write_status_t::stored,
                           store ()
                             .remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, token)
                             .result ()
                             .value ());
                auto changed = descriptor;
                ++changed.lifecycle_generation;
                ASSERT_EQ (location_write_status_t::stored,
                           store ()
                             .update_mesh_node (changed, location_write_intent_t::new_claim)
                             .result ()
                             .value ()
                             .status);
            } else
                provider.hold_publication = memory.hold_publication = true;
        }
        task_completion_source_t<bool> publication;
        task_completion_source_t<void> submitted;
        lease_access::publisher (runtime, [&] (auto intent, auto cancellation, auto deadline) {
            ++publications;
            EXPECT_EQ (location_write_intent_t::renew, intent);
            if (mesh_host) {
                submitted.complete (result_t<void>::success ());
                return mesh_host->republish_after_store_recovery (intent, cancellation, deadline);
            }
            const auto owner = runtime.descriptor_publication_owner ().value ();
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            ++descriptor.descriptor_revision;
            const auto written = store ().update_mesh_node (descriptor, intent).result ().value ();
            EXPECT_EQ (location_write_status_t::stored, written.status);
            submitted.complete (result_t<void>::success ());
            return publication.task ();
        });
        auto discard_gates =
          std::make_shared<std::vector<std::weak_ptr<serial_execution_queue_t>>> ();
        auto discard_requests = std::make_shared<std::array<task_completion_source_t<void>, 3>> ();
        runtime
          .on_execution_discard (
            [discard_gates, discard_requests] (auto admission) {
                for (std::size_t i = 0; i < discard_gates->size (); ++i)
                    if (const auto queue = (*discard_gates)[i].lock ())
                        if (admission (*queue) == status_t::ownership_lost) {
                            queue->cancel_pending (status_t::ownership_lost);
                            (*discard_requests)[i].complete (result_t<void>::success ());
                        }
            },
            {},
            [&] (location_runtime_t::execution_recovery_t recover, monotonic_time observed_at) {
                ++recoveries;
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed_at);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        auto spot = executor->spot_queue ();
        const std::array queues{gate, late, spot};
        const std::array names{"early Actor", "late Actor", "Spot"};
        if (loss == -43 && !shared) {
            const std::array logical_keys{actor_authority_key ("early"),
                                          actor_authority_key ("late"),
                                          spot_authority_key ("registered-spot")};
            auto &expected =
              GetParam () ? provider.expected_authorities : memory.expected_authorities;
            expected.emplace ();
            for (std::size_t i = 0; i < logical_keys.size (); ++i) {
                if (GetParam ()) {
                    // The repository's first read generates its Store key; do not duplicate its codec.
                    provider.capture_read_key.emplace ();
                    repository.read_authority_core (logical_keys[i]).result ().value ();
                    (*expected)[i] = provider.capture_read_key->task ().result ().value ().value;
                    provider.capture_read_key.reset ();
                } else
                    (*expected)[i] = logical_keys[i].value;
            }
        }
        const auto original_object =
          loss == -13 ? native->objects ().find (stateful::object_kind_t::actor, "early")
                      : std::optional<stateful::object_ref_t>{};
        *discard_gates = {gate, late, spot};
        if (loss == -47) {
            const auto object = *native->objects ().find (stateful::object_kind_t::actor, "late");
            const auto created =
              second_owner->begin_reserved_object_async (object, "player", {}).result ().value ();
            ASSERT_EQ (stateful::stateful_error_t::none, created.error);
            ASSERT_EQ (stateful::stateful_error_t::none,
                       second_owner->commit_create (created.attempt));
            native->objects ().bind_execution_gate (object, {}).result ().value ();
            second_owner->bind_execution_gate (object, late).result ().value ();
            second_owner
              ->confirm_location_ready (object, ready_snapshots.at ("late"),
                                        late->ready_confirmation (), clock_now.load ())
              .result ()
              .value ();
        }
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        if (loss == -32 || loss == -50) {
            const auto &before = ready_snapshots.at (loss == -50 ? "registered-spot" : "early");
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
              store ()
                .compare_exchange_authority_core (
                  loss == -50 ? spot_authority_key ("registered-spot")
                              : actor_authority_key ("early"),
                  before.store_version, authority_put_t{before.payload})
                .result ()
                .value ()));
        }
        store ().release_owner_lease (token).result ().value ();
        const auto claim_at = lease_access::snapshot (runtime)->last_successful_lease->started_at
                              + options.owner_lease_ttl + options.owner_lease_fencing_margin;
        move_to ((loss == -33 || loss == -35 || loss == -36 || loss == -37 || loss <= -52)
                   ? episode->cutoff - options.owner_lease_renew_timeout
                 : loss == -34 ? episode->cutoff - options.owner_lease_renew_timeout + 1ns
                               : claim_at);
        if (loss == -40 || loss == -41 || loss == -43 || loss == -45 || loss == -46 || loss == -47
            || loss <= -52)
            provider.hold_cas = memory.hold_cas = true;
        if (loss == -42)
            provider.fail_cas = memory.fail_cas = true;
        int handlers = 0, late_handlers = 0, spot_handlers = 0;
        task_completion_source_t<void> terminal, late_terminal, spot_terminal;
        const auto enqueue = [&] (auto queue, auto &signal, int &calls) {
            serial_work_options_t work;
            work.completion = [&signal] (result_t<void> result) { signal.complete (result); };
            EXPECT_TRUE (queue->try_post (
              "new-claim",
              [&signal, &calls] {
                  ++calls;
                  signal.complete (result_t<void>::success ());
              },
              work));
        };
        enqueue (gate, terminal, handlers);
        enqueue (late, late_terminal, late_handlers);
        enqueue (spot, spot_terminal, spot_handlers);
        test::scope_exit_t release_held_cas ([&] {
            provider.cas_release.complete (result_t<void>::success ());
            memory.cas_release.complete (result_t<void>::success ());
        });
        if (loss == -38)
            runtime.stop ();
        provider.preserve_writes = memory.preserve_writes = 0;
        provider.submitted_authorities = memory.submitted_authorities = 0;
        lease_access::heartbeat (runtime, clock_now.load ());
        lease_access::lane (runtime).run_sync ([] {});
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        // The object pass submits Store work to the runtime lane.
        lease_access::lane (runtime).run_sync ([] {});
        const auto next_token = *lease_access::snapshot (runtime)->token;
        EXPECT_NE (token.lease_generation, next_token.lease_generation);
        EXPECT_EQ (loss == -38 ? 0u : 1u, recoveries);
        EXPECT_EQ (loss == -38 ? 0u : 1u, publications);
        EXPECT_EQ (loss == -47 ? 1u : 0u, second_recoveries);
        EXPECT_EQ (loss != -38, submitted.task ().await_ready ());
        if (loss == -33)
            EXPECT_EQ (3u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        if (loss == -34 || loss == -38)
            EXPECT_EQ (0u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        if (loss == -48) {
            if (publications != 0)
                (GetParam () ? provider.descriptors_submitted.task ()
                             : memory.descriptors_submitted.task ())
                  .result ()
                  .value ();
            EXPECT_EQ (2u, GetParam () ? provider.descriptor_submissions.load ()
                                       : memory.descriptor_submissions.load ());
        }
        EXPECT_NE (status_t::allowed, gate->execution_status ());
        const auto exact = std::get<authority_snapshot_t> (
          store ().read_authority_core (actor_authority_key ("early")).result ().value ());
        const bool preserved =
          loss != -32 && loss != -34 && loss != -38 && loss != -42 && loss != -49;
        EXPECT_EQ (preserved ? next_token.lease_generation : token.lease_generation,
                   exact.owner.lease_generation);
        const auto collect_operations = [&] {
            std::vector<outage_operation_t> pending_operations;
            for (std::size_t i = 0; i < queues.size (); ++i)
                if (auto pending = queues[i]->pending_execution_operation ())
                    pending_operations.push_back ({names[i], queues[i], std::move (*pending)});
            return pending_operations;
        };
        auto operations = collect_operations ();
        if (loss == -43 && !shared) {
            const auto submitted_mask = GetParam () ? provider.submitted_authorities.load ()
                                                    : memory.submitted_authorities.load ();
            for (std::size_t i = 0; i < queues.size (); ++i)
                ASSERT_TRUE (submitted_mask & (1u << i)) << "CAS submission: " << names[i];
            ASSERT_EQ (3u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ())
              << "CAS submission count";
            for (std::size_t i = 0; i < queues.size (); ++i)
                ASSERT_TRUE (queues[i]->pending_execution_operation ())
                  << "CAS operation: " << names[i];
            ASSERT_EQ (3u, operations.size ()) << "CAS operation count";
        }
        const auto wait_operations = [&] (const char *stage) {
            return wait_outage_operations (operations, loss, stage);
        };
        scheduler->run_all ();
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (0, late_handlers);
        EXPECT_EQ (0, spot_handlers);
        if (loss == -35 || loss == -36 || loss == -37)
            move_to (episode->cutoff + (loss == -35 ? -1ns : loss == -36 ? 0ns : 1ns));
        if (loss == -45) {
            advance (1ms);
            auto renewed = lease_access::heartbeat_async (runtime, clock_now.load ());
            renewed.result ().value ();
            EXPECT_EQ (2u, recoveries);
            EXPECT_EQ (3u, GetParam () ? provider.preserve_writes.load ()
                                       : memory.preserve_writes.load ());
        }
        if (loss == -48) {
            provider.publication_release.complete (result_t<void>::success ());
            memory.publication_release.complete (result_t<void>::success ());
        }
        if (loss == -46) {
            std::latch launch (3);
            auto publish = std::async (std::launch::async, [&] {
                launch.count_down ();
                launch.wait ();
                publication.complete (result_t<bool>::success (true));
            });
            auto cas = std::async (std::launch::async, [&] {
                launch.count_down ();
                launch.wait ();
                provider.cas_release.complete (result_t<void>::success ());
                memory.cas_release.complete (result_t<void>::success ());
            });
            launch.count_down ();
            launch.wait ();
            publish.get ();
            cas.get ();
        } else if (loss == -31 || loss == -41)
            publication.complete (result_t<bool>::failure (
              framework_error_kind_t::not_configured, "injected definite publication rejection"));
        else if (loss == -39)
            publication.complete (detail::result_access_t::failure<bool> (std::make_exception_ptr (
              std::runtime_error ("injected native publication failure"))));
        else
            publication.complete (result_t<bool>::success (true));
        if (loss != -34 && loss != -38 && loss != -40 && loss != -41 && loss != -43 && loss != -45
            && loss != -47 && loss > -52)
            ASSERT_TRUE (wait_operations ("recovery operation"));
        // Flush the object lane after its operation's terminal continuation.
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        scheduler->run_all ();
        if (loss == -34 || loss == -38) {
            EXPECT_EQ (0, handlers);
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        }
        if (loss == -39 || loss == -42) {
            EXPECT_EQ (0, handlers);
            if (loss == -39) {
                EXPECT_EQ (3u, operations.size ());
                for (auto &operation : operations) {
                    const auto failed = detail::task_result_access_t::read (operation.terminal);
                    EXPECT_FALSE (failed);
                    EXPECT_EQ (nullptr, failed.error ());
                }
            }
            // A renew may reuse an in-flight pass; finish the injected failure first.
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            ASSERT_TRUE (lease_access::wait_on_lane (
              runtime,
              [&] {
                  for (const auto &queue : {gate, late, spot})
                      if (queue->pending_execution_operation ())
                          return false;
                  return true;
              },
              std::chrono::steady_clock::now () + auto_connect_wait));
            provider.fail_cas = memory.fail_cas = false;
            publication = task_completion_source_t<bool>{};
            publication.complete (result_t<bool>::success (true));
            advance (1ms);
            lease_access::heartbeat (runtime, clock_now.load ());
            lease_access::lane (runtime).run_sync ([] {});
            native->objects ()
              .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
              .result ()
              .value ();
            EXPECT_EQ (2u, recoveries);
            EXPECT_EQ (loss == -39 ? 2u : 1u, publications);
            auto retried_operations = collect_operations ();
            ASSERT_TRUE (
              wait_outage_operations (retried_operations, loss, "renew recovery operation"));
        }
        if (loss == -40 || loss == -41 || loss == -43 || loss == -45 || loss == -47
            || loss <= -52) {
            if (loss == -41)
                ASSERT_TRUE (wait_operations ("recovery operation"));
            scheduler->run_all ();
            EXPECT_EQ (0, handlers);
            if (loss == -41)
                for (auto *signal : {&terminal, &late_terminal, &spot_terminal}) {
                    auto completed = signal->task ();
                    ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
                    const auto result = completed.result ();
                    EXPECT_FALSE (result);
                    if (!result)
                        EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
                }
            if (loss == -43) {
                advance (options.owner_lease_renew_timeout);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                native->objects ()
                  .execution_gate (
                    *native->objects ().find (stateful::object_kind_t::actor, "late"))
                  .result ()
                  .value ();
                scheduler->run_all ();
                if (!shared) {
                    ASSERT_LT (clock_now.load (), episode->cutoff) << "CAS timeout: before T";
                    ASSERT_TRUE (wait_operations ("CAS timeout"));
                    for (const auto &operation : operations) {
                        const auto result = detail::task_result_access_t::read (operation.terminal);
                        ASSERT_FALSE (result) << "CAS timeout: " << operation.name;
                        ASSERT_EQ (framework_error_kind_t::deadline_exceeded, result.error_kind ())
                          << "CAS timeout: " << operation.name;
                    }
                } else
                    ASSERT_TRUE (wait_operations ("recovery operation"));
                EXPECT_EQ (status_t::stopped, gate->execution_status ());
                if (!shared) {
                    ASSERT_EQ (0, handlers) << "CAS timeout: " << names[0] << " handler";
                    ASSERT_EQ (0, late_handlers) << "CAS timeout: " << names[1] << " handler";
                    ASSERT_EQ (0, spot_handlers) << "CAS timeout: " << names[2] << " handler";
                }
            }
            if (loss <= -52)
                move_to (episode->cutoff + (loss == -52 ? -1ns : loss == -53 ? 0ns : 1ns));
            if (loss == -40)
                ASSERT_LT (clock_now.load (), episode->cutoff - options.owner_lease_renew_timeout)
                  << "CAS response: before timeout and T";
            provider.cas_release.complete (result_t<void>::success ());
            memory.cas_release.complete (result_t<void>::success ());
            native->objects ()
              .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
              .result ()
              .value ();
            if (loss == -43) {
                move_to (episode->cutoff);
                detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                if (!shared) {
                    auto cutoff_terminal = episode->terminal.task ();
                    ASSERT_TRUE (cutoff_terminal.result_for (auto_connect_wait))
                      << "T latch: outage episode";
                    ASSERT_TRUE (episode->discarded.load ()) << "T latch: outage episode";
                    for (std::size_t i = 0; i < 3; ++i) {
                        auto request = (*discard_requests)[i].task ();
                        ASSERT_TRUE (request.result_for (auto_connect_wait))
                          << "gate cancellation request: " << names[i];
                    }
                }
            }
        }
        if (loss != -38)
            ASSERT_TRUE (wait_operations ("recovery operation"));
        if (loss != -38) {
            const std::array signals{&terminal, &late_terminal, &spot_terminal};
            for (std::size_t i = 0; i < signals.size (); ++i) {
                auto completed = signals[i]->task ();
                ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait))
                  << "work completion: " << names[i];
            }
        }
        scheduler->run_all ();
        const bool allowed = loss != -31 && loss != -32 && loss != -34 && loss != -36 && loss != -37
                             && loss != -38 && loss != -41 && loss != -43 && loss != -49
                             && loss != -50 && loss != -53 && loss != -54;
        EXPECT_EQ (allowed ? 1 : 0, handlers);
        EXPECT_EQ ((allowed || loss == -32) ? 1 : 0, late_handlers);
        EXPECT_EQ ((allowed || loss == -32) ? 1 : 0, spot_handlers);
        if (loss != -38) {
            const auto check_outcome = [] (auto &signal, bool expected_allowed, const char *name) {
                const auto result = signal.task ().result ();
                EXPECT_EQ (expected_allowed, result.has_value ()) << "work result: " << name;
                if (!expected_allowed && !result)
                    EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ())
                      << "work result: " << name;
            };
            check_outcome (terminal, allowed, names[0]);
            check_outcome (late_terminal, allowed || loss == -32, names[1]);
            check_outcome (spot_terminal, allowed || loss == -32, names[2]);
        }
        if (loss == -44) {
            ASSERT_TRUE (lease_access::recovery_terminal (runtime, episode, auto_connect_wait));
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            advance (1s);
            lease_access::communication_failure (runtime);
            const auto second_episode = lease_access::snapshot (runtime)->outage;
            advance (1s);
            lease_access::heartbeat (runtime, clock_now.load ());
            ASSERT_TRUE (
              lease_access::recovery_terminal (runtime, second_episode, auto_connect_wait));
            ASSERT_TRUE (lease_access::wait_on_lane (
              runtime,
              [&] {
                  return !gate->pending_execution_operation ()
                         && !spot->pending_execution_operation ();
              },
              std::chrono::steady_clock::now () + auto_connect_wait));
            scheduler->run_all ();
            EXPECT_EQ (status_t::allowed, gate->execution_status ());
            EXPECT_EQ (status_t::allowed, spot->execution_status ());
        }
        if (mesh_host) {
            provider.hold_publication = memory.hold_publication = false;
            provider.publication_release.complete (result_t<void>::success ());
            memory.publication_release.complete (result_t<void>::success ());
            runtime.stop ();
        }
        if (loss == -13) {
            const auto recovered =
              native->objects ().find (stateful::object_kind_t::actor, "early");
            ASSERT_TRUE (original_object);
            ASSERT_TRUE (recovered);
            EXPECT_EQ (*original_object, *recovered);
            EXPECT_EQ (gate, native->objects ().execution_gate (*recovered).result ().value ());
            ASSERT_TRUE (gate->try_post ("same-object-after-preserve", [&] { ++handlers; }));
            scheduler->run_all ();
            EXPECT_EQ (2, handlers);
        }
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss <= -10) {
        commit (placement_object_kind_t::actor, "late", "player");
        // Install recovery through the same owner registry used by the host.
        runtime
          .on_execution_discard (
            [] (auto) {}, {},
            [native] (location_runtime_t::execution_recovery_t recover,
                      monotonic_time observed_at) {
                return stateful::stateful_object_runtime_t::recover_location_execution (
                  {native, &native->objects ()}, std::move (recover), observed_at);
            })
          .result ()
          .value ();
        auto executor = context->ensure_spot_serial_executor ();
        auto spot = executor->spot_queue ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        scheduler->run_all ();
        const auto key = actor_authority_key ("early");
        if (loss == -11) {
            const auto removed =
              store ()
                .compare_exchange_authority (key, ready_snapshots.at ("early").store_version,
                                             authority_delete_t{})
                .result ()
                .value ();
            ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (removed));
        }
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        advance (1s);
        node->actor_types_by_id["new"] = "player";
        node->actor_spot_ids["player:new"] = context->spot_id;
        node->actor_generations["player:new"] = 1;
        commit (placement_object_kind_t::actor, "new", "player");
        auto fresh = executor->actor_executor ("player:new")->queue ();
        const auto held = loss == -12 ? spot_authority_key ("registered-spot") : key;
        memory.held_authority = held.value;
        provider.held_authority = std::string ("authority\0", 10)
                                  + (loss == -12 ? std::string ("spot\0registered-spot", 20)
                                                 : std::string ("actor\0early", 11));
        provider.authority_reads = memory.authority_reads = 0;
        int early_calls = 0, late_calls = 0, spot_calls = 0, fresh_calls = 0;
        task_completion_source_t<void> early_terminal, late_terminal, spot_terminal, fresh_terminal;
        serial_work_options_t early_work;
        early_work.completion = [&] (result_t<void> result) {
            early_terminal.complete (std::move (result));
        };
        EXPECT_TRUE (early->try_post (
          "held-exact",
          [&] {
              ++early_calls;
              early_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { late_terminal.complete (result); };
        EXPECT_TRUE (late->try_post (
          "matching-exact",
          [&] {
              ++late_calls;
              late_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { spot_terminal.complete (result); };
        EXPECT_TRUE (spot->try_post (
          "spot-exact",
          [&] {
              ++spot_calls;
              spot_terminal.complete (result_t<void>::success ());
          },
          early_work));
        early_work.completion = [&] (result_t<void> result) { fresh_terminal.complete (result); };
        EXPECT_TRUE (fresh->try_post (
          "created-after-outage",
          [&] {
              ++fresh_calls;
              fresh_terminal.complete (result_t<void>::success ());
          },
          early_work));
        auto renewing = std::async (std::launch::async, [&] {
            lease_access::heartbeat (runtime, clock_now.load ());
            // Also release the observation boundary in the unfixed implementation.
            provider.authority_entered.complete (result_t<void>::success ());
            memory.authority_entered.complete (result_t<void>::success ());
        });
        (GetParam () ? provider.authority_entered.task () : memory.authority_entered.task ())
          .result ()
          .value ();
        native->objects ()
          .execution_gate (*native->objects ().find (stateful::object_kind_t::actor, "late"))
          .result ()
          .value ();
        lease_access::lane (runtime).run_sync ([] {});
        ASSERT_TRUE (lease_access::wait_on_lane (
          runtime, [&] { return !late->pending_execution_operation (); },
          std::chrono::steady_clock::now () + auto_connect_wait));
        scheduler->run_all ();
        EXPECT_EQ (loss == -12 ? 0 : 1, fresh_calls);
        EXPECT_EQ (0, early_calls);
        const auto late_ready = late->execution_ready_at ();
        EXPECT_TRUE (late_ready);
        if (late_ready)
            EXPECT_GE (*late_ready, episode->observed_at);
        if (loss == -12) {
            EXPECT_EQ (0, late_calls);
            EXPECT_EQ (0, spot_calls);
        } else {
            EXPECT_EQ (1, late_calls);
            EXPECT_EQ (1, spot_calls);
        }
        provider.authority_release.complete (result_t<void>::success ());
        memory.authority_release.complete (result_t<void>::success ());
        renewing.get ();
        auto terminal = early_terminal.task ();
        ASSERT_TRUE (scheduler->run_until (terminal, auto_connect_wait));
        for (auto *signal : {&late_terminal, &spot_terminal, &fresh_terminal}) {
            auto completed = signal->task ();
            ASSERT_TRUE (scheduler->run_until (completed, auto_connect_wait));
        }
        scheduler->run_all ();
        if (loss == -11) {
            EXPECT_FALSE (terminal.result ());
            EXPECT_EQ (framework_error_kind_t::not_found, terminal.result ().error_kind ());
        } else
            EXPECT_TRUE (terminal.result ());
        EXPECT_EQ (loss == -11 ? 0 : 1, early_calls);
        EXPECT_EQ (1, late_calls);
        EXPECT_EQ (1, spot_calls);
        EXPECT_EQ (1, fresh_calls);
        EXPECT_EQ (3u, GetParam () ? provider.authority_reads : memory.authority_reads);
        EXPECT_EQ (loss == -11 ? serial_execution_queue_t::execution_status_t::ownership_lost
                               : serial_execution_queue_t::execution_status_t::allowed,
                   early->execution_status ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                   late->execution_status ());
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss == -3) {
        auto executor = context->ensure_spot_serial_executor ();
        auto gate = executor->actor_executor ("player:early")->queue ();
        context->actor_count = 1;
        node->actor_instances["player:early"] = std::make_shared<int> (1);
        runtime
          .on_execution_discard ([weak = std::weak_ptr (node)] (auto policy) {
              if (auto state = weak.lock ())
                  detail::spot_node_runtime_t (state).request_location_execution_discard (
                    std::move (policy));
          })
          .result ()
          .value ();
        scheduler->run_all ();
        ASSERT_TRUE (gate->execution_ready_at ());
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        std::latch entered (1), release (1);
        auto blocked = std::async (std::launch::async, [&] {
            node->lane.run_sync ([&] {
                entered.count_down ();
                release.wait ();
            });
        });
        entered.wait ();
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        runtime.stop ();
        release.count_down ();
        blocked.get ();
        // Fence the owner's queued discard decision before observing its worker submissions.
        node->lane.run_sync ([] {});
        EXPECT_EQ (0u, scheduler->pending_work ());
        EXPECT_FALSE (context->closed);
        EXPECT_EQ (1u, context->actor_count);
        EXPECT_TRUE (node->actor_instances.contains ("player:early"));
        gate->cancel_pending ();
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        node->native_node.reset ();
        return;
    }
    if (loss == -1) {
        auto executor = context->ensure_spot_serial_executor ();
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        move_to (*lease_access::snapshot (runtime)->admission_deadline);
        protocol::spot_route_fence_t route{.spot_id = "registered-spot",
                                           .object_generation = 1,
                                           .target_node_routing_id = descriptor.rid.to_bytes (),
                                           .target_node_generation = 1,
                                           .authority_owner_generation = 1,
                                           .owner_lease_generation =
                                             static_cast<std::uint64_t> (token.lease_generation)};
        stateful::accepted_record_authority_query_t actor_query;
        actor_query.target = *native->objects ().find (stateful::object_kind_t::actor, "early");
        actor_query.requested_authority_owner_generation =
          actor_query.target.authority_owner_generation;
        actor_query.requested_object_generation = actor_query.target.object_generation;
        actor_query.target_node_routing_id = descriptor.rid.to_bytes ();
        actor_query.target_node_generation = 1;
        actor_query.source_node_routing_id = descriptor.rid.to_bytes ();
        actor_query.source_node_generation = 1;
        actor_query.source_original =
          mesh::request_source_t{std::make_shared<const mesh_node_descriptor_t> (descriptor)};
        provider.forbid_io = memory.forbid_io = true;
        const auto actor_authority =
          native->accepted_record_authority (actor_query, runtime.current_owner_observation ())
            .result ()
            .value ();
        EXPECT_EQ (retained, actor_authority.has_value ());
        auto prepared = owner.prepare_spot_dispatch ("registered-spot", route).result ();
        EXPECT_EQ (retained, bool (prepared));
        if (prepared)
            prepared.value ()->terminal ();
        EXPECT_EQ (0, provider.forbidden_calls.load ());
        EXPECT_EQ (0, memory.forbidden_calls.load ());
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_FALSE (owner.prepare_spot_dispatch ("registered-spot", route).result ());
        EXPECT_FALSE (
          native->accepted_record_authority (actor_query, runtime.current_owner_observation ())
            .result ()
            .value ());
        provider.forbid_io = memory.forbid_io = false;
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    if (loss != 0) {
        node->snapshot.routing_id = descriptor.rid;
        context->actor_count = 2;
        commit (placement_object_kind_t::actor, "late", "player");
        node->actor_instances["player:early"] = std::make_shared<int> (1);
        node->actor_instances["player:late"] = std::make_shared<int> (2);
        const std::weak_ptr<void> early_instance = node->actor_instances.at ("player:early");
        const std::weak_ptr<void> late_instance = node->actor_instances.at ("player:late");
        runtime
          .on_execution_discard (
            [weak = std::weak_ptr (node)] (auto policy) {
                if (auto state = weak.lock ())
                    detail::spot_node_runtime_t (state).request_location_execution_discard (
                      std::move (policy));
            },
            [weak = std::weak_ptr (node),
             owner_id = runtime.owner_id ()] (authority_key_t key, authority_snapshot_t snapshot) {
                if (auto state = weak.lock ())
                    detail::spot_node_runtime_t (state).observe_location_authority (
                      std::move (key), std::move (snapshot), owner_id);
            })
          .result ()
          .value ();
        auto finished = std::make_shared<task_completion_source_t<bool>> ();
        context->merged_close_results.push_back (
          [finished] (result_t<bool> result) { finished->complete (std::move (result)); });
        auto executor = context->ensure_spot_serial_executor ();
        auto early = executor->actor_executor ("player:early")->queue ();
        auto late = executor->actor_executor ("player:late")->queue ();
        auto spot_gate = executor->spot_queue ();
        const auto key =
          loss % 2 == 0 ? spot_authority_key ("registered-spot") : actor_authority_key ("early");
        const auto before = store ().read_authority (key).result ().value ();
        const auto snapshot = std::get<authority_snapshot_t> (before);
        const authority_mutation_t mutation =
          loss > 2 ? authority_mutation_t{authority_retarget_t{snapshot.payload,
                                                               snapshot.allocation.target}}
                   : authority_mutation_t{authority_reincarnate_t{snapshot.payload}};
        const auto changed = store ()
                               .compare_exchange_authority (key, snapshot.store_version, mutation)
                               .result ()
                               .value ();
        ASSERT_TRUE (std::holds_alternative<authority_stored_t> (changed));
        scheduler->run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        int handlers = 0;
        std::atomic_int cancelled{0};
        auto cancellations = std::make_shared<task_completion_source_t<void>> ();
        auto cancellations_finished = cancellations->task ();
        const auto expected_cancellations = loss % 2 == 0 ? 2 : 1;
        serial_work_options_t pending;
        pending.completion = [&] (result_t<void> result) {
            EXPECT_FALSE (result);
            if (!result && result.error ())
                EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
            if (cancelled.fetch_add (1) + 1 == expected_cancellations)
                cancellations->complete (result_t<void>::success ());
        };
        ASSERT_TRUE (early->try_post ("lost-actor", [&] { ++handlers; }, pending));
        if (loss % 2 == 0)
            ASSERT_TRUE (spot_gate->try_post ("lost-spot", [&] { ++handlers; }, pending));
        // This exact read confirms a changed incarnation before T.
        store ().read_authority (key).result ().value ();
        lease_access::lane (runtime).run_sync ([] {});
        node->lane.run_sync ([] {});
        native->objects ().find_async (stateful::object_kind_t::actor, "early").result ().value ();
        node->lane.run_sync ([] {});
        provider.forbid_io = memory.forbid_io = true;
        scheduler->run_all ();
        ASSERT_TRUE (scheduler->run_until (cancellations_finished, auto_connect_wait));
        if (loss % 2 == 0 && spot_gate->closed ()) {
            auto closed = finished->task ();
            ASSERT_TRUE (scheduler->run_until (closed, auto_connect_wait));
            EXPECT_TRUE (closed.result ().value ());
        }
        EXPECT_EQ (0, handlers);
        EXPECT_EQ (expected_cancellations, cancelled.load ());
        EXPECT_TRUE (early->closed ());
        if (loss % 2 == 0) {
            EXPECT_TRUE (late->closed ());
            EXPECT_TRUE (spot_gate->closed ());
        } else {
            ASSERT_TRUE (late->try_post ("other-preserved", [&] { ++handlers; }));
            scheduler->run_all ();
            EXPECT_EQ (1, handlers);
            EXPECT_FALSE (late_instance.expired ());
            EXPECT_FALSE (episode->discarded.load ());
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            lease_access::lane (runtime).run_sync ([] {});
            node->lane.run_sync ([] {});
            scheduler->run_all ();
            EXPECT_TRUE (late->closed ());
            auto closed = finished->task ();
            ASSERT_TRUE (scheduler->run_until (closed, auto_connect_wait));
            EXPECT_TRUE (closed.result ().value ());
        }
        EXPECT_TRUE (early_instance.expired ());
        EXPECT_TRUE (late_instance.expired ());
        EXPECT_EQ (0, provider.forbidden_calls.load ());
        EXPECT_EQ (0, memory.forbidden_calls.load ());
        provider.forbid_io = memory.forbid_io = false;
        const auto token_before_claim = *lease_access::snapshot (runtime)->token;
        store ().release_owner_lease (token_before_claim).result ().value ();
        // Object discard does not authorize an owner claim before normal expiry.
        move_to (std::max (clock_now.load (),
                           lease_access::snapshot (runtime)->last_successful_lease->started_at
                             + options.owner_lease_ttl + options.owner_lease_fencing_margin));
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_NE (token_before_claim.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        lease_access::heartbeat (runtime, clock_now.load ());
        EXPECT_FALSE (early->try_post ("cannot-resume", [&] { ++handlers; }));
        if (loss % 2 == 0)
            EXPECT_FALSE (spot_gate->try_post ("cannot-resume-spot", [&] { ++handlers; }));
        ASSERT_TRUE (close_executor (executor));
        context->node.reset ();
        node->spot_contexts_by_id.clear ();
        return;
    }
    std::vector<std::shared_ptr<detail::spot_context_state_t>> excluded_spots;
    auto closing = std::make_shared<task_completion_source_t<host::spot_close_commit_t>> ();
    auto closing_finished = std::make_shared<task_completion_source_t<bool>> ();
    for (const auto &id : {"creating-spot", "closing-spot", "source-spot", "target-spot"}) {
        auto excluded = std::make_shared<detail::spot_context_state_t> ();
        excluded->node = node;
        excluded->lane_owner = node;
        excluded->serial_executor = scheduler;
        excluded->spot_id = spot_id_t (id);
        excluded->spot_name = "room";
        excluded->mesh_name = descriptor.mesh_name;
        excluded->node_rid = context->node_rid;
        excluded->object_generation = 1;
        excluded->authority_owner_generation = 1;
        excluded->execution_mode = context->execution_mode;
        node->spot_contexts_by_id.emplace (id, detail::spot_context_access_t::create (excluded));
        if (std::string_view (id) != "creating-spot")
            commit (placement_object_kind_t::user_spot, id, "room");
        if (std::string_view (id) == "closing-spot") {
            auto late_confirmation =
              native
                ->prepare_store_ready ({stateful::object_kind_t::user_spot, id, 1, 1,
                                        descriptor.mesh_name, descriptor.rid.to_string ()})
                .result ()
                .value ();
            excluded->request_close ([closing] { return closing->task (); },
                                     [finished = closing_finished] (result_t<bool> result) mutable {
                                         finished->complete (std::move (result));
                                     });
            scheduler->run_all ();
            EXPECT_NE (0u, excluded->close_reservation);
            ASSERT_TRUE (late_confirmation);
            late_confirmation (std::nullopt).result ().value ();
        }
        if (std::string_view (id) == "source-spot" || std::string_view (id) == "target-spot") {
            const auto object = native->objects ().find (stateful::object_kind_t::user_spot, id);
            ASSERT_TRUE (object);
            const auto seal =
              native->objects ().try_seal_relocation_aggregate ({*object}).result ().value ();
            ASSERT_EQ (stateful::stateful_error_t::none, seal.error);
        }
        excluded_spots.push_back (excluded);
    }
    const auto creating = native->objects ().begin_create (
      {stateful::object_kind_t::actor, "creating-actor", "player", descriptor.mesh_name});
    ASSERT_EQ (stateful::stateful_error_t::none, creating.error);
    node->actor_types_by_id["creating-actor"] = "player";
    node->actor_spot_ids["player:creating-actor"] = context->spot_id;
    node->actor_generations["player:creating-actor"] = creating.object.object_generation;
    auto creating_gate =
      context->ensure_spot_serial_executor ()->actor_executor ("player:creating-actor")->queue ();
    EXPECT_FALSE (native->objects ().find (stateful::object_kind_t::actor, "creating-actor"));
    auto executor = context->ensure_spot_serial_executor ();
    auto early = executor->actor_executor ("player:early")->queue ();
    auto late = executor->actor_executor ("player:late")->queue ();
    auto timer = executor->timer_queue ("registered-timer");
    advance (5s);
    // A source that performed Store I/O would fail here.
    provider.fail = memory.fail = true;

    lease_access::communication_failure (runtime);

    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    auto new_timer = executor->timer_queue ("after-snapshot-timer");
    provider.fail = memory.fail = false;
    // This later CAS has exactly observed_at: eligibility requires a strict earlier time.
    commit (placement_object_kind_t::actor, "late", "player");
    move_to (episode->cutoff - std::chrono::steady_clock::duration{1});
    int early_calls = 0, late_calls = 0, timer_calls = 0, new_timer_calls = 0;
    ASSERT_TRUE (early->try_post ("early", [&] { ++early_calls; }));
    ASSERT_TRUE (timer->try_post ("timer", [&] { ++timer_calls; }));
    ASSERT_TRUE (new_timer->try_post ("new-timer", [&] { ++new_timer_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, new_timer_calls);
    EXPECT_EQ (1, early_calls);
    EXPECT_EQ (1, timer_calls);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_TRUE (episode->discarded.load ());
    EXPECT_FALSE (creating_gate->execution_allowed ());
    for (const auto &excluded : excluded_spots) {
        const auto gate = excluded->ensure_spot_serial_executor ()->spot_queue ();
        EXPECT_FALSE (gate->execution_allowed ());
    }
    ASSERT_TRUE (early->try_post ("at-cutoff", [&] { ++early_calls; }));
    ASSERT_TRUE (late->try_post ("late-ready", [&] { ++late_calls; }));
    ASSERT_TRUE (timer->try_post ("at-cutoff-timer", [&] { ++timer_calls; }));
    ASSERT_TRUE (new_timer->try_post ("at-cutoff-new-timer", [&] { ++new_timer_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, early_calls);
    EXPECT_EQ (0, late_calls);
    EXPECT_EQ (1, timer_calls);
    EXPECT_EQ (1, new_timer_calls);
    lease_access::heartbeat (runtime, clock_now.load ());
    auto next_token = *lease_access::snapshot (runtime)->token;
    ++next_token.lease_generation;
    lease_access::accept (runtime, next_token, std::chrono::system_clock::time_point{} + 30s,
                          std::chrono::system_clock::time_point{}, clock_now.load (), false);
    const auto early_ref = native->objects ().find (stateful::object_kind_t::actor, "early");
    ASSERT_TRUE (early_ref);
    auto replay_confirmation = native->prepare_store_ready (*early_ref).result ().value ();
    ASSERT_TRUE (replay_confirmation);
    replay_confirmation (std::nullopt).result ().value ();
    early->invalidate_execution_ready ();
    early->confirm_execution_ready (clock_now.load ());
    ASSERT_TRUE (early->try_post ("recovered-new-claim", [&] { ++early_calls; }));
    scheduler->run_all ();
    EXPECT_EQ (1, early_calls);
    early->cancel_pending ();
    late->cancel_pending ();
    executor->spot_queue ()->cancel_pending ();
    timer->cancel_pending ();
    new_timer->cancel_pending ();
    scheduler->run_all ();
    closing->complete (result_t<host::spot_close_commit_t>::success ({}));
    auto close_terminal = closing_finished->task ();
    ASSERT_TRUE (scheduler->run_until (close_terminal, auto_connect_wait));
    EXPECT_FALSE (close_terminal.result ().value ());
    scheduler->run_all ();
    for (const auto &excluded : excluded_spots) {
        excluded->ensure_spot_serial_executor ()->spot_queue ()->cancel_pending ();
        excluded->node.reset ();
    }
    creating_gate->cancel_pending ();
    scheduler->run_all ();
    node->spot_contexts_by_id.clear ();
    node->native_node.reset ();
    context->node.reset ();
}
} // namespace zlink::framework::runtime
