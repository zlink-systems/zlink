/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace zlink::framework::test::actor_maintenance_join
{
class accepted_source_repository_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    accepted_source_repository_t ()
    {
        configure_reclaim_margin (std::chrono::milliseconds::zero ());
    }

    std::optional<fw::owner_lease_read_result_t> lease_override;
    int mesh_reads = 0, lease_reads = 0;
    fw::task_t<fw::location_page_t<fw::mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh, fw::location_page_request_t page = {}) override
    {
        ++mesh_reads;
        return in_memory_location_repository_t::list_mesh_nodes_core (std::move (mesh),
                                                                      std::move (page));
    }
    fw::task_t<fw::owner_lease_read_result_t> read_owner_lease (std::string owner_id) override
    {
        ++lease_reads;
        if (lease_override)
            return fw::task_t<fw::owner_lease_read_result_t> (
              fw::result_t<fw::owner_lease_read_result_t>::success (*lease_override));
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }
};

enum class accepted_source_case_t
{
    live,
    expired,
    wrong_owner,
    stale_node_generation,
    lease_generation_mismatch,
    replaced_live_owner
};

// Location §9.3: target restore uses the recorded source even after discovery or lease changes.
TEST (ActorMaintenanceJoin, RelocationPreservesAcceptedSourcesWithoutStoreRead)
{
    using source_case = accepted_source_case_t;
    for (const auto mode :
         {source_case::live, source_case::expired, source_case::wrong_owner,
          source_case::stale_node_generation, source_case::lease_generation_mismatch,
          source_case::replaced_live_owner}) {
        SCOPED_TRACE (static_cast<int> (mode));
        auto store = std::make_shared<accepted_source_repository_t> ();
        const auto token =
          std::get<fw::owner_lease_claimed_t> (
            store->claim_owner_lease ("accepted-source-owner", 15s).result ().value ())
            .token;
        fw::mesh_node_descriptor_t descriptor{.mesh_name = "source-boundary",
                                              .rid = zlink::routing_id_t::from ("source-node"),
                                              .lifecycle_generation = 1,
                                              .descriptor_revision = 1,
                                              .endpoint = "tcp://127.0.0.1:1",
                                              .state = fw::framework_runtime_state_t::serving,
                                              .security_identity = "test",
                                              .owner_id = token.owner_id,
                                              .lease_generation = token.lease_generation};
        ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   fw::location_write_status_t::stored);
        const auto now = std::chrono::system_clock::now ();
        if (mode == source_case::expired)
            store->lease_override = fw::owner_lease_found_t{token, now, now};
        else if (mode == source_case::wrong_owner) {
            auto other = token;
            other.owner_id = "other-owner";
            store->lease_override = fw::owner_lease_found_t{other, now + 15s, now};
        } else if (mode == source_case::lease_generation_mismatch) {
            auto newer = token;
            ++newer.lease_generation;
            store->lease_override = fw::owner_lease_found_t{newer, now + 15s, now};
        } else if (mode == source_case::replaced_live_owner) {
            const auto replacement =
              std::get<fw::owner_lease_claimed_t> (
                store->claim_owner_lease ("replacement-owner", 15s).result ().value ())
                .token;
            ASSERT_EQ (store->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, token)
                         .result ()
                         .value (),
                       fw::location_write_status_t::stored);
            descriptor.owner_id = replacement.owner_id;
            descriptor.lease_generation = replacement.lease_generation;
            ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status,
                       fw::location_write_status_t::stored);
        }
        fw::runtime::host::host_options_t options;
        options.mesh.descriptor = {"source-boundary",
                                   zlink::routing_id_t::from ("target-node").to_bytes (),
                                   1,
                                   1,
                                   "tcp://127.0.0.1:0",
                                   {},
                                   fw::runtime::mesh::service_node_state_t::preparing};
        options.mesh.runtime_failures =
          std::make_shared<fw::runtime::runtime_failure_collector_t> ();
        auto host =
          std::make_shared<fw::runtime::host::public_host_runtime_t> (std::move (options));
        const fw::runtime::protocol::request_source_fence_t source{
          token.owner_id, static_cast<std::uint64_t> (token.lease_generation),
          descriptor.rid.to_bytes (), mode == source_case::stale_node_generation ? 2u : 1u};
        auto materialized = std::make_shared<std::atomic<int>> (0);
        store->mesh_reads = store->lease_reads = 0;
        const auto restored =
          fw::runtime::host::public_host_runtime_test_access_t::restore_accepted_spot (
            *host, store, source, materialized)
            .result ();
        ASSERT_TRUE (restored);
        EXPECT_TRUE (restored.value ());
        EXPECT_EQ (materialized->load (), 1);
        EXPECT_EQ (store->mesh_reads, 0);
        EXPECT_EQ (store->lease_reads, 0);
        const auto object =
          host->objects ().resolve_spot_async ("accepted-spot").result ().value ();
        EXPECT_TRUE (object.has_value ());
        if (object)
            EXPECT_EQ (
              host->objects ().pending (*object, fw::runtime::stateful::turn_domain_t::application),
              1u);
    }
}

TEST (ActorMaintenanceJoin, WarmLocalAcceptedRequestDoesNotReadStore)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    host_t host;
    configure (host, evidence_owner, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (
      client.request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ().result ());
    // Use the provider's existing key conversion, as the pending-Store fixture does.
    auto captured = std::make_shared<intake_store_t> ();
    store->observe_authority_reads (captured->authority_store_key);
    const auto reply =
      client.request (fw::actor_id_t (actor_id), probe_t{23}).async<probe_t> ().result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 23);
    const auto [reads, scans] = store->accepted_store_calls ();
    EXPECT_EQ (reads, 0u);
    EXPECT_EQ (scans, 0u);
}

TEST (ActorMaintenanceJoin, StaleAttemptActivationDoesNotLeaveRawIdentity)
{
    namespace runtime = fw::runtime;
    for (const auto [replaced, activation_started] :
         {std::pair{false, false}, std::pair{true, false}, std::pair{false, true},
          std::pair{true, true}}) {
        SCOPED_TRACE (activation_started ? "activation queued" : "before activation");
        SCOPED_TRACE (replaced ? "replaced pending assembly" : "removed attempt");
        const auto rid = zlink::routing_id_t::from (second_rid);
        auto host = std::make_shared<runtime::host::public_host_runtime_t> (
          runtime::host::host_options_t{runtime::mesh::raw_mesh_node_options_t{
            .descriptor = {mesh_name,
                           rid.to_bytes (),
                           1,
                           1,
                           "tcp://127.0.0.1:0",
                           {},
                           runtime::mesh::service_node_state_t::preparing},
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        const runtime::stateful::object_ref_t target{
          runtime::stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, second_rid};
        const auto activated =
          runtime::host::public_host_runtime_test_access_t::activate_stale_actor_attempt (
            *host, replaced, activation_started, target, actor_type)
            .result ();
        ASSERT_FALSE (activated);
        ASSERT_TRUE (activated.error ());
        EXPECT_EQ (activated.error ()->kind (), fw::framework_error_kind_t::shutting_down);
        const auto candidate = runtime::host::public_host_runtime_t::remote_actor_ref (
          rid, actor_id, target.object_generation);
        EXPECT_FALSE (host->resolve_actor_async (candidate).result ().value ());
    }
}
} // namespace zlink::framework::test::actor_maintenance_join
