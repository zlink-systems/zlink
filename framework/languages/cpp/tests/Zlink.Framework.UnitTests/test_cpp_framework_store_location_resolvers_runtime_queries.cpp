/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../support/store_location_resolver_repository_fixture.hpp"

namespace zlink::framework::runtime
{
struct store_location_runtime_query_test_access_t
{
    static std::size_t encoded_object_size (const location_object_entry_t &entry)
    {
        return store_location_runtime_query_t::encoded_object_size (entry);
    }

    static bool encoded_object_page_fits (std::size_t items_bytes,
                                          std::size_t item_count,
                                          std::optional<std::string_view> continuation_token)
    {
        return store_location_runtime_query_t::encoded_object_page_fits (items_bytes, item_count,
                                                                         continuation_token);
    }
};
} // namespace zlink::framework::runtime

namespace
{
using namespace zlink::framework::test::store_location_resolvers;

using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_object_entry_t;
using zlink::framework::location_object_filter_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_page_t;
using zlink::framework::location_role_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_resolvers_t;
using zlink::framework::runtime::store_location_runtime_query_t;

class other_test_location_repository_t : public test_location_repository_t
{
};

enum class owner_lease_confirmation_mode_t
{
    same_lease,
    missing,
    transport_failure
};

class renew_failure_owner_lease_store_t final : public test_location_repository_t
{
  public:
    explicit renew_failure_owner_lease_store_t (owner_lease_confirmation_mode_t confirmation_mode) :
        _confirmation_mode (confirmation_mode)
    {
    }

    std::size_t confirmation_read_count () const noexcept
    {
        return _confirmation_read_count.load (std::memory_order_relaxed);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t, std::chrono::milliseconds) override
    {
        return zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t> (
          zlink::framework::result_t<zlink::framework::owner_lease_renew_result_t>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "owner lease renewal failed"));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        _confirmation_read_count.fetch_add (1, std::memory_order_relaxed);
        if (_confirmation_mode == owner_lease_confirmation_mode_t::missing) {
            return zlink::framework::task_t<zlink::framework::owner_lease_read_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_read_result_t>::success (
                zlink::framework::owner_lease_read_result_t{
                  zlink::framework::owner_lease_missing_t{}}));
        }
        if (_confirmation_mode == owner_lease_confirmation_mode_t::transport_failure) {
            return zlink::framework::task_t<zlink::framework::owner_lease_read_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "owner lease confirmation read failed"));
        }
        return test_location_repository_t::read_owner_lease (std::move (owner_id));
    }

  private:
    owner_lease_confirmation_mode_t _confirmation_mode;
    std::atomic_size_t _confirmation_read_count{0};
};

class fake_location_runtime_query_t final : public location_runtime_query_t
{
  public:
    bool fail_lists = false;
    std::atomic_bool store_healthy{true};
    std::atomic_int status_calls{0};

    zlink::framework::task_t<location_runtime_status_t> get_status () override
    {
        ++status_calls;
        return completed (
          location_runtime_status_t{.store_healthy = store_healthy.load (),
                                    .watch_enabled = false,
                                    .polling_interval = std::chrono::milliseconds (10),
                                    .owner_lease_healthy = true});
    }

    zlink::framework::task_t<location_page_t<location_topology_entry_t>>
    list_topology (location_topology_filter_t, location_page_request_t = {}) override
    {
        if (fail_lists) {
            return failed<location_page_t<location_topology_entry_t>> ();
        }
        return completed (
          location_page_t<location_topology_entry_t>{.items = {location_topology_entry_t{
                                                       .mesh_name = "mesh-a",
                                                       .state = location_topology_state_t::ready,
                                                     }}});
    }

    zlink::framework::task_t<location_page_t<location_service_summary_t>>
    list_service_summaries (location_service_summary_filter_t,
                            location_page_request_t = {}) override
    {
        if (fail_lists) {
            return failed<location_page_t<location_service_summary_t>> ();
        }
        return completed (location_page_t<location_service_summary_t>{
          .items = {location_service_summary_t{
            .mesh_name = "mesh-a", .total_count = 1, .ready_count = 1}}});
    }

    zlink::framework::task_t<std::optional<location_object_entry_t>>
    find_actor_location (zlink::framework::actor_id_t) override
    {
        return completed (std::optional<location_object_entry_t>{});
    }

    zlink::framework::task_t<std::optional<location_object_entry_t>>
    find_spot_location (zlink::framework::spot_id_t) override
    {
        return completed (std::optional<location_object_entry_t>{});
    }

    zlink::framework::task_t<location_page_t<location_object_entry_t>>
    list_object_locations (location_object_filter_t, location_page_request_t = {}) override
    {
        return completed (location_page_t<location_object_entry_t>{});
    }

  private:
    template <typename T> static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }

    template <typename T> static zlink::framework::task_t<T> failed ()
    {
        return zlink::framework::task_t<T> (zlink::framework::result_t<T>::failure (
          zlink::framework::framework_error_kind_t::internal_failure,
          "location query list failed"));
    }
};
} // namespace

namespace
{
TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryReportsHealthyStoreStatus)
{
    in_memory_location_repository_t store;
    location_options_t options;
    options.polling_interval = std::chrono::milliseconds (125);
    location_runtime_t runtime (store, options, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    store_location_runtime_query_t query (store, runtime, options);

    const auto status = query.get_status ().result ().value ();

    EXPECT_TRUE (status.store_healthy);
    EXPECT_FALSE (status.watch_enabled);
    EXPECT_EQ (std::chrono::milliseconds (125), status.polling_interval);
    EXPECT_TRUE (status.owner_lease_healthy);
    EXPECT_TRUE (status.owner_lease_renewed_at.has_value ());
    runtime.stop ();
    EXPECT_TRUE (status.last_refresh_at.has_value ());
    EXPECT_FALSE (status.last_error.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      RuntimeQueryConvergesHealthyWhenRenewFailureConfirmsSameLease)
{
    renew_failure_owner_lease_store_t store (owner_lease_confirmation_mode_t::same_lease);
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    runtime.renew_owner_lease_once ();
    store_location_runtime_query_t query (store, runtime, options);

    const auto status = query.get_status ().result ().value ();

    EXPECT_TRUE (status.store_healthy);
    EXPECT_TRUE (status.owner_lease_healthy);
    EXPECT_FALSE (status.last_error.has_value ());
    EXPECT_EQ (1u, store.confirmation_read_count ());
    runtime.stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      RuntimeQueryReportsStoreFailureWhenRenewConfirmationFails)
{
    for (const auto mode : {owner_lease_confirmation_mode_t::missing,
                            owner_lease_confirmation_mode_t::transport_failure}) {
        SCOPED_TRACE (mode == owner_lease_confirmation_mode_t::missing
                        ? "confirmation missing"
                        : "confirmation transport failure");
        renew_failure_owner_lease_store_t store (mode);
        location_options_t options;
        location_runtime_t runtime (store, options, "owner-a");
        runtime.start (zlink::routing_id_t::from ("node-a"));
        runtime.renew_owner_lease_once ();
        store_location_runtime_query_t query (store, runtime, options);

        const auto status = query.get_status ().result ().value ();

        EXPECT_FALSE (status.store_healthy);
        EXPECT_FALSE (status.owner_lease_healthy);
        EXPECT_EQ (1u, store.confirmation_read_count ());
        ASSERT_TRUE (status.last_error.has_value ());
        EXPECT_NE (std::string::npos,
                   status.last_error->find (mode == owner_lease_confirmation_mode_t::missing
                                              ? "owner lease renewal failed"
                                              : "owner lease confirmation read failed"));
        runtime.stop ();
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryProjectsMeshNodeDescriptors)
{
    in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = claim_test_owner (store, "owner-a");
    for (const auto &[rid, state] :
         std::vector<std::pair<std::string, zlink::framework::framework_runtime_state_t>>{
           {"node-a", zlink::framework::framework_runtime_state_t::serving},
           {"node-b", zlink::framework::framework_runtime_state_t::draining}}) {
        ASSERT_EQ (location_write_status_t::stored,
                   store
                     .update_mesh_node (
                       zlink::framework::mesh_node_descriptor_t{
                         .mesh_name = "mesh-a",
                         .rid = zlink::routing_id_t::from (rid),
                         .lifecycle_generation = 1,
                         .descriptor_revision = 1,
                         .endpoint = "tcp://127.0.0.1:7001",
                         .application_version = 1,
                         .object_role = zlink::framework::object_role_t::server,
                         .capacity = {.actors = {.limit = 128}, .spots = {.limit = 128}},
                         .activation_concurrency = {.limit = 128},
                         .state = state,
                         .security_identity = "test",
                         .owner_id = owner.owner_id,
                         .lease_generation = owner.lease_generation},
                       location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
    }
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (store, runtime, options);

    const auto topology = query.list_topology ({.mesh_name = "mesh-a"}).result ().value ();
    ASSERT_EQ (2u, topology.items.size ());
    EXPECT_TRUE (std::any_of (topology.items.begin (), topology.items.end (),
                              [] (const auto &entry) { return entry.draining; }));
    const auto summaries =
      query.list_service_summaries ({.mesh_name = "mesh-a"}).result ().value ();
    ASSERT_EQ (1u, summaries.items.size ());
    EXPECT_EQ (2u, summaries.items.front ().total_count);
    EXPECT_EQ (2u, summaries.items.front ().ready_count);
}

// Location runtime §7.4: a descriptor whose owner lease is gone still counts,
// as stopped, even though its stored state says serving.
TEST (ZLinkFrameworkStoreLocationResolvers, ServiceSummaryCountsReleasedOwnerAsStopped)
{
    in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto live_owner = claim_test_owner (store, "owner-live");
    const auto released_owner = claim_test_owner (store, "owner-released");
    for (const auto &[rid, owner] :
         std::vector<std::pair<std::string, zlink::framework::location_owner_token_t>>{
           {"node-live", live_owner}, {"node-released", released_owner}}) {
        ASSERT_EQ (location_write_status_t::stored,
                   store
                     .update_mesh_node (
                       zlink::framework::mesh_node_descriptor_t{
                         .mesh_name = "mesh-a",
                         .rid = zlink::routing_id_t::from (rid),
                         .lifecycle_generation = 1,
                         .descriptor_revision = 1,
                         .endpoint = "tcp://127.0.0.1:7001",
                         .application_version = 1,
                         .object_role = zlink::framework::object_role_t::server,
                         .capacity = {.actors = {.limit = 128}, .spots = {.limit = 128}},
                         .activation_concurrency = {.limit = 128},
                         .state = zlink::framework::framework_runtime_state_t::serving,
                         .security_identity = "test",
                         .owner_id = owner.owner_id,
                         .lease_generation = owner.lease_generation},
                       location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
    }
    (void) store.release_owner_lease (released_owner).result ();
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (store, runtime, options);

    const auto summaries =
      query.list_service_summaries ({.mesh_name = "mesh-a"}).result ().value ();
    ASSERT_EQ (1u, summaries.items.size ());
    EXPECT_EQ (2u, summaries.items.front ().total_count);
    EXPECT_EQ (1u, summaries.items.front ().ready_count);
    EXPECT_EQ (1u, summaries.items.front ().stopped_count);
    EXPECT_EQ (0u, summaries.items.front ().error_count);
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryProjectsExactAndPagedObjects)
{
    using namespace zlink::framework;
    in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = claim_test_owner (store, "owner-objects");
    const auto unavailable_owner = claim_test_owner (store, "owner-unavailable");
    const auto publish_node = [&] (const location_owner_token_t &token, const std::string &rid) {
        const auto written =
          store
            .update_mesh_node (
              mesh_node_descriptor_t{
                .mesh_name = "object-mesh",
                .rid = zlink::routing_id_t::from (rid),
                .lifecycle_generation = 1,
                .descriptor_revision = 1,
                .endpoint = "tcp://127.0.0.1:7001",
                .application_version = 1,
                .object_capabilities = {{placement_object_kind_t::actor, "player",
                                         maintenance_policy_kind_t::disabled, false, 0},
                                        {placement_object_kind_t::user_spot, "room",
                                         maintenance_policy_kind_t::disabled, false, 0},
                                        {placement_object_kind_t::instance_spot, "lobby",
                                         maintenance_policy_kind_t::disabled, false, 0}},
                .object_role = object_role_t::server,
                .capacity =
                  {.actors = {.limit = 8},
                   .spots = {.limit = 8},
                   .spot_types = {{placement_object_kind_t::user_spot, "room", {.limit = 8}},
                                  {placement_object_kind_t::instance_spot, "lobby", {.limit = 8}}}},
                .activation_concurrency = {.limit = 8},
                .state = framework_runtime_state_t::serving,
                .security_identity = "test",
                .owner_id = token.owner_id,
                .lease_generation = token.lease_generation},
              location_write_intent_t::new_claim)
            .result ()
            .value ();
        ASSERT_EQ (location_write_status_t::stored, written.status);
    };
    publish_node (owner, "node-objects");
    publish_node (unavailable_owner, "node-unavailable");

    const auto reserve = [&] (placement_object_kind_t kind, std::string id, std::string stable_type,
                              const location_owner_token_t &token, std::string rid, bool commit) {
        object_reserve_request_t request{
          .key = {kind, std::move (id)},
          .intent = {.stable_type = stable_type},
          .target = {.mesh_name = "object-mesh",
                     .node_rid = node_rid_t::from_string (std::move (rid)),
                     .node_lifecycle_generation = 1,
                     .owner = token},
          .creating_payload = {std::byte{0x01}},
          .capacity_bundle = {.actor_slots = kind == placement_object_kind_t::actor ? 1u : 0u,
                              .spot_slots = kind == placement_object_kind_t::actor ? 0u : 1u,
                              .spot_type = kind == placement_object_kind_t::actor
                                             ? std::nullopt
                                             : std::optional<spot_type_capacity_delta_t>{
                                                 {kind, std::move (stable_type), 1}}}};
        const auto reserved = store.reserve (request).result ().value ();
        const auto *value = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (nullptr, value);
        if (commit && value != nullptr) {
            const auto committed =
              store.commit ({request.key, value->fence, {std::byte{0x02}}}).result ().value ();
            EXPECT_NE (nullptr, std::get_if<object_committed_t> (&committed));
        }
    };
    reserve (placement_object_kind_t::actor, "actor-a", "player", owner, "node-objects", true);
    reserve (placement_object_kind_t::actor, "actor-b", "player", owner, "node-objects", true);
    reserve (placement_object_kind_t::user_spot, "spot-a", "room", owner, "node-objects", false);
    reserve (placement_object_kind_t::instance_spot, "instance-a", "lobby", unavailable_owner,
             "node-unavailable", true);

    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (store, runtime, options);

    const auto actor = query.find_actor_location (actor_id_t ("actor-a")).result ().value ();
    ASSERT_TRUE (actor.has_value ());
    EXPECT_EQ ("actor-a", actor->global_id);
    EXPECT_EQ ("object-mesh", actor->mesh_name);
    EXPECT_EQ ("node-objects", actor->node_rid.to_string ());
    EXPECT_EQ ("player", actor->stable_type);
    EXPECT_EQ (location_object_state_t::ready, actor->state);
    EXPECT_FALSE (
      query.find_actor_location (actor_id_t ("missing")).result ().value ().has_value ());

    const auto spot = query.find_spot_location ("spot-a").result ().value ();
    ASSERT_TRUE (spot.has_value ());
    EXPECT_EQ (location_object_state_t::creating, spot->state);

    const auto first =
      query.list_object_locations ({.object_kind = location_object_kind_t::actor}, {.page_size = 1})
        .result ()
        .value ();
    ASSERT_EQ (1u, first.items.size ());
    ASSERT_TRUE (first.continuation_token.has_value ());
    const auto second =
      query
        .list_object_locations ({.object_kind = location_object_kind_t::actor},
                                {.page_size = 1, .continuation_token = first.continuation_token})
        .result ()
        .value ();
    ASSERT_EQ (1u, second.items.size ());
    EXPECT_FALSE (second.continuation_token.has_value ());

    const auto rooms = query
                         .list_object_locations ({.object_kind = location_object_kind_t::user_spot,
                                                  .stable_type = "room",
                                                  .mesh_name = "object-mesh"},
                                                 {.page_size = 1})
                         .result ()
                         .value ();
    ASSERT_EQ (1u, rooms.items.size ());
    EXPECT_EQ ("spot-a", rooms.items.front ().global_id);

    const auto released = store.release_owner_lease (unavailable_owner).result ().value ();
    ASSERT_NE (nullptr, std::get_if<owner_lease_released_t> (&released));
    const auto unavailable = query.find_spot_location ("instance-a").result ().value ();
    ASSERT_TRUE (unavailable.has_value ());
    EXPECT_EQ (location_object_state_t::unavailable, unavailable->state);

    EXPECT_THROW (query.list_object_locations ({.object_kind = location_object_kind_t::actor},
                                               {.page_size = 0}),
                  std::invalid_argument);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ObjectLocationPagesFollowSharedFixture)
{
    using namespace zlink::framework;
    auto root = std::filesystem::path (__FILE__).parent_path ();
    while (!std::filesystem::exists (
      root / "framework/runtime/conformance/object-location-pages-v1.json")) {
        ASSERT_NE (root, root.parent_path ());
        root = root.parent_path ();
    }
    std::ifstream input (root / "framework/runtime/conformance/object-location-pages-v1.json");
    const auto fixture = nlohmann::json::parse (input);
    const std::string escaped_utf8_probe = "é\\\"\b\t\n\f\r\x01";
    EXPECT_EQ (nlohmann::json (escaped_utf8_probe).dump ().size (),
               escaped_utf8_probe.size () + 2 + 2 + 5 + 5);
    class fixture_store_t final : public test_location_repository_t
    {
      public:
        std::vector<authority_entry_t> entries;
        std::size_t store_page_size;
        std::size_t scan_calls = 0;
        task_t<authority_scan_result_t>
        list_authorities (std::string prefix,
                          std::optional<authority_scan_cursor_t> cursor,
                          std::size_t limit,
                          std::stop_token = {}) override
        {
            ++scan_calls;
            EXPECT_TRUE (prefix == "zla1:a:" || prefix == "zla1:s:");
            std::vector<authority_entry_t> rows;
            for (const auto &entry : entries)
                if (entry.key.value.starts_with (prefix))
                    rows.push_back (entry);
            const auto offset = cursor ? std::stoul (std::string (cursor->encoded ())) : 0u;
            const auto end = std::min (offset + std::min (limit, store_page_size), rows.size ());
            authority_page_t page;
            page.items.assign (rows.begin () + offset, rows.begin () + end);
            if (end < rows.size ())
                page.next_cursor.emplace (std::to_string (end));
            co_return authority_scan_result_t{std::move (page)};
        }
    };
    const zlink::framework::location_object_entry_t encoding_probe{
      .global_id = "actor-é\"\\\b\t\n\f\r\x01",
      .object_generation = 1,
      .mesh_name = "mesh-é\"\\\b\t\n\f\r\x01",
      .node_rid = zlink::routing_id_t::from (std::string ("node-1")),
      .state = zlink::framework::location_object_state_t::ready,
      .stable_type = "type-é\"\\\b\t\n\f\r\x01"};
    const auto encoded_probe =
      nlohmann::json{{"globalId", encoding_probe.global_id},
                     {"objectGeneration", encoding_probe.object_generation},
                     {"meshName", encoding_probe.mesh_name},
                     {"nodeRid", encoding_probe.node_rid.to_string ()},
                     {"state", "ready"},
                     {"stableType", encoding_probe.stable_type}}
        .dump ();
    EXPECT_EQ (
      encoded_probe.size (),
      zlink::framework::runtime::store_location_runtime_query_test_access_t::encoded_object_size (
        encoding_probe));
    for (const auto &scenario : fixture.at ("cases")) {
        [&] {
            SCOPED_TRACE (scenario.at ("name").get<std::string> ());
            fixture_store_t store;
            store.store_page_size = scenario.at ("storePageSize").get<std::size_t> ();
            std::map<std::string, std::string> ids;
            for (const auto &row :
                 fixture.at ("datasets").at (scenario.at ("dataset").get<std::string> ())) {
                const auto &profile =
                  fixture.at ("profiles").at (row.at ("profile").get<std::string> ());
                const auto id = row.at ("id").get<std::string> ();
                const auto global_id = profile.at ("idPrefix").get<std::string> ()
                                       + row.at ("globalIdSuffix").get<std::string> ();
                ids.emplace (id, global_id);
                const auto kind = row.at ("kind").get<std::string> ();
                authority_snapshot_t snapshot;
                snapshot.store_version = "v1";
                snapshot.object_generation = 1;
                snapshot.authority_owner_generation = 1;
                snapshot.owner = {"owner", 1};
                snapshot.allocation.state = placement_allocation_state_t::active;
                snapshot.allocation.object_kind = kind == "actor" ? placement_object_kind_t::actor
                                                  : kind == "user_spot"
                                                    ? placement_object_kind_t::user_spot
                                                    : placement_object_kind_t::instance_spot;
                snapshot.allocation.stable_type = profile.at ("stableType").get<std::string> ();
                snapshot.allocation.target.mesh_name = profile.at ("meshName").get<std::string> ();
                snapshot.allocation.target.node_rid =
                  node_rid_t::from_string (profile.at ("nodeRid").get<std::string> ());
                store.entries.push_back (
                  {kind == "actor" ? zlink::framework::runtime::actor_authority_key (global_id)
                                   : zlink::framework::runtime::spot_authority_key (global_id),
                   std::move (snapshot)});
            }
            location_options_t options;
            location_runtime_t runtime (store, options, "owner-query");
            store_location_runtime_query_t query (store, runtime, options);
            const auto kind = scenario.at ("objectKind").get<std::string> ();
            const location_object_filter_t filter{
              .object_kind = kind == "actor"       ? location_object_kind_t::actor
                             : kind == "user_spot" ? location_object_kind_t::user_spot
                                                   : location_object_kind_t::instance_spot};
            std::optional<std::string> cursor;
            std::vector<std::string> all;
            std::optional<std::vector<location_object_entry_t>> previous_boundary_items;
            std::optional<std::string> previous_boundary_cursor;
            for (const auto &expected : scenario.at ("expectedPages")) {
                auto result = query
                                .list_object_locations (
                                  filter, {.page_size = scenario.at ("pageSize").get<int> (),
                                           .continuation_token = cursor})
                                .result ();
                ASSERT_TRUE (result.has_value ())
                  << (result.error () ? result.error ()->what () : "no Framework error");
                const auto &page = result.value ();
                const auto encoded_page = [] (const auto &items, const auto &token) {
                    nlohmann::json encoded_items = nlohmann::json::array ();
                    std::size_t encoded_items_bytes = 0;
                    for (const auto &entry : items) {
                        const auto state =
                          entry.state == location_object_state_t::creating ? "creating"
                          : entry.state == location_object_state_t::ready  ? "ready"
                                                                           : "unavailable";
                        nlohmann::json encoded_entry = {
                          {"globalId", entry.global_id},
                          {"objectGeneration", entry.object_generation},
                          {"meshName", entry.mesh_name},
                          {"nodeRid", entry.node_rid.to_string ()},
                          {"state", state},
                          {"stableType", entry.stable_type}};
                        EXPECT_EQ (
                          encoded_entry.dump ().size (),
                          zlink::framework::runtime::store_location_runtime_query_test_access_t::
                            encoded_object_size (entry));
                        encoded_items_bytes += zlink::framework::runtime::
                          store_location_runtime_query_test_access_t::encoded_object_size (entry);
                        encoded_items.push_back (std::move (encoded_entry));
                    }
                    const auto encoded_token =
                      token ? nlohmann::json (*token) : nlohmann::json (nullptr);
                    const auto encoded = nlohmann::json{
                      {"items", std::move (encoded_items)},
                      {"continuationToken",
                       encoded_token}}.dump ();
                    const auto token_view =
                      token ? std::optional<std::string_view> (*token) : std::nullopt;
                    const auto boundary_items_bytes =
                      encoded_items_bytes + 4u * 1024u * 1024u - encoded.size ();
                    using access =
                      zlink::framework::runtime::store_location_runtime_query_test_access_t;
                    EXPECT_TRUE (access::encoded_object_page_fits (boundary_items_bytes,
                                                                   items.size (), token_view));
                    EXPECT_FALSE (access::encoded_object_page_fits (boundary_items_bytes + 1,
                                                                    items.size (), token_view));
                    return encoded;
                };
                EXPECT_LE (encoded_page (page.items, page.continuation_token).size (),
                           4u * 1024u * 1024u);
                if (previous_boundary_items && scenario.at ("pageSize").get<int> () == 1000
                    && !page.items.empty () && previous_boundary_items->size () < 1000) {
                    auto candidate_items = *previous_boundary_items;
                    candidate_items.push_back (page.items.front ());
                    EXPECT_GT (encoded_page (candidate_items, previous_boundary_cursor).size (),
                               4u * 1024u * 1024u);
                }
                previous_boundary_items = page.items;
                previous_boundary_cursor = page.continuation_token;
                std::vector<std::string> actual_ids, expected_ids;
                for (const auto &entry : page.items)
                    actual_ids.push_back (entry.global_id);
                for (const auto &id : expected)
                    expected_ids.push_back (ids.at (id.get<std::string> ()));
                ASSERT_EQ (expected_ids.size (), actual_ids.size ());
                for (std::size_t index = 0; index < expected_ids.size (); ++index)
                    EXPECT_EQ (expected_ids[index], actual_ids[index]);
                all.insert (all.end (), actual_ids.begin (), actual_ids.end ());
                cursor = page.continuation_token;
            }
            EXPECT_FALSE (cursor);
            EXPECT_EQ (scenario.at ("expectedScanCalls").get<std::size_t> (), store.scan_calls);
            std::vector<std::string> expected_all;
            for (const auto &id : scenario.at ("expectedAll"))
                expected_all.push_back (ids.at (id.get<std::string> ()));
            EXPECT_EQ (expected_all, all);
            EXPECT_EQ (all.size (), std::set<std::string> (all.begin (), all.end ()).size ());
        }();
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, ObjectPageRechecksCursorForConsumedRows)
{
    using namespace zlink::framework;
    class cursor_fixture_store_t final : public test_location_repository_t
    {
      public:
        std::vector<authority_entry_t> entries;
        std::map<std::size_t, std::size_t> calls_by_offset;
        std::size_t requery_calls = 0;
        std::optional<std::size_t> first_row_requery_offset;

        task_t<authority_scan_result_t>
        list_authorities (std::string prefix,
                          std::optional<authority_scan_cursor_t> cursor,
                          std::size_t limit,
                          std::stop_token = {}) override
        {
            if (limit == 0)
                throw std::invalid_argument ("authority scan limit must be positive");
            const auto offset = cursor ? std::stoul (std::string (cursor->encoded ())) : 0u;
            const auto calls = calls_by_offset[offset]++;
            if (calls > 0)
                ++requery_calls;
            if (calls > 0 && limit == 1 && !first_row_requery_offset)
                first_row_requery_offset = offset;
            std::vector<authority_entry_t> rows;
            for (const auto &entry : entries)
                if (entry.key.value.starts_with (prefix))
                    rows.push_back (entry);
            const auto end = std::min (offset + std::min (limit, std::size_t{2}), rows.size ());
            authority_page_t page;
            page.items.assign (rows.begin () + offset, rows.begin () + end);
            if (end < rows.size ()) {
                auto next = std::to_string (end);
                if (calls > 0)
                    next += ":" + std::string (3000, 'c');
                page.next_cursor.emplace (std::move (next));
            }
            co_return authority_scan_result_t{std::move (page)};
        }
    };

    cursor_fixture_store_t store;
    std::vector<std::string> expected;
    const auto stable_type = std::string (4000, 'r');
    for (std::size_t index = 0; index < 1050; ++index) {
        auto id = "room-" + std::to_string (10000 + index) + "-" + std::string (240, '%');
        expected.push_back (id);
        authority_snapshot_t snapshot;
        snapshot.store_version = "v1";
        snapshot.object_generation = 1;
        snapshot.authority_owner_generation = 1;
        snapshot.owner = {"owner", 1};
        snapshot.allocation.state = placement_allocation_state_t::active;
        snapshot.allocation.object_kind = placement_object_kind_t::user_spot;
        snapshot.allocation.stable_type = stable_type;
        snapshot.allocation.target.mesh_name = "play";
        snapshot.allocation.target.node_rid = node_rid_t::from_string ("node-1");
        store.entries.push_back (
          {zlink::framework::runtime::spot_authority_key (id), std::move (snapshot)});
    }
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (store, runtime, options);
    std::vector<std::string> actual;
    std::optional<std::string> cursor;
    do {
        auto result = query
                        .list_object_locations ({.object_kind = location_object_kind_t::user_spot},
                                                {.page_size = 1000, .continuation_token = cursor})
                        .result ();
        ASSERT_TRUE (result.has_value ());
        const auto &page = result.value ();
        if (store.first_row_requery_offset && cursor
            && std::stoul (*cursor) == *store.first_row_requery_offset) {
            ASSERT_FALSE (page.items.empty ());
            EXPECT_EQ (expected[*store.first_row_requery_offset], page.items.front ().global_id);
        }
        nlohmann::json items = nlohmann::json::array ();
        for (const auto &entry : page.items)
            items.push_back (
              {{"globalId", entry.global_id},
               {"objectGeneration", entry.object_generation},
               {"meshName", entry.mesh_name},
               {"nodeRid", entry.node_rid.to_string ()},
               {"state", entry.state == location_object_state_t::ready ? "ready" : "unavailable"},
               {"stableType", entry.stable_type}});
        const auto encoded_token = page.continuation_token
                                     ? nlohmann::json (*page.continuation_token)
                                     : nlohmann::json (nullptr);
        const auto encoded = nlohmann::json{
          {"items", std::move (items)},
          {"continuationToken",
           encoded_token}}.dump ();
        ASSERT_LE (encoded.size (), 4u * 1024u * 1024u);
        for (const auto &entry : page.items)
            actual.push_back (entry.global_id);
        cursor = page.continuation_token;
    } while (cursor);
    EXPECT_TRUE (store.first_row_requery_offset.has_value ());
    EXPECT_GT (store.requery_calls, 0u);
    EXPECT_EQ (expected, actual);
    EXPECT_EQ (actual.size (), std::set<std::string> (actual.begin (), actual.end ()).size ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeObjectQueryMapsStoreFailureToUnavailable)
{
    class fault_store_t final : public zlink::framework::location_store_t
    {
      public:
        zlink::framework::task_t<zlink::framework::store_read_result_t>
        read (zlink::framework::store_key_t key) override
        {
            if (fail)
                throw std::runtime_error ("provider read failed");
            return inner.read (std::move (key));
        }
        zlink::framework::task_t<zlink::framework::store_write_result_t>
        write (zlink::framework::store_write_request_t request) override
        {
            return inner.write (std::move (request));
        }
        zlink::framework::task_t<zlink::framework::store_scan_result_t>
        scan (zlink::framework::store_scan_request_t request) override
        {
            if (fail)
                throw std::runtime_error ("provider scan failed");
            return inner.scan (std::move (request));
        }
        bool fail = false;
        in_memory_location_store_t inner;
    } provider;
    zlink::framework::runtime::provider_location_repository_t store (provider);
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query-failure");
    store_location_runtime_query_t query (store, runtime, options);
    provider.fail = true;

    const auto exact =
      query.find_actor_location (zlink::framework::actor_id_t ("actor-a")).result ();
    ASSERT_FALSE (exact.has_value ());
    EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, exact.error_kind ());

    const auto page =
      query.list_object_locations ({.object_kind = zlink::framework::location_object_kind_t::actor})
        .result ();
    ASSERT_FALSE (page.has_value ());
    EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, page.error_kind ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeObjectQueryPreservesFrameworkFailure)
{
    test_location_repository_t store;
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-typed-query-failure");
    store_location_runtime_query_t query (store, runtime, options);
    store.fail_authority_queries = true;
    const auto exact =
      query.find_actor_location (zlink::framework::actor_id_t ("actor-a")).result ();
    ASSERT_FALSE (exact.has_value ());
    EXPECT_EQ (zlink::framework::framework_error_kind_t::internal_failure, exact.error_kind ());
    const auto page =
      query.list_object_locations ({.object_kind = zlink::framework::location_object_kind_t::actor})
        .result ();
    ASSERT_FALSE (page.has_value ());
    EXPECT_EQ (zlink::framework::framework_error_kind_t::internal_failure, page.error_kind ());
}
} // namespace
