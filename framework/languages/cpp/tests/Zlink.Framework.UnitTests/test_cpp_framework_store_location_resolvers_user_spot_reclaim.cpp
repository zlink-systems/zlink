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

class occupied_user_spot_t final : public actor_free_user_spot_contract_t
{
  public:
    explicit occupied_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
};

class failing_user_spot_t final : public actor_free_user_spot_contract_t
{
  public:
    explicit failing_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
        throw std::runtime_error ("intentional User Spot materialization failure");
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
};

class generated_user_spot_collision_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit generated_user_spot_collision_client_t (
      zlink::framework::app_t &app, std::shared_ptr<test_location_repository_t> store) :
        _app (&app), _store (std::move (store))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        try {
            auto &manager = services.get_required<zlink::framework::spot_manager_t> ();
            const auto collision = zlink::framework::spot_id_t ("user:spot-collision-node:1");
            auto occupied = manager.get_or_create (collision, "occupied")
                              .timeout (std::chrono::seconds (2))
                              .async ()
                              .result ();
            if (!occupied) {
                last_error = occupied.error () ? occupied.error ()->what ()
                                               : "Failed to create the collision identity";
                _app->stop ();
                co_return;
            }
            _store->force_reserve_conflict.store (true, std::memory_order_release);
            auto generated =
              manager.create ("room").timeout (std::chrono::seconds (2)).async ().result ();
            if (generated || !generated.error ()
                || generated.error ()->kind ()
                     != zlink::framework::framework_error_kind_t::already_exists) {
                last_error = "Generated User Spot collision was not rejected immediately";
                _app->stop ();
                co_return;
            }
            auto existing = manager.get_or_create (collision, "occupied")
                              .timeout (std::chrono::seconds (2))
                              .async ()
                              .result ();
            if (!existing
                || existing.value ().state != zlink::framework::spot_create_state_t::existing
                || existing.value ().spot.object_generation ()
                     != occupied.value ().spot.object_generation ()) {
                last_error = "GetOrCreate did not preserve the caller RID incarnation";
                _app->stop ();
                co_return;
            }
            auto mismatch = manager.get_or_create (collision, "room")
                              .timeout (std::chrono::seconds (2))
                              .async ()
                              .result ();
            if (mismatch || !mismatch.error ()
                || mismatch.error ()->kind ()
                     != zlink::framework::framework_error_kind_t::type_mismatch) {
                last_error = "GetOrCreate changed caller RID type mismatch semantics";
                _app->stop ();
                co_return;
            }
            observed = true;
        }
        catch (const std::exception &error) {
            last_error = error.what ();
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
    std::shared_ptr<test_location_repository_t> _store;
};

class source_cleanup_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit source_cleanup_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto &manager = services.get_required<zlink::framework::spot_manager_t> ();
        const auto result =
          manager.get_or_create (zlink::framework::spot_id_t ("source-cleanup"), "failing")
            .timeout (std::chrono::seconds (2))
            .async ()
            .result ();
        try {
            result.value ();
            last_error = "User Spot creation unexpectedly succeeded";
        }
        catch (const std::runtime_error &error) {
            observed = typeid (error) == typeid (std::runtime_error)
                       && std::string_view (error.what ())
                            == "intentional User Spot materialization failure";
            if (!observed)
                last_error = error.what ();
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

class ended_user_request_entry_t final
    : public zlink::framework::entry_spot_t<zlink::framework::actor_t>
{
  public:
    explicit ended_user_request_entry_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::entry_spot_context_t _context;
};


class lease_target_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit lease_target_actor_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

  private:
    zlink::framework::actor_context_t _context;
};

class lease_target_actor_entry_t final : public zlink::framework::entry_spot_t<lease_target_actor_t>
{
  public:
    explicit lease_target_actor_entry_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}
    zlink::framework::task_t<void> on_actor_joined (lease_target_actor_t &) override { co_return; }
    zlink::framework::task_t<void> on_leave_actor (lease_target_actor_t &) override { co_return; }

  private:
    zlink::framework::entry_spot_context_t _context;
};

class lease_target_actor_factory_t final
    : public zlink::framework::actor_factory_t<lease_target_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<lease_target_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        ++calls;
        co_return std::make_shared<lease_target_actor_t> (std::move (context));
    }
    std::atomic<int> calls{0};
};

class actor_candidate_lease_store_t final : public zlink::framework::location_store_t
{
  public:
    enum class view_t
    {
        missing,
        expired,
        replaced,
        live,
        missing_expiry,
        read_failure
    };
    explicit actor_candidate_lease_store_t (view_t view) : view (view) {}

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        using namespace zlink::framework;
        const auto &flow = runtime::flow_context_t::current ();
        const bool observed = flow && flow->flow_id == flow_id;
        if (observed && key.value.find ("lease-new-actor") != std::string::npos)
            ++authority_reads;
        auto result = co_await inner.read (key);
        if (observed && key.value.starts_with (std::string ("owner-lease") + '\0')) {
            ++owner_reads;
            if (view == view_t::read_failure)
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "injected Actor candidate owner read failure");
            if (auto *found = std::get_if<store_found_t> (&result)) {
                // Candidate reads use a Store clock independent of the host clock.
                found->value.store_now =
                  std::chrono::system_clock::time_point (std::chrono::hours (24));
                found->value.expires_at = found->value.store_now + std::chrono::minutes (1);
                if (view == view_t::missing)
                    co_return store_read_result_t{store_missing_t{found->value.store_now}};
                if (view == view_t::expired)
                    found->value.expires_at = found->value.store_now;
                if (view == view_t::missing_expiry)
                    found->value.expires_at.reset ();
                if (view == view_t::replaced) {
                    auto record = nlohmann::json::parse (
                      reinterpret_cast<const char *> (found->value.bytes.data ()),
                      reinterpret_cast<const char *> (found->value.bytes.data ()
                                                      + found->value.bytes.size ()));
                    record["leaseGeneration"] = "9223372036854775807";
                    const auto encoded = record.dump ();
                    found->value.bytes.assign (
                      reinterpret_cast<const std::byte *> (encoded.data ()),
                      reinterpret_cast<const std::byte *> (encoded.data () + encoded.size ()));
                }
            }
        }
        co_return result;
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

    in_memory_location_store_t inner;
    const view_t view;
    const std::string flow_id = zlink::framework::runtime::flow_id_t::create ();
    std::atomic<int> owner_reads{0};
    std::atomic<int> authority_reads{0};
};

TEST (ZLinkFrameworkStoreLocationResolvers, ActorCandidatesRequireLiveDescriptorOwner)
{
    namespace fw = zlink::framework;
    using view_t = actor_candidate_lease_store_t::view_t;
    for (const auto view : {view_t::missing, view_t::expired, view_t::replaced, view_t::live,
                            view_t::missing_expiry, view_t::read_failure}) {
        SCOPED_TRACE (static_cast<int> (view));
        auto store = std::make_shared<actor_candidate_lease_store_t> (view);
        auto factory = std::make_shared<lease_target_actor_factory_t> ();
        auto app = fw::app_t::create ();
        app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
            options.configure_locations ().owner_lease_renew_interval =
              std::chrono::milliseconds (10);
            options.configure_locations ().owner_lease_fencing_margin =
              std::chrono::milliseconds (1);
            options.add_location_store (store);
            options.add_route_mesh ("actor-lease-mesh")
              .set_object_role (fw::object_role_t::server)
              .set_routing_id (zlink::routing_id_t::from ("only-actor-target"))
              .set_placement_weight (1)
              .listen ("tcp://127.0.0.1:0")
              .add_entry_spot<lease_target_actor_entry_t> ([] (fw::entry_spot_context_t context) {
                  return std::make_shared<lease_target_actor_entry_t> (std::move (context));
              })
              .add_actor_factory<lease_target_actor_t, lease_target_actor_factory_t> (
                "lease-actor", factory, [] (auto &options) { options.disable_relocation (); });
        });
        app.set_message_flow_mode (fw::message_flow_log_mode_t::normal);
        int app_result = -1;
        auto services = app.advanced ().services ().build_provider ();
        std::jthread host ([&] { app_result = app.run (0, nullptr); });
        bool created = false;
        std::optional<fw::framework_error_kind_t> error;
        try {
            fw::test::wait_for_serving (services);
            fw::runtime::flow_context_t::scope_t flow (
              fw::runtime::flow_value_t{store->flow_id, fw::flow_origin_t::application,
                                        fw::message_flow_log_mode_t::normal, std::nullopt});
            const auto result = services.get_required<fw::actor_manager_t> ()
                                  .create (fw::actor_id_t ("lease-new-actor"), "lease-actor")
                                  .in_mesh ("actor-lease-mesh")
                                  .timeout (std::chrono::seconds (2))
                                  .async ()
                                  .result ();
            created =
              result && std::holds_alternative<fw::actor_create_created_t> (result.value ());
            if (!result)
                error = result.error_kind ();
        }
        catch (const fw::framework_exception_t &failure) {
            error = failure.kind ();
        }
        app.stop ();
        host.join ();
        EXPECT_EQ (0, app_result);
        EXPECT_GT (store->owner_reads.load (), 0);
        if (view == view_t::live) {
            EXPECT_TRUE (created);
            EXPECT_FALSE (error.has_value ());
            EXPECT_EQ (1, factory->calls.load ());
        } else {
            EXPECT_FALSE (created);
            EXPECT_EQ (0, factory->calls.load ());
            // Exclusive creation has no authority fallback: no reservation or activation is reached.
            EXPECT_EQ (0, store->authority_reads.load ());
            EXPECT_EQ (std::optional<fw::framework_error_kind_t> (
                         view == view_t::missing_expiry
                           ? fw::framework_error_kind_t::internal_failure
                           : fw::framework_error_kind_t::unavailable),
                       error);
        }
    }
}

class ended_user_reclaim_client_t final : public zlink::framework::hosted_service_t
{
  public:
    ended_user_reclaim_client_t (zlink::framework::app_t &app,
                                 bool allowed,
                                 std::atomic_int &factories) :
        app (&app), allowed (allowed), factories (&factories)
    {
    }
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _worker = std::jthread ([this, services] () mutable {
            zlink::framework::test::wait_for_serving (services);
            execute (services).result ().value ();
        });
        co_return;
    }

    zlink::framework::task_t<void> execute (zlink::framework::service_provider_t &services)
    {
        namespace fw = zlink::framework;
        namespace rt = fw::runtime;
        auto &repository = services.get_required<fw::location_repository_t> ();
        fw::location_reclaim_test_access_t clock;
        clock.bind (repository);
        auto &manager = services.get_required<fw::spot_manager_t> ();
        const auto before = std::get<fw::authority_snapshot_t> (
          repository.read_authority (rt::spot_authority_key ("ended-user")).result ().value ());
        (void) manager.find (fw::spot_id_t ("ended-user")).result ();
        auto route = app->advanced ().zlink ().route_client (
          services.get_required<fw::serializer_registry_t> ());
        const auto ordinary =
          route.request_to_spot (fw::spot_id_t ("ended-user"), user_spot_delivery_probe_t{1})
            .timeout (std::chrono::seconds (3))
            .async<user_spot_delivery_probe_t> ()
            .result ();
        EXPECT_FALSE (ordinary);
        if (!ordinary)
            EXPECT_EQ (ordinary.error_kind (), fw::framework_error_kind_t::unavailable);
        const auto retained = std::get<fw::authority_snapshot_t> (
          repository.read_authority (rt::spot_authority_key ("ended-user")).result ().value ());
        EXPECT_EQ (before.store_version, retained.store_version);
        EXPECT_EQ (before.payload, retained.payload);
        EXPECT_EQ (factories->load (), 0);
        const auto waiting = manager.get_or_create (fw::spot_id_t ("ended-user"), "room")
                               .in_mesh ("factory-mesh")
                               .timeout (std::chrono::seconds (3))
                               .async ()
                               .result ();
        EXPECT_FALSE (waiting);
        if (!waiting)
            EXPECT_EQ (waiting.error_kind (), fw::framework_error_kind_t::unavailable);
        const auto during_wait = std::get<fw::authority_snapshot_t> (
          repository.read_authority (rt::spot_authority_key ("ended-user")).result ().value ());
        EXPECT_EQ (before.store_version, during_wait.store_version);
        EXPECT_EQ (factories->load (), 0);
        clock.advance (std::chrono::seconds (15));
        const auto created = manager.get_or_create (fw::spot_id_t ("ended-user"), "room")
                               .in_mesh ("factory-mesh")
                               .timeout (std::chrono::seconds (3))
                               .async ()
                               .result ();
        if (allowed) {
            EXPECT_TRUE (created) << (created.error () ? created.error ()->what () : "");
            if (created) {
                EXPECT_EQ (created.value ().spot.node_rid ().value (), "factory-node");
                EXPECT_GT (created.value ().spot.object_generation (), before.object_generation);
                const auto after = std::get<fw::authority_snapshot_t> (
                  repository.read_authority (rt::spot_authority_key ("ended-user"))
                    .result ()
                    .value ());
                EXPECT_GT (after.object_generation, before.object_generation);
                EXPECT_NE (after.payload, before.payload);
            }
            EXPECT_EQ (factories->load (), 1);
        } else {
            EXPECT_FALSE (created);
            EXPECT_EQ (created.error_kind (), fw::framework_error_kind_t::unavailable);
            const auto after = std::get<fw::authority_snapshot_t> (
              repository.read_authority (rt::spot_authority_key ("ended-user")).result ().value ());
            EXPECT_EQ (after.store_version, before.store_version);
            EXPECT_EQ (after.payload, before.payload);
            EXPECT_EQ (factories->load (), 0);
        }
        observed = true;
        app->stop ();
        co_return;
    }
    void stop () noexcept override {}
    bool observed = false;

  private:
    zlink::framework::app_t *app;
    bool allowed;
    std::atomic_int *factories;

  private:
    std::jthread _worker;
};

static void verify_ended_user_policy_source (bool provider, unsigned policy)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    using namespace std::chrono_literals;
    auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_repository_t> repository =
      provider ? std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::provider_location_repository_t> (*opaque))
               : std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::in_memory_location_repository_t> ());
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         repository->claim_owner_lease ("ended-user-old", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t old;
    old.mesh_name = "factory-mesh";
    old.rid = zlink::routing_id_t::from ("old-user-node");
    old.lifecycle_generation = 1;
    old.descriptor_revision = 1;
    old.endpoint = "tcp://127.0.0.1:7010";
    old.owner_id = owner.owner_id;
    old.lease_generation = owner.lease_generation;
    old.object_role = fw::object_role_t::server;
    old.state = fw::framework_runtime_state_t::serving;
    old.security_identity = "old-user-node";
    old.activation_concurrency.limit = 1;
    old.entry_spot_id = "old-entry";
    old.object_capabilities = {{fw::placement_object_kind_t::user_spot, "room",
                                fw::maintenance_policy_kind_t::disabled, false, 0}};
    old.capacity.spots.limit = 1;
    old.capacity.spot_types = {{fw::placement_object_kind_t::user_spot, "room", {1, 0, 0}}};
    ASSERT_EQ (repository->update_mesh_node (old, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    fw::object_reserve_request_t reserve;
    reserve.key = {fw::placement_object_kind_t::user_spot, "ended-user"};
    reserve.intent.stable_type = "room";
    reserve.target = {old.mesh_name, fw::node_rid_t::from_string ("old-user-node"), 1, owner};
    reserve.capacity_bundle = {
      0, 1, fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::user_spot, "room", 1}};
    const auto first =
      std::get<fw::object_reserved_t> (repository->reserve (reserve).result ().value ());
    rt::user_spot_authority_payload_t payload{.stable_type = "room",
                                              .spot_id = "ended-user",
                                              .owner_id = owner.owner_id,
                                              .owner_lease_generation =
                                                static_cast<std::uint64_t> (owner.lease_generation),
                                              .mesh_name = old.mesh_name,
                                              .node_rid = reserve.target.node_rid,
                                              .node_generation = 1};
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      repository
        ->commit ({reserve.key, first.fence, rt::encode_user_spot_authority_payload (payload)})
        .result ()
        .value ()));
    if (policy == 2) {
        constexpr auto final_lease_ttl = 1ms;
        ASSERT_TRUE (std::holds_alternative<fw::owner_lease_renewed_t> (
          repository->renew_owner_lease (owner, final_lease_ttl).result ().value ()));
        std::this_thread::sleep_for (final_lease_ttl);
        rt::live_location_reader_t reader (*repository);
        ASSERT_FALSE (reader.owner_available (owner).result ().value ());
    } else {
        ASSERT_TRUE (std::holds_alternative<fw::owner_lease_released_t> (
          repository->release_owner_lease (owner).result ().value ()));
    }
    auto app = fw::app_t::create ();
    std::atomic_int factories{0};
    app.advanced ().services ().add_factory<fw::location_repository_t> (
      [repository] (fw::service_provider_t &) { return repository; },
      fw::service_lifetime_t::singleton);
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (opaque);
        options.add_relocation_store (std::make_shared<rt::in_memory_relocation_store_t> ());
        auto request = options.add_route_mesh ("request-mesh");
        request.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("request-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<ended_user_request_entry_t> ();
        if (policy != 2)
            request.add_spot_factory<context_owned_user_spot_t> (
              "room",
              [] (fw::spot_context_t context) {
                  return std::make_shared<context_owned_user_spot_t> (std::move (context));
              },
              [policy] (auto &factory) {
                  if (policy == 0)
                      factory.disable_relocation ();
                  else
                      factory.recreate_on_relocation ();
              });
        auto target = options.add_route_mesh ("factory-mesh");
        target.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("factory-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<context_owned_user_spot_t> (
            "room",
            [&] (fw::spot_context_t context) {
                ++factories;
                return std::make_shared<context_owned_user_spot_t> (std::move (context));
            },
            [policy] (auto &factory) {
                if (policy == 0)
                    factory.recreate_on_relocation ();
                else
                    factory.disable_relocation ();
            });
    });
    auto client = std::make_unique<ended_user_reclaim_client_t> (app, policy != 0, factories);
    auto *observed = client.get ();
    app.add_hosted_service (std::move (client));
    EXPECT_EQ (app.run (0, nullptr), 0);
    EXPECT_TRUE (observed->observed);
}
TEST (ZLinkFrameworkStoreLocationResolvers,
      EndedUserGetOrCreateUsesFactoryTargetWithoutRequesterFactory)
{
    verify_ended_user_policy_source (true, 2);
    verify_ended_user_policy_source (false, 2);
}

TEST (ZLinkFrameworkStoreLocationResolvers, EndedEmptyUserProviderUsesFactoryTargetPolicy)
{
    for (unsigned policy = 0; policy != 2; ++policy)
        verify_ended_user_policy_source (true, policy);
}
TEST (ZLinkFrameworkStoreLocationResolvers, EndedEmptyUserInMemoryUsesFactoryTargetPolicy)
{
    for (unsigned policy = 0; policy != 2; ++policy)
        verify_ended_user_policy_source (false, policy);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      GeneratedUserSpotFirstAuthorityConflictFailsWithoutRetry)
{
    auto store = std::make_shared<test_location_repository_t> ();
    auto public_store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    generated_user_spot_collision_client_t *client = nullptr;

    app.advanced ().services ().add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (public_store);
        auto node = options.add_route_mesh ("spot-collision-mesh");
        node.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("spot-collision-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<occupied_user_spot_t> (
            "occupied",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<occupied_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_spot_factory<local_user_spot_t> (
            "room",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<generated_user_spot_collision_client_t> (app, store);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SourceCreatedReservationIsReconciledAfterExactCreateFailure)
{
    auto store = std::make_shared<test_location_repository_t> ();
    auto public_store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    source_cleanup_client_t *client = nullptr;

    app.advanced ().services ().add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (public_store);
        auto node = options.add_route_mesh ("source-cleanup-mesh");
        node.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("source-cleanup-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_spot_factory<failing_user_spot_t> (
            "failing",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<failing_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto service = std::make_unique<source_cleanup_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_GE (store->abort_count.load (std::memory_order_relaxed), 2u);
    const auto authority = store->read_authority ({"2:source-cleanup"}).result ().value ();
    EXPECT_TRUE (std::holds_alternative<zlink::framework::authority_missing_t> (authority));
}

TEST (ZLinkFrameworkStoreLocationResolvers, UserSpotTerminalMappingPreservesExactPublicErrors)
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::foundation::operation_terminal_t;
    using zlink::framework::runtime::protocol::reply_header_t;
    using zlink::framework::runtime::user_spot_terminal::map_user_spot_operation_failure;

    EXPECT_EQ (framework_error_kind_t::deadline_exceeded,
               map_user_spot_operation_failure (operation_terminal_t::timed_out, {}, true));
    EXPECT_EQ (
      framework_error_kind_t::invalid_operation,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 107, 33}, true));
    EXPECT_EQ (
      framework_error_kind_t::unavailable,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 107, 34}, false));
    EXPECT_EQ (
      framework_error_kind_t::type_mismatch,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 107, 7}, true));
    // A peer's Busy(108)+None identifies an unavailable remote target.
    EXPECT_EQ (
      framework_error_kind_t::unavailable,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 108, 0}, true));
    // Legacy Backpressured(113)+None identifies an unavailable remote target.
    EXPECT_EQ (
      framework_error_kind_t::unavailable,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 113, 0}, true));
    EXPECT_EQ (
      framework_error_kind_t::rejected,
      map_user_spot_operation_failure (operation_terminal_t::completed, {1, 106, 15}, true));
    EXPECT_EQ (framework_error_kind_t::invalid_operation,
               map_user_spot_operation_failure (operation_terminal_t::cancelled, {}, false));
    EXPECT_EQ (framework_error_kind_t::shutting_down,
               map_user_spot_operation_failure (operation_terminal_t::shutdown, {}, false));
}
} // namespace
