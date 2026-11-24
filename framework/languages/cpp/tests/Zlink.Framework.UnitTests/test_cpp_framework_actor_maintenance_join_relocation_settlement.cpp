/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

TEST (ActorMaintenanceJoin, ActiveInventoryExcludesCreatingRecoveringAndFinalizedSource)
{
    namespace stateful = fw::runtime::stateful;
    const auto make_node = [] (std::string rid) {
        const std::vector<std::uint8_t> routing_bytes (rid.begin (), rid.end ());
        fw::runtime::mesh::service_node_descriptor_t descriptor{
          mesh_name,
          routing_bytes,
          1,
          1,
          "tcp://127.0.0.1:0",
          {},
          fw::runtime::mesh::service_node_state_t::preparing};
        auto native = std::make_shared<fw::runtime::host::public_host_runtime_t> (
          fw::runtime::host::host_options_t{fw::test::runtime_failure_options (
            fw::runtime::mesh::raw_mesh_node_options_t{.descriptor = std::move (descriptor)})});
        auto state =
          fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
        state->snapshot.name = mesh_name;
        state->snapshot.routing_id = zlink::routing_id_t::from (rid);
        state->native_node = native;
        return std::pair{std::move (native), std::move (state)};
    };
    auto [source_native, source_state] = make_node (first_rid);
    auto &source = source_native->objects ();
    const fw::detail::spot_node_runtime_t source_runtime (source_state);
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, first_rid};
    const auto materialized_key = std::string (actor_type) + ":" + actor_id;
    const auto reserved = begin_reserved_object (source, owner, actor_type, {});
    ASSERT_EQ (reserved.error, stateful::stateful_error_t::none);
    source_state->lane
      .run (
        [&] { source_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    source_state->lane.run ([&] { source_state->actor_instances.erase (materialized_key); }).get ();
    ASSERT_EQ (source.commit_create (reserved.attempt), stateful::stateful_error_t::none);
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    source_state->lane
      .run (
        [&] { source_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    const auto active = source_runtime.local_actor_refs ();
    ASSERT_EQ (active.size (), 1u);
    EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (active.front ()), actor_type);
    EXPECT_EQ (active.front ().object_generation (), owner.object_generation);
    EXPECT_EQ (active.front ().actor_id ().value (), owner.key);
    EXPECT_EQ (source.inventory_async ().result ().value ().front ().owner, owner);
    EXPECT_EQ (active.front ().node_rid ().value (), owner.node_id);
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 1u);

    {
        auto [remote_native, remote_state] = make_node (first_rid);
        auto &remote_objects = remote_native->objects ();
        const fw::detail::spot_node_runtime_t remote_runtime (remote_state);
        const stateful::object_ref_t remote{
          stateful::object_kind_t::actor, "remote-inventory", 1, 1, mesh_name, second_rid};
        const auto remote_reserved = begin_reserved_object (remote_objects, remote, actor_type, {});
        ASSERT_EQ (remote_reserved.error, stateful::stateful_error_t::none);
        ASSERT_EQ (remote_objects.commit_create (remote_reserved.attempt),
                   stateful::stateful_error_t::none);
        const auto remote_key = std::string (actor_type) + ":" + remote.key;
        remote_state->lane
          .run (
            [&] { remote_state->actor_instances.emplace (remote_key, std::make_shared<int> ()); })
          .get ();
        EXPECT_EQ (remote_objects.inventory_async ().result ().value ().front ().owner, remote);
        EXPECT_TRUE (remote_runtime.local_actor_refs ().empty ());
        EXPECT_EQ (remote_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    }

    const stateful::object_ref_t destination{
      stateful::object_kind_t::user_spot, local_spot, 1, 1, mesh_name, second_rid};
    const auto move =
      source.begin_remote_membership_move_async (owner, destination).result ().value ();
    ASSERT_EQ (move.first, stateful::stateful_error_t::none);
    EXPECT_EQ (source_runtime.local_actor_refs ().size (), 1u);
    ASSERT_EQ (source.abort_membership_move_async (move.second).result ().value (),
               stateful::stateful_error_t::none);

    const auto sealed = source.try_seal_relocation_aggregate ({owner}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    EXPECT_EQ (source_runtime.local_actor_refs ().size (), 1u);
    ASSERT_EQ (source.begin_relocation_boundary (sealed.seal.token).first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (source.finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (source.inventory_async ().result ().value ().size (), 1u);
    EXPECT_TRUE (
      source.finalized_relocation_source (owner.kind, owner.key, owner.object_generation));
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (source_runtime.monitoring_counts_async ().result ().value ().first, 0u);

    auto [target_native, target_state] = make_node (second_rid);
    auto &target = target_native->objects ();
    const fw::detail::spot_node_runtime_t target_runtime (target_state);
    EXPECT_TRUE (target_runtime.local_actor_refs ().empty ());
    auto relocated = owner;
    relocated.node_id = second_rid;
    ++relocated.authority_owner_generation;
    const stateful::relocation_restore_identity_t identity{"active-inventory-relocation"};
    ASSERT_EQ (target.restore_relocation (sealed.seal.participants.front (), relocated, identity)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto staged = target.inventory_async ().result ().value ();
    ASSERT_EQ (staged.size (), 1u);
    EXPECT_EQ (staged.front ().owner, relocated);
    EXPECT_EQ (staged.front ().state, stateful::object_state_t::recovering);
    target_state->lane
      .run (
        [&] { target_state->actor_instances.emplace (materialized_key, std::make_shared<int> ()); })
      .get ();
    EXPECT_TRUE (target_runtime.local_actor_refs ().empty ());
    EXPECT_EQ (target_runtime.monitoring_counts_async ().result ().value ().first, 0u);
    ASSERT_EQ (target.commit_relocation_restore (relocated, identity),
               stateful::stateful_error_t::none);
    const auto committed = target_runtime.local_actor_refs ();
    ASSERT_EQ (committed.size (), 1u);
    EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (committed.front ()), actor_type);
    EXPECT_EQ (committed.front ().object_generation (), relocated.object_generation);
    EXPECT_EQ (committed.front ().actor_id ().value (), relocated.key);
    EXPECT_EQ (target.inventory_async ().result ().value ().front ().owner, relocated);
    EXPECT_EQ (committed.front ().node_rid ().value (), relocated.node_id);
    EXPECT_EQ (target_runtime.monitoring_counts_async ().result ().value ().first, 1u);
    EXPECT_TRUE (source_runtime.local_actor_refs ().empty ());
}

class join_settlement_store_t final : public fw::location_store_t
{
  public:
    explicit join_settlement_store_t (std::shared_ptr<fw::location_store_t> inner) :
        inner (std::move (inner))
    {
    }
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        const auto actor_authority =
          cleanup_started.task ().await_ready () && key.value == authority_key.value;
        auto result = co_await inner->read (std::move (key));
        if (actor_authority && std::holds_alternative<fw::store_missing_t> (result))
            missing.complete (fw::result_t<void>::success ());
        co_return result;
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return inner->write (std::move (request));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return inner->scan (std::move (request));
    }
    std::shared_ptr<fw::location_store_t> inner;
    fw::store_key_t authority_key;
    fw::task_completion_source_t<void> cleanup_started;
    fw::task_completion_source_t<void> release;
    fw::task_completion_source_t<void> missing;
};

// Pause the source decision at its owner, after boundary/cutover can progress.
// Every authority operation still delegates to the production Store adapter.
class join_settlement_authority_t final : public fw::runtime::stateful::authority_relocation_port_t
{
    using object = fw::runtime::stateful::object_ref_t;
    using kind = fw::runtime::stateful::object_kind_t;
    using fence = fw::runtime::stateful::relocation_authority_fence_t;
    using authority = fw::runtime::stateful::relocation_authority_t;
    using published = fw::runtime::stateful::authority_publish_result_t;
    using reference = fw::runtime::stateful::authority_relocation_reference_t;
    using digest = fw::runtime::stateful::inventory_digest_t;

  public:
    join_settlement_authority_t (fw::location_repository_t &repository,
                                 std::shared_ptr<join_settlement_store_t> control) :
        _inner (repository), _control (std::move (control))
    {
    }
    authority observe_relocation (const fence &value) override
    {
        return _inner.observe_relocation (value);
    }
    fw::runtime::stateful::source_relocation_settlement_t settle_source_relocation (
      const fence &value, const std::vector<object> &participants, bool preserve) override
    {
        _control->cleanup_started.complete (fw::result_t<void>::success ());
        fw::runtime::infrastructure_result ([control = _control] {
            return control->release.task ();
        }).value ();
        return _inner.settle_source_relocation (value, participants, preserve);
    }
    std::optional<bool> owner_lease_live (const fw::location_owner_token_t &owner) override
    {
        return _inner.owner_lease_live (owner);
    }
    std::optional<std::vector<fw::runtime::stateful::relocation_participant_identity_t>>
    list_participant_identities () override
    {
        return _inner.list_participant_identities ();
    }
    published publish (const object &source,
                       const object &target,
                       fw::location_owner_token_t owner,
                       fw::object_creation_target_t placement,
                       std::string relocation_reference,
                       std::uint32_t checksum,
                       digest inventory,
                       std::vector<std::byte> payload,
                       std::string version,
                       fw::runtime::protocol::relocation_id_t relocation,
                       fw::location_owner_token_t source_owner,
                       const fw::runtime::protocol::relocation_prepare_t *prepare) override
    {
        return _inner.publish (source, target, std::move (owner), std::move (placement),
                               std::move (relocation_reference), checksum, inventory,
                               std::move (payload), std::move (version), relocation,
                               std::move (source_owner), prepare);
    }
    bool finish_relocation (const object &target,
                            const fw::runtime::protocol::relocation_prepare_t &prepare) override
    {
        return _inner.finish_relocation (target, prepare);
    }
    std::optional<reference> read (kind type, const std::string &key) override
    {
        return _inner.read (type, key);
    }
    published publish_completion (kind type,
                                  const std::string &key,
                                  const std::string &version,
                                  std::uint64_t generation,
                                  std::string relocation_reference,
                                  std::uint32_t checksum) override
    {
        return _inner.publish_completion (type, key, version, generation,
                                          std::move (relocation_reference), checksum);
    }
    published replace_completion (kind type,
                                  const std::string &key,
                                  std::uint64_t generation,
                                  const std::string &previous,
                                  std::uint32_t previous_checksum,
                                  std::string relocation_reference,
                                  std::uint32_t checksum) override
    {
        return _inner.replace_completion (type, key, generation, previous, previous_checksum,
                                          std::move (relocation_reference), checksum);
    }
    bool release_completion (kind type,
                             const std::string &key,
                             std::uint64_t generation,
                             const std::string &relocation_reference,
                             std::uint32_t checksum) override
    {
        return _inner.release_completion (type, key, generation, relocation_reference, checksum);
    }

  private:
    fw::runtime::stateful::public_authority_store_adapter_t _inner;
    std::shared_ptr<join_settlement_store_t> _control;
};

// Observe the real deferred Join terminal without replacing its execution gate.
class observed_join_barrier_t final : public fw::detail::deferred_barrier_t
{
  public:
    observed_join_barrier_t (
      std::shared_ptr<fw::detail::deferred_barrier_t> inner,
      std::shared_ptr<fw::task_completion_source_t<fw::result_t<void>>> result) :
        _inner (std::move (inner)), _result (std::move (result))
    {
    }
    fw::result_t<void> activate (std::function<void ()> work) override
    {
        return _inner->activate (std::move (work));
    }
    fw::result_t<void> activate_async (async_work_t work) override
    {
        return _inner->activate_async ([work = std::move (work),
                                        result = _result] (auto complete) mutable {
            work ([result, complete = std::move (complete)] (fw::result_t<void> terminal) mutable {
                result->complete (fw::result_t<fw::result_t<void>>::success (terminal));
                complete (std::move (terminal));
            });
        });
    }
    fw::task_t<void> run (std::function<fw::task_t<void> ()> work) override
    {
        return _inner->run (std::move (work));
    }
    fw::task_t<void> terminal () override { return _inner->terminal (); }
    void cancel () noexcept override { _inner->cancel (); }

  private:
    std::shared_ptr<fw::detail::deferred_barrier_t> _inner;
    std::shared_ptr<fw::task_completion_source_t<fw::result_t<void>>> _result;
};

TEST (ActorMaintenanceJoin, RelayedSourceCleanupFailureDoesNotFailDeferredJoin)
{
    for (const auto kind :
         {fw::framework_error_kind_t::internal_failure, fw::framework_error_kind_t::data_lost}) {
        SCOPED_TRACE (static_cast<int> (kind));
        fw::runtime::offload_executor_t executor (1, "join-cleanup-contract");
        fw::runtime::serial_execution_queue_t caller (executor), owner (executor);
        fw::detail::actor_gateway_runtime_t gateway;
        const auto source = fw::detail::actor_ref_access_t::make (
          fw::node_rid_t::from_string ("cleanup-source"), "member", "cleanup-actor", 1);
        const auto target = fw::detail::actor_ref_access_t::make (
          fw::node_rid_t::from_string ("cleanup-target"), "member", "cleanup-actor", 1);
        auto terminal = std::make_shared<fw::task_completion_source_t<fw::result_t<void>>> ();
        gateway.on_join_barrier ([&] (const fw::actor_ref_t &) {
            auto reserved = owner.reserve_barrier_next ("join-cleanup", true);
            if (!reserved)
                return reserved;
            return fw::result_t<std::shared_ptr<fw::detail::deferred_barrier_t>>::success (
              std::make_shared<observed_join_barrier_t> (reserved.value (), terminal));
        });
        gateway.on_join_spot ([target, kind] (const auto &, auto, const auto &, auto, auto,
                                              auto) -> fw::task_t<fw::detail::actor_join_reply_t> {
            co_return fw::detail::actor_join_reply_t{
              fw::detail::actor_join_reply_t::relayed, target, {}, [kind] {
                  return [] (fw::framework_error_kind_t failure) -> fw::task_t<void> {
                      throw fw::framework_exception_t (failure,
                                                       "source settlement did not complete");
                      co_return;
                  }(kind);
              }};
        });
        auto context = gateway.actor_context (source);
        caller.run ("join-handler", [&] { context.join_spot ("target-spot").defer (); });
        const auto completed = terminal->task ().result_for (wait_budget);
        ASSERT_TRUE (completed);
        ASSERT_TRUE (*completed);
        EXPECT_TRUE (completed->value ());
        EXPECT_EQ (context.actor_ref ().node_rid ().value (), source.node_rid ().value ());
        caller.drain ();
        owner.drain ();
    }
}

TEST (ActorMaintenanceJoin, MissingAfterTargetCommitAndDestroyDoesNotCompleteOnSource)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    auto source_store = std::make_shared<join_settlement_store_t> (store);
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    struct release_store_t
    {
        std::shared_ptr<join_settlement_store_t> store;
        ~release_store_t () { store->release.complete (fw::result_t<void>::success ()); }
    } release_store{source_store};
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, source_store, relocations,
               default_follow_duration, 0, true);
    source.app.advanced ()
      .services ()
      .add_factory<fw::runtime::stateful::authority_relocation_port_t> (
        [source_store] (fw::service_provider_t &provider) {
            return std::unique_ptr<fw::runtime::stateful::authority_relocation_port_t> (
              std::make_unique<join_settlement_authority_t> (
                provider.get_required<fw::location_repository_t> (), source_store));
        },
        fw::service_lifetime_t::singleton);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto join_terminal = std::make_shared<fw::task_completion_source_t<fw::result_t<void>>> ();
    auto infrastructure_failure =
      std::make_shared<fw::task_completion_source_t<std::exception_ptr>> ();
    auto &gateway = services.get_required<fw::detail::actor_gateway_runtime_t> ();
    const auto reserve = gateway.weak_state ().lock ()->join_barrier_reserver;
    gateway.on_join_barrier ([reserve, join_terminal] (const fw::actor_ref_t &actor) {
        auto reserved = reserve (actor);
        if (!reserved)
            return reserved;
        return fw::result_t<std::shared_ptr<fw::detail::deferred_barrier_t>>::success (
          std::make_shared<observed_join_barrier_t> (reserved.value (), join_terminal));
    });
    fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ())
      .front ()
      ->spot_state->runtime_failures ()
      ->bind_runtime_reporter ([infrastructure_failure] (std::exception_ptr failure) {
          infrastructure_failure->complete (fw::result_t<std::exception_ptr>::success (failure));
      });
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    const auto key = fw::runtime::actor_authority_key (actor_id);
    const auto authority = std::get<fw::authority_snapshot_t> (
      services.get_required<fw::location_repository_t> ().read_authority (key).result ().value ());
    const auto page =
      std::get<fw::store_scan_page_t> (store->scan ({"", std::nullopt, 100}).result ().value ());
    for (const auto &item : page.items)
        if (item.value.version.value == authority.store_version)
            source_store->authority_key = item.key;
    ASSERT_FALSE (source_store->authority_key.value.empty ());
    ASSERT_TRUE (set_published_placement_weight (services, 0));
    configure (target, evidence_owner, second_rid, store, relocations, default_follow_duration, 0,
               true);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    ASSERT_TRUE (source_store->cleanup_started.task ().result_for (wait_budget));
    ASSERT_TRUE (
      move_to (target_services.get_required<fw::actor_client_t> (), evidence, entry_target, 2));
    const auto entry_ready = target_services.get_required<fw::actor_client_t> ()
                               .request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                               .async<probe_t> ()
                               .result ();
    ASSERT_TRUE (entry_ready);
    EXPECT_GT (entry_ready.value ().value, 0);
    std::optional<fw::actor_ref_t> current;
    {
        std::lock_guard lock (evidence.mutex);
        current = std::get<fw::actor_join_accepted_t> (evidence.completions.back ()).actor;
    }
    ASSERT_TRUE (
      target_services.get_required<fw::actor_manager_t> ().destroy (*current).result ().value ());
    ASSERT_TRUE (std::holds_alternative<fw::authority_missing_t> (
      target_services.get_required<fw::location_repository_t> ()
        .read_authority (key)
        .result ()
        .value ()));
    source_store->release.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (source_store->missing.task ().result_for (wait_budget));
    const auto public_result = join_terminal->task ().result_for (wait_budget);
    ASSERT_TRUE (public_result);
    ASSERT_TRUE (*public_result);
    EXPECT_TRUE (public_result->value ())
      << (public_result->value ().error () ? public_result->value ().error ()->what () : "");
    const auto infrastructure = infrastructure_failure->task ().result_for (wait_budget);
    ASSERT_TRUE (infrastructure);
    ASSERT_TRUE (*infrastructure);
    ASSERT_NE (infrastructure->value (), nullptr);
    try {
        std::rethrow_exception (infrastructure->value ());
    }
    catch (const fw::framework_exception_t &error) {
        EXPECT_EQ (error.kind (), fw::framework_error_kind_t::unavailable);
    }
    evidence.release_leave.complete (fw::result_t<void>::success ());
    const auto stopped = source.app.shutdown (wait_budget).result ();
    ASSERT_TRUE (stopped);
    ASSERT_EQ (stopped.value ().outcome, fw::termination_outcome_t::stopped);
    std::lock_guard lock (evidence.mutex);
    ASSERT_EQ (evidence.completions.size (), 2u);
    EXPECT_TRUE (std::ranges::all_of (evidence.completion_owners,
                                      [] (const auto &owner) { return owner == second_rid; }));
}
} // namespace zlink::framework::test::actor_maintenance_join
