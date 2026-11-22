/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/telemetry_observation.hpp"
#include "metric_test_reader.hpp"
#include <zlink/framework.hpp>
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime_context_hooks_environment.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include "../support/instance_spot_activation_messages.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace
{
// Spot address messaging §9: authority following stays within one logical request.
TEST (ZLinkFrameworkInstanceSpotActivation, AuthorityFollowConformance)
{
    namespace fw = zlink::framework;
    namespace messaging = fw::runtime::messaging;
    using fw::runtime::spot_address_t;
    auto root = std::filesystem::path (__FILE__).parent_path ();
    while (!std::filesystem::exists (root / "framework/runtime/conformance/spot-close-v1.json")) {
        ASSERT_NE (root, root.parent_path ());
        root = root.parent_path ();
    }
    std::ifstream input (root / "framework/runtime/conformance/spot-close-v1.json");
    const auto fixture = nlohmann::json::parse (input);
    class creating_repository_t final : public fw::runtime::in_memory_location_repository_t
    {
      public:
        fw::task_t<fw::authority_read_result_t> read_authority_core (fw::authority_key_t,
                                                                     std::stop_token = {}) override
        {
            fw::authority_snapshot_t snapshot;
            snapshot.store_version = "same-attempt";
            snapshot.object_generation = 3;
            snapshot.authority_owner_generation = 4;
            snapshot.owner = {"owner", 1};
            snapshot.payload = fw::runtime::encode_instance_spot_authority_payload (
              {.state = fw::runtime::instance_spot_authority_state_t::cold_activating,
               .stable_type = "player-quest",
               .spot_id = "player-alice",
               .owner_id = "owner",
               .owner_lease_generation = 1,
               .mesh_name = "gamequest",
               .node_rid = fw::node_rid_t::from_string ("new-owner"),
               .node_generation = 3});
            snapshot.allocation.state = fw::placement_allocation_state_t::reserved;
            snapshot.allocation.object_kind = fw::placement_object_kind_t::instance_spot;
            snapshot.allocation.stable_type = "player-quest";
            snapshot.allocation.target = {"gamequest", fw::node_rid_t::from_string ("new-owner"), 3,
                                          snapshot.owner};
            snapshot.pending_creation.emplace ().reservation_id = "same-attempt";
            co_return snapshot;
        }
    };
    class authority_resolver_t final : public fw::runtime::spot_address_resolver_t
    {
      public:
        fw::task_t<std::optional<spot_address_t>> resolve_spot_address (std::string,
                                                                        std::string) override
        {
            ++reads;
            if (reads > 1 && creating) {
                creating_repository_t repository;
                fw::runtime::store_location_resolvers_t resolver (repository);
                co_return co_await resolver.resolve_spot_address ({}, "player-alice", true);
            }
            co_return reads == 1 ? cached : authority;
        }
        void invalidate_spot_address (std::string_view) override { ++invalidations; }
        void invalidate_all_routes_after_store_recovery () override {}
        std::optional<spot_address_t> cached;
        std::optional<spot_address_t> authority;
        bool creating = false;
        int reads = 0;
        int invalidations = 0;
    };
    const messaging::request_failure_mapper_t mapper;
    for (const auto &scenario : fixture.at ("authorityFollowCases")) {
        const auto failure_code = scenario.value ("failureCode", 34u);
        const auto refusal = *mapper.target_failure_reply (
          failure_code == 33 ? fw::framework_error_kind_t::invalid_operation
                             : fw::framework_error_kind_t::unavailable,
          failure_code);
        SCOPED_TRACE (scenario.at ("name").get<std::string> ());
        fw::serializer_registry_t serializers;
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        auto resolver = std::make_shared<authority_resolver_t> ();
        resolver->cached =
          spot_address_t{"gamequest", zlink::routing_id_t::from ("old-owner"), "player-alice", 7};
        if (scenario.at ("authority") == "Ready") {
            resolver->authority = *resolver->cached;
            resolver->authority->node_rid = zlink::routing_id_t::from ("new-owner");
        }
        resolver->creating = scenario.at ("authority") == "Creating";
        runtime.bind_spot_address_resolver (resolver);
        int attempts = 0;
        const int refusals = scenario.at ("refusals");
        std::optional<std::chrono::milliseconds> previous_remaining;
        runtime.bind_instance_spot_activator (
          [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
              std::chrono::system_clock::time_point) -> fw::task_t<fw::result_t<void>> {
              co_return fw::result_t<void>::failure (fw::framework_error_kind_t::internal_failure,
                                                     "unused send");
          },
          [&] (const auto &, const auto &, const std::optional<spot_address_t> &route, std::string,
               std::type_index, auto, std::chrono::milliseconds remaining,
               auto) -> fw::request_task_t<zlink::message_t> {
              co_await fw::detail::request_admitted_t{};
              ++attempts;
              EXPECT_EQ (attempts, resolver->reads);
              if (previous_remaining)
                  EXPECT_LE (remaining, *previous_remaining);
              previous_remaining = remaining;
              if (attempts > 1 && route)
                  EXPECT_EQ (zlink::routing_id_t::from ("new-owner"), route->node_rid);
              if (refusals < 0 || attempts <= refusals)
                  co_return fw::detail::result_access_t::failure<zlink::message_t> (
                    mapper.reply_header_exception (refusal.terminal_result, refusal.failure_code,
                                                   "owner fence"));
              co_return fw::result_t<zlink::message_t>::success (
                fw::detail::encoded_payload_to_raw (
                  serializers.get<reply_t> ().serialize (reply_t{9})));
          });
        // The no-intent branch uses the ordinary direct request transport.
        runtime.bind_spot_mesh_transport (
          "gamequest",
          [] (const auto &, const auto &, std::uint64_t, auto) -> fw::task_t<fw::result_t<void>> {
              co_return fw::result_t<void>::failure (fw::framework_error_kind_t::internal_failure,
                                                     "unused send");
          },
          [&] (const auto &, const auto &, std::uint64_t, messaging::message_parts_t,
               std::chrono::milliseconds)
            -> fw::request_task_t<fw::result_t<messaging::message_parts_t>> {
              ++attempts;
              throw mapper.reply_header_exception (refusal.terminal_result, refusal.failure_code,
                                                   "owner fence");
              co_return fw::result_t<messaging::message_parts_t>::success ({});
          });
        auto call =
          builder.route_client (serializers).request_to_spot ("player-alice", request_t{1});
        if (scenario.at ("intent").get<bool> ())
            call.instance_spot ("player-quest");
        call.timeout (refusals < 0 ? std::chrono::milliseconds (20)
                                   : std::chrono::milliseconds (1000));
        const auto reply = call.async<reply_t> ().result ();
        if (scenario.at ("terminal") == "reply") {
            ASSERT_TRUE (reply);
            EXPECT_EQ (9, reply.value ().value);
            EXPECT_EQ (refusals + 1, attempts);
            EXPECT_EQ (refusals + 1, resolver->reads);
        } else {
            ASSERT_FALSE (reply);
            EXPECT_EQ (scenario.at ("terminal") == "DeadlineExceeded"
                         ? fw::framework_error_kind_t::deadline_exceeded
                         : fw::framework_error_kind_t::unavailable,
                       reply.error_kind ());
            if (refusals >= 0)
                EXPECT_EQ (1, attempts);
        }
    }
}

// Spot address messaging §5: a request completes terminal-once. A fence refusal
// that failed admission is already the caller's terminal, so the operation's later
// physical terminal ends the operation instead of continuing as cold activation.
TEST (ZLinkFrameworkInstanceSpotActivation, FenceRefusalAfterPublicTerminalDoesNotColdActivate)
{
    namespace messaging = zlink::framework::runtime::messaging;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::spot_address_t;

    class missing_after_invalidate_resolver_t final
        : public zlink::framework::runtime::spot_address_resolver_t
    {
      public:
        zlink::framework::task_t<std::optional<spot_address_t>>
        resolve_spot_address (std::string, std::string) override
        {
            if (!cached)
                ++authority_reads;
            return zlink::framework::task_t<std::optional<spot_address_t>> (
              zlink::framework::result_t<std::optional<spot_address_t>>::success (cached));
        }

        void invalidate_spot_address (std::string_view) override { cached.reset (); }

        void invalidate_all_routes_after_store_recovery () override { cached.reset (); }

        std::optional<spot_address_t> cached;
        std::atomic<int> authority_reads{0};
    };

    const messaging::request_failure_mapper_t mapper;
    const auto refusal = *mapper.target_failure_reply (
      framework_error_kind_t::unavailable,
      static_cast<std::uint32_t> (
        zlink::framework::runtime::protocol::framework_error_code::spotMoving));
    const auto refused = std::make_exception_ptr (mapper.reply_header_exception (
      refusal.terminal_result, refusal.failure_code, "Instance Spot request"));

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver = std::make_shared<missing_after_invalidate_resolver_t> ();
    resolver->cached =
      spot_address_t{"gamequest", zlink::routing_id_t::from ("quest-mission"), "player-alice", 7};
    runtime.bind_spot_address_resolver (resolver);
    zlink::framework::task_completion_source_t<zlink::message_t> physical_reply;
    std::atomic<int> activations{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
          std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            framework_error_kind_t::internal_failure, "unused one-way activation");
      },
      [&] (const auto &, const auto &, const auto &, std::string, std::type_index, auto,
           std::chrono::milliseconds, auto) -> zlink::framework::request_task_t<zlink::message_t> {
          if (activations++ != 0)
              return zlink::framework::request_task_t<zlink::message_t> (
                zlink::framework::result_t<zlink::message_t>::success (
                  zlink::framework::detail::encoded_payload_to_raw (
                    serializers.get<reply_t> ().serialize (reply_t{9}))));
          // The owner refused the fence before admission; the physical reply stays open.
          auto admitted = std::make_shared<zlink::framework::task_completion_source_t<void>> ();
          admitted->complete (zlink::framework::detail::result_access_t::failure<void> (refused));
          return zlink::framework::request_task_t<zlink::message_t> (physical_reply.task (),
                                                                     admitted);
      });

    auto pending = builder.route_client (serializers)
                     .request_to_spot ("player-alice", request_t{1})
                     .instance_spot ("player-quest")
                     .async<reply_t> ();
    const auto reply = pending.result ();
    ASSERT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
    physical_reply.complete (
      zlink::framework::detail::result_access_t::failure<zlink::message_t> (refused));
    // Waits for the operation's own terminal (no sleep).
    (void) zlink::framework::detail::request_task_access_t::physical_reply (pending).result ();
    EXPECT_EQ (1, activations.load ()) << "cold activation ran after the public terminal";
    EXPECT_EQ (0, resolver->authority_reads.load ());
    EXPECT_FALSE (resolver->cached) << "the refused route is invalidated for the next call";
}

TEST (ZLinkFrameworkInstanceSpotActivation, RetiredOwnerRequestCompletesWithUnavailableTerminal)
{
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("retired-owner");
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder, "retired-owner");
    ASSERT_TRUE (runtime);
    zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
      .bind_serializers (serializers);
    runtime->set_route_client (builder.route_client (serializers));

    zlink::framework::service_collection_t services;
    services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();

    const auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "retired-owner",
                              .node_routing_id =
                                zlink::routing_id_t::from ("retired-owner-reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    std::vector<zlink::message_t> reply_parts;
    host::receive_record_t record{
      .kind = host::record_kind_t::spot_request,
      .domain = host::ready_domain_t::application,
      .spot_route =
        zlink::framework::runtime::protocol::spot_route_fence_t{"retired-spot", 1, {}, 1, 1, 1}};
    record.reply_token.host = reply_host;
    record.reply_token.local_reply = [&reply_parts] (const std::vector<zlink::message_t> &parts) {
        reply_parts = parts;
        return true;
    };
    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::spot,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = "retired-spot"};
    messaging::envelope_codec_t codec;
    auto encoded = codec.encode_parts (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "retired-owner",
                                   .message_name = traced_request_t::packet_name,
                                   .correlation_id = "retired-request"},
      traced_request_t{7}, serializers);
    auto request_parts = std::move (encoded).take_items ();

    EXPECT_TRUE (runtime->dispatch_mesh_record (
      owner, record, request_parts,
      zlink::framework::runtime::decode_mesh_record_header (record, request_parts, false).value (),
      provider, serializers));
    ASSERT_EQ (2u, reply_parts.size ());
    const auto reply_header = codec.decode_header (messaging::message_parts_t (reply_parts));
    ASSERT_TRUE (reply_header);
    EXPECT_EQ (messaging::message_kind_t::error, reply_header.value ().kind);
    EXPECT_EQ ("unavailable", reply_header.value ().error_code.value_or (""));
    EXPECT_EQ ("Spot route owner is no longer registered",
               reply_header.value ().error_message.value_or (""));
    /* Framework-generated route errors carry the zlink.origin=framework
     * marker so callers can tell them apart from application errors. */
    const auto &reply_metadata = reply_header.value ().metadata;
    const auto origin = reply_metadata.find ("zlink.origin");
    ASSERT_NE (origin, reply_metadata.end ());
    EXPECT_EQ ("framework", origin->second);
    EXPECT_TRUE (messaging::has_framework_origin (reply_metadata));
}
} // namespace
