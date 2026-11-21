/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include "../support/relocation_seal_execution_fixture.hpp"
#include "../support/relocation_seal_native_records.hpp"

using namespace framework_relocation_seal_fixture;

namespace zlink::framework::runtime
{
struct in_memory_location_repository_test_access_t
{
    static std::promise<void> hold_lane (in_memory_location_repository_t &store)
    {
        std::promise<void> release, entered;
        auto held = release.get_future ().share ();
        auto running = entered.get_future ();
        if (!store._lane.try_post ([held, &entered] {
                entered.set_value ();
                held.wait ();
            }))
            throw std::runtime_error ("store lane is closed");
        running.wait ();
        return release;
    }
};
} // namespace zlink::framework::runtime

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, source_store_reads_return_before_store_lane_completes)
{
    runtime::in_memory_location_repository_t store;
    const auto verify = [&] (auto read) {
        auto release = runtime::in_memory_location_repository_test_access_t::hold_lane (store);
        auto returned = std::async (std::launch::async, read);
        EXPECT_EQ (returned.wait_for (std::chrono::milliseconds (100)), std::future_status::ready);
        release.set_value ();
        auto completed = returned.get ();
        EXPECT_TRUE (completed.result ());
    };
    verify ([&] { return store.list_mesh_nodes ("mesh"); });
    verify ([&] { return store.read_owner_lease ("owner"); });
}

TEST (FrameworkRelocationSealContract, source_lookup_is_shared_by_claim_and_cut)
{
    struct deferred_store_t : runtime::in_memory_location_repository_t
    {
        int reads = 0;
        task_completion_source_t<location_page_t<mesh_node_descriptor_t>> listed;
        task_t<location_page_t<mesh_node_descriptor_t>>
        list_mesh_nodes_core (std::string, location_page_request_t = {}) override
        {
            ++reads;
            return listed.task ();
        }
    } store;
    const auto source_owner =
      std::get<owner_lease_claimed_t> (
        store.claim_owner_lease ("source-owner", std::chrono::seconds (15)).result ().value ())
        .token;
    runtime::live_location_reader_t reader (store);
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("source-lookup"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const auto local =
      std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3});
    host->configure_stateful_dispatch ([local] { return local; }, &reader);
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    auto &mailbox = host->transport ().mailbox ();
    auto original = spot_send (*host, spot, 1, 2);
    const auto key = original.owner;
    ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    auto claim =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, key, 1, 4096);
    ASSERT_TRUE (claim);
    auto prepared = mailbox.prepare ({claim, 0}, [&] (auto &record) {
        return stateful::claimed_ingress_t::prepare (record, &reader, "mesh");
    });
    ASSERT_TRUE (prepared);
    EXPECT_EQ (store.reads, 0);
    stateful::accepted_record_authority_query_t query;
    query.target = spot;
    query.source_node_routing_id = {2};
    query.source_node_generation = 1;
    query.source_original = prepared->source_original;
    const auto descriptor = host->transport ().topology ().local_descriptor ();
    query.target_node_routing_id = descriptor.node_routing_id;
    query.target_node_generation = descriptor.lifecycle_generation;
    auto first = host->accepted_record_authority (query, local, &reader);
    EXPECT_EQ (store.reads, 1);
    EXPECT_FALSE (first.await_ready ());
    auto cut = mailbox.cut_owners ({key}, {}, [] {});
    ASSERT_TRUE (cut);
    ASSERT_EQ ((*cut)[0].size (), 1u);
    auto cut_query = stateful::claimed_ingress_t::prepare ((*cut)[0][0], &reader, "mesh");
    query.source_original = cut_query.source_original;
    auto second = host->accepted_record_authority (query, local, &reader);
    EXPECT_EQ (store.reads, 1);
    EXPECT_FALSE (second.await_ready ());
    location_page_t<mesh_node_descriptor_t> page;
    page.items.push_back ({.mesh_name = "mesh",
                           .rid = zlink::routing_id_t::from (std::vector<std::uint8_t>{2}),
                           .lifecycle_generation = 1,
                           .descriptor_revision = 1,
                           .endpoint = "tcp://127.0.0.1:1",
                           .state = framework_runtime_state_t::serving,
                           .security_identity = "test",
                           .owner_id = source_owner.owner_id,
                           .lease_generation = source_owner.lease_generation});
    store.listed.complete (
      result_t<location_page_t<mesh_node_descriptor_t>>::success (std::move (page)));
    const auto accepted = first.result ().value ();
    const auto captured = second.result ().value ();
    ASSERT_TRUE (accepted);
    ASSERT_TRUE (captured);
    EXPECT_EQ (accepted->source, captured->source);
    EXPECT_EQ (captured->source.owner_id, source_owner.owner_id);
    EXPECT_EQ (captured->source.lease_generation,
               static_cast<std::uint64_t> (source_owner.lease_generation));
    EXPECT_EQ (store.reads, 1);
    if (const auto credit = (*cut)[0][0].transferred)
        EXPECT_TRUE (credit ());
    host->close ();
}

TEST (FrameworkRelocationSealContract, unavailable_source_read_keeps_unavailable_result)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("source-read-error"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    stateful::raw_stateful_dispatch_t dispatch (
      host->objects (), host->transport (), [] (const auto &) {
          return task_t<std::optional<stateful::accepted_record_authority_t>> (
            result_t<std::optional<stateful::accepted_record_authority_t>>::failure (
              framework_error_kind_t::unavailable, "source repository is unavailable"));
      });
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "actor", 1, 1, "mesh", "source"};
    protocol::actor_message_header_t header;
    header.target = {actor.key, 1, {1}, 1, 1, 3};
    header.operation = {1, 1};
    const auto [error, authority] =
      dispatch.accepted_actor_authority (actor, header, {2}, 1, std::nullopt, false, std::nullopt)
        .result ()
        .value ();
    EXPECT_EQ (error, stateful::stateful_error_t::unavailable);
    EXPECT_FALSE (authority);
    host->close ();
}
