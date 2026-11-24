/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/provider_location_repository.hpp"
#include "../support/opaque_store_creation_fixture.hpp"

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
#include "../support/redis_location_options.hpp"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <fstream>
#include <iostream>
#include <thread>
#include <tuple>

namespace
{
using namespace zlink::framework::test::opaque_store;

// A fresh Store is required. Existing v11 records are outside this contract.
class contention_store_t final : public location_store_t
{
  public:
    contention_store_t (location_store_t &provider, bool synchronize_first_reserves) :
        provider (provider), synchronize_first_reserves (synchronize_first_reserves)
    {
    }

    task_t<store_read_result_t> read (store_key_t key) override
    {
        ++reads;
        return provider.read (std::move (key));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        ++scans;
        return provider.scan (std::move (request));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        ++writes;
        const bool authority_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<store_put_t> (&mutation);
              return put && put->key.value.starts_with (std::string ("authority") + '\0');
          });
        if (authority_write) {
            const auto attempt = authority_writes.fetch_add (1);
            if (synchronize_first_reserves && attempt < 2) {
                if (attempt == 1)
                    first_reserves.complete (result_t<void>::success ());
                co_await first_reserves.task ();
            }
            for (const auto &condition : request.conditions)
                if (std::holds_alternative<store_sum_at_most_condition_t> (condition))
                    ++reservation_conditions;
        } else if (request.mutations.empty () && !request.conditions.empty ()
                   && std::all_of (
                     request.conditions.begin (), request.conditions.end (),
                     [] (const auto &condition) {
                         return std::holds_alternative<store_sum_at_most_condition_t> (condition);
                     })) {
            ++capacity_probes;
        }
        auto result = co_await provider.write (std::move (request));
        if (authority_write && std::holds_alternative<store_write_conflict_t> (result))
            ++conflicts;
        co_return result;
    }

    location_store_t &provider;
    const bool synchronize_first_reserves;
    task_completion_source_t<void> first_reserves;
    std::atomic<int> reads{0};
    std::atomic<int> scans{0};
    std::atomic<int> writes{0};
    std::atomic<int> authority_writes{0};
    std::atomic<int> conflicts{0};
    std::atomic<int> reservation_conditions{0};
    std::atomic<int> capacity_probes{0};
};

using contention_case_t = std::tuple<bool, const char *>;
class LocationContention : public testing::TestWithParam<contention_case_t>
{
};

TEST_P (LocationContention, DistinctActorsDoNotContend)
{
    const auto [redis, case_name] = GetParam ();
    std::unique_ptr<location_store_t> provider;
    if (redis) {
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
        if (const auto options = zlink::framework::test::find_redis_options ())
            provider = std::make_unique<zlink::framework::redis::redis_location_store_t> (*options);
#endif
        if (!provider)
            GTEST_SKIP () << "Set ZLINK_REDIS_TEST_ENDPOINT with Redis support enabled";
    } else {
        provider = std::make_unique<in_memory_location_store_t> ();
    }

    std::ifstream fixture (ZLINK_LOCATION_CONTENTION_FIXTURE_PATH);
    ASSERT_TRUE (fixture.is_open ());
    const auto contract = nlohmann::json::parse (fixture).at ("distinctActorCreations");
    const auto expected = contract.at ("expected");
    const auto settings = contract.at ("cases").at (case_name);
    const int actor_count = contract.at ("actorCount");
    const int limit = settings.at ("actorLimit");
    const std::string mesh = contract.at ("mesh");
    const std::string actor_type = contract.at ("actorType");
    const auto actor_prefix =
      contract.at ("actorIdPrefix").get<std::string> ()
      + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()) + "-";
    contention_store_t store (*provider, !redis);
    provider_location_repository_t repository (store);
    const auto lease = repository.claim_owner_lease (actor_prefix, 120s).result ().value ();
    const auto &owner = std::get<owner_lease_claimed_t> (lease).token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = mesh;
    descriptor.rid = zlink::routing_id_t::from (actor_prefix);
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.capacity.actors.limit = limit;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, actor_type, maintenance_policy_kind_t::recreate, false, 0});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const auto make_request = [&] (int actor) {
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, actor_prefix + std::to_string (actor)};
        request.intent.stable_type = actor_type;
        request.target = {mesh, node_rid_t::from_string (actor_prefix), 1, owner};
        request.creating_payload = bytes ("creating");
        request.capacity_bundle.actor_slots = 1;
        request.operation_deadline = std::chrono::system_clock::now () + 90s;
        return request;
    };

    store.reads = 0;
    store.scans = 0;
    store.writes = 0;
    std::atomic<int> created{0};
    std::atomic<int> first_generation{0};
    std::barrier start (actor_count);
    std::vector<std::jthread> actors;
    for (int actor = 0; actor < actor_count; ++actor) {
        actors.emplace_back ([&, actor] {
            start.arrive_and_wait ();
            auto request = make_request (actor);
            const auto reserved = repository.reserve (request).result ();
            if (!reserved)
                return;
            const auto *record = std::get_if<object_reserved_t> (&reserved.value ());
            if (!record)
                return;
            if (record->fence.object_generation == expected.at ("objectGeneration")
                && record->fence.authority_owner_generation == expected.at ("objectGeneration"))
                ++first_generation;
            const creation_terminal_publication_t terminal{
              {request.target.node_rid, 1, {1, static_cast<std::uint64_t> (actor + 1)}},
              bytes ("terminal"),
              request.operation_deadline};
            const auto committed =
              repository
                .complete_creation ({request.key, record->fence,
                                     object_creation_completed_t{bytes ("ready"), terminal}})
                .result ();
            if (committed
                && std::holds_alternative<object_creation_completed_result_t> (committed.value ()))
                ++created;
        });
    }
    actors.clear ();

    std::cout << case_name << " created=" << created
              << " authority_writes=" << store.authority_writes << " conflicts=" << store.conflicts
              << " reads=" << store.reads << " scans=" << store.scans << " writes=" << store.writes
              << '\n';
    EXPECT_EQ (created, expected.at ("createdActors").get<int> ());
    EXPECT_EQ (store.authority_writes,
               actor_count * expected.at ("authorityWritesPerActor").get<int> ());
    EXPECT_EQ (store.conflicts, expected.at ("writeConflicts").get<int> ());
    EXPECT_EQ (first_generation, actor_count);
    EXPECT_EQ (store.reservation_conditions,
               settings.at ("expectedReservationConditions").get<int> ());

    EXPECT_LE (store.reads.load (), actor_count * 12);
    EXPECT_EQ (store.scans.load (), 0);
    const auto active = capacity_key (descriptor, "actors", "active");
    const auto pending = capacity_key (descriptor, "actors", "pending");
    const auto active_before = std::get<store_found_t> (provider->read (active).result ().value ());
    const auto pending_before =
      std::get<store_found_t> (provider->read (pending).result ().value ());
    EXPECT_EQ (active_before.value.bytes,
               bytes (std::to_string (expected.at ("activeActorSlots").get<int> ())));
    EXPECT_EQ (pending_before.value.bytes,
               bytes (std::to_string (expected.at ("pendingActorSlots").get<int> ())));

    const auto listed = repository.list_mesh_nodes_core (mesh).result ().value ();
    const auto target =
      std::find_if (listed.items.begin (), listed.items.end (),
                    [&] (const auto &item) { return item.rid == descriptor.rid; });
    ASSERT_NE (target, listed.items.end ());
    EXPECT_EQ (target->capacity.actors.active, expected.at ("activeActorSlots").get<int> ());
    EXPECT_EQ (target->capacity.actors.reserved, expected.at ("pendingActorSlots").get<int> ());
    EXPECT_EQ (store.capacity_probes.load (), 0);
    if (limit != 0) {
        const auto writes_before_overflow = store.writes.load ();
        const auto overflow = make_request (actor_count);
        EXPECT_TRUE (std::holds_alternative<object_placement_capacity_exhausted_t> (
          repository.reserve (overflow).result ().value ()));
        EXPECT_EQ (store.writes.load (), writes_before_overflow + 2);
        EXPECT_EQ (store.capacity_probes.load (), 1);
        const auto after_active =
          std::get<store_found_t> (provider->read (active).result ().value ());
        const auto after_pending =
          std::get<store_found_t> (provider->read (pending).result ().value ());
        EXPECT_EQ (after_active.value.version.value, active_before.value.version.value);
        EXPECT_EQ (after_pending.value.version.value, pending_before.value.version.value);
        EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
          repository.read_authority (actor_authority_key (overflow.key.global_id))
            .result ()
            .value ()));
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (
          provider->read (generation_key ("actor", overflow.key.global_id)).result ().value ()));
    }
}

INSTANTIATE_TEST_SUITE_P (InMemory,
                          LocationContention,
                          testing::Values (contention_case_t{false, "unlimited"},
                                           contention_case_t{false, "at-limit"}));
INSTANTIATE_TEST_SUITE_P (Redis,
                          LocationContention,
                          testing::Values (contention_case_t{true, "unlimited"},
                                           contention_case_t{true, "at-limit"}));
} // namespace
