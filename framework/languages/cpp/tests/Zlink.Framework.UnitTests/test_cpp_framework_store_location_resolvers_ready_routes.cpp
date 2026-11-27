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

namespace zlink::framework::runtime
{
struct auto_connect_cache_test_access_t
{
    static std::chrono::steady_clock::time_point
    cache_expiry (store_location_resolvers_t &resolver,
                  std::string key,
                  spot_address_t address,
                  std::chrono::steady_clock::time_point owner_admission_deadline)
    {
        const auto route_key = key;
        resolver
          .cache_ready_route (resolver._spot_routes, std::move (key), std::move (address),
                              owner_admission_deadline)
          .result ();
        return resolver._spot_routes.at (route_key).expires_at;
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

class counting_location_store_t final : public zlink::framework::location_store_t
{
  public:
    explicit counting_location_store_t (zlink::framework::location_store_t &inner) : _inner (inner)
    {
    }

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        {
            std::lock_guard lock (_mutex);
            ++_read_counts[key.value];
        }
        co_return co_await _inner.read (std::move (key));
    }

    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        co_return co_await _inner.write (std::move (request));
    }

    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        co_return co_await _inner.scan (std::move (request));
    }

    void clear_read_counts ()
    {
        std::lock_guard lock (_mutex);
        _read_counts.clear ();
    }

    std::size_t read_count (std::string_view key_prefix) const
    {
        std::lock_guard lock (_mutex);
        std::size_t count = 0;
        for (const auto &[key, value] : _read_counts)
            if (key.starts_with (key_prefix))
                count += value;
        return count;
    }

    std::size_t exact_read_count (std::string_view key) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _read_counts.find (std::string (key));
        return found == _read_counts.end () ? 0 : found->second;
    }

  private:
    zlink::framework::location_store_t &_inner;
    mutable std::mutex _mutex;
    std::map<std::string, std::size_t> _read_counts;
};

struct options_fixture_t
{
    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    options_fixture_t ()
    {
        zlink::framework::detail::bind_zlink_monitoring (
          zlink, zlink::framework::test::runtime_failure_monitoring ());
    }
    zlink::framework::zlink_framework_options_t make_options ()
    {
        return zlink::framework::zlink_framework_options_t (services, handlers, serializers, zlink);
    }
};
} // namespace

namespace
{
TEST (ZLinkFrameworkStoreLocationResolvers, EncodesCanonicalAuthorityKeys)
{
    EXPECT_EQ ("zla1:a:9:tenant%3A42",
               zlink::framework::runtime::actor_authority_key ("tenant:42").value);
    EXPECT_EQ ("zla1:s:5:a%2Fb%20%25",
               zlink::framework::runtime::spot_authority_key ("a/b %").value);
    EXPECT_EQ ("zla1:s:4:~._-", zlink::framework::runtime::spot_authority_key ("~._-").value);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesSpotAddressFromStore)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    store.set_authority (
      "zla1:s:6:spot-a",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("spot-a", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::user_spot,
                       .stable_type = "play",
                       .target = {.mesh_name = "mesh-a",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});
    store_location_resolvers_t resolvers (store);

    const auto address = resolvers.resolve_spot_address ("mesh-a", "spot-a").result ().value ();

    ASSERT_TRUE (address.has_value ());
    EXPECT_EQ ("mesh-a", address->mesh_name);
    EXPECT_EQ ("node-a", address->node_rid.to_string ());
    EXPECT_EQ ("spot-a", address->spot_id);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      DirectSendAndRequestResolveCanonicalInstanceSpotAuthority)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-instance");
    const auto owner = live_owner_token (store, "owner-instance");
    store.set_authority (
      "zla1:s:13:instance-spot",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = zlink::framework::runtime::encode_instance_spot_authority_payload (
          {.state = zlink::framework::runtime::instance_spot_authority_state_t::ready,
           .stable_type = "cart",
           .spot_id = "instance-spot",
           .owner_id = owner.owner_id,
           .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
           .mesh_name = "mesh-instance",
           .node_rid = zlink::framework::node_rid_t::from_string ("node-instance"),
           .node_generation = 7}),
        .object_generation = 11,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {
          .state = zlink::framework::placement_allocation_state_t::active,
          .object_kind = zlink::framework::placement_object_kind_t::instance_spot,
          .stable_type = "cart",
          .target = {.mesh_name = "mesh-instance",
                     .node_rid = zlink::framework::node_rid_t::from_string ("node-instance"),
                     .node_lifecycle_generation = 7,
                     .owner = owner}}});

    const auto resolvers_owner = std::make_shared<store_location_resolvers_t> (store);
    auto &resolvers = *resolvers_owner;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_spot_address_resolver (resolvers_owner);

    std::atomic_int sends{0};
    std::atomic_int requests{0};
    zlink::framework::runtime::messaging::envelope_codec_t envelope_codec;
    zlink::framework::serializer_registry_t serializers;
    runtime.bind_spot_mesh_transport (
      "mesh-instance",
      [&] (const zlink::routing_id_t &node_rid, const std::string &spot_id,
           std::uint64_t generation, zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          if (node_rid.to_string () == "node-instance" && spot_id == "instance-spot"
              && generation == 11)
              ++sends;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &node_rid, const std::string &spot_id,
           std::uint64_t generation, zlink::framework::runtime::messaging::message_parts_t parts,
           std::chrono::milliseconds)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          if (node_rid.to_string () == "node-instance" && spot_id == "instance-spot"
              && generation == 11)
              ++requests;
          auto header = envelope_codec.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          lease_target_reply_t reply{"node-instance"};
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelope_codec.encode_parts (header, reply, serializers));
      });

    auto client = builder.route_client (serializers);
    const auto sent =
      client.send_to_spot ("instance-spot", user_spot_delivery_probe_t{41}).async ().result ();
    const auto reply = client.request_to_spot ("instance-spot", lease_target_request_t{42})
                         .timeout (std::chrono::milliseconds (100))
                         .async<lease_target_reply_t> ()
                         .result ();

    EXPECT_TRUE (sent) << (sent.error () ? sent.error ()->what () : "no error detail");
    EXPECT_TRUE (reply) << (reply.error () ? reply.error ()->what () : "no error detail");
    if (reply)
        EXPECT_EQ ("node-instance", reply.value ().node_rid);
    EXPECT_EQ (1, sends.load ());
    EXPECT_EQ (1, requests.load ());
    std::vector<std::pair<std::size_t, std::size_t>> lookup_counts;
    for (bool instance_intent : {false, true}) {
        location_options_t options;
        options.route_cache_max_age = std::chrono::seconds (1);
        store_location_resolvers_t measured (store, options);
        store.authority_read_count = 0;
        store.owner_read_count = 0;
        const auto ready =
          measured.resolve_spot_address ({}, "instance-spot", instance_intent).result ().value ();
        ASSERT_TRUE (ready);
        EXPECT_EQ (1u, ready->authority_owner_generation);
        lookup_counts.emplace_back (store.authority_read_count.load (),
                                    store.owner_read_count.load ());
        ASSERT_TRUE (
          measured.resolve_spot_address ({}, "instance-spot", instance_intent).result ().value ());
        EXPECT_EQ (lookup_counts.back ().first, store.authority_read_count);
        EXPECT_EQ (lookup_counts.back ().second, store.owner_read_count);
    }
    EXPECT_EQ (1u, lookup_counts.front ().first);
    EXPECT_EQ (lookup_counts.front (), lookup_counts.back ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, ReadyCacheMissReadsOwnerLeaseOnce)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    store.set_authority (
      "zla1:s:9:spot-once",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("spot-once", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::user_spot,
                       .stable_type = "play",
                       .target = {.mesh_name = "mesh-a",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});
    store.set_authority (
      "zla1:a:10:actor-once",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = zlink::framework::runtime::encode_actor_authority_payload (
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("node-a"), "player", "actor-once", 1),
          "spot-once", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::actor,
                       .stable_type = "player",
                       .target = {.mesh_name = "mesh-a",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});

    location_options_t options;
    options.route_cache_max_age = std::chrono::seconds (1);
    store_location_resolvers_t resolvers (
      store, options, std::make_shared<zlink::framework::runtime::actor_location_observer_t> (),
      "mesh-a");

    store.owner_read_count = 0;
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-once").result ().value ());
    EXPECT_EQ (1u, store.owner_read_count.load ());
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-once").result ().value ());
    EXPECT_EQ (1u, store.owner_read_count.load ());
    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-once").result ().value ());
    EXPECT_EQ (2u, store.owner_read_count.load ());

    auto no_cache_options = options;
    no_cache_options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t uncached (
      store, no_cache_options,
      std::make_shared<zlink::framework::runtime::actor_location_observer_t> (), "mesh-a");
    store.owner_read_count = 0;
    ASSERT_TRUE (uncached.resolve_spot_address ({}, "spot-once").result ().value ());
    ASSERT_TRUE (uncached.resolve_spot_address ({}, "spot-once").result ().value ());
    EXPECT_EQ (2u, store.owner_read_count.load ());

    store.owner_read_count = 0;
    ASSERT_TRUE (resolvers.resolve_actor_address ("actor-once").result ().value ());
    EXPECT_EQ (1u, store.owner_read_count.load ());
    ASSERT_TRUE (resolvers.resolve_actor_address ("actor-once").result ().value ());
    EXPECT_EQ (1u, store.owner_read_count.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, ReadyRouteCacheDoesNotOutliveOwnerAdmission)
{
    location_options_t options;
    options.route_cache_max_age = std::chrono::seconds (5);
    test_location_repository_t store;
    store_location_resolvers_t resolvers (store, options);
    const auto admission_read_at = std::chrono::steady_clock::now ();
    const auto owner_admission_lifetime = std::chrono::milliseconds (200);
    const auto owner_admission_deadline = admission_read_at + owner_admission_lifetime;
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::framework::runtime::spot_address_t address{
      "mesh-a", zlink::routing_id_t::from ("node-a"), "spot-deadline", 1};
    address.store_version = "1";
    address.object_generation = 1;
    address.authority_owner_generation = 1;
    address.owner = {"owner-a", 1};
    const auto cache_expiry =
      zlink::framework::runtime::auto_connect_cache_test_access_t::cache_expiry (
        resolvers, "spot-deadline", std::move (address), owner_admission_deadline);

    EXPECT_LE (cache_expiry, owner_admission_deadline);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ReadySpotMissCountsRawProviderReadsByKey)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    fw::runtime::in_memory_location_store_t provider;
    counting_location_store_t counted_provider (provider);
    fw::runtime::provider_location_repository_t repository (counted_provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim = repository.claim_owner_lease ("owner-raw", 30s).result ().value ();
    const auto *claimed = std::get_if<fw::owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "mesh-raw";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-raw"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::user_spot, "room",
                                               fw::maintenance_policy_kind_t::snapshot, true, 10});
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::actor, "player",
                                               fw::maintenance_policy_kind_t::snapshot, true, 10});
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);

    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::user_spot, "spot-raw"};
    request.intent.stable_type = "room";
    request.target = {"mesh-raw", fw::node_rid_t::from_string ("node-raw"), 1, claimed->token};
    request.creating_payload = {std::byte{0}};
    request.capacity_bundle.spot_slots = 1;
    request.capacity_bundle.spot_type =
      fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::user_spot, "room", 1};
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<fw::object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      repository
        .commit ({request.key, reservation->fence, user_spot_authority_payload ("spot-raw", 1)})
        .result ()
        .value ()));

    location_options_t options;
    options.route_cache_max_age = 1s;
    store_location_resolvers_t resolvers (repository, options);
    counted_provider.clear_read_counts ();
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-raw").result ().value ());

    const std::string authority_key{"authority\0spot\0spot-raw", 23};
    const std::string owner_key{"owner-lease\0owner-raw", 21};
    EXPECT_EQ (1u, counted_provider.exact_read_count (authority_key));
    EXPECT_EQ (1u, counted_provider.exact_read_count (owner_key));
    EXPECT_EQ (1u, counted_provider.read_count ("zlink:v11:aggregate-lock:"));

    auto unrelated_descriptor = descriptor;
    unrelated_descriptor.mesh_name = "noise-mesh";
    unrelated_descriptor.rid = zlink::routing_id_t::from (std::string{"noise-node"});
    ASSERT_EQ (
      repository.update_mesh_node (unrelated_descriptor, fw::location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status,
      fw::location_write_status_t::stored);
    auto unrelated_request = request;
    unrelated_request.key.global_id = "spot-noise";
    const auto unrelated_reserved = repository.reserve (unrelated_request).result ().value ();
    const auto *unrelated_reservation = std::get_if<fw::object_reserved_t> (&unrelated_reserved);
    ASSERT_NE (unrelated_reservation, nullptr);
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      repository
        .commit ({unrelated_request.key, unrelated_reservation->fence,
                  user_spot_authority_payload ("spot-noise", 1)})
        .result ()
        .value ()));

    fw::object_reserve_request_t actor_request;
    actor_request.key = {fw::placement_object_kind_t::actor, "actor-raw"};
    actor_request.intent.stable_type = "player";
    actor_request.target = request.target;
    actor_request.creating_payload = {std::byte{0}};
    actor_request.capacity_bundle.actor_slots = 1;
    const auto actor_reserved = repository.reserve (actor_request).result ().value ();
    const auto *actor_reservation = std::get_if<fw::object_reserved_t> (&actor_reserved);
    ASSERT_NE (actor_reservation, nullptr);
    const auto actor_ref = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("node-raw"), "player", "actor-raw",
      actor_reservation->fence.object_generation);
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      repository
        .commit ({actor_request.key, actor_reservation->fence,
                  fw::runtime::encode_actor_authority_payload (actor_ref, "spot-raw", 1)})
        .result ()
        .value ()));

    counted_provider.clear_read_counts ();
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-noise").result ().value ());
    const std::string unrelated_authority_key{"authority\0spot\0spot-noise", 25};
    EXPECT_EQ (1u, counted_provider.exact_read_count (unrelated_authority_key));
    EXPECT_EQ (1u, counted_provider.exact_read_count (owner_key));
    EXPECT_EQ (1u, counted_provider.read_count ("zlink:v11:aggregate-lock:"));

    counted_provider.clear_read_counts ();
    ASSERT_TRUE (resolvers.resolve_actor_address ("actor-raw").result ().value ());
    const std::string actor_authority_key{"authority\0actor\0actor-raw", 25};
    EXPECT_EQ (1u, counted_provider.exact_read_count (actor_authority_key));
    EXPECT_EQ (1u, counted_provider.exact_read_count (owner_key));
    EXPECT_EQ (1u, counted_provider.read_count ("zlink:v11:aggregate-lock:"));
}

TEST (ZLinkFrameworkStoreLocationResolvers, DirectReadyRouteUsesPositiveCacheOnly)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-cache");
    const auto owner = live_owner_token (store, "owner-cache");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "10",
      .payload = user_spot_authority_payload ("spot-cache", 1),
      .object_generation = 1,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = zlink::framework::placement_allocation_t{
        .state = zlink::framework::placement_allocation_state_t::active,
        .object_kind = zlink::framework::placement_object_kind_t::user_spot,
        .stable_type = "play",
        .target = zlink::framework::object_creation_target_t{
          .mesh_name = "mesh-cache",
          .node_rid = zlink::framework::node_rid_t::from_string ("node-cache"),
          .node_lifecycle_generation = 1,
          .owner = owner}}};
    store.set_authority ("zla1:s:10:spot-cache", authority);
    location_options_t options;
    options.route_cache_max_age = std::chrono::seconds (1);
    store_location_resolvers_t resolvers (store, options);

    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    authority.store_version = "11";
    authority.authority_owner_generation = 2;
    store.set_authority ("zla1:s:10:spot-cache", authority);
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    resolvers.invalidate_all_routes_after_store_recovery ();
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    EXPECT_FALSE (resolvers.resolve_spot_address ({}, "missing").result ().value ());
    EXPECT_FALSE (resolvers.resolve_spot_address ({}, "missing").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, InvalidatedInFlightReadyReadCannotRepopulateCache)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-cache");
    const auto owner = live_owner_token (store, "owner-cache");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "10",
      .payload = user_spot_authority_payload ("spot-cache", 1),
      .object_generation = 1,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = zlink::framework::placement_allocation_t{
        .state = zlink::framework::placement_allocation_state_t::active,
        .object_kind = zlink::framework::placement_object_kind_t::user_spot,
        .stable_type = "play",
        .target = zlink::framework::object_creation_target_t{
          .mesh_name = "mesh-cache",
          .node_rid = zlink::framework::node_rid_t::from_string ("node-cache"),
          .node_lifecycle_generation = 1,
          .owner = owner}}};
    store.set_authority ("zla1:s:10:spot-cache", authority);
    location_options_t options;
    options.route_cache_max_age = std::chrono::seconds (1);
    store_location_resolvers_t resolvers (store, options);

    store.before_owner_read = [&] {
        store.set_authority ("zla1:s:10:spot-cache", std::nullopt);
        resolvers.invalidate_spot_address ("spot-cache");
    };
    const auto first = resolvers.resolve_spot_address ({}, "spot-cache").result ().value ();
    ASSERT_TRUE (first);
    const auto reads = store.authority_read_count.load ();
    EXPECT_FALSE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_GT (store.authority_read_count.load (), reads);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ClosingRoutePreservesOwnerFencesWithoutPositiveCache)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    for (const auto kind :
         {placement_object_kind_t::user_spot, placement_object_kind_t::instance_spot}) {
        test_location_repository_t store;
        const auto owner = claim_test_owner (store, "closing-owner");
        const std::string spot_id = "closing-route";
        const auto key = spot_authority_key (spot_id);
        authority_snapshot_t authority{
          .store_version = "closing-version",
          .object_generation = 7,
          .authority_owner_generation = 11,
          .owner = owner,
          .allocation = {.state = placement_allocation_state_t::active,
                         .object_kind = kind,
                         .stable_type = "play",
                         .target = {.mesh_name = "closing-mesh",
                                    .node_rid = node_rid_t::from_string ("closing-node"),
                                    .node_lifecycle_generation = 13,
                                    .owner = owner}}};
        if (kind == placement_object_kind_t::user_spot)
            authority.payload = encode_user_spot_authority_payload (
              {.state = user_spot_authority_state_t::closing,
               .stable_type = authority.allocation.stable_type,
               .spot_id = spot_id,
               .owner_id = owner.owner_id,
               .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
               .mesh_name = authority.allocation.target.mesh_name,
               .node_rid = authority.allocation.target.node_rid,
               .node_generation = authority.allocation.target.node_lifecycle_generation});
        else
            authority.payload = encode_instance_closing_state (
              {authority.allocation.stable_type, spot_id, authority.object_generation,
               authority.authority_owner_generation});
        store.set_authority (key.value, authority);
        location_options_t options;
        options.route_cache_max_age = std::chrono::seconds (1);
        store_location_resolvers_t resolvers (store, options);
        const auto address = resolvers.resolve_spot_address ({}, spot_id).result ().value ();
        ASSERT_TRUE (address);
        EXPECT_EQ (spot_id, address->spot_id);
        EXPECT_EQ (authority.allocation.target.mesh_name, address->mesh_name);
        EXPECT_EQ (authority.allocation.target.node_rid.value (), address->node_rid.to_string ());
        EXPECT_EQ (authority.allocation.target.node_lifecycle_generation, address->node_generation);
        EXPECT_EQ (authority.store_version, address->store_version);
        EXPECT_EQ (authority.object_generation, address->spot_generation);
        EXPECT_EQ (authority.object_generation, address->object_generation);
        EXPECT_EQ (authority.authority_owner_generation, address->authority_owner_generation);
        EXPECT_EQ (owner.owner_id, address->owner.owner_id);
        EXPECT_EQ (owner.lease_generation, address->owner.lease_generation);

        authority.store_version = "closing-version-next";
        store.set_authority (key.value, authority);
        const auto refreshed = resolvers.resolve_spot_address ({}, spot_id).result ().value ();
        ASSERT_TRUE (refreshed);
        EXPECT_EQ (authority.store_version, refreshed->store_version);
        store.set_authority (key.value, std::nullopt);
        EXPECT_FALSE (resolvers.resolve_spot_address ({}, spot_id).result ().value ());
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, AddLocationStoreRegistersOpaqueProvider)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();
    auto store = std::make_shared<in_memory_location_store_t> ();

    options.add_location_store (store);
    options.apply ();

    auto provider = fixture.services.build_provider ();
    EXPECT_EQ (store.get (), &provider.get_required<zlink::framework::location_store_t> ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, AppFrameworkUsesConfiguredLocationStore)
{
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<in_memory_location_store_t> ();

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (25);
    });

    auto provider = app.advanced ().services ().build_provider ();
    EXPECT_EQ (store.get (), &provider.get_required<zlink::framework::location_store_t> ());
    EXPECT_EQ (std::chrono::milliseconds (25),
               provider.get_required<zlink::framework::runtime::location_runtime_t> ()
                 .options ()
                 .owner_lease_renew_interval);
}

TEST (ZLinkFrameworkStoreLocationResolvers, DuplicateLocationStoreRegistrationIsRejected)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();
    auto first = std::make_shared<in_memory_location_store_t> ();
    auto second = std::make_shared<in_memory_location_store_t> ();

    options.add_location_store (first);

    EXPECT_THROW (options.add_location_store (second), zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers, RejectsInvalidRoutingAndRelocationLimitsBeforeApply)
{
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        EXPECT_EQ (std::chrono::milliseconds (3000),
                   options.configure_locations ().session_relocation_seal_timeout);
        options.configure_locations ().session_relocation_seal_timeout =
          std::chrono::milliseconds::zero ();
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        EXPECT_EQ (std::chrono::milliseconds (30000),
                   options.session_replacement_callback_timeout ());
        options.set_session_replacement_callback_timeout (std::chrono::milliseconds (17));
        EXPECT_EQ (std::chrono::milliseconds (17), options.session_replacement_callback_timeout ());
        EXPECT_THROW (
          options.set_session_replacement_callback_timeout (std::chrono::milliseconds::zero ()),
          zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().route_cache_max_age = std::chrono::seconds (26);
        options.configure_locations ().message_follow_duration = std::chrono::seconds (30);
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().max_active_outbound_relocations = 0;
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().owner_lease_ttl = std::chrono::seconds (15);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::seconds (7);
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      FanoutAutomaticAndManualSubscriberSourcesFailConfiguration)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();

    options.add_fanout_channel ("events").enable_subscriber ().connect ("tcp://127.0.0.1:7001");

    try {
        options.apply ();
        FAIL () << "mixed fanout subscriber sources must fail configuration";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (zlink::framework::framework_error_kind_t::protocol_error, error.kind ());
        EXPECT_NE (std::string::npos,
                   std::string (error.what ())
                     .find ("cannot combine automatic discovery with manual subscriber endpoints"));
    }
}
} // namespace
