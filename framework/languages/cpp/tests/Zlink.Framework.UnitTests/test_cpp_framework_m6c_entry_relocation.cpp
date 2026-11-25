/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_memory_authority_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <utility>

using namespace zlink::framework::runtime::stateful;

struct entry_relocation_test_payload_t
{
    int value = 0;
};

template <> struct zlink::json::dto_traits<entry_relocation_test_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("value", &entry_relocation_test_payload_t::value)};
};

class entry_relocation_test_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit entry_relocation_test_actor_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
        ++create_count;
    }
    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

    static inline int create_count = 0;

  private:
    zlink::framework::actor_context_t _context;
};

class entry_relocation_test_actor_factory_t final
    : public zlink::framework::actor_factory_t<entry_relocation_test_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<entry_relocation_test_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<entry_relocation_test_actor_t> (std::move (context));
    }
};

/* An Entry Spot Actor moving as a single-Actor relocation unit never carries
 * a Spot on the wire (spec 28: the Entry Spot is already present on the
 * target node). This fixture proves the target-local Entry Spot is resolved
 * during materialize_relocation_state, membership is restored without join
 * callbacks, and the Actor immediately serves an application message through it. */
class entry_relocation_test_entry_spot_t
    : public zlink::framework::entry_spot_t<entry_relocation_test_actor_t>
{
  public:
    explicit entry_relocation_test_entry_spot_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_send<&entry_relocation_test_entry_spot_t::on_message> (
          "entry-relocation-test-message");
    }

    zlink::framework::task_t<void> on_message (entry_relocation_test_actor_t &,
                                               zlink::framework::message_context_t &,
                                               const entry_relocation_test_payload_t &payload)
    {
        ++message_served_count;
        last_message_value = payload.value;
        co_return;
    }

    zlink::framework::task_t<void> on_actor_joined (entry_relocation_test_actor_t &) override
    {
        ++joined_count;
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (entry_relocation_test_actor_t &) override
    {
        ++leave_count;
        co_return;
    }

    static inline int joined_count = 0;
    static inline int leave_count = 0;
    static inline int message_served_count = 0;
    static inline int last_message_value = 0;

  private:
    zlink::framework::entry_spot_context_t _context;
};

class failing_source_leave_entry_spot_t final : public entry_relocation_test_entry_spot_t
{
  public:
    using entry_relocation_test_entry_spot_t::entry_relocation_test_entry_spot_t;

    zlink::framework::task_t<void> on_leave_actor (entry_relocation_test_actor_t &) override
    {
        ++leave_count;
        throw std::runtime_error ("source leave failed");
        co_return;
    }
};

namespace m6c_test
{

/* checklist B: cpp's standalone Entry Spot Actor direct-relocation restore
 * previously always failed in the real wired path because the target-side
 * Entry Spot was never resolved (restore_relocation always passed
 * target_spot=std::nullopt, and materialize_relocation_state unconditionally
 * rejected actor targets without one). This test exercises the exact
 * function mesh_node_host_service.cpp wires into production
 * (configure_relocation_materialization -> materialize_relocation_state)
 * and proves the fix: it resolves this node's own local Entry Spot. */
void test_entry_spot_actor_relocation_restore_resolves_local_entry_spot (test_context_t &test)
{
    entry_relocation_test_actor_t::create_count = 0;
    entry_relocation_test_entry_spot_t::joined_count = 0;
    entry_relocation_test_entry_spot_t::leave_count = 0;
    entry_relocation_test_entry_spot_t::message_served_count = 0;
    entry_relocation_test_entry_spot_t::last_message_value = 0;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("entry-relocation-restore-mesh");
    mesh.add_entry_spot<entry_relocation_test_entry_spot_t> (
      [] (zlink::framework::entry_spot_context_t context) {
          return std::make_shared<entry_relocation_test_entry_spot_t> (std::move (context));
      });
    mesh.add_actor_factory<entry_relocation_test_actor_t, entry_relocation_test_actor_factory_t> (
      "entry-relocation-test-actor", std::make_shared<entry_relocation_test_actor_factory_t> (),
      [] (auto &factory) { factory.recreate_on_relocation (); });

    auto found_runtime = zlink::framework::detail::spot_node_runtime_t::from (
      builder, "entry-relocation-restore-mesh");
    test.require (found_runtime.has_value (),
                  "entry Spot relocation restore test must resolve its Spot runtime");
    if (!found_runtime)
        return;
    auto runtime = *found_runtime;
    const auto entry_created = runtime.create_spot ("entry");
    test.require (entry_created.state == zlink::framework::spot_create_state_t::created,
                  "the fixture must create its own local Entry Spot before restore");

    const object_ref_t source_actor{.kind = object_kind_t::actor,
                                    .key = "entry-relocation-test-actor-id",
                                    .object_generation = 1,
                                    .authority_owner_generation = 1,
                                    .mesh_name = "mesh",
                                    .node_id = "source"};
    const frozen_object_state_t frozen{.owner = source_actor,
                                       .stable_type = "entry-relocation-test-actor",
                                       .application_state = {},
                                       .pending_application = {},
                                       .timers = {}};
    const object_ref_t target_actor{.kind = object_kind_t::actor,
                                    .key = "entry-relocation-test-actor-id",
                                    .object_generation = 1,
                                    .authority_owner_generation = 2,
                                    .mesh_name = "mesh",
                                    .node_id = "target"};

    const auto materialized =
      runtime.materialize_relocation_state (frozen, target_actor, std::nullopt, {})
        .result ()
        .value ();
    test.require (materialized && entry_relocation_test_actor_t::create_count == 1
                    && entry_relocation_test_entry_spot_t::joined_count == 0,
                  "a standalone Actor relocation unit (target_spot=nullopt) must "
                  "resolve the target startup Entry and restore membership without "
                  "joined callbacks, per host relocation section 10.1");

    const auto entry_state =
      zlink::framework::detail::spot_context_access_t::state (entry_created.context);
    auto *entry =
      entry_state
        ? static_cast<entry_relocation_test_entry_spot_t *> (entry_state->spot_instance.get ())
        : nullptr;
    const auto imported = runtime.actor_instance<entry_relocation_test_actor_t> (
      ::zlink::framework::detail::actor_ref_access_t::make (
        zlink::framework::node_rid_t::from_string (target_actor.node_id), frozen.stable_type,
        target_actor.key, target_actor.object_generation));
    test.require (
      entry && imported,
      "the relocated Actor must be registered in the target Entry without joined callbacks");
    if (entry && imported) {
        zlink::framework::service_provider_t services;
        zlink::framework::serializer_registry_t serializers;
        const auto served = entry->context ().handlers ().invoke_actor_packet (
          "entry-relocation-test-message", *entry, imported->get (), services, serializers,
          zlink::message_t::from (R"({"value":42})"));
        test.require (served && entry_relocation_test_entry_spot_t::message_served_count == 1
                        && entry_relocation_test_entry_spot_t::last_message_value == 42,
                      "the relocated Actor must serve an application message through "
                      "its target Entry Spot immediately after restore");
    }

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

/* A->B->A can return before B's one-way OnLeave notification has cleaned A's
 * retained source instance. The exact A->B Message Follow fence proves that
 * the local instance is the departed source remnant; a newer Store-confirmed
 * target authority may replace it, while the old leave is sealed so it cannot
 * run against the newly materialized Actor. */
void test_return_actor_relocation_replaces_departed_spot_instance (test_context_t &test,
                                                                   bool source_leave_fails,
                                                                   bool delayed_source_completion)
{
    // The observer can outlive this scope, so it owns its counter.
    const auto failed_leave_diagnostics = std::make_shared<std::atomic_int> (0);
    entry_relocation_test_actor_t::create_count = 0;
    entry_relocation_test_entry_spot_t::joined_count = 0;
    entry_relocation_test_entry_spot_t::leave_count = 0;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("return-relocation-remnant-mesh");
    mesh.add_entry_spot<entry_relocation_test_entry_spot_t> (
      [source_leave_fails] (zlink::framework::entry_spot_context_t context)
        -> std::shared_ptr<entry_relocation_test_entry_spot_t> {
          if (source_leave_fails)
              return std::make_shared<failing_source_leave_entry_spot_t> (std::move (context));
          return std::make_shared<entry_relocation_test_entry_spot_t> (std::move (context));
      });
    mesh.add_actor_factory<entry_relocation_test_actor_t, entry_relocation_test_actor_factory_t> (
      "entry-relocation-test-actor", std::make_shared<entry_relocation_test_actor_factory_t> (),
      [] (auto &factory) { factory.recreate_on_relocation (); });

    auto found_runtime = zlink::framework::detail::spot_node_runtime_t::from (
      builder, "return-relocation-remnant-mesh");
    test.require (found_runtime.has_value (),
                  "return relocation fixture must resolve its Spot runtime");
    if (!found_runtime)
        return;
    auto runtime = *found_runtime;
    const auto entry_created = runtime.create_spot ("entry");
    test.require (entry_created.state == zlink::framework::spot_create_state_t::created,
                  "return relocation fixture must create its local Entry Spot");
    const auto state = runtime.weak_state ().lock ();
    test.require (state != nullptr, "return relocation fixture must retain its runtime state");
    if (!state)
        return;
    auto leave_logs = std::make_shared<
      zlink::framework::test::telemetry_observations_t<zlink::framework::log_record_t>> ();
    zlink::framework::logging_builder_t logging;
    logging.use_provider ("return-leave",
                          [leave_logs] (const auto &record) { leave_logs->record (record); });
    zlink::framework::detail::dispatch_options_access_t::set_logger (
      state->dispatch, logging.create_logger ("return-leave"));
    state->dispatch.message_flow (zlink::framework::message_flow_log_mode_t::errors);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      state->dispatch,
      [failed_leave_diagnostics] (const zlink::framework::message_flow_event_t &event) {
          if (event.packet_name == "spot_actor_leave"
              && event.outcome == zlink::framework::message_flow_outcome_t::completed
              && event.surface == zlink::framework::dispatch_error_surface_t::spot_actor
              && event.message_kind == zlink::framework::dispatch_message_kind_t::control
              && event.result == zlink::framework::message_flow_result_t::failed && !event.reason
              && event.exception)
              ++*failed_leave_diagnostics;
      });
    auto authority = std::make_shared<memory_authority_store_t> ();
    runtime.bind_relocation_authority (authority);

    const object_ref_t first_source{.kind = object_kind_t::actor,
                                    .key = "returning-entry-actor",
                                    .object_generation = 1,
                                    .authority_owner_generation = 1,
                                    .mesh_name = "mesh",
                                    .node_id = "node-x"};
    const frozen_object_state_t first_frozen{.owner = first_source,
                                             .stable_type = "entry-relocation-test-actor",
                                             .application_state = {},
                                             .pending_application = {},
                                             .timers = {}};
    const object_ref_t first_target{.kind = object_kind_t::actor,
                                    .key = "returning-entry-actor",
                                    .object_generation = 1,
                                    .authority_owner_generation = 2,
                                    .mesh_name = "mesh",
                                    .node_id = "node-a"};
    test.require (
      runtime.materialize_relocation_state (first_frozen, first_target, std::nullopt, {})
        .result ()
        .value (),
      "fixture must first materialize the Actor on node A");

    const auto key = std::string ("entry-relocation-test-actor:returning-entry-actor");
    const auto old_instance = state->actor_instances.at (key);
    const auto source_actor = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("node-a"), "entry-relocation-test-actor",
      "returning-entry-actor", 1);
    const auto returned_source = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("node-b"), "entry-relocation-test-actor",
      "returning-entry-actor", 1);
    const auto source_fence = zlink::framework::runtime::protocol::actor_route_fence_t{
      "returning-entry-actor", 1, zlink::routing_id_t::from ("node-a").to_bytes (), 1, 2, 4};
    const auto target_fence = zlink::framework::runtime::protocol::actor_route_fence_t{
      "returning-entry-actor", 1, zlink::routing_id_t::from ("node-b").to_bytes (), 1, 3, 5};
    const auto entry_id = state->spot_ids_by_name.at ("entry");
    state->actor_authority_fences.erase (key);
    if (delayed_source_completion) {
        test.require (state->actor_transfer_coordinator->try_begin_source_remote (key, "return-a-b")
                        .has_value (),
                      "fixture must retain the previous source Join until publication");
    } else {
        zlink::framework::detail::record_actor_route_unlocked (
          *state, key,
          zlink::framework::spot_route_t{zlink::framework::node_rid_t::from_string ("node-b"),
                                         zlink::framework::spot_id_t ("spot-b"), "entry"},
          1);
        state->actor_transfer_coordinator->activate_message_follow (
          key, source_fence, returned_source,
          zlink::framework::spot_route_t{zlink::framework::node_rid_t::from_string ("node-b"),
                                         zlink::framework::spot_id_t ("spot-b"), "entry"},
          target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
          "return-a-b");
        state->pending_remote_source_cleanups.push_back (
          zlink::framework::detail::spot_node_builder_state_t::pending_remote_source_cleanup_t{
            .source_actor = source_actor,
            .source_fence = source_fence,
            .transfer_id = "return-a-b",
            .source_spot_id = entry_id,
            .source_spot_generation = 1,
            .target_spot_id = zlink::framework::spot_id_t ("spot-b"),
            .not_before = std::chrono::steady_clock::now () + std::chrono::seconds (30),
            .leave_submitted = false,
            .leave_completed = false,
            .leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30)});
    }

    const object_ref_t returning_source{.kind = object_kind_t::actor,
                                        .key = "returning-entry-actor",
                                        .object_generation = 1,
                                        .authority_owner_generation = 3,
                                        .mesh_name = "mesh",
                                        .node_id = "node-b"};
    const frozen_object_state_t returning_frozen{.owner = returning_source,
                                                 .stable_type = "entry-relocation-test-actor",
                                                 .application_state = {},
                                                 .pending_application = {},
                                                 .timers = {}};
    auto returning_target = first_target;
    returning_target.authority_owner_generation = 4;
    auto returning =
      runtime.materialize_relocation_state (returning_frozen, returning_target, std::nullopt, {});
    if (delayed_source_completion) {
        test.require (
          !returning.await_ready () && state->actor_instances.at (key) == old_instance,
          "return must await the previous source Join terminal before replacing its instance");
        auto source_completion = runtime.complete_remote_actor_transfer (
          source_actor, returned_source,
          zlink::framework::spot_route_t{zlink::framework::node_rid_t::from_string ("node-b"),
                                         zlink::framework::spot_id_t ("spot-b"), "entry"},
          source_fence,
          {target_fence.target_node_routing_id,
           target_fence.target_node_generation,
           {},
           target_fence.owner_lease_generation},
          target_fence.authority_owner_generation, "return-a-b");
        source_completion.result ().value ();
    }
    const auto returned = returning.result ().value ();
    if (delayed_source_completion) {
        (void) runtime
          .submit_remote_actor_leave_async ("return-a-b", source_actor, entry_id, 1,
                                            zlink::framework::spot_id_t ("spot-b"), target_fence)
          .result ()
          .value ();
        zlink::framework::detail::spot_context_access_t::state (entry_created.context)
          ->run_serial_task ("after-late-source-leave",
                             [] () -> zlink::framework::task_t<void> { co_return; })
          .result ()
          .value ();
    }

    const auto cleanup = std::find_if (
      state->pending_remote_source_cleanups.begin (), state->pending_remote_source_cleanups.end (),
      [] (const auto &candidate) { return candidate.transfer_id == "return-a-b"; });
    test.require (returned && state->actor_instances.at (key) != old_instance
                    && entry_relocation_test_actor_t::create_count == 2
                    && entry_relocation_test_entry_spot_t::joined_count == 0
                    && entry_relocation_test_entry_spot_t::leave_count == 1
                    && cleanup != state->pending_remote_source_cleanups.end ()
                    && cleanup->leave_submitted && cleanup->leave_completed,
                  "a newer exact-fenced return must retire the old source lifecycle, "
                  "suppress its late leave, and install a fresh local Actor");
    zlink::framework::test::acknowledge_delivery ();
    const auto leave_records = leave_logs->snapshot ();
    test.require (
      std::count_if (
        leave_records.begin (), leave_records.end (),
        [] (const auto &record) {
            const auto has = [&] (std::string_view key, std::string_view value) {
                return std::any_of (
                  record.fields.begin (), record.fields.end (),
                  [&] (const auto &field) { return field.key == key && field.value == value; });
            };
            return has (zlink::framework::detail::dispatch_event_field::packet, "spot_actor_leave")
                   && has (zlink::framework::detail::dispatch_event_field::error_type,
                           typeid (std::runtime_error).name ())
                   && has (zlink::framework::detail::dispatch_event_field::error_message,
                           "source leave failed");
        })
        == (source_leave_fails ? 1 : 0),
      "return Leave must retain the exact owned failure summary");
    test.require (
      *failed_leave_diagnostics == (source_leave_fails ? 1 : 0),
      "source OnLeave failure must be diagnosed once while return restores a fresh Actor");
    test.require (state->actor_generations.at (key) == first_source.object_generation,
                  "fresh return instance must retain ObjectGeneration");
    if (cleanup == state->pending_remote_source_cleanups.end ())
        return;
    runtime.cleanup_expired_actor_admissions_at (cleanup->not_before);
    zlink::framework::test::acknowledge_delivery ();
    test.require (entry_relocation_test_entry_spot_t::leave_count == 1
                    && *failed_leave_diagnostics == (source_leave_fails ? 1 : 0)
                    && state->actor_instances.contains (key)
                    && state->actor_instances.at (key) != old_instance
                    && state->actor_routes.at (key).node_rid.value () == "node-a",
                  "retired source cleanup must not rerun leave or erase newer local authority");

    // The ordinary source cleanup may have already erased the instance and
    // its cleanup record while the longer-lived remote Message Follow route
    // remains. That exact remote route is cache state, not a competing local
    // Actor, and a still newer Store-validated return replaces it directly.
    state->actor_authority_fences.erase (key);
    state->pending_remote_source_cleanups.clear ();
    zlink::framework::detail::record_actor_route_unlocked (
      *state, key,
      zlink::framework::spot_route_t{zlink::framework::node_rid_t::from_string ("node-b"),
                                     zlink::framework::spot_id_t ("spot-b"), "entry"},
      1);
    auto route_only_source = returning_source;
    route_only_source.authority_owner_generation = 5;
    auto route_only_frozen = returning_frozen;
    route_only_frozen.owner = route_only_source;
    auto route_only_target = returning_target;
    route_only_target.authority_owner_generation = 6;
    const auto route_only_returned =
      runtime.materialize_relocation_state (route_only_frozen, route_only_target, std::nullopt, {})
        .result ()
        .value ();
    test.require (route_only_returned && entry_relocation_test_actor_t::create_count == 3
                    && entry_relocation_test_entry_spot_t::joined_count == 0
                    && entry_relocation_test_entry_spot_t::leave_count == 1
                    && state->actor_instances.contains (key)
                    && state->actor_routes.at (key).node_rid.value () == "node-a",
                  "an exact departed remote route without local authority, even when its "
                  "instance erase lagged, must not veto a newer Store-validated return");

    // Message Follow expiry can remove the remote route before its membership
    // cache. With no instance, route, authority, or cleanup left, the exact-
    // generation actor_spot_ids entry is an orphan and cannot veto the next
    // Store-validated return.
    zlink::framework::detail::erase_actor_instance_index_unlocked (
      *state, "entry-relocation-test-actor", "returning-entry-actor");
    state->actor_instances.erase (key);
    state->actor_authority_fences.erase (key);
    state->actor_routes.erase (key);
    state->pending_remote_source_cleanups.clear ();
    auto orphan_source = route_only_source;
    orphan_source.authority_owner_generation = 7;
    auto orphan_frozen = route_only_frozen;
    orphan_frozen.owner = orphan_source;
    auto orphan_target = route_only_target;
    orphan_target.authority_owner_generation = 8;
    const auto orphan_returned =
      runtime.materialize_relocation_state (orphan_frozen, orphan_target, std::nullopt, {})
        .result ()
        .value ();
    test.require (orphan_returned && entry_relocation_test_actor_t::create_count == 4
                    && entry_relocation_test_entry_spot_t::joined_count == 0
                    && state->actor_instances.contains (key)
                    && state->actor_routes.at (key).node_rid.value () == "node-a",
                  "an exact-generation orphan Spot membership cache must not veto a "
                  "newer Store-validated return");

    auto committed_orphan_target = orphan_target;
    committed_orphan_target.authority_owner_generation = 11;
    state->actor_authority_fences.at (key).target_node_generation = 23;
    const auto staged_fence = state->actor_authority_fences.at (key);
    const auto committed_adopted = runtime.adopt_committed_actor_relocation_authority (
      committed_orphan_target, staged_fence.target_node_generation, 17);
    auto stale_committed_target = committed_orphan_target;
    --stale_committed_target.authority_owner_generation;
    const auto stale_adopted = runtime.adopt_committed_actor_relocation_authority (
      stale_committed_target, staged_fence.target_node_generation, 19);
    test.require (committed_adopted && !stale_adopted
                    && state->actor_authority_fences.at (key).authority_owner_generation == 11
                    && state->actor_authority_fences.at (key).owner_lease_generation == 17,
                  "post-CAS target adoption must install the exact Store fence before route "
                  "publication and reject a stale generation");
    {
        std::lock_guard lock (authority->mutex);
        authority->rows[authority_key (object_kind_t::actor, orphan_target.key)] =
          authority_relocation_reference_t{.source = orphan_source,
                                           .target = committed_orphan_target,
                                           .target_owner = {"return-target-owner", 17}};
    }
    const auto materialization_committed =
      runtime.commit_relocation_materialization ({orphan_target});
    const auto committed_fence = state->actor_authority_fences.find (key);
    test.require (materialization_committed
                    && committed_fence != state->actor_authority_fences.end ()
                    && committed_fence->second.authority_owner_generation == 11
                    && committed_fence->second.owner_lease_generation == 17,
                  "materialization commit must retain the Store-confirmed target authority "
                  "generation and lease when the Store advances by more than one");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

/* Negative counterpart: a genuinely spot-less arrival (no local Entry Spot
 * resolvable on the target node) must still fail explicitly, never crash. */
void test_entry_spot_actor_relocation_restore_fails_without_local_entry_spot (test_context_t &test)
{
    entry_relocation_test_actor_t::create_count = 0;

    zlink::framework::spot_node_builder_t builder;
    builder
      .add_actor_factory<entry_relocation_test_actor_t, entry_relocation_test_actor_factory_t> (
        "entry-relocation-test-actor", std::make_shared<entry_relocation_test_actor_factory_t> (),
        [] (auto &factory) { factory.recreate_on_relocation (); });

    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder);
    /* Deliberately no add_entry_spot / create_spot ("entry"): the target
       node has no local Entry Spot to resolve. */

    const object_ref_t source_actor{.kind = object_kind_t::actor,
                                    .key = "entry-relocation-test-actor-id",
                                    .object_generation = 1,
                                    .authority_owner_generation = 1,
                                    .mesh_name = "mesh",
                                    .node_id = "source"};
    const frozen_object_state_t frozen{.owner = source_actor,
                                       .stable_type = "entry-relocation-test-actor",
                                       .application_state = {},
                                       .pending_application = {},
                                       .timers = {}};
    const object_ref_t target_actor{.kind = object_kind_t::actor,
                                    .key = "entry-relocation-test-actor-id",
                                    .object_generation = 1,
                                    .authority_owner_generation = 2,
                                    .mesh_name = "mesh",
                                    .node_id = "target"};

    const auto materialized =
      runtime.materialize_relocation_state (frozen, target_actor, std::nullopt, {})
        .result ()
        .value ();
    test.require (!materialized && entry_relocation_test_actor_t::create_count == 0,
                  "a standalone Actor relocation unit must fail explicitly (not "
                  "crash) when the target node has no local Entry Spot to resolve");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

} // namespace m6c_test
