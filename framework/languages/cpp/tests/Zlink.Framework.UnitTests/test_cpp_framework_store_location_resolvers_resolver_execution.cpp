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
#include "../support/store_location_resolver_user_spot_fixture.hpp"

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

/* The descriptor owner order checks above rely on this observation: only the
 * observed flow's reads count, and a wrong order or a read from another flow
 * leaves the check false. */
TEST (ZLinkFrameworkStoreLocationResolvers, OwnerReadOrderObservationCountsOnlyTheObservedFlow)
{
    namespace fw = zlink::framework;
    using fw::runtime::flow_context_t;
    using fw::runtime::flow_id_t;
    using fw::runtime::flow_value_t;
    using fw::tests::owner_lease_time_store_t;

    in_memory_location_store_t inner;
    owner_lease_time_store_t store (inner, "observed-owner",
                                    owner_lease_time_store_t::lease_view_t::live);
    const fw::store_key_t owner_key{std::string ("owner-lease") + '\0' + "observed-owner"};
    const fw::store_key_t authority_key{"zla1:s:13:observed-spot"};
    const auto enter = [] (const std::string &flow_id) {
        return flow_context_t::scope_t (flow_value_t{flow_id, fw::flow_origin_t::application,
                                                     fw::message_flow_log_mode_t::normal,
                                                     std::nullopt});
    };
    const auto read = [&] (const fw::store_key_t &key) { (void) store.read (key).result (); };

    // Expected order: one authority read, then the owner read.
    auto observed = flow_id_t::create ();
    store.observe_owner_read_order ("observed-spot", 1, observed);
    {
        auto scope = enter (observed);
        read (authority_key);
        read (owner_key);
    }
    EXPECT_TRUE (store.owner_was_read_in_expected_order ());

    // Wrong order: the owner is read before the authority.
    observed = flow_id_t::create ();
    store.observe_owner_read_order ("observed-spot", 1, observed);
    {
        auto scope = enter (observed);
        read (owner_key);
        read (authority_key);
    }
    EXPECT_FALSE (store.owner_was_read_in_expected_order ());

    // Reads of another flow and reads without a flow are not counted.
    observed = flow_id_t::create ();
    store.observe_owner_read_order ("observed-spot", 0, observed);
    {
        auto scope = enter (flow_id_t::create ());
        read (owner_key);
    }
    read (owner_key);
    EXPECT_FALSE (store.owner_was_read_in_expected_order ());
    {
        auto scope = enter (observed);
        read (owner_key);
    }
    EXPECT_TRUE (store.owner_was_read_in_expected_order ());
}

// The provider holds each handler's authority read until the test has checked that
// both executor workers can run another continuation.
constexpr std::array<std::string_view, 2> yielding_spot_ids{"yielding-spot-0", "yielding-spot-1"};

class held_user_spot_store_t final : public zlink::framework::location_store_t
{
  public:
    using completion_t =
      zlink::framework::task_completion_source_t<zlink::framework::store_read_result_t>;
    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        if (std::none_of (yielding_spot_ids.begin (), yielding_spot_ids.end (),
                          [&] (auto id) { return key.value.ends_with (id); }))
            return inner.read (std::move (key));
        auto completion = std::make_shared<completion_t> ();
        auto task = completion->task ();
        {
            std::lock_guard lock (mutex);
            held.push_back (completion);
        }
        entered.notify_all ();
        return task;
    }
    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }
    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    bool wait_for_handlers ()
    {
        std::unique_lock lock (mutex);
        return entered.wait_for (lock, std::chrono::seconds (2), [&] { return held.size () == 2; });
    }
    void release ()
    {
        std::vector<std::shared_ptr<completion_t>> completions;
        {
            std::lock_guard lock (mutex);
            completions.swap (held);
        }
        for (const auto &completion : completions)
            completion->complete (
              zlink::framework::result_t<zlink::framework::store_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "held Store authority read released"));
    }
    zlink::framework::runtime::in_memory_location_store_t inner;
    std::mutex mutex;
    std::condition_variable entered;
    std::vector<std::shared_ptr<completion_t>> held;
};

class yielding_user_spot_client_t final : public zlink::framework::hosted_service_t
{
  public:
    yielding_user_spot_client_t (zlink::framework::app_t &app, held_user_spot_store_t &store) :
        app (app), store (store)
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        namespace fw = zlink::framework;
        auto &manager = services.get_required<fw::spot_manager_t> ();
        fw::runtime::install_host_context_hooks ();
        std::promise<void> probe;
        auto probed = probe.get_future ();
        std::promise<void> completed;
        auto done = completed.get_future ();
        std::atomic_int remaining{2};
        for (const auto spot_id : yielding_spot_ids) {
            fw::runtime::handler_coroutine_executor ().post_native_continuation (
              [&manager, &remaining, &completed, spot_id] {
                  auto pending = manager.get_or_create (fw::spot_id_t (spot_id), "room").async ();
                  fw::detail::observe_task_completion (
                    pending,
                    [&remaining, &completed] (const fw::result_t<fw::spot_create_result_t> &) {
                        if (--remaining == 0)
                            completed.set_value ();
                    });
              });
        }
        both_handlers_entered = store.wait_for_handlers ();
        fw::runtime::handler_coroutine_executor ().post_native_continuation (
          [&probe] { probe.set_value (); });
        worker_available =
          probed.wait_for (std::chrono::milliseconds (300)) == std::future_status::ready;
        store.release ();
        done.wait ();
        probed.wait ();
        app.stop ();
        co_return;
    }
    void stop () noexcept override {}
    bool both_handlers_entered = false;
    bool worker_available = false;
    zlink::framework::app_t &app;
    held_user_spot_store_t &store;
};

TEST (ZLinkFrameworkStoreLocationResolvers,
      UserSpotCreationOnEveryHandlerWorkerYieldsDuringStoreLookup)
{
    namespace fw = zlink::framework;
    fw::runtime::configure_handler_coroutine_executor (2);
    auto store = std::make_shared<held_user_spot_store_t> ();
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        options.handler_coroutine_workers (2);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        options.add_route_mesh ("yielding-spot-mesh")
          .set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("yielding-spot-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<local_user_spot_t> (
            "room",
            [] (fw::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<yielding_user_spot_client_t> (app, *store);
    auto *client = service.get ();
    app.add_hosted_service (std::move (service));
    EXPECT_EQ (0, app.run (0, nullptr));
    EXPECT_TRUE (client->both_handlers_entered);
    EXPECT_TRUE (client->worker_available);
    fw::runtime::shutdown_handler_coroutine_executor ();
}

zlink::framework::task_t<void> resolve_on_handler_executor (store_location_resolvers_t &resolvers,
                                                            std::atomic_int &resolved,
                                                            std::atomic_int &remaining,
                                                            std::promise<void> &all_resolved)
{
    try {
        const auto address = co_await resolvers.resolve_spot_address ({}, "spot-1");
        if (address && address->node_rid.to_string () == "node-1")
            ++resolved;
    }
    catch (...) {
    }
    if (--remaining == 0)
        all_resolved.set_value ();
}

/* Execution gate §12: a handler that awaits a Spot request yields its
 * executor worker, so the Location Store read that completes the request
 * still progresses when every worker runs such a handler. Before #1380 the
 * resolver read the authority with blocking waits whose continuations were
 * queued behind those same workers. */
TEST (ZLinkFrameworkStoreLocationResolvers,
      SpotResolutionOnEveryHandlerWorkerDoesNotBlockStoreContinuations)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    constexpr std::size_t workers = 2;
    constexpr int requests = 8;

    fw::runtime::in_memory_location_store_t provider;
    fw::runtime::provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim = repository.claim_owner_lease ("owner-1", 30s).result ().value ();
    const auto *claimed = std::get_if<fw::owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "mesh-1";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-1"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::user_spot, "room",
                                               fw::maintenance_policy_kind_t::snapshot, true, 10});
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::user_spot, "spot-1"};
    request.intent.stable_type = "room";
    request.target = {"mesh-1", fw::node_rid_t::from_string ("node-1"), 1, claimed->token};
    request.creating_payload = {std::byte{0}};
    request.capacity_bundle.spot_slots = 1;
    request.capacity_bundle.spot_type =
      fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::user_spot, "room", 1};
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<fw::object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    const auto committed =
      repository
        .commit ({request.key, reservation->fence, user_spot_authority_payload ("spot-1", 1)})
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (committed));

    location_options_t options;
    options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t resolvers (repository, options);

    fw::runtime::configure_handler_coroutine_executor (workers);
    fw::runtime::install_host_context_hooks ();
    std::atomic_int resolved{0};
    std::atomic_int remaining{requests};
    std::promise<void> all_resolved;
    auto finished = all_resolved.get_future ();
    for (int index = 0; index < requests; ++index) {
        fw::runtime::handler_coroutine_executor ().post_native_continuation ([&] {
            (void) resolve_on_handler_executor (resolvers, resolved, remaining, all_resolved);
        });
    }
    if (finished.wait_for (10s) != std::future_status::ready) {
        // Every handler worker is parked in a blocking wait; the executor
        // cannot be drained, so end the process instead of hanging teardown.
        std::fprintf (stderr, "Spot resolution deadlocked the handler executor (%d/%d)\n",
                      resolved.load (), requests);
        std::fflush (stderr);
        std::_Exit (EXIT_FAILURE);
    }
    fw::runtime::shutdown_handler_coroutine_executor ();
    EXPECT_EQ (requests, resolved.load ());
}
} // namespace
