/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/timers/async_delay.hpp"
#include <zlink/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace zlink::framework::runtime::stateful;

class aggregate_materialized_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit aggregate_materialized_actor_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

    int value = 37;

  private:
    zlink::framework::actor_context_t _context;
};

class aggregate_materialized_actor_factory_t final
    : public zlink::framework::actor_factory_t<aggregate_materialized_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<aggregate_materialized_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        if (record_target.load (std::memory_order_acquire))
            factory_order.store (++sequence, std::memory_order_release);
        co_return std::make_shared<aggregate_materialized_actor_t> (std::move (context));
    }

    static inline std::atomic_bool record_target{false};
    static inline std::atomic_int sequence{0};
    static inline std::atomic_int factory_order{0};
};

class aggregate_materialized_actor_adapter_t final
    : public zlink::framework::actor_relocation_adapter_t<aggregate_materialized_actor_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (aggregate_materialized_actor_t &actor,
                                                              std::stop_token) override
    {
        capture_count.fetch_add (1, std::memory_order_acq_rel);
        co_return std::vector<std::byte>{static_cast<std::byte> (actor.value)};
    }

    zlink::framework::task_t<void> restore (aggregate_materialized_actor_t &actor,
                                            std::vector<std::byte> payload,
                                            std::stop_token) override
    {
        actor.value = payload.empty () ? -1 : std::to_integer<int> (payload.front ());
        restored_value.store (actor.value, std::memory_order_release);
        restore_order.store (++aggregate_materialized_actor_factory_t::sequence,
                             std::memory_order_release);
        co_return;
    }

    static inline std::atomic_int capture_count{0};
    static inline std::atomic_int restore_order{0};
    static inline std::atomic_int restored_value{-1};
};

struct aggregate_lifecycle_probe_message_t
{
};

template <> struct zlink::json::dto_traits<aggregate_lifecycle_probe_message_t>
{
    static constexpr auto fields = std::tuple{};
};


class aggregate_materialized_spot_t final
    : public zlink::framework::spot_t<aggregate_materialized_actor_t>
{
  public:
    explicit aggregate_materialized_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override { configure_lifecycle_probe (); }

    using lifecycle_probe_message_t = aggregate_lifecycle_probe_message_t;

    zlink::framework::task_t<void> lifecycle_probe (aggregate_materialized_actor_t &,
                                                    zlink::framework::message_context_t &,
                                                    const lifecycle_probe_message_t &)
    {
        co_return;
    }

    void configure_lifecycle_probe ()
    {
        _context.handlers ().add_actor_send<&aggregate_materialized_spot_t::lifecycle_probe> (
          "aggregate-lifecycle-probe");
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }
    zlink::framework::task_t<void> on_initialize () override { co_return; }
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        membership_callbacks.fetch_add (1, std::memory_order_acq_rel);
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void> on_actor_joined (aggregate_materialized_actor_t &) override
    {
        membership_callbacks.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (aggregate_materialized_actor_t &) override
    {
        membership_callbacks.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }
    zlink::framework::task_t<void> on_relocation_ready_completed (
      const zlink::framework::spot_relocation_ready_completion_t &completion) override
    {
        ready_after_cas.store (completion.outcome
                                   == zlink::framework::spot_relocation_ready_outcome_t::relocated
                                 && authority_commit_count && authority_commit_count () == 1,
                               std::memory_order_release);
        const bool visible = membership_visible && co_await membership_visible ();
        ready_saw_membership_state.store (visible, std::memory_order_release);
        ready_before_dispatch.store (
          target_objects && target_actor
            && target_objects->try_claim (*target_actor, turn_domain_t::application).first
                 == stateful_error_t::moving,
          std::memory_order_release);
        ready_order.store (++aggregate_materialized_actor_factory_t::sequence,
                           std::memory_order_release);
        co_return;
    }

    int value = 19;
    static inline std::function<zlink::framework::task_t<bool> ()> membership_visible;
    static inline std::function<int ()> authority_commit_count;
    static inline stateful_object_runtime_t *target_objects = nullptr;
    static inline const object_ref_t *target_actor = nullptr;
    static inline std::atomic_bool ready_saw_membership_state{false};
    static inline std::atomic_bool restore_before_cas{false};
    static inline std::atomic_bool restore_held_ingress{false};
    static inline std::atomic_bool ready_after_cas{false};
    static inline std::atomic_bool ready_before_dispatch{false};
    static inline std::atomic_int restore_order{0};
    static inline std::atomic_int membership_callbacks{0};
    static inline std::atomic_int ready_order{0};

  private:
    zlink::framework::spot_context_t _context;
};

class aggregate_materialized_spot_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<aggregate_materialized_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (aggregate_materialized_spot_t &spot,
                                                              std::stop_token) override
    {
        capture_count.fetch_add (1, std::memory_order_acq_rel);
        co_return std::vector<std::byte>{static_cast<std::byte> (spot.value)};
    }
    zlink::framework::task_t<void> restore (aggregate_materialized_spot_t &spot,
                                            std::vector<std::byte> payload,
                                            std::stop_token) override
    {
        spot.value = payload.empty () ? -1 : std::to_integer<int> (payload.front ());
        restored_value.store (spot.value, std::memory_order_release);
        if (aggregate_materialized_actor_factory_t::record_target.load (
              std::memory_order_acquire)) {
            aggregate_materialized_spot_t::restore_before_cas.store (
              aggregate_materialized_spot_t::authority_commit_count
                && aggregate_materialized_spot_t::authority_commit_count () == 0,
              std::memory_order_release);
            aggregate_materialized_spot_t::restore_held_ingress.store (
              aggregate_materialized_spot_t::target_objects
                && aggregate_materialized_spot_t::target_actor
                && aggregate_materialized_spot_t::target_objects->enqueue (
                     *aggregate_materialized_spot_t::target_actor, turn_domain_t::application,
                     {201, {0x51}})
                     == stateful_error_t::none,
              std::memory_order_release);
            aggregate_materialized_spot_t::restore_order.store (
              ++aggregate_materialized_actor_factory_t::sequence, std::memory_order_release);
        }
        co_return;
    }

    static inline std::atomic_int capture_count{0};
    static inline std::atomic_int restored_value{-1};
};

namespace m6c_test
{

void test_application_user_spot_aggregate_remote_production_path (test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;

    framework::serializer_registry_t source_serializers;
    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_state = [core_context] (const std::string &rid) {
        auto state = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::mesh_node_builder_state_t> ("production-aggregate-mesh");
        state->core_context = core_context;
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_builder.add_spot_factory<aggregate_materialized_spot_t> (
          "production.aggregate.spot",
          [] (framework::spot_context_t context) {
              return std::make_shared<aggregate_materialized_spot_t> (std::move (context));
          },
          [] (auto &factory) {
              factory.set_execution_mode (framework::user_spot_execution_mode_t::spot_wide);
              factory.set_relocation_coordination_mode (
                framework::spot_relocation_coordination_mode_t::application_signaled);
              factory.template preserve_state_with<aggregate_materialized_spot_adapter_t> ();
          });
        state->spot_builder.add_actor_factory<aggregate_materialized_actor_t> (
          "production.aggregate.actor", std::make_shared<aggregate_materialized_actor_factory_t> (),
          [] (auto &factory) {
              factory.template preserve_state_with<aggregate_materialized_actor_adapter_t> ();
          });
        return state;
    };
    aggregate_materialized_actor_factory_t::record_target.store (false);
    aggregate_materialized_actor_factory_t::sequence.store (0);
    aggregate_materialized_actor_factory_t::factory_order.store (0);
    aggregate_materialized_actor_adapter_t::capture_count.store (0);
    aggregate_materialized_actor_adapter_t::restore_order.store (0);
    aggregate_materialized_actor_adapter_t::restored_value.store (-1);
    aggregate_materialized_spot_adapter_t::capture_count.store (0);
    aggregate_materialized_spot_adapter_t::restored_value.store (-1);
    aggregate_materialized_spot_t::ready_saw_membership_state.store (false);
    aggregate_materialized_spot_t::restore_before_cas.store (false);
    aggregate_materialized_spot_t::restore_held_ingress.store (false);
    aggregate_materialized_spot_t::ready_after_cas.store (false);
    aggregate_materialized_spot_t::ready_before_dispatch.store (false);
    aggregate_materialized_spot_t::restore_order.store (0);
    aggregate_materialized_spot_t::membership_callbacks.store (0);
    aggregate_materialized_spot_t::ready_order.store (0);
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates = std::make_shared<memory_aggregate_authority_t> (authority);
    const auto source_state = make_state ("production-aggregate-source");
    const auto target_state = make_state ("production-aggregate-target");
    std::mutex delay_mutex;
    detail::delay_owner_t delays (delay_mutex);
    detail::mesh_node_runtime_t source (source_state);
    detail::mesh_node_runtime_t target (target_state);
    source.bind_delay_owner (delays);
    target.bind_delay_owner (delays);
    source.configure_relocation_runtime (authority, roots, aggregates);
    target.configure_relocation_runtime (authority, roots, aggregates);
    source.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"source-owner", 1}}; });
    target.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"target-owner", 9}}; });
    source.start ().result ().value ();
    target.start ().result ().value ();
    const auto configure_materialization =
      [authority] (detail::mesh_node_runtime_t &node,
                   const std::shared_ptr<detail::spot_node_builder_state_t> &spot_state) {
          detail::spot_node_runtime_t (spot_state).bind_relocation_authority (authority);
          auto &objects = node.native_node ().objects ();
          objects.configure_relocation_state (
            [spot_state] (const object_ref_t &object, const std::string &stable_type,
                          std::stop_token cancellation) {
                return detail::spot_node_runtime_t (spot_state)
                  .capture_relocation_state (object, stable_type, cancellation);
            },
            [spot_state] (const frozen_object_state_t &frozen, const object_ref_t &object,
                          std::stop_token cancellation) {
                return detail::spot_node_runtime_t (spot_state)
                  .restore_spot_relocation_state (frozen, object, cancellation);
            });
          objects.configure_relocation_materialization (
            [spot_state] (const frozen_object_state_t &frozen, const object_ref_t &object,
                          const std::optional<object_ref_t> &spot, std::stop_token cancellation) {
                return detail::spot_node_runtime_t (spot_state)
                  .materialize_relocation_state (frozen, object, spot, cancellation);
            },
            [spot_state] (const std::vector<object_ref_t> &objects,
                          const auto &before_join_completion) {
                return detail::spot_node_runtime_t (spot_state)
                  .commit_relocation_materialization (objects, before_join_completion);
            },
            [spot_state] (const std::vector<object_ref_t> &objects) {
                return detail::spot_node_runtime_t (spot_state)
                  .abort_relocation_materialization_async (objects);
            });
      };
    configure_materialization (source, source_state->spot_state);
    configure_materialization (target, target_state->spot_state);
    source.connect_peer (target.status ().routing_id (), target.status ().local_endpoint ());
    const auto admission_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (target.status ().routing_id (),
                                       target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < admission_deadline) {
        (void) source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
        (void) target.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    const auto source_admitted = source.has_admitted_peer (
      target.status ().routing_id (), target.status ().lifecycle_generation ());
    const auto target_admitted = target.has_admitted_peer (
      source.status ().routing_id (), source.status ().lifecycle_generation ());
    test.require (source_admitted && target_admitted,
                  "production aggregate source and target must both admit the peer");
    if (!source_admitted || !target_admitted) {
        source.stop ();
        target.stop ();
        return;
    }

    detail::spot_node_runtime_t source_spots (source_state->spot_state);
    const auto source_native_node = source_state->spot_state->native_node.lock ();
    source_state->spot_state->native_node.reset ();
    // The source is already Ready; target restore remains staged until commit.
    source_state->spot_state->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    source_state->spot_state->channel_runtime->serializers = &source_serializers;
    const auto application_spot = source_spots.get_or_create_spot (
      "production.aggregate.spot", framework::spot_id_t ("production-aggregate-spot"), {}, 1,
      "production-aggregate-mesh", 1);
    source_state->spot_state->native_node = source_native_node;
    test.require (application_spot.state == framework::spot_create_state_t::created,
                  "production aggregate source Spot application must be materialized");
    if (application_spot.state != framework::spot_create_state_t::created) {
        source.stop ();
        target.stop ();
        return;
    }

    const auto created_actor = source.create_application_actor (
      "production.aggregate.actor", "production-aggregate-actor", std::nullopt, 1s);
    test.require (static_cast<bool> (created_actor), "production aggregate Actor must be created");
    if (!created_actor) {
        source.stop ();
        target.stop ();
        return;
    }
    const auto actor_handle = created_actor.value ();
    auto &source_objects = source.native_node ().objects ();
    source_objects.replace_placement_candidates (
      {{.mesh_name = "production-aggregate-mesh",
        .node_id = source.status ().routing_id ().to_string (),
        .stable_types = {"production.aggregate.actor", "production.aggregate.spot"},
        .weight = 100,
        .active_capacity = 100,
        .active_count = 1,
        .pending_capacity = 100,
        .pending_count = 0}});
    auto created_spot = source_objects.begin_create (
      {.kind = object_kind_t::user_spot,
       .key = "production-aggregate-spot",
       .stable_type = "production.aggregate.spot",
       .mesh_name = std::optional<std::string>{"production-aggregate-mesh"},
       .creation_request = {},
       .exclusive = true,
       .instance_intent = false});
    test.require (created_spot.status == create_status_t::reserved
                    && source_objects.commit_create (created_spot.attempt)
                         == stateful_error_t::none,
                  "production aggregate User Spot must be created");
    const auto actor = source.native_node ().resolve_actor_async (actor_handle).result ().value ();
    const auto spot = source_objects.find (object_kind_t::user_spot, "production-aggregate-spot");
    if (!actor || !spot) {
        test.require (false, "production aggregate participants must resolve");
        source.stop ();
        target.stop ();
        return;
    }
    const auto [join_error, join] =
      source_objects.begin_membership_move_async (*actor, *spot).result ().value ();
    const auto [commit_error, joined_actor] =
      source_objects.commit_membership_move_async (join).result ().value ();
    test.require (join_error == stateful_error_t::none && commit_error == stateful_error_t::none,
                  "production aggregate Actor must join its User Spot");

    const auto application_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
      framework::node_rid_t::from_string (joined_actor.node_id), "production.aggregate.actor",
      joined_actor.key, joined_actor.object_generation);
    detail::actor_gateway_runtime_t source_actor_gateway;
    auto source_actor_context = source_actor_gateway.actor_context (application_actor_ref);
    std::shared_ptr<void> application_actor;
    source_state->spot_state->lane
      .run_checked ([&] {
          const auto factory =
            source_state->spot_state->actor_factories.find ("production.aggregate.actor");
          if (factory != source_state->spot_state->actor_factories.end ()) {
              application_actor =
                factory->second.create_context_instance (std::move (source_actor_context));
          }
      })
      .get ();
    test.require (static_cast<bool> (application_actor),
                  "production aggregate source Actor application must be materialized");
    if (!application_actor) {
        source.stop ();
        target.stop ();
        return;
    }
    source_state->spot_state->lane
      .run_checked ([&] {
          const auto key = std::string ("production.aggregate.actor:") + joined_actor.key;
          detail::record_actor_instance_index_unlocked (
            *source_state->spot_state, application_actor_ref, application_actor.get ());
          source_state->spot_state->actor_instances.emplace (key, application_actor);
      })
      .get ();
    source_spots.record_actor_spot (application_actor_ref,
                                    framework::spot_id_t ("production-aggregate-spot"));
    test.require (source_objects.register_timer (*spot, {101, 1000, 250, 7})
                      == stateful_error_t::none
                    && source_objects.register_timer (joined_actor, {102, 2000, 0, 8})
                         == stateful_error_t::none,
                  "production aggregate timers must be registered");
    object_ref_t expected_spot = *spot;
    expected_spot.node_id = target.status ().routing_id ().to_string ();
    object_ref_t expected_actor = joined_actor;
    expected_actor.node_id = target.status ().routing_id ().to_string ();
    // Relocation-flow §4.3: hidden staging keeps the source fence until Store CAS.
    aggregate_materialized_spot_t::authority_commit_count = [aggregates, authority,
                                                             &expected_actor] {
        std::lock_guard lock (aggregates->mutex);
        if (aggregates->commit_count != 0) {
            const auto committed = authority->read (expected_actor.kind, expected_actor.key);
            if (committed)
                expected_actor.authority_owner_generation =
                  committed->target.authority_owner_generation;
        }
        return aggregates->commit_count;
    };
    aggregate_materialized_spot_t::target_objects = &target.native_node ().objects ();
    aggregate_materialized_spot_t::target_actor = &expected_actor;
    aggregate_materialized_spot_t::membership_visible = [target_state, &expected_actor,
                                                         expected_spot] {
        return target_state->spot_state->lane.run_task ([target_state, &expected_actor,
                                                         expected_spot] {
            const auto key = std::string ("production.aggregate.actor:") + expected_actor.key;
            const auto spot_id = target_state->spot_state->actor_spot_ids.find (key);
            const auto generation = target_state->spot_state->actor_generations.find (key);
            const auto instance = target_state->spot_state->actor_instances.find (key);
            const auto fence = target_state->spot_state->actor_authority_fences.find (key);
            return instance != target_state->spot_state->actor_instances.end ()
                   && static_cast<aggregate_materialized_actor_t *> (instance->second.get ())->value
                        == 37
                   && spot_id != target_state->spot_state->actor_spot_ids.end ()
                   && std::string (spot_id->second) == expected_spot.key
                   && generation != target_state->spot_state->actor_generations.end ()
                   && generation->second == expected_actor.object_generation
                   && fence != target_state->spot_state->actor_authority_fences.end ()
                   && fence->second.authority_owner_generation
                        == expected_actor.authority_owner_generation;
        });
    };
    try {
        const auto source_spot_state =
          source_spots.capture_relocation_state (*spot, "production.aggregate.spot", {})
            .result ()
            .value ()
            .application_state;
        test.require (source_spot_state == std::vector<std::uint8_t>{19},
                      "production aggregate source Spot adapter must be capturable");
    }
    catch (const std::exception &error) {
        std::cerr << "V11-M6C-CPP source Spot capture diagnostic: " << error.what () << '\n';
        test.require (false, "production aggregate source Spot adapter must be capturable");
    }
    aggregate_materialized_spot_adapter_t::capture_count.store (0);
    aggregate_materialized_actor_factory_t::record_target.store (true, std::memory_order_release);
    {
        /* The direct-transfer stream carries no participant identity: the
         * target reconstructs the inventory from Location Store authority
         * rows.  Serve those rows from the shared in-memory authority. */
        std::lock_guard lock (authority->mutex);
        authority->participant_identities = {
          relocation_participant_identity_t{*spot, "production.aggregate.spot", std::nullopt},
          relocation_participant_identity_t{
            joined_actor, "production.aggregate.actor",
            std::pair{std::string ("production-aggregate-spot"), spot->object_generation}}};
    }

    const auto make_authority = [&] (const object_ref_t &object,
                                     framework::placement_object_kind_t kind,
                                     std::string stable_type, std::string store_version) {
        return framework::authority_snapshot_t{
          .store_version = std::move (store_version),
          .payload = {},
          .object_generation = object.object_generation,
          .authority_owner_generation = object.authority_owner_generation,
          .owner = {"source-owner", 1},
          .store_now = std::chrono::system_clock::now (),
          .allocation = {
            framework::placement_allocation_state_t::active,
            kind,
            std::move (stable_type),
            {"production-aggregate-mesh",
             framework::node_rid_t::from_string (source.status ().routing_id ().to_string ()),
             source.status ().lifecycle_generation (),
             {"source-owner", 1}},
            {1, 0, std::nullopt}}};
    };
    framework::mesh_node_descriptor_t target_descriptor;
    target_descriptor.mesh_name = "production-aggregate-mesh";
    target_descriptor.rid = target.status ().routing_id ();
    target_descriptor.lifecycle_generation = target.status ().lifecycle_generation ();
    target_descriptor.application_version = 1;
    target_descriptor.owner_id = "target-owner";
    target_descriptor.lease_generation = 9;

    aggregate_relocation_result_t result;
    std::atomic<bool> stop_dispatch{false};
    const auto dispatch = [&] (detail::mesh_node_runtime_t &node) {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            (void) node.dispatch_ready ([] (const auto &, const auto &, auto, auto) {});
            std::this_thread::sleep_for (1ms);
        }
    };
    std::thread relocation_thread ([&] {
        result = await_task (source.relocate_application_unit (
          {*spot, joined_actor}, {"production.aggregate.spot", "production.aggregate.actor"},
          target_descriptor,
          {make_authority (*spot, framework::placement_object_kind_t::user_spot,
                           "production.aggregate.spot", "aggregate-spot-v1"),
           make_authority (joined_actor, framework::placement_object_kind_t::actor,
                           "production.aggregate.actor", "aggregate-actor-v1")},
          std::chrono::steady_clock::now () + std::chrono::seconds (5)));
    });
    std::thread source_dispatch ([&] { dispatch (source); });
    std::thread target_dispatch ([&] { dispatch (target); });
    relocation_thread.join ();
    const auto target_committed = wait_until_bounded (
      [&] {
          return aggregates->commit_count == 1
                 && target.native_node ().objects ().find (object_kind_t::user_spot,
                                                           "production-aggregate-spot")
                 && target.native_node ().objects ().find (object_kind_t::actor,
                                                           "production-aggregate-actor");
      },
      5s);
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();

    const auto published_spot = authority->read (expected_spot.kind, expected_spot.key);
    const auto published_actor = authority->read (expected_actor.kind, expected_actor.key);
    test.require (published_spot && published_actor,
                  "target fences must come from the committed Store rows");
    if (published_spot)
        expected_spot.authority_owner_generation =
          published_spot->target.authority_owner_generation;
    if (published_actor)
        expected_actor.authority_owner_generation =
          published_actor->target.authority_owner_generation;
    const auto restored_spot =
      target.native_node ().objects ().find (object_kind_t::user_spot, expected_spot.key);
    const auto restored_actor =
      target.native_node ().objects ().find (object_kind_t::actor, expected_actor.key);
    const auto [saved_claim_error, saved_claim] =
      target.native_node ().objects ().try_claim (expected_actor, turn_domain_t::application);
    const auto saved_completed = saved_claim
                                 && target.native_node ().objects ().complete_claim (
                                      expected_actor, turn_domain_t::application)
                                      == stateful_error_t::none;
    const auto lifecycle_ordered =
      aggregate_materialized_spot_t::restore_order.load (std::memory_order_acquire) == 1
      && aggregate_materialized_actor_factory_t::factory_order.load (std::memory_order_acquire) == 2
      && aggregate_materialized_actor_adapter_t::restore_order.load (std::memory_order_acquire) == 3
      && aggregate_materialized_spot_t::membership_callbacks.load (std::memory_order_acquire) == 0
      && aggregate_materialized_spot_t::ready_order.load (std::memory_order_acquire) == 4;
    if (result.terminal != relocation_terminal_t::completed || !result.target_handoff
        || !target_committed || aggregates->prepare_count != 1 || aggregates->commit_count != 1
        || restored_spot != std::optional<object_ref_t>{expected_spot}
        || restored_actor != std::optional<object_ref_t>{expected_actor}
        || target.native_node ().objects ().actor_membership (expected_actor)
             != std::optional<std::string>{expected_spot.key}
        || target.native_node ().objects ().timers (expected_spot).size () != 1
        || target.native_node ().objects ().timers (expected_actor).size () != 1
        || !lifecycle_ordered || !aggregate_materialized_spot_t::restore_held_ingress.load ()
        || !aggregate_materialized_spot_t::ready_saw_membership_state.load ()
        || !aggregate_materialized_spot_t::ready_before_dispatch.load ())
        std::cerr
          << "V11-M6C-CPP aggregate diagnostic: terminal=" << static_cast<int> (result.terminal)
          << " reason=" << static_cast<int> (result.reason)
          << " authority=" << result.authority.size () << " prepare=" << aggregates->prepare_count
          << " commit=" << aggregates->commit_count << " spot=" << restored_spot.has_value ()
          << " actor=" << restored_actor.has_value () << " membership="
          << target.native_node ().objects ().actor_membership (expected_actor).value_or ("<none>")
          << " spot-timers=" << target.native_node ().objects ().timers (expected_spot).size ()
          << " actor-timers=" << target.native_node ().objects ().timers (expected_actor).size ()
          << " lifecycle-order=" << lifecycle_ordered
          << " spot-capture=" << aggregate_materialized_spot_adapter_t::capture_count.load ()
          << " actor-capture=" << aggregate_materialized_actor_adapter_t::capture_count.load ()
          << " membership-callbacks=" << aggregate_materialized_spot_t::membership_callbacks.load ()
          << " ready-membership-state="
          << aggregate_materialized_spot_t::ready_saw_membership_state.load ()
          << " restore-before-cas=" << aggregate_materialized_spot_t::restore_before_cas.load ()
          << " restore-ingress=" << aggregate_materialized_spot_t::restore_held_ingress.load ()
          << " ready-before-dispatch="
          << aggregate_materialized_spot_t::ready_before_dispatch.load ()
          << " saved-claim=" << static_cast<int> (saved_claim_error) << '\n';
    test.require (result.terminal == relocation_terminal_t::completed && result.target_handoff
                    && target_committed && aggregates->prepare_count == 1
                    && aggregates->commit_count == 1
                    && restored_spot == std::optional<object_ref_t>{expected_spot}
                    && restored_actor == std::optional<object_ref_t>{expected_actor}
                    && target.native_node ().objects ().actor_membership (expected_actor)
                         == std::optional<std::string>{expected_spot.key}
                    && target.native_node ().objects ().timers (expected_spot).size () == 1
                    && target.native_node ().objects ().timers (expected_actor).size () == 1,
                  "production aggregate relocation must commit and restore the User Spot, member "
                  "Actor, membership, and timers");
    test.require (
      aggregate_materialized_spot_adapter_t::capture_count.load (std::memory_order_acquire) == 1
        && aggregate_materialized_actor_adapter_t::capture_count.load (std::memory_order_acquire)
             == 1
        && aggregate_materialized_spot_adapter_t::restored_value.load (std::memory_order_acquire)
             == 19
        && aggregate_materialized_actor_adapter_t::restored_value.load (std::memory_order_acquire)
             == 37
        && aggregate_materialized_spot_t::ready_saw_membership_state.load (
          std::memory_order_acquire)
        && aggregate_materialized_spot_t::restore_before_cas.load (std::memory_order_acquire)
        && aggregate_materialized_spot_t::restore_held_ingress.load (std::memory_order_acquire)
        && aggregate_materialized_spot_t::ready_after_cas.load (std::memory_order_acquire)
        && aggregate_materialized_spot_t::ready_before_dispatch.load (std::memory_order_acquire)
        && lifecycle_ordered && saved_claim_error == stateful_error_t::none && saved_claim
        && saved_claim->sequence == 201 && saved_completed,
      "host relocation must restore Spot then Actor without join or leave callbacks, publish "
      "membership and target-only CAS before ready completion, and open saved ingress last");
    aggregate_materialized_spot_t::membership_visible = {};
    aggregate_materialized_spot_t::authority_commit_count = {};
    aggregate_materialized_spot_t::target_objects = nullptr;
    aggregate_materialized_spot_t::target_actor = nullptr;
    source.stop ();
    target.stop ();
}

} // namespace m6c_test
