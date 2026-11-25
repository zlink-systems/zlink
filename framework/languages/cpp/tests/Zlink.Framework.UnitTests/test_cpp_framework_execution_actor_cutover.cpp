/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/execution_cases.hpp"
#include "../support/execution_actor_join_fixture.hpp"

#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/timers/async_delay.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace zlink::framework::test::execution
{

class actor_cutover_probe_t final : public zlink::framework::actor_t
{
  public:
    actor_cutover_probe_t (zlink::framework::actor_context_t context, bool target) :
        _context (std::move (context)), _target (target)
    {
    }

    bool target () const noexcept { return _target; }

    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

    zlink::framework::task_t<void>
    on_join_completed (const zlink::framework::actor_join_completion_t &completion) override
    {
        const auto *accepted = std::get_if<zlink::framework::actor_join_accepted_t> (&completion);
        if (!accepted) {
            failed_completions.fetch_add (1, std::memory_order_release);
            co_return;
        }
        if (_target) {
            target_operation_high.store (accepted->operation_id_high, std::memory_order_release);
            target_operation_low.store (accepted->operation_id_low, std::memory_order_release);
            target_completions.fetch_add (1, std::memory_order_release);
            target_completion_entered.store (true, std::memory_order_release);
            co_await target_completion_gate->task ();
            target_completion_finished.store (true, std::memory_order_release);
        } else {
            source_operation_high.store (accepted->operation_id_high, std::memory_order_release);
            source_operation_low.store (accepted->operation_id_low, std::memory_order_release);
            source_completions.fetch_add (1, std::memory_order_release);
        }
    }

    static void reset ()
    {
        source_completions.store (0, std::memory_order_release);
        target_completions.store (0, std::memory_order_release);
        failed_completions.store (0, std::memory_order_release);
        source_operation_high.store (0, std::memory_order_release);
        source_operation_low.store (0, std::memory_order_release);
        target_operation_high.store (0, std::memory_order_release);
        target_operation_low.store (0, std::memory_order_release);
        target_completion_entered.store (false, std::memory_order_release);
        target_completion_finished.store (false, std::memory_order_release);
        target_joined.store (false, std::memory_order_release);
        submit_bound_push_on_join.store (false, std::memory_order_release);
        joined_bound_push_submitted.store (false, std::memory_order_release);
        source_leave_before_target_joined.store (false, std::memory_order_release);
        source_leave_calls.store (0, std::memory_order_release);
        target_completion_gate =
          std::make_shared<zlink::framework::task_completion_source_t<void>> ();
        source_leave_gate = std::make_shared<zlink::framework::task_completion_source_t<void>> ();
        source_leave_started =
          std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    }

    static inline std::atomic_int source_completions{0};
    static inline std::atomic_int target_completions{0};
    static inline std::atomic_int failed_completions{0};
    static inline std::atomic_uint64_t source_operation_high{0};
    static inline std::atomic_uint64_t source_operation_low{0};
    static inline std::atomic_uint64_t target_operation_high{0};
    static inline std::atomic_uint64_t target_operation_low{0};
    static inline std::atomic_bool target_completion_entered{false};
    static inline std::atomic_bool target_completion_finished{false};
    static inline std::atomic_bool target_joined{false};
    static inline std::atomic_bool submit_bound_push_on_join{false};
    static inline std::atomic_bool joined_bound_push_submitted{false};
    static inline std::atomic_bool source_leave_before_target_joined{false};
    static inline std::atomic_int source_leave_calls{0};
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>>
      target_completion_gate =
        std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>>
      source_leave_gate = std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>>
      source_leave_started = std::make_shared<zlink::framework::task_completion_source_t<void>> ();

  private:
    zlink::framework::actor_context_t _context;
    bool _target;
};

class actor_cutover_probe_factory_t final
    : public zlink::framework::actor_factory_t<actor_cutover_probe_t>
{
  public:
    explicit actor_cutover_probe_factory_t (bool target) : _target (target) {}

    zlink::framework::task_t<std::shared_ptr<actor_cutover_probe_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<actor_cutover_probe_t> (std::move (context), _target);
    }

  private:
    bool _target;
};

class actor_cutover_probe_spot_t final : public zlink::framework::spot_t<actor_cutover_probe_t>
{
  public:
    explicit actor_cutover_probe_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&actor_cutover_probe_spot_t::on_probe> (
          "actor.cutover.probe.noop");
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
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void> on_actor_joined (actor_cutover_probe_t &actor) override
    {
        actor_cutover_probe_t::target_joined.store (true, std::memory_order_release);
        if (actor.target ()
            && actor_cutover_probe_t::submit_bound_push_on_join.load (std::memory_order_acquire)) {
            const auto submitted = actor.context ()
                                     .bound_session ()
                                     .send (std::string ("target-joined"))
                                     .async ()
                                     .result ();
            actor_cutover_probe_t::joined_bound_push_submitted.store (static_cast<bool> (submitted),
                                                                      std::memory_order_release);
        }
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (actor_cutover_probe_t &actor) override
    {
        if (!actor.target ()) {
            actor_cutover_probe_t::source_leave_calls.fetch_add (1, std::memory_order_release);
            actor_cutover_probe_t::source_leave_before_target_joined.store (
              !actor_cutover_probe_t::target_joined.load (std::memory_order_acquire),
              std::memory_order_release);
            actor_cutover_probe_t::source_leave_started->complete (
              zlink::framework::result_t<void>::success ());
            co_await actor_cutover_probe_t::source_leave_gate->task ();
        }
        co_return;
    }

    zlink::framework::task_t<void>
    on_probe (actor_cutover_probe_t &,
              zlink::framework::message_context_t &,
              const zlink::framework::detail::spot_actor_handoff_packet_t &)
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

bool verify_remote_actor_cutover_completion_is_target_owned (bool cancel_source_delay,
                                                             bool unknown_at_teardown,
                                                             bool stopped_management)
{
    using namespace std::chrono_literals;
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    struct source_settlement_probe_t final : stateful::authority_relocation_port_t
    {
        stateful::authority_relocation_port_t &delegate;
        std::atomic_bool leave_accepted{false};
        std::atomic_int observations{0};
        std::atomic_bool cancellation_requested{false};
        bool keep_unknown = false;
        std::function<void ()> cancel_management;
        explicit source_settlement_probe_t (stateful::authority_relocation_port_t &port) :
            delegate (port)
        {
        }
        task_t<std::optional<std::string>>
        begin_source_capture (const std::vector<stateful::object_ref_t> &participants,
                              const stateful::relocation_authority_fence_t &fence,
                              const std::string &target_node_id,
                              std::uint64_t target_node_generation,
                              std::uint64_t application_version) override
        {
            return delegate.begin_source_capture (participants, fence, target_node_id,
                                                  target_node_generation, application_version);
        }
        task_t<bool> capture_source (const stateful::object_ref_t &primary,
                                     const std::string &preparing_version) override
        {
            return delegate.capture_source (primary, preparing_version);
        }
        stateful::source_relocation_settlement_t
        settle_source_relocation (const stateful::relocation_authority_fence_t &fence,
                                  const std::vector<stateful::object_ref_t> &participants,
                                  bool preserve) override
        {
            observations.fetch_add (1, std::memory_order_relaxed);
            // Stop the delay synchronously in the source decision that observes
            // the real early Leave. That decision stays UNKNOWN, so the next
            // delay must observe cancellation before any confirmed read.
            if (!leave_accepted.load (std::memory_order_acquire))
                return {};
            if (!cancellation_requested.exchange (true, std::memory_order_acq_rel)) {
                cancel_management ();
                return {};
            }
            if (keep_unknown)
                return {};
            return delegate.settle_source_relocation (fence, participants, preserve);
        }
        stateful::authority_publish_result_t
        publish (const stateful::object_ref_t &source,
                 const stateful::object_ref_t &target,
                 location_owner_token_t owner,
                 object_creation_target_t placement,
                 std::string reference,
                 std::uint32_t checksum,
                 stateful::inventory_digest_t digest,
                 std::vector<std::byte> payload,
                 std::string version,
                 runtime::protocol::relocation_id_t relocation,
                 location_owner_token_t source_owner,
                 const runtime::protocol::relocation_prepare_t *prepare) override
        {
            return delegate.publish (source, target, std::move (owner), std::move (placement),
                                     std::move (reference), checksum, digest, std::move (payload),
                                     std::move (version), relocation, std::move (source_owner),
                                     prepare);
        }
        bool finish_relocation (const stateful::object_ref_t &target,
                                const runtime::protocol::relocation_prepare_t &prepare) override
        {
            return delegate.finish_relocation (target, prepare);
        }
        std::optional<stateful::authority_relocation_reference_t>
        read (stateful::object_kind_t kind, const std::string &key) override
        {
            return delegate.read (kind, key);
        }
        std::optional<bool> owner_lease_live (const location_owner_token_t &owner) override
        {
            return delegate.owner_lease_live (owner);
        }
    };

    const auto join_timeout = 5s;
    actor_cutover_probe_t::reset ();
    if (cancel_source_delay) {
        actor_cutover_probe_t::source_leave_gate->complete (result_t<void>::success ());
        actor_cutover_probe_t::target_completion_gate->complete (result_t<void>::success ());
    }
    serializer_registry_t serializers;
    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_state = [&] (const std::string &rid, bool target) {
        auto state = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::mesh_node_builder_state_t> ("actor-cutover-mesh");
        state->core_context = core_context;
        if (cancel_source_delay)
            state->spot_state->dispatch.message_flow (message_flow_log_mode_t::normal);
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_state->snapshot.routing_id = *state->routing_id;
        state->spot_state->channel_runtime =
          zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
        state->spot_state->channel_runtime->serializers = &serializers;
        state->spot_builder.add_spot_factory<actor_cutover_probe_spot_t> (
          "actor.cutover.spot",
          [] (spot_context_t context) {
              return std::make_shared<actor_cutover_probe_spot_t> (std::move (context));
          },
          [] (auto &factory) { factory.recreate_on_relocation (); });
        state->spot_builder
          .add_actor_factory<actor_cutover_probe_t, actor_cutover_probe_factory_t> (
            "actor.cutover.probe", std::make_shared<actor_cutover_probe_factory_t> (target),
            [] (auto &factory) { factory.recreate_on_relocation (); });
        return state;
    };

    auto locations = std::make_shared<runtime::in_memory_location_repository_t> ();
    locations->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_owner =
      std::get<owner_lease_claimed_t> (
        locations->claim_owner_lease ("source-owner", 30s).result ().value ())
        .token;
    const auto target_owner =
      std::get<owner_lease_claimed_t> (
        locations->claim_owner_lease ("target-owner", 30s).result ().value ())
        .token;
    auto relocation_store = std::make_shared<runtime::in_memory_relocation_store_t> ();
    auto relocation_repository =
      std::make_shared<runtime::provider_relocation_repository_t> (*relocation_store);
    auto authority = std::make_shared<stateful::public_authority_store_adapter_t> (*locations);
    auto relocations =
      std::make_shared<stateful::public_relocation_store_adapter_t> (relocation_repository);
    auto source_state = make_state ("actor-cutover-source", false);
    auto target_state = make_state ("actor-cutover-target", true);
    std::mutex delay_mutex;
    delay_owner_t delays (delay_mutex);
    mesh_node_runtime_t source (source_state);
    mesh_node_runtime_t target (target_state);
    source.bind_delay_owner (delays);
    target.bind_delay_owner (delays);
    struct spot_route_fixture_t
    {
        zlink::routing_id_t node = zlink::routing_id_t::from (std::uint32_t{0});
        std::uint64_t generation = 0;
        runtime::host::route_fence_t fence;
    };
    std::map<std::string, spot_route_fixture_t> spot_routes;
    const auto resolve_spot_route = [&spot_routes] (zlink::routing_id_t node, std::string spot_id,
                                                    std::uint64_t generation)
      -> zlink::framework::task_t<std::optional<runtime::host::route_fence_t>> {
        const auto found = spot_routes.find (spot_id);
        if (found == spot_routes.end () || found->second.node != node
            || found->second.generation != generation) {
            co_return std::nullopt;
        }
        co_return found->second.fence;
    };
    source.bind_serializers (serializers);
    target.bind_serializers (serializers);
    source.configure_spot_route_fence_resolver (resolve_spot_route, 0ms);
    target.configure_spot_route_fence_resolver (resolve_spot_route, 0ms);
    source.configure_user_spot_operations (
      locations,
      [] (const stateful::object_ref_t &, const std::string &, const std::vector<std::byte> &)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          co_return runtime::host::user_spot_materialize_result_t{true, std::nullopt};
      });
    auto source_settlement = std::make_shared<source_settlement_probe_t> (*authority);
    source_settlement->keep_unknown = unknown_at_teardown;
    task_completion_source_t<void> management_stopped;
    source_settlement->cancel_management = [&] {
        source.request_stop ();
        delays.request_stop (unknown_at_teardown
                               ? std::chrono::steady_clock::now ()
                               : std::chrono::steady_clock::now () + join_timeout);
        if (stopped_management)
            management_stopped.task ().result ().value ();
    };
    source.configure_relocation_runtime (
      cancel_source_delay
        ? std::static_pointer_cast<stateful::authority_relocation_port_t> (source_settlement)
        : authority,
      relocations);
    target.configure_relocation_runtime (authority, relocations);
    source.configure_session_route_owner (
      [source_owner] { return std::optional<location_owner_token_t>{source_owner}; });
    target.configure_session_route_owner (
      [target_owner] { return std::optional<location_owner_token_t>{target_owner}; });
    source.start ().result ().value ();
    target.start ().result ().value ();
    const auto configure_materialization =
      [] (mesh_node_runtime_t &node, const std::shared_ptr<mesh_node_builder_state_t> &state) {
          auto &objects = node.native_node ().objects ();
          objects.configure_relocation_state (
            [spot_state = state->spot_state] (const stateful::object_ref_t &object,
                                              const std::string &stable_type,
                                              std::stop_token cancellation) {
                return spot_node_runtime_t (spot_state)
                  .capture_relocation_state (object, stable_type, cancellation);
            },
            [spot_state = state->spot_state] (const stateful::frozen_object_state_t &frozen,
                                              const stateful::object_ref_t &object,
                                              std::stop_token cancellation) {
                return spot_node_runtime_t (spot_state)
                  .restore_spot_relocation_state (frozen, object, cancellation);
            });
          objects.configure_relocation_materialization (
            [spot_state = state->spot_state] (
              const stateful::frozen_object_state_t &frozen, const stateful::object_ref_t &object,
              const std::optional<stateful::object_ref_t> &spot, std::stop_token cancellation) {
                return spot_node_runtime_t (spot_state)
                  .materialize_relocation_state (frozen, object, spot, cancellation);
            },
            [spot_state = state->spot_state] (const std::vector<stateful::object_ref_t> &objects,
                                              const auto &before_join_completion) {
                return spot_node_runtime_t (spot_state)
                  .commit_relocation_materialization (objects, before_join_completion);
            },
            [spot_state = state->spot_state] (const std::vector<stateful::object_ref_t> &objects) {
                return spot_node_runtime_t (spot_state)
                  .abort_relocation_materialization_async (objects);
            });
      };
    configure_materialization (source, source_state);
    configure_materialization (target, target_state);

    const auto register_node = [&] (mesh_node_runtime_t &node,
                                    const location_owner_token_t &owner) {
        const auto status = node.status ();
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "actor-cutover-mesh";
        descriptor.rid = status.routing_id ();
        descriptor.lifecycle_generation = status.lifecycle_generation ();
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = status.local_endpoint ();
        descriptor.application_version = 1;
        descriptor.object_capabilities = {
          {.object_kind = placement_object_kind_t::actor, .stable_type = "actor.cutover.probe"},
          {.object_kind = placement_object_kind_t::user_spot, .stable_type = "actor.cutover.spot"}};
        descriptor.object_role = object_role_t::server;
        descriptor.capacity = {.actors = {.limit = 8}, .spots = {.limit = 8}};
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "actor-cutover-test";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        const auto updated =
          locations->update_mesh_node (std::move (descriptor), location_write_intent_t::new_claim)
            .result ()
            .value ();
        if (updated.status != location_write_status_t::stored)
            std::cerr << "cutover node registration status=" << static_cast<int> (updated.status)
                      << '\n';
        return updated.status == location_write_status_t::stored;
    };
    if (!register_node (source, source_owner) || !register_node (target, target_owner)) {
        std::cerr << "cutover production setup: node registration failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    spot_node_runtime_t source_spots (source_state->spot_state);
    spot_node_runtime_t target_spots (target_state->spot_state);
    // The two-node command-28 receiver resolves the source Actor type and
    // exact fence from the same authority repository the fixture commits
    // below.  Bind that production dependency before peer traffic starts.
    service_collection_t location_services;
    location_services.add_factory<runtime::live_location_reader_t> (
      [locations] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*locations);
      },
      service_lifetime_t::singleton);
    location_services.add_singleton<actor_gateway_runtime_t> ();
    auto location_provider = location_services.build_provider ();
    source_spots.bind_service_provider (location_provider);
    target_spots.bind_service_provider (location_provider);
    source_spots.bind_relocation_store (relocations);
    source_spots.bind_relocation_authority (authority);
    target_spots.bind_relocation_store (relocations);
    target_spots.bind_relocation_authority (authority);
    const auto source_spot = source_spots.create_spot ("actor.cutover.spot");
    const auto target_spot_id = new_user_spot_id ();
    auto source_native_spot =
      source.get_or_create_spot_async (std::string (source_spot.spot_id)).result ().value ();
    auto source_native_entry_spot = source.native_node ().entry_spot ();

    const object_reserve_request_t source_spot_reserve{
      .key = {placement_object_kind_t::user_spot, source_native_spot.spot_id ()},
      .intent = {.stable_type = "actor.cutover.spot"},
      .target = {.mesh_name = "actor-cutover-mesh",
                 .node_rid = node_rid_t::from_string (source.status ().routing_id ().to_string ()),
                 .node_lifecycle_generation = source.status ().lifecycle_generation (),
                 .owner = source_owner},
      .capacity_bundle = {.spot_slots = 1,
                          .spot_type = spot_type_capacity_delta_t{
                            placement_object_kind_t::user_spot, "actor.cutover.spot", 1}}};
    const auto ready_spot_payload = [] (const object_reserve_request_t &request) {
        return runtime::encode_user_spot_authority_payload (runtime::user_spot_authority_payload_t{
          .stable_type = request.intent.stable_type,
          .spot_id = request.key.global_id,
          .owner_id = request.target.owner.owner_id,
          .owner_lease_generation =
            static_cast<std::uint64_t> (request.target.owner.lease_generation),
          .mesh_name = request.target.mesh_name,
          .node_rid = request.target.node_rid,
          .node_generation = request.target.node_lifecycle_generation});
    };
    const auto source_spot_reserved_value =
      locations->reserve (source_spot_reserve).result ().value ();
    const auto *source_spot_reserved = std::get_if<object_reserved_t> (&source_spot_reserved_value);
    if (!source_spot_reserved
        || !std::holds_alternative<object_committed_t> (
          locations
            ->commit ({source_spot_reserve.key, source_spot_reserved->fence,
                       ready_spot_payload (source_spot_reserve)})
            .result ()
            .value ())) {
        std::cerr << "cutover production setup: source Spot authority failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    spot_routes.insert_or_assign (
      source_native_spot.spot_id (),
      spot_route_fixture_t{source.status ().routing_id (),
                           source_spot_reserved->fence.object_generation,
                           {source_spot_reserved->fence.authority_owner_generation,
                            static_cast<std::uint64_t> (source_owner.lease_generation),
                            source.status ().lifecycle_generation ()}});

    const object_reserve_request_t target_spot_reserve{
      .key = {placement_object_kind_t::user_spot, std::string (target_spot_id)},
      .intent = {.stable_type = "actor.cutover.spot"},
      .target = {.mesh_name = "actor-cutover-mesh",
                 .node_rid = node_rid_t::from_string (target.status ().routing_id ().to_string ()),
                 .node_lifecycle_generation = target.status ().lifecycle_generation (),
                 .owner = target_owner},
      .capacity_bundle = {.spot_slots = 1,
                          .spot_type = spot_type_capacity_delta_t{
                            placement_object_kind_t::user_spot, "actor.cutover.spot", 1}}};
    const auto target_spot_reserved_value =
      locations->reserve (target_spot_reserve).result ().value ();
    const auto *target_spot_reserved = std::get_if<object_reserved_t> (&target_spot_reserved_value);
    if (!target_spot_reserved
        || !std::holds_alternative<object_committed_t> (
          locations
            ->commit ({target_spot_reserve.key, target_spot_reserved->fence,
                       ready_spot_payload (target_spot_reserve)})
            .result ()
            .value ())) {
        std::cerr << "cutover production setup: target Spot authority failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto local_target_spot_authority_generation =
      target_spot_reserved->fence.authority_owner_generation == 1 ? 2 : 1;
    const auto target_spot = target_spots.get_or_create_spot (
      "actor.cutover.spot", target_spot_id, zlink::message_t{},
      target_spot_reserved->fence.object_generation, {}, local_target_spot_authority_generation);
    auto target_native_spot =
      target.get_or_create_spot_async (std::string (target_spot.spot_id)).result ().value ();
    const auto target_context_authority_differs_from_committed_store =
      local_target_spot_authority_generation
      != target_spot_reserved->fence.authority_owner_generation;
    spot_routes.insert_or_assign (
      target_native_spot.spot_id (),
      spot_route_fixture_t{target.status ().routing_id (),
                           target_spot_reserved->fence.object_generation,
                           {target_spot_reserved->fence.authority_owner_generation,
                            static_cast<std::uint64_t> (target_owner.lease_generation),
                            target.status ().lifecycle_generation ()}});

    source.connect_peer (target.status ().routing_id (), target.status ().local_endpoint ());
    const auto discard = [] (const auto &, const auto &, auto, auto) {};
    const auto admitted_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (target.status ().routing_id (),
                                       target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < admitted_deadline) {
        (void) source.dispatch_ready (discard);
        (void) target.dispatch_ready (discard);
        if (!cancel_source_delay)
            std::this_thread::sleep_for (1ms);
    }
    if (!source.has_admitted_peer (target.status ().routing_id (),
                                   target.status ().lifecycle_generation ())
        || !target.has_admitted_peer (source.status ().routing_id (),
                                      source.status ().lifecycle_generation ())) {
        std::cerr << "cutover production setup: peer admission failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    const object_reserve_request_t actor_reserve{
      .key = {placement_object_kind_t::actor, "actor-cutover-probe"},
      .intent = {.stable_type = "actor.cutover.probe"},
      .target = {.mesh_name = "actor-cutover-mesh",
                 .node_rid = node_rid_t::from_string (source.status ().routing_id ().to_string ()),
                 .node_lifecycle_generation = source.status ().lifecycle_generation (),
                 .owner = source_owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto actor_reserved_value = locations->reserve (actor_reserve).result ().value ();
    const auto *actor_reserved = std::get_if<object_reserved_t> (&actor_reserved_value);
    if (!actor_reserved) {
        std::cerr << "cutover production setup: authority reserve failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto created_actor =
      source
        .create_application_actor_async ("actor.cutover.probe", "actor-cutover-probe",
                                         actor_reserved->fence.object_generation,
                                         actor_reserved->fence.authority_owner_generation)
        .result ();
    if (source_spot.state != spot_create_state_t::created
        || target_spot.state != spot_create_state_t::created || !created_actor) {
        std::cerr << "cutover production setup: application materialization failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto actor = created_actor.value ();
    auto bound_delivery_gate = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool bound_delivery_started{false};
    std::atomic_int bound_delivery_calls{0};
    auto &binding_gateway = location_provider.get_required<actor_gateway_runtime_t> ();
    binding_gateway.bind_serializers (serializers);
    const auto bound = binding_gateway.bind_session_sink (
      actor,
      [bound_delivery_gate, &bound_delivery_started, &bound_delivery_calls] (
        std::string, stream_codec_t, const zlink::message_t &) -> task_t<void> {
          bound_delivery_calls.fetch_add (1, std::memory_order_release);
          bound_delivery_started.store (true, std::memory_order_release);
          co_await bound_delivery_gate->task ();
      });
    if (!bound) {
        std::cerr << "cutover production setup: bound Session sink failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    actor_cutover_probe_t::submit_bound_push_on_join.store (!cancel_source_delay,
                                                            std::memory_order_release);
    actor_gateway_runtime_t source_actor_gateway;
    source_state->spot_state->lane
      .run_checked ([&] {
          source_state->spot_state->actor_instances.insert_or_assign (
            "actor.cutover.probe:actor-cutover-probe",
            std::make_shared<actor_cutover_probe_t> (source_actor_gateway.actor_context (actor),
                                                     false));
      })
      .get ();
    auto source_actor_object =
      source.native_node ().objects ().find (stateful::object_kind_t::actor, "actor-cutover-probe");
    auto source_spot_object = source.native_node ().objects ().find (
      stateful::object_kind_t::user_spot, source_native_spot.spot_id ());
    if (!source_actor_object || !source_spot_object) {
        std::cerr << "cutover production setup: Core object lookup failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto [join_error, join] =
      source.native_node ()
        .objects ()
        .begin_membership_move_async (*source_actor_object, *source_spot_object)
        .result ()
        .value ();
    const auto [commit_error, joined_actor] =
      source.native_node ().objects ().commit_membership_move_async (join).result ().value ();
    if (join_error != stateful::stateful_error_t::none
        || commit_error != stateful::stateful_error_t::none) {
        std::cerr << "cutover production setup: Core membership failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    source_spots.record_actor_spot (actor, spot_id_t (source_native_spot.spot_id ()));

    // Actor creation can leave CurrentSpot at the Entry Spot after live
    // membership has moved to a User Spot. Canonical cutover must keep the
    // locally captured source membership for target-to-source OnLeave.
    const auto actor_committed_value =
      locations
        ->commit (
          {actor_reserve.key, actor_reserved->fence,
           runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
             .stable_type = "actor.cutover.probe",
             .actor_id = "actor-cutover-probe",
             .current_spot_id = source_native_entry_spot.spot_id (),
             .current_spot_generation = source.status ().lifecycle_generation (),
             .current_spot_kind = runtime::actor_authority_spot_kind_t::entry,
             .owner_id = source_owner.owner_id,
             .owner_lease_generation = static_cast<std::uint64_t> (source_owner.lease_generation),
             .mesh_name = "actor-cutover-mesh",
             .node_rid = actor.node_rid (),
             .node_generation = source.status ().lifecycle_generation ()})})
        .result ()
        .value ();
    if (!std::holds_alternative<object_committed_t> (actor_committed_value)) {
        std::cerr << "cutover production setup: authority commit failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    std::atomic_bool stop_dispatch{false};
    source_spots.set_route_client (route_client_t{});
    target_spots.set_route_client (route_client_t{});
    // OnLeave travels node-level (the source Entry Spot is fixed to node
    // lifecycle and is never published into mesh spot routing), mirroring
    // how production (app.cpp) wires actor_leave_notification_sender to
    // application_mesh->send_to_node.
    target_spots.on_actor_leave_notification (
      [&target] (const zlink::routing_id_t &node_rid,
                 std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
          co_return co_await target.send_to_node (node_rid, parts);
      });
    auto source_provider = location_provider;
    auto target_provider = location_provider;
    auto source_shared = std::shared_ptr<mesh_node_runtime_t> (&source, [] (auto *) {});
    task_completion_source_t<bool> early_leave;
    auto early_leave_ready = early_leave.task ();
    task_completion_source_t<void> active_drained;
    const auto dispatch_source = [&] {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            (void) source
              .dispatch_ready ([&] (const host::ready_record_t &owner,
                                    const host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts, auto permit) {
                  if (!cancel_source_delay) {
                      (void) source_spots.dispatch_mesh_record (
                        owner, record, parts,
                        runtime::decode_mesh_record_header (record, parts, false).value (),
                        source_provider, serializers, {}, nullptr,
                        runtime::handler_entry_t (record.before_application_handler,
                                                  std::move (permit)));
                      return;
                  }
                  // Match the hosted dispatcher admission/terminal ownership. The
                  // early Leave transfers this terminal into its actual reservation.
                  source.application_work_enqueued ();
                  source.application_work_started ();
                  if (record.retain_mailbox_reservation)
                      record.retain_mailbox_reservation ();
                  auto terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
                    source_shared, record.complete_stateful_dispatch,
                    record.release_mailbox_reservation);
                  bool deferred = false;
                  (void) source_spots.dispatch_mesh_record (
                    owner, record, parts,
                    runtime::decode_mesh_record_header (record, parts, false).value (),
                    source_provider, serializers,
                    [&, terminal] {
                        terminal->settle ();
                        if (source_settlement->leave_accepted.load (std::memory_order_acquire)
                            && source.active_application_callbacks () == 0)
                            active_drained.complete (result_t<void>::success ());
                    },
                    &deferred,
                    runtime::handler_entry_t (record.before_application_handler,
                                              std::move (permit)));
                  if (!deferred)
                      terminal->settle ();
                  const auto retained =
                    source_state->spot_state->lane
                      .run ([&] {
                          const auto &pending =
                            source_state->spot_state->pending_remote_actor_leaves;
                          return !pending.empty () && pending.front ().transfer_owner_reservation
                                 && source_state->spot_state->actor_transfer_coordinator->phase (
                                      "actor.cutover.probe:actor-cutover-probe")
                                      == actor_move_phase_t::source_remote;
                      })
                      .get ();
                  if (retained
                      && !source_settlement->leave_accepted.exchange (true,
                                                                      std::memory_order_acq_rel)) {
                      early_leave.complete (
                        result_t<bool>::success (source.active_application_callbacks () == 1));
                  }
              })
              .result ()
              .value ();
            if (cancel_source_delay) {
                // The production receive loop advances this same management
                // owner after a dispatch turn; it returns abandoned early Leave
                // reservations only after their canonical source move closes.
                source_spots.advance_management_async (false).result ().value ();
                if (!stop_dispatch.load (std::memory_order_acquire))
                    (void) source.native_node ().wait_for_dispatch_activity (
                      std::chrono::milliseconds (-1), true, std::nullopt);
            } else
                std::this_thread::sleep_for (1ms);
        }
    };
    const auto dispatch_target = [&] {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            (void) target
              .dispatch_ready ([&] (const host::ready_record_t &owner,
                                    const host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts, auto permit) {
                  (void) target_spots.dispatch_mesh_record (
                    owner, record, parts,
                    zlink::framework::runtime::decode_mesh_record_header (record, parts, false)
                      .value (),
                    target_provider, serializers, {}, nullptr,
                    runtime::handler_entry_t (record.before_application_handler,
                                              std::move (permit)));
              })
              .result ()
              .value ();
            if (cancel_source_delay) {
                if (!stop_dispatch.load (std::memory_order_acquire))
                    (void) target.native_node ().wait_for_dispatch_activity (
                      std::chrono::milliseconds (-1), true, std::nullopt);
            } else
                std::this_thread::sleep_for (1ms);
        }
    };
    std::thread source_dispatch (dispatch_source);
    std::thread target_dispatch (dispatch_target);
    const auto stop_management = [&] {
        stop_dispatch.store (true, std::memory_order_release);
        source.signal_dispatch_activity ();
        target.signal_dispatch_activity ();
        if (source_dispatch.joinable ())
            source_dispatch.join ();
        if (target_dispatch.joinable ())
            target_dispatch.join ();
    };
    const auto source_lifecycle_generation = source.status ().lifecycle_generation ();
    const runtime::spot_address_t target_address{
      .mesh_name = "actor-cutover-mesh",
      .node_rid = target.status ().routing_id (),
      .spot_id = target_native_spot.spot_id (),
      .spot_generation = target_spot_reserved->fence.object_generation,
      .object_generation = target_spot_reserved->fence.object_generation,
      .authority_owner_generation = target_spot_reserved->fence.authority_owner_generation,
      .owner = target_owner,
      .node_generation = target.status ().lifecycle_generation ()};
    source.observe_spot_authority (
      target_address.node_rid, target_address.spot_id, target_address.object_generation,
      target_address.node_generation, target_address.authority_owner_generation,
      static_cast<std::uint64_t> (target_address.owner.lease_generation));
    const auto target_resolver = std::make_shared<wire_join_spot_resolver_t> ();
    target_resolver->address = target_address;
    auto joined = std::async (std::launch::async, [&] {
        auto result = std::move (source.join_application_actor_to_spot (
                                   actor, spot_id_t (target_address.spot_id), target_resolver,
                                   zlink::message_t{}, join_timeout))
                        .result ();
        if (result && result.value ().result_code == actor_join_reply_t::relayed) {
            const auto cleanup = result.value ().source_cleanup ().result ();
            if (!cleanup)
                return result_t<actor_join_reply_t>::failure (
                  cleanup.error_kind (),
                  cleanup.error () ? cleanup.error ()->what () : "source cleanup failed");
            auto reply = result.value ();
            reply.source_cleanup = {};
            reply.result_code = actor_join_reply_t::accepted;
            result = result_t<actor_join_reply_t>::success (std::move (reply));
        }
        if (cancel_source_delay)
            early_leave.complete (result_t<bool>::success (false));
        return result;
    });
    if (cancel_source_delay) {
        const bool retained_active = early_leave_ready.result ().value ();
        if (stopped_management) {
            // The source decision is held while its real management producers
            // stop. Only the canonical source terminal can return this Leave.
            stop_management ();
            management_stopped.complete (result_t<void>::success ());
        }
        const auto joined_result = joined.get ();
        const bool expected_terminal =
          unknown_at_teardown
            ? !joined_result && joined_result.error_kind () == framework_error_kind_t::unavailable
            : static_cast<bool> (joined_result);
        // Membership §Actor Join completion: the one-way source Leave does not
        // delay target completion. Observe its own entry before counting calls.
        if (expected_terminal && !unknown_at_teardown)
            (void) actor_cutover_probe_t::source_leave_started->task ().result_for (join_timeout);
        if (expected_terminal && !stopped_management)
            active_drained.task ().result ().value ();
        const auto active = source.active_application_callbacks ();
        const auto pending =
          source_state->spot_state->lane
            .run ([&] { return source_state->spot_state->pending_remote_actor_leaves.size (); })
            .get ();
        const auto inventory =
          source.native_node ().objects ().inventory_async ().result ().value ();
        const bool dispatch_closed =
          std::none_of (inventory.begin (), inventory.end (), [] (const auto &item) {
              return item.owner.kind == stateful::object_kind_t::actor
                     && item.owner.key == "actor-cutover-probe"
                     && item.state == stateful::object_state_t::ready;
          });
        const auto leave_calls =
          actor_cutover_probe_t::source_leave_calls.load (std::memory_order_acquire);
        std::cerr << "Source mesh cancellation joined=" << static_cast<bool> (joined_result)
                  << " retained-active=" << retained_active << " active=" << active
                  << " pending-leaves=" << pending << " unknown=" << unknown_at_teardown
                  << " stopped-management=" << stopped_management
                  << " expected-terminal=" << expected_terminal
                  << " dispatch-closed=" << dispatch_closed << " source-leave-calls=" << leave_calls
                  << " authority-observations=" << source_settlement->observations.load ()
                  << " error="
                  << (joined_result.error () ? joined_result.error ()->what () : "none") << '\n';
        const bool passed = expected_terminal && retained_active && active == 0 && pending == 0
                            && dispatch_closed && leave_calls == (unknown_at_teardown ? 0 : 1);
        if (!passed) {
            // Failure was recorded in the snapshot above. Release only the failed
            // fixture's reservation so the red process can stop cooperatively.
            source_state->spot_state->actor_transfer_coordinator->cancel_move (
              "actor.cutover.probe:actor-cutover-probe");
            source_spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now ());
            std::cerr << "Source mesh cancellation red fixture teardown released\n";
        }
        bound_delivery_gate->complete (result_t<void>::success ());
        actor_cutover_probe_t::source_leave_gate->complete (result_t<void>::success ());
        actor_cutover_probe_t::target_completion_gate->complete (result_t<void>::success ());
        stop_management ();
        source.stop ();
        target.stop ();
        return passed;
    }
    const auto source_leave_observation =
      actor_cutover_probe_t::source_leave_started->task ().result_for (2s);
    const auto source_leave_started =
      source_leave_observation && static_cast<bool> (*source_leave_observation);
    const auto returned_while_source_leave_blocked =
      joined.wait_for (250ms) == std::future_status::ready;
    const auto target_completion_deadline = std::chrono::steady_clock::now () + 250ms;
    while (!actor_cutover_probe_t::target_completion_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < target_completion_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto target_completion_started_while_source_leave_blocked =
      actor_cutover_probe_t::target_completion_entered.load (std::memory_order_acquire);
    actor_cutover_probe_t::source_leave_gate->complete (result_t<void>::success ());
    const auto returned_before_target_completion =
      joined.wait_for (2s) == std::future_status::ready;
    const auto target_completion_after_leave_deadline = std::chrono::steady_clock::now () + 2s;
    while (!actor_cutover_probe_t::target_completion_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < target_completion_after_leave_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto target_completion_started =
      actor_cutover_probe_t::target_completion_entered.load (std::memory_order_acquire);
    result_t<actor_join_reply_t> joined_result =
      returned_before_target_completion
        ? joined.get ()
        : result_t<actor_join_reply_t>::failure (framework_error_kind_t::deadline_exceeded,
                                                 "cutover source awaited target completion");
    bool target_actor_authority_matches_committed_store = false;
    if (joined_result) {
        const auto target_actor = target.native_node ()
                                    .resolve_actor_async (joined_result.value ().actor)
                                    .result ()
                                    .value ();
        const auto committed_authority =
          authority->read (stateful::object_kind_t::actor,
                           std::string (joined_result.value ().actor.actor_id ().value ()));
        target_actor_authority_matches_committed_store =
          target_actor && committed_authority && committed_authority->target == *target_actor;
    }
    const auto source_completions_before_release =
      actor_cutover_probe_t::source_completions.load (std::memory_order_acquire);
    actor_cutover_probe_t::target_completion_gate->complete (result_t<void>::success ());
    const auto completion_finished_deadline = std::chrono::steady_clock::now () + 2s;
    while (!actor_cutover_probe_t::target_completion_finished.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < completion_finished_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto bound_delivery_deadline = std::chrono::steady_clock::now () + 2s;
    while (!bound_delivery_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < bound_delivery_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto unfenced_settlement_deadline = std::chrono::steady_clock::now () + 100ms;
    while (target_state->spot_state->actor_transfer_coordinator->phase (
             "actor.cutover.probe:actor-cutover-probe")
           && std::chrono::steady_clock::now () < unfenced_settlement_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto completion_waited_for_bound_delivery =
      target_state->spot_state->actor_transfer_coordinator
        ->phase ("actor.cutover.probe:actor-cutover-probe")
        .has_value ();
    bound_delivery_gate->complete (result_t<void>::success ());
    actor_cutover_probe_t::submit_bound_push_on_join.store (false, std::memory_order_release);
    const auto target_completed =
      actor_cutover_probe_t::target_completions.load (std::memory_order_acquire) == 1;
    const auto target_operation_high =
      actor_cutover_probe_t::target_operation_high.load (std::memory_order_acquire);
    const auto target_operation_low =
      actor_cutover_probe_t::target_operation_low.load (std::memory_order_acquire);

    /* The relocated-in Actor exists in the target public host but was never
     * inserted into mesh_node_runtime_t's create-time _actors map.  A later
     * same-node User-Spot Join must bind a current host handle and complete
     * the ordinary local membership lane instead of returning NotFound. */
    bool relocated_actor_local_rejoin_passed = false;
    if (joined_result) {
        const auto move_closed_deadline = std::chrono::steady_clock::now () + 2s;
        while (target_state->spot_state->actor_transfer_coordinator->phase (
                 "actor.cutover.probe:actor-cutover-probe")
               && std::chrono::steady_clock::now () < move_closed_deadline) {
            std::this_thread::sleep_for (1ms);
        }
        const auto local_spot = target_spots.create_spot ("actor.cutover.spot");
        auto local_native_spot =
          target.get_or_create_spot_async (std::string (local_spot.spot_id)).result ().value ();
        const auto local_spot_generation = local_native_spot.status ().lifecycle_generation ();
        const runtime::spot_address_t local_address{.mesh_name = "actor-cutover-mesh",
                                                    .node_rid = target.status ().routing_id (),
                                                    .spot_id = local_native_spot.spot_id (),
                                                    .spot_generation = local_spot_generation,
                                                    .object_generation = local_spot_generation,
                                                    .authority_owner_generation = 1,
                                                    .owner = target_owner,
                                                    .node_generation =
                                                      target.status ().lifecycle_generation ()};
        const auto local_resolver = std::make_shared<wire_join_spot_resolver_t> ();
        local_resolver->address = local_address;
        auto local_join = std::async (std::launch::async, [&] {
            return std::move (target.join_application_actor_to_spot (
                                joined_result.value ().actor, spot_id_t (local_address.spot_id),
                                local_resolver, zlink::message_t{}, 2s))
              .result ();
        });
        if (local_join.wait_for (2s) == std::future_status::ready) {
            const auto local_result = local_join.get ();
            const auto current_spot = local_result
                                        ? target_spots.actor_spot (local_result.value ().actor)
                                        : std::optional<spot_id_t>{};
            relocated_actor_local_rejoin_passed =
              local_result && local_result.value ().result_code == 0 && current_spot
              && *current_spot == local_native_spot.spot_id ();
        }
    }
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();
    source.stop ();
    target.stop ();
    if (!returned_before_target_completion)
        (void) joined.get ();

    const auto passed =
      target_context_authority_differs_from_committed_store && joined_result
      && joined_result.value ().result_code == 0 && target_actor_authority_matches_committed_store
      && relocated_actor_local_rejoin_passed && source_leave_started
      && returned_while_source_leave_blocked && target_completion_started_while_source_leave_blocked
      && !actor_cutover_probe_t::source_leave_before_target_joined.load (std::memory_order_acquire)
      && actor_cutover_probe_t::source_leave_calls.load (std::memory_order_acquire) == 1
      && target_completion_started && target_completed
      && actor_cutover_probe_t::target_completion_finished.load (std::memory_order_acquire)
      && actor_cutover_probe_t::joined_bound_push_submitted.load (std::memory_order_acquire)
      && bound_delivery_started.load (std::memory_order_acquire)
      && bound_delivery_calls.load (std::memory_order_acquire) == 1
      && completion_waited_for_bound_delivery && source_completions_before_release == 0
      && actor_cutover_probe_t::source_completions.load (std::memory_order_acquire) == 0
      && actor_cutover_probe_t::target_completions.load (std::memory_order_acquire) == 2
      && actor_cutover_probe_t::failed_completions.load (std::memory_order_acquire) == 0
      && target_operation_high != 0 && target_operation_low != 0
      && target_operation_high == source_lifecycle_generation
      && actor_cutover_probe_t::source_operation_high.load (std::memory_order_acquire) == 0
      && actor_cutover_probe_t::source_operation_low.load (std::memory_order_acquire) == 0;
    if (!passed) {
        std::cerr << "cutover production diagnostic returned=" << returned_before_target_completion
                  << " returned-with-leave-blocked=" << returned_while_source_leave_blocked
                  << " leave-started=" << source_leave_started << " leave-before-target-joined="
                  << actor_cutover_probe_t::source_leave_before_target_joined.load ()
                  << " target-started-with-leave-blocked="
                  << target_completion_started_while_source_leave_blocked
                  << " joined=" << static_cast<bool> (joined_result)
                  << " result=" << (joined_result ? joined_result.value ().result_code : -1)
                  << " error="
                  << (joined_result.error () ? joined_result.error ()->what () : "<none>")
                  << " target-started=" << target_completion_started
                  << " target-completed=" << target_completed
                  << " local-rejoin=" << relocated_actor_local_rejoin_passed
                  << " target-host-authority=" << target_actor_authority_matches_committed_store
                  << " source=" << actor_cutover_probe_t::source_completions.load ()
                  << " target=" << actor_cutover_probe_t::target_completions.load ()
                  << " failed=" << actor_cutover_probe_t::failed_completions.load ()
                  << " op=" << target_operation_high << ':' << target_operation_low
                  << " source-op=" << actor_cutover_probe_t::source_operation_high.load () << ':'
                  << actor_cutover_probe_t::source_operation_low.load ()
                  << " source-spot=" << source_native_spot.spot_id () << ':'
                  << source_native_spot.status ().lifecycle_generation ()
                  << " target-spot=" << target_native_spot.spot_id () << ':'
                  << target_native_spot.status ().lifecycle_generation () << '\n';
    }
    return passed;
}

bool verify_remote_actor_completion_keeps_session_ref_until_route_ack ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("remote-source-publication-node");
    spot_node_runtime_t spots (node);
    actor_gateway_runtime_t gateway;
    auto session = gateway.manager ();
    session_actor_manager_access_t::attach (session, stream_t{});
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player",
                                                  "remote-source-actor", 7);
    const auto target = actor_ref_access_t::make (node_rid_t::from_string ("target-node"), "player",
                                                  "remote-source-actor", 7);
    if (!session.bind (source).async ().result ())
        return false;

    int publications = 0;
    spots.on_actor_ref_updated ([&] (const actor_ref_t &actor) {
        ++publications;
        return gateway.update_actor_ref (actor);
    });
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "remote-source-actor", 7, zlink::routing_id_t::from ("source-node").to_bytes (), 11, 13, 17};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "remote-source-actor", 7, zlink::routing_id_t::from ("target-node").to_bytes (), 12, 14, 18};
    try {
        std::move (spots.complete_remote_actor_transfer (
                     source, target,
                     spot_route_t{node_rid_t::from_string ("target-node"),
                                  spot_id_t ("target-spot"), "game"},
                     source_fence,
                     {target_fence.target_node_routing_id,
                      target_fence.target_node_generation,
                      {},
                      target_fence.owner_lease_generation},
                     target_fence.authority_owner_generation, "remote-source-transfer"))
          .result ()
          .value ();
    }
    catch (...) {
        return false;
    }
    const auto current = session.find ("remote-source-actor");
    if (publications != 0 || !current || current->ref ().node_rid ().value () != "source-node") {
        return false;
    }
    return node->lane
      .run_checked ([&] {
          const auto route = node->actor_routes.find ("player:remote-source-actor");
          return route != node->actor_routes.end ()
                 && route->second.node_rid.value () == "target-node"
                 && route->second.spot_id == "target-spot";
      })
      .get ();
}
}
