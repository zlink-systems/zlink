/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/actor_authority_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_counter_conflict_fixture.hpp"
#include "../support/opaque_store_record_fixture.hpp"

namespace zlink::framework::detail
{
struct spot_node_runtime_test_access_t
{
    template <class Runtime>
    static task_t<void>
    commit_membership (Runtime &runtime,
                       spot_context_t &context,
                       const actor_ref_t &actor,
                       std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
    {
        bool committed = false;
        spot_actor_admission_callbacks_t callbacks;
        if constexpr (requires {
                          runtime.commit_accepted_actor_join ("join-test", context, actor,
                                                              typeid (int), nullptr, callbacks,
                                                              false, {}, {}, committed, deadline);
                      }) {
            co_await runtime.commit_accepted_actor_join ("join-test", context, actor, typeid (int),
                                                         nullptr, callbacks, false, {}, {},
                                                         committed, deadline);
        } else {
            co_await runtime.commit_accepted_actor_join ("join-test", context, actor, typeid (int),
                                                         nullptr, callbacks, false, {}, {},
                                                         committed);
        }
    }
};
}

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

// Both backends must preserve the membership transaction's failure cause.
template <class Conflict> framework_error_kind_t membership_kind (const Conflict &conflict)
{
    if constexpr (requires { conflict.membership_failure; })
        return conflict.membership_failure.value_or (framework_error_kind_t::unavailable);
    return framework_error_kind_t::unavailable;
}


template <class Put>
std::optional<std::chrono::steady_clock::time_point> membership_deadline (const Put &put)
{
    if constexpr (requires { put.operation_deadline; })
        return put.operation_deadline;
    return std::nullopt;
}

template <class Runtime>
task_t<detail::actor_join_reply_t>
join_before_deadline (Runtime &runtime,
                      const actor_ref_t &actor,
                      actor_context_t context,
                      spot_id_t spot,
                      std::chrono::steady_clock::time_point deadline)
{
    if constexpr (requires {
                      runtime.join_actor_to_spot_erased (actor, spot, {}, std::nullopt,
                                                         std::move (context), 0, 0, nullptr,
                                                         deadline);
                  }) {
        co_return co_await runtime.join_actor_to_spot_erased (
          actor, spot, {}, std::nullopt, std::move (context), 0, 0, nullptr, deadline);
    } else {
        co_return co_await runtime.join_actor_to_spot_erased (actor, spot, {});
    }
}

class join_result_repository_t final : public in_memory_location_repository_t
{
  public:
    join_result_repository_t (location_repository_t &inner, int fault) :
        _inner (inner), _fault (fault)
    {
    }
    task_t<authority_read_result_t> read_authority_core (authority_key_t key,
                                                         std::stop_token cancellation = {}) override
    {
        co_return co_await _inner.read_authority (std::move (key), cancellation);
    }
    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override
    {
        ++commits;
        if (const auto *put = std::get_if<authority_put_t> (&mutation))
            observed_deadline = membership_deadline (*put);
        if (_fault == 7)
            co_return authority_compare_exchange_result_t{authority_generation_exhausted_t{}};
        if (_fault == 8) {
            auto current = std::get<authority_snapshot_t> (co_await _inner.read_authority (key));
            current.object_generation += 1000;
            co_return authority_compare_exchange_result_t{
              authority_conflict_t{std::move (current)}};
        }
        if (_fault == 9)
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Injected Store failure");
        co_return co_await _inner.compare_exchange_authority (std::move (key), std::move (expected),
                                                              std::move (mutation), cancellation);
    }
    int commits = 0;
    std::optional<std::chrono::steady_clock::time_point> observed_deadline;

  private:
    location_repository_t &_inner;
    int _fault;
};

class JoinMembershipFailure : public testing::TestWithParam<std::tuple<bool, int>>
{
};

TEST_P (JoinMembershipFailure, StorePreservesCommitFailureKind)
{
    const auto [provider_backend, scenario] = GetParam ();
    counter_conflict_store_t provider;
    provider_location_repository_t persistent (provider);
    persistent.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory;
    memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_repository_t &repository = provider_backend
                                          ? static_cast<location_repository_t &> (persistent)
                                          : static_cast<location_repository_t &> (memory);
    const auto actor_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("join-actor-owner", 30s).result ().value ())
        .token;
    const auto spot_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("join-spot-owner", 30s).result ().value ())
        .token;
    const auto publish = [&] (const char *id, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "join-mesh";
        descriptor.rid = zlink::routing_id_t::from (std::string (id));
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false,
           0}};
        descriptor.capacity.actors.limit = 8;
        descriptor.capacity.spots.limit = 8;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {8, 0, 0}});
        descriptor.application_version = 1;
        descriptor.security_identity = id;
        descriptor.activation_concurrency.limit = 8;
        EXPECT_EQ (location_write_status_t::stored,
                   repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
    };
    publish ("actor-node", actor_owner);
    publish ("spot-node", spot_owner);
    const auto create = [&] (placement_object_kind_t kind, const char *id,
                             const location_owner_token_t &owner, const char *node,
                             const std::vector<std::byte> &payload) {
        object_reserve_request_t request;
        request.key = {kind, id};
        request.creating_payload = bytes ("creating");
        request.intent.stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
        request.target = {"join-mesh", node_rid_t::from_string (node), 1, owner};
        if (kind == placement_object_kind_t::actor)
            request.capacity_bundle.actor_slots = 1;
        else {
            request.capacity_bundle.spot_slots = 1;
            request.capacity_bundle.spot_type = spot_type_capacity_delta_t{kind, "room", 1};
        }
        auto reserved =
          std::get<object_reserved_t> (repository.reserve (request).result ().value ());
        if (provider_backend && kind == placement_object_kind_t::user_spot) {
            // The join must use the Spot's generation.
            EXPECT_TRUE (std::holds_alternative<object_aborted_t> (
              repository.abort ({request.key, reserved.fence}).result ().value ()));
            reserved =
              std::get<object_reserved_t> (repository.reserve (request).result ().value ());
        }
        return std::get<object_committed_t> (
                 repository.commit ({request.key, reserved.fence, payload}).result ().value ())
          .ready;
    };
    actor_authority_payload_t member{.stable_type = "player",
                                     .actor_id = "join-actor",
                                     .current_spot_id = "entry",
                                     .current_spot_generation = 101,
                                     .current_spot_kind = actor_authority_spot_kind_t::entry,
                                     .owner_id = actor_owner.owner_id,
                                     .owner_lease_generation =
                                       static_cast<std::uint64_t> (actor_owner.lease_generation),
                                     .mesh_name = "join-mesh",
                                     .node_rid = node_rid_t::from_string ("actor-node"),
                                     .node_generation = 1};
    auto actor = create (placement_object_kind_t::actor, "join-actor", actor_owner, "actor-node",
                         encode_actor_authority_payload (member));
    user_spot_authority_payload_t spot_payload{
      .stable_type = "room",
      .spot_id = "join-spot",
      .owner_id = spot_owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (spot_owner.lease_generation),
      .mesh_name = "join-mesh",
      .node_rid = node_rid_t::from_string ("spot-node"),
      .node_generation = 1};
    if (scenario == 2)
        spot_payload.state = user_spot_authority_state_t::closing;
    std::optional<authority_snapshot_t> spot;
    if (scenario != 0)
        spot = create (placement_object_kind_t::user_spot, "join-spot", spot_owner, "spot-node",
                       encode_user_spot_authority_payload (spot_payload));
    if (spot)
        ASSERT_NE (actor.object_generation, spot->object_generation);
    member.current_spot_id = "join-spot";
    member.current_spot_kind = actor_authority_spot_kind_t::user;
    member.current_spot_generation = spot ? spot->object_generation : 99;
    if (scenario == 1)
        ++member.current_spot_generation;
    if (scenario == 3 || (!provider_backend && scenario == 6))
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository.release_owner_lease (spot_owner).result ().value ()));
    if (scenario == 4) {
        aggregate_prepare_request_t aggregate;
        aggregate.aggregate_id.value[0] = std::byte{1};
        aggregate.aggregate_generation = 1;
        aggregate.participants = {{spot_authority_key ("join-spot"),
                                   spot->store_version,
                                   authority_generation_transition_t::new_owner,
                                   encode_user_spot_authority_payload (spot_payload),
                                   {}}};
        aggregate.target_owner = actor_owner;
        aggregate.target_descriptor = {"join-mesh", zlink::routing_id_t::from ("actor-node")};
        aggregate.target_descriptor_lifecycle_generation = 1;
        aggregate.capacity_bundle.spot_slots = 1;
        aggregate.capacity_bundle.spot_type =
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (
          repository.prepare_aggregate (aggregate).result ().value ()));
    }
    if (scenario == 10) {
        ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository.release_owner_lease (spot_owner).result ().value ()));
        const auto replacement =
          std::get<owner_lease_claimed_t> (
            repository.claim_owner_lease (spot_owner.owner_id, 30s).result ().value ())
            .token;
        ASSERT_NE (replacement.lease_generation, spot_owner.lease_generation);
    }
    if (scenario == 11) {
        ASSERT_TRUE (std::holds_alternative<owner_lease_renewed_t> (
          repository.renew_owner_lease (spot_owner, 1ms).result ().value ()));
        std::this_thread::sleep_for (2ms);
    }
    if (scenario == 12) {
        std::ifstream fixture (ZLINK_AUTHORITY_RELOCATION_STATE_GOLDEN_PATH);
        const auto golden = nlohmann::json::parse (fixture);
        const auto slot =
          tests::from_hex (golden.at ("valid").at (0).at ("hex").get<std::string> ());
        const auto relocating = tests::with_relocation_slot (
          encode_actor_authority_payload (member), {slot.begin () + 5, slot.end ()});
        const auto updated =
          repository
            .compare_exchange_authority (actor_authority_key ("join-actor"), actor.store_version,
                                         authority_put_t{relocating})
            .result ()
            .value ();
        ASSERT_TRUE (std::holds_alternative<authority_stored_t> (updated));
        actor = std::get<authority_stored_t> (updated).snapshot;
        ASSERT_TRUE (decode_direct_actor_authority_payload (actor.payload)->has_relocation_state);
    }
    if (provider_backend && scenario >= 5 && scenario <= 6) {
        provider.remaining_conflicts = 1;
        if (scenario == 6) {
            provider.on_conflict = [&] () -> task_t<void> {
                co_await repository.release_owner_lease (spot_owner);
            };
        } else {
            // Only the Spot row's opaque revision changes. Its membership eligibility stays valid.
            provider.conflict_key = store_key_t{std::string ("authority\0spot\0join-spot", 24)};
        }
    }
    authority_compare_exchange_result_t result = authority_generation_exhausted_t{};
    if (scenario < 7 && scenario != 5)
        result =
          repository
            .compare_exchange_authority (actor_authority_key ("join-actor"), actor.store_version,
                                         authority_put_t{encode_actor_authority_payload (member)})
            .result ()
            .value ();
    {
        if (scenario < 7 && scenario != 5) {
            const auto *conflict = std::get_if<authority_conflict_t> (&result);
            ASSERT_NE (conflict, nullptr);
            EXPECT_EQ (scenario == 0 ? framework_error_kind_t::not_found
                                     : framework_error_kind_t::unavailable,
                       membership_kind (*conflict));
            const auto after = std::get<authority_snapshot_t> (
              repository.read_authority (actor_authority_key ("join-actor")).result ().value ());
            EXPECT_EQ (actor.store_version, after.store_version);
        }

        join_result_repository_t observed (repository, scenario);
        const auto node =
          zlink::framework::test::runtime_failure_fixture<detail::spot_node_builder_state_t> (
            "join-membership-kind");
        location_runtime_t locations (repository);
        location_lifecycle_t lifecycle (locations);
        node->location_lifecycle = &lifecycle;
        service_collection_t services;
        services.add_factory<location_repository_t> (
          [&] (service_provider_t &) {
              return std::shared_ptr<location_repository_t> (&observed, [] (auto *) {});
          },
          service_lifetime_t::singleton);
        node->root_services = services.build_provider ();
        node->snapshot.routing_id = zlink::routing_id_t::from ("actor-node");
        auto state = std::make_shared<detail::spot_context_state_t> ();
        state->node = node;
        state->node_rid = node_rid_t::from_string ("actor-node");
        state->spot_id = spot_id_t ("join-spot");
        state->object_generation = member.current_spot_generation;
        state->spot_instance = std::make_shared<int> (1);
        state->lifecycle_domain = detail::spot_lifecycle_domain_t::user ();
        node->spot_contexts_by_id.emplace (std::string (state->spot_id),
                                           detail::spot_context_access_t::create (state));
        auto &context = node->spot_contexts_by_id.at (std::string (state->spot_id));
        detail::spot_node_runtime_t runtime (node);
        const auto actor_ref = detail::actor_ref_access_t::make (
          node_rid_t::from_string ("actor-node"), "player", "join-actor", actor.object_generation);
        if (scenario == 5) {
            std::atomic_int factories{0}, admissions{0}, joined_callbacks{0};
            serializer_registry_t serializers;
            state->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
            state->channel_runtime->serializers = &serializers;
            state->serial_executor = std::make_shared<offload_executor_t> (2, "join-conflict-test");
            state->serial_queue = std::make_shared<serial_execution_queue_t> (
              *state->serial_executor, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{}, serial_lane_policy_t::spot_wide ());
            detail::spot_node_builder_state_t::actor_factory_registration_t registration;
            registration.actor_type = typeid (int);
            registration.create_instance = [&] (std::string) {
                ++factories;
                return std::make_shared<int> (1);
            };
            registration.configure_instance = [] (void *, const auto &, void *) {};
            node->actor_factories.emplace ("player", std::move (registration));
            detail::spot_actor_admission_callbacks_t callbacks;
            callbacks.join = [&] (void *, auto, const auto &, auto &) {
                ++admissions;
                return spot_actor_join_result_t::accept ();
            };
            callbacks.on_actor_joined = [&] (void *, void *) {
                ++joined_callbacks;
                return task_t<void> (result_t<void>::success ());
            };
            state->actor_admissions.emplace (typeid (int), std::move (callbacks));
            const auto deadline = std::chrono::steady_clock::now () + 30s;
            detail::actor_gateway_runtime_t gateway;
            const auto accepted =
              join_before_deadline (runtime, actor_ref, gateway.actor_context (actor_ref),
                                    state->spot_id, deadline)
                .result ();
            EXPECT_EQ (deadline, observed.observed_deadline);
            EXPECT_TRUE (accepted);
            if (accepted)
                EXPECT_EQ (0, accepted.value ().result_code);
            EXPECT_EQ (1, factories.load ());
            EXPECT_EQ (1, admissions.load ());
            EXPECT_EQ (1, joined_callbacks.load ());
            EXPECT_EQ (1, observed.commits);
            const auto current = std::get<authority_snapshot_t> (
              repository.read_authority (actor_authority_key ("join-actor")).result ().value ());
            EXPECT_EQ ("join-spot",
                       decode_direct_actor_authority_payload (current.payload)->current_spot_id);
            state->serial_queue->drain ();
            state->channel_runtime->serializers = nullptr;
        } else {
            const auto failed = detail::spot_node_runtime_test_access_t::commit_membership (
                                  runtime, context, actor_ref)
                                  .result ();
            ASSERT_FALSE (failed);
            const auto expected = scenario == 0   ? framework_error_kind_t::not_found
                                  : scenario == 7 ? framework_error_kind_t::internal_failure
                                  : scenario == 8 ? framework_error_kind_t::invalid_operation
                                                  : framework_error_kind_t::unavailable;
            EXPECT_EQ (expected, failed.error_kind ());
            EXPECT_EQ (scenario == 12 ? 0 : 1, observed.commits);
            const auto stale_actor =
              detail::actor_ref_access_t::make (node_rid_t::from_string ("actor-node"), "player",
                                                "join-actor", actor.object_generation + 1000);
            const auto stale = detail::spot_node_runtime_test_access_t::commit_membership (
                                 runtime, context, stale_actor)
                                 .result ();
            ASSERT_FALSE (stale);
            EXPECT_EQ (framework_error_kind_t::invalid_operation, stale.error_kind ());
            if (scenario == 0) {
                const auto expired =
                  detail::spot_node_runtime_test_access_t::commit_membership (
                    runtime, context, actor_ref, std::chrono::steady_clock::now () - 1ms)
                    .result ();
                EXPECT_FALSE (expired);
                if (!expired)
                    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, expired.error_kind ());
            }
        }
        runtime.request_stop ();
        runtime.cancel_pending_dispatch ();
        detail::drain_spot_node_executors (*node);
        runtime.release_native_handles ();
        node->location_lifecycle = nullptr;
    }
    if (provider_backend && scenario >= 5 && scenario <= 6)
        EXPECT_EQ (1u, provider.rejected);
}

INSTANTIATE_TEST_SUITE_P (Backends,
                          JoinMembershipFailure,
                          testing::Combine (testing::Bool (), testing::Range (0, 13)));
}
