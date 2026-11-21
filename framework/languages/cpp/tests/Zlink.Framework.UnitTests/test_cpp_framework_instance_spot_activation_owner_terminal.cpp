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
class close_after_reply_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    close_after_reply_instance_spot_t (
      zlink::framework::instance_spot_context_t context,
      std::shared_ptr<std::optional<zlink::framework::task_t<bool>>> close_result) :
        _context (std::move (context)), _close_result (std::move (close_result))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&close_after_reply_instance_spot_t::on_request> ();
    }

    zlink::framework::task_t<reply_t> on_request (const request_t &request)
    {
        _close_result->emplace (_context.close ());
        co_return reply_t{request.value + 1};
    }

  private:
    zlink::framework::instance_spot_context_t _context;
    std::shared_ptr<std::optional<zlink::framework::task_t<bool>>> _close_result;
};

TEST (ZLinkFrameworkInstanceSpotActivation, CloseFromRequestReturnsReplyBeforeCloseCompletes)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto close_result = std::make_shared<std::optional<zlink::framework::task_t<bool>>> ();
    builder.add_route_mesh ("instance-close-after-reply")
      .add_instance_spot_factory<close_after_reply_instance_spot_t> (
        "closing-player",
        [close_result] (zlink::framework::instance_spot_context_t context) {
            return std::make_shared<close_after_reply_instance_spot_t> (std::move (context),
                                                                        close_result);
        },
        [] (auto &factory) { factory.disable_relocation (); });
    auto runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder, "instance-close-after-reply");
    ASSERT_TRUE (runtime);
    zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
      .bind_serializers (serializers);

    const auto spot_id = zlink::framework::spot_id_t ("closing-player-1");
    const auto created = runtime->get_or_create_spot ("closing-player", spot_id);
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const auto payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<request_t> ().serialize (request_t{41}));
    std::function<void ()> accepted_turn_terminal;
    const auto first =
      runtime
        ->dispatch_instance_activation (
          spot_id, request_t::packet_name, serializers.get<request_t> ().content_type (),
          payload.to_bytes (), {}, true, "close-request-1", provider, serializers, [] {},
          std::nullopt, std::nullopt, &accepted_turn_terminal)
        .result ();
    ASSERT_TRUE (first) << (first.error () ? first.error ()->what () : "unknown error");
    ASSERT_TRUE (accepted_turn_terminal);
    const auto reply = serializers.get<reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (first.value ()));
    EXPECT_EQ (42, reply.value);
    accepted_turn_terminal ();
    ASSERT_TRUE (close_result->has_value ());
    const auto closed = close_result->value ().result ();
    ASSERT_TRUE (closed);
    EXPECT_TRUE (closed.value ());

    const auto second =
      runtime
        ->dispatch_instance_activation (
          spot_id, request_t::packet_name, serializers.get<request_t> ().content_type (),
          payload.to_bytes (), {}, true, "close-request-2", provider, serializers, [] {})
        .result ();
    ASSERT_FALSE (second);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, second.error_kind ());
    ASSERT_NE (nullptr, second.error ());
    EXPECT_EQ (static_cast<std::uint32_t> (
                 zlink::framework::runtime::protocol::framework_error_code::spotMoving),
               zlink::framework::detail::failure_code (*second.error ()));
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIsNotCachedAndEachOperationAttemptsActivationOnce)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver_owner = std::make_shared<resolver_t> ();
    auto &resolver = *resolver_owner;
    runtime.bind_spot_address_resolver (resolver_owner);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "simulated cold activation rejection");
      },
      [] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, "unused"));
      });

    auto client = builder.route_client (serializers);
    EXPECT_FALSE (
      client.send_to_spot ("missing", event_t{1}).instance_spot ("quest").async ().result ());
    EXPECT_FALSE (
      client.send_to_spot ("missing", event_t{1}).instance_spot ("quest").async ().result ());

    EXPECT_EQ (2, resolver.reads.load ());
    EXPECT_EQ (2, activations.load ());
}

// Routing §2.2: ShuttingDown is not a stale-route result. A separate cache
// invalidation, such as a Message Follow notice, enables the next authority read.
TEST (ZLinkFrameworkInstanceSpotActivation, ClosingOwnerTerminalKeepsRouteUntilInvalidation)
{
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver_owner = std::make_shared<resolver_t> ();
    auto &resolver = *resolver_owner;
    resolver.addresses.insert_or_assign (
      "player-alice",
      zlink::framework::runtime::spot_address_t{
        "gamequest", zlink::routing_id_t::from ("quest-mission"), "player-alice", 7});
    runtime.bind_spot_address_resolver (resolver_owner);

    std::atomic_int cold_activations{0};
    std::atomic_int ready_failures{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
          std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&serializers, &cold_activations, &ready_failures] (
        const zlink::framework::spot_id_t &spot_id,
        const zlink::framework::detail::spot_activation_intent_t &intent,
        const std::optional<zlink::framework::runtime::spot_address_t> &cached_route, std::string,
        std::type_index, auto, std::chrono::milliseconds,
        auto) -> zlink::framework::request_task_t<zlink::message_t> {
          EXPECT_EQ ("player-alice", std::string (spot_id));
          EXPECT_EQ (std::optional<std::string> ("gamequest"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("player-quest"), intent.stable_type);
          if (cached_route) {
              ++ready_failures;
              co_return zlink::framework::detail::result_access_t::failure<zlink::message_t> (
                zlink::framework::detail::make_framework_origin_exception (
                  zlink::framework::framework_error_kind_t::shutting_down,
                  "spot serial queue is closed or stopping"));
          }
          ++cold_activations;
          /* Models OnInitialize replaying the durable event stream before the
           * activation-owned first request is dispatched. */
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::result_t<zlink::message_t>::success (
            zlink::framework::detail::encoded_payload_to_raw (
              serializers.get<reply_t> ().serialize (reply_t{3})));
      });

    messaging::envelope_codec_t envelopes;
    zlink::framework::detail::channel_reply_writer_t replies;
    std::atomic_int direct_requests{0};
    runtime.bind_spot_mesh_transport (
      "gamequest",
      [] (const auto &, const auto &, std::uint64_t,
          auto) -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure, "unused direct send");
      },
      [&direct_requests, &envelopes, &replies] (const zlink::routing_id_t &, const std::string &,
                                                std::uint64_t, messaging::message_parts_t parts,
                                                std::chrono::milliseconds)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<messaging::message_parts_t>> {
          ++direct_requests;
          const auto request_header = envelopes.decode_header (parts);
          if (!request_header) {
              co_return zlink::framework::result_t<messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::protocol_error,
                "request header decode failed");
          }
          const auto error_header =
            replies.create_error_header ("gamequest", request_header.value (),
                                         zlink::framework::detail::make_framework_origin_exception (
                                           zlink::framework::framework_error_kind_t::shutting_down,
                                           "spot serial queue is closed or stopping"));
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::result_t<messaging::message_parts_t>::success (
            replies.reply_raw_envelope (error_header, zlink::message_t::from ("")));
      });

    auto client = builder.route_client (serializers);
    const auto stale = client.request_to_spot ("player-alice", request_t{1})
                         .instance_spot ("player-quest")
                         .in_mesh ("gamequest")
                         .async<reply_t> ()
                         .result ();
    ASSERT_FALSE (stale);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::shutting_down, stale.error_kind ());
    EXPECT_EQ (0, direct_requests.load ());
    EXPECT_EQ (1, ready_failures.load ());
    EXPECT_EQ (0, cold_activations.load ());
    EXPECT_TRUE (resolver.addresses.contains ("player-alice"));

    const auto still_closing = client.request_to_spot ("player-alice", request_t{2})
                                 .instance_spot ("player-quest")
                                 .in_mesh ("gamequest")
                                 .async<reply_t> ()
                                 .result ();
    ASSERT_FALSE (still_closing);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::shutting_down,
               still_closing.error_kind ());
    EXPECT_TRUE (resolver.addresses.contains ("player-alice"));
    EXPECT_EQ (2, ready_failures.load ());
    EXPECT_EQ (0, cold_activations.load ());

    resolver.invalidate_spot_address ("player-alice");

    const auto rehydrated = client.request_to_spot ("player-alice", request_t{3})
                              .instance_spot ("player-quest")
                              .in_mesh ("gamequest")
                              .async<reply_t> ()
                              .result ();
    ASSERT_TRUE (rehydrated);
    EXPECT_EQ (3, rehydrated.value ().value);
    EXPECT_EQ (0, direct_requests.load ());
    EXPECT_EQ (2, ready_failures.load ());
    EXPECT_EQ (1, cold_activations.load ());
    EXPECT_EQ (3, resolver.reads.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, FrameworkOriginMarkerIsAttachedOnlyToFrameworkErrors)
{
    namespace messaging = zlink::framework::runtime::messaging;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::framework_exception_t;

    messaging::envelope_header_t request;
    request.kind = messaging::message_kind_t::request;
    request.channel_name = "origin-mesh";
    request.message_name = "origin.request";
    request.correlation_id = "origin-request";

    zlink::framework::detail::channel_reply_writer_t replies;

    /* Framework-generated failure: marker attached. */
    const auto framework_reply = replies.create_error_header (
      "origin-mesh", request,
      zlink::framework::detail::make_framework_origin_exception (framework_error_kind_t::not_found,
                                                                 "spot handler is not registered"));
    EXPECT_TRUE (messaging::has_framework_origin (framework_reply.metadata));

    /* Application handler failure: no marker. */
    const auto application_reply = replies.create_error_header (
      "origin-mesh", request,
      framework_exception_t (framework_error_kind_t::not_found, "application says not found"));
    EXPECT_FALSE (messaging::has_framework_origin (application_reply.metadata));
    EXPECT_EQ (application_reply.metadata.find ("zlink.origin"), application_reply.metadata.end ());

    /* The marker survives the envelope wire round trip. */
    messaging::envelope_codec_t codec;
    auto parts = codec.encode_raw_body_parts (framework_reply, zlink::message_t::from (""));
    const auto decoded = codec.decode_header (parts);
    ASSERT_TRUE (decoded);
    EXPECT_TRUE (messaging::has_framework_origin (decoded.value ().metadata));

    /* Caller-side classification: unmarked remote errors are application
     * origin and must not be read as a stale-route signal. */
    using zlink::framework::detail::error_origin_t;
    const auto marked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (decoded.value ().metadata) ? error_origin_t::framework
                                                                  : error_origin_t::application);
    EXPECT_EQ (error_origin_t::framework, zlink::framework::detail::error_origin (marked));
    const auto unmarked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (application_reply.metadata) ? error_origin_t::framework
                                                                   : error_origin_t::application);
    EXPECT_EQ (error_origin_t::application, zlink::framework::detail::error_origin (unmarked));
}
} // namespace
