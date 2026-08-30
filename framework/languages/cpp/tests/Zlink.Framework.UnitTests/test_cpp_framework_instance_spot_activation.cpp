/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace
{

struct event_t
{
    static constexpr const char *packet_name = "instance.event";
    int value{};
};

struct request_t
{
    static constexpr const char *packet_name = "instance.request";
    int value{};
};

struct reply_t
{
    int value{};
};

struct traced_event_t
{
    static constexpr const char *packet_name = "instance.traced.event";
    int value{};
};

struct traced_request_t
{
    static constexpr const char *packet_name = "instance.traced.request";
    int value{};
};

struct traced_reply_t
{
    int value{};
};

template <typename T>
requires (std::is_same_v<T, event_t> || std::is_same_v<T, request_t>
          || std::is_same_v<T, reply_t> || std::is_same_v<T, traced_event_t>
          || std::is_same_v<T, traced_request_t> || std::is_same_v<T, traced_reply_t>)
void to_json (nlohmann::json &json, const T &value)
{
    json = value.value;
}

template <typename T>
requires (std::is_same_v<T, event_t> || std::is_same_v<T, request_t>
          || std::is_same_v<T, reply_t> || std::is_same_v<T, traced_event_t>
          || std::is_same_v<T, traced_request_t> || std::is_same_v<T, traced_reply_t>)
void from_json (const nlohmann::json &json, T &value)
{
    value.value = json.get<int> ();
}

class resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        ++reads;
        const auto found = addresses.find (spot_id);
        co_return found == addresses.end ()
                    ? std::nullopt
                    : std::optional<zlink::framework::runtime::spot_address_t> (
                        found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override
    {
        addresses.clear ();
    }

    std::atomic_int reads{0};
    std::map<std::string, zlink::framework::runtime::spot_address_t> addresses;
};

class traced_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit traced_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&traced_instance_spot_t::on_event> ();
        _context.handlers ().add_handler<&traced_instance_spot_t::on_request> ();
    }

    void on_event (const traced_event_t &event)
    {
        last_event = event.value;
    }

    traced_reply_t on_request (const traced_request_t &request)
    {
        return {request.value + 1};
    }

    int last_event = 0;

  private:
    zlink::framework::instance_spot_context_t _context;
};

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIntentActivatesOnceAndReadyOwnerIgnoresPlacementHints)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder;
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::string &, std::type_index,
           std::function<zlink::framework::encoded_payload_t (
             zlink::framework::serializer_registry_t &)>,
           const std::map<std::string, std::string> &)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-17", std::string (spot_id));
          EXPECT_EQ (std::optional<std::string> ("commerce"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("shopping-cart"),
                     intent.stable_type);
          ++activations;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          resolver.addresses.insert_or_assign ("cart-17", address);
          co_return zlink::framework::result_t<void>::success ();
      },
      [] (const auto &, const auto &, std::string, std::type_index,
          auto, std::chrono::milliseconds, auto) {
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "Ready resolve should bypass cold activation"));
      });

    std::atomic_int sends{0};
    std::atomic_int requests{0};
    zlink::framework::runtime::messaging::envelope_codec_t envelopes;
    runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &node, const std::string &spot,
           std::uint64_t generation,
           zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-node", node.to_string ());
          EXPECT_EQ ("cart-17", spot);
          EXPECT_EQ (1u, generation);
          ++sends;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts,
           std::chrono::milliseconds)
        -> zlink::framework::task_t<zlink::framework::result_t<
          zlink::framework::runtime::messaging::message_parts_t>> {
          ++requests;
          auto header = envelopes.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{71};
          co_return zlink::framework::result_t<
            zlink::framework::runtime::messaging::message_parts_t>::success (
            envelopes.encode_parts (header, reply, serializers));
      });

    auto client = builder.route_client (serializers);
    const auto sent = client.send_to_spot ("cart-17", event_t{1})
                        .instance_spot ("shopping-cart")
                        .in_mesh ("commerce")
                        .submit ().result ();
    ASSERT_TRUE (sent);

    const auto reply = client.request_to_spot ("cart-17", request_t{2})
                         .instance_spot ("different-type")
                         .in_mesh ("different-mesh")
                         .submit<reply_t> ().result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (71, reply.value ().value);
    EXPECT_EQ (1, activations.load ());
    EXPECT_EQ (0, sends.load ());
    EXPECT_EQ (1, requests.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingWithoutIntentDoesNotActivate)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder;
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, auto, auto,
           const auto &)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "must not activate");
      },
      [&] (const auto &, const auto &, auto, auto, auto, auto, auto) {
          ++activations;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "must not activate"));
      });

    const auto result = builder.route_client (serializers)
                          .send_to_spot ("missing", event_t{1})
                          .submit ().result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found,
               result.error_kind ());
    EXPECT_EQ (0, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingRequestUsesDefaultTimeoutForColdActivation)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder;
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::chrono::milliseconds observed_timeout{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, auto, auto,
          const auto &)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&observed_timeout] (const auto &, const auto &, auto, auto, auto,
                           std::chrono::milliseconds timeout, auto) {
          observed_timeout = timeout;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "activation probe completed"));
      });

    const auto result = builder.route_client (serializers)
                          .request_to_spot ("cold-cart", request_t{7})
                          .instance_spot ("shopping-cart")
                          .in_mesh ("commerce")
                          .submit<reply_t> ()
                          .result ();

    EXPECT_FALSE (result);
    EXPECT_EQ (std::chrono::seconds (30), observed_timeout);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIsNotCachedAndEachOperationAttemptsActivationOnce)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder;
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, auto, auto,
           const auto &)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "simulated cold activation rejection");
      },
      [] (const auto &, const auto &, auto, auto, auto, auto, auto) {
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "unused"));
      });

    auto client = builder.route_client (serializers);
    EXPECT_FALSE (client.send_to_spot ("missing", event_t{1})
                    .instance_spot ("quest")
                    .submit ().result ());
    EXPECT_FALSE (client.send_to_spot ("missing", event_t{1})
                    .instance_spot ("quest")
                    .submit ().result ());

    EXPECT_EQ (2, resolver.reads.load ());
    EXPECT_EQ (2, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      ClosingOwnerTerminalInvalidatesBeforeNextColdActivation)
{
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder;
    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    resolver.addresses.insert_or_assign (
      "player-alice", zlink::framework::runtime::spot_address_t{
                        "gamequest", zlink::routing_id_t::from ("quest-mission"),
                        "player-alice", 7});
    runtime.bind_spot_address_resolver (resolver);

    std::atomic_int cold_activations{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, auto, auto,
          const auto &)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&serializers, &cold_activations] (
        const zlink::framework::spot_id_t &spot_id,
        const zlink::framework::detail::spot_activation_intent_t &intent,
        std::string, std::type_index, auto, std::chrono::milliseconds, auto)
        -> zlink::framework::task_t<zlink::message_t> {
          EXPECT_EQ ("player-alice", std::string (spot_id));
          EXPECT_EQ (std::optional<std::string> ("gamequest"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("player-quest"),
                     intent.stable_type);
          ++cold_activations;
          /* Models OnInitialize replaying the durable event stream before the
           * activation-owned first request is dispatched. */
          co_return zlink::framework::result_t<zlink::message_t>::success (
            zlink::framework::detail::encoded_payload_to_raw (
              serializers.get<reply_t> ().serialize (reply_t{3})));
      });

    messaging::envelope_codec_t envelopes;
    zlink::framework::detail::channel_reply_writer_t replies;
    std::atomic_int direct_requests{0};
    runtime.bind_spot_mesh_transport (
      "gamequest",
      [] (const auto &, const auto &, std::uint64_t, auto)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused direct send");
      },
      [&direct_requests, &envelopes, &replies] (
        const zlink::routing_id_t &, const std::string &, std::uint64_t,
        messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::task_t<
          zlink::framework::result_t<messaging::message_parts_t>> {
          ++direct_requests;
          const auto request_header = envelopes.decode_header (parts);
          if (!request_header) {
              co_return zlink::framework::result_t<
                messaging::message_parts_t>::failure (
                zlink::framework::framework_error_kind_t::protocol_error,
                "request header decode failed");
          }
          const auto error_header = replies.create_error_header (
            "gamequest", request_header.value (),
            zlink::framework::detail::make_framework_origin_exception (
              zlink::framework::framework_error_kind_t::shutting_down,
              "spot serial queue is closed or stopping"));
          co_return zlink::framework::result_t<
            messaging::message_parts_t>::success (
            replies.reply_raw_envelope (error_header,
                                        zlink::message_t::from ("")));
      });

    auto client = builder.route_client (serializers);
    const auto stale = client.request_to_spot ("player-alice", request_t{1})
                         .instance_spot ("player-quest")
                         .in_mesh ("gamequest")
                         .submit<reply_t> ()
                         .result ();
    ASSERT_FALSE (stale);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::shutting_down,
               stale.error_kind ());
    EXPECT_EQ (1, direct_requests.load ());
    EXPECT_EQ (0, cold_activations.load ());
    EXPECT_FALSE (resolver.addresses.contains ("player-alice"));

    const auto rehydrated = client.request_to_spot ("player-alice", request_t{2})
                              .instance_spot ("player-quest")
                              .in_mesh ("gamequest")
                              .submit<reply_t> ()
                              .result ();
    ASSERT_TRUE (rehydrated);
    EXPECT_EQ (3, rehydrated.value ().value);
    EXPECT_EQ (1, direct_requests.load ());
    EXPECT_EQ (1, cold_activations.load ());
    EXPECT_EQ (2, resolver.reads.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      RetiredOwnerRequestCompletesWithUnavailableTerminal)
{
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder;
    auto mesh = builder.add_route_mesh ("retired-owner");
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (
      builder, "retired-owner");
    ASSERT_TRUE (runtime);
    zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ())
      .bind_serializers (serializers);
    runtime->set_route_client (builder.route_client (serializers));

    zlink::framework::service_collection_t services;
    services.add_singleton<
      zlink::framework::detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();

    const auto reply_host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        .mesh = {
          .descriptor = {
            .mesh_name = "retired-owner",
            .node_routing_id =
              zlink::routing_id_t::from ("retired-owner-reply").to_bytes (),
            .lifecycle_generation = 1,
            .descriptor_revision = 1,
            .advertised_endpoint = "tcp://127.0.0.1:0"}}});
    std::vector<zlink::message_t> reply_parts;
    host::receive_record_t record{
      .kind = host::record_kind_t::spot_request,
      .domain = host::ready_domain_t::application,
      .spot_route = zlink::framework::runtime::protocol::spot_route_fence_t{
        "retired-spot", 1, {}, 1, 1, 1}};
    record.reply_token.host = reply_host;
    record.reply_token.local_reply =
      [&reply_parts] (const std::vector<zlink::message_t> &parts) {
          reply_parts = parts;
          return true;
      };
    const host::ready_record_t owner{
      .owner_kind = host::owner_kind_t::spot,
      .domain = host::ready_domain_t::application,
      .spot_id = "retired-spot"};
    messaging::envelope_codec_t codec;
    auto encoded = codec.encode_parts (
      messaging::envelope_header_t{
        .kind = messaging::message_kind_t::request,
        .channel_name = "retired-owner",
        .message_name = traced_request_t::packet_name,
        .correlation_id = "retired-request"},
      traced_request_t{7}, serializers);
    auto request_parts = std::move (encoded).take_items ();

    EXPECT_TRUE (runtime->dispatch_mesh_record (
      owner, record, request_parts, provider, serializers));
    ASSERT_EQ (2u, reply_parts.size ());
    const auto reply_header = codec.decode_header (
      messaging::message_parts_t (reply_parts));
    ASSERT_TRUE (reply_header);
    EXPECT_EQ (messaging::message_kind_t::error,
               reply_header.value ().kind);
    EXPECT_EQ ("unavailable",
               reply_header.value ().error_code.value_or (""));
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

TEST (ZLinkFrameworkInstanceSpotActivation,
      FrameworkOriginMarkerIsAttachedOnlyToFrameworkErrors)
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
      zlink::framework::detail::make_framework_origin_exception (
        framework_error_kind_t::not_found, "spot handler is not registered"));
    EXPECT_TRUE (messaging::has_framework_origin (framework_reply.metadata));

    /* Application handler failure: no marker. */
    const auto application_reply = replies.create_error_header (
      "origin-mesh", request,
      framework_exception_t (framework_error_kind_t::not_found,
                             "application says not found"));
    EXPECT_FALSE (messaging::has_framework_origin (application_reply.metadata));
    EXPECT_EQ (application_reply.metadata.find ("zlink.origin"),
               application_reply.metadata.end ());

    /* The marker survives the envelope wire round trip. */
    messaging::envelope_codec_t codec;
    auto parts =
      codec.encode_raw_body_parts (framework_reply, zlink::message_t::from (""));
    const auto decoded = codec.decode_header (parts);
    ASSERT_TRUE (decoded);
    EXPECT_TRUE (messaging::has_framework_origin (decoded.value ().metadata));

    /* Caller-side classification: unmarked remote errors are application
     * origin and must not be read as a stale-route signal. */
    using zlink::framework::detail::error_origin_t;
    const auto marked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (decoded.value ().metadata)
        ? error_origin_t::framework
        : error_origin_t::application);
    EXPECT_EQ (error_origin_t::framework,
               zlink::framework::detail::error_origin (marked));
    const auto unmarked = zlink::framework::detail::with_error_origin (
      framework_exception_t (framework_error_kind_t::not_found, "remote"),
      messaging::has_framework_origin (application_reply.metadata)
        ? error_origin_t::framework
        : error_origin_t::application);
    EXPECT_EQ (error_origin_t::application,
               zlink::framework::detail::error_origin (unmarked));
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      ColdActivationDispatchEmitsReceivedAndTerminalFlowEvents)
{
    zlink::framework::serializer_registry_t serializers;

    std::mutex events_mutex;
    std::condition_variable events_changed;
    std::vector<zlink::framework::message_flow_event_t> events;
    zlink::framework::dispatch_options_t dispatch;
    dispatch.message_flow (zlink::framework::message_flow_log_mode_t::normal);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch,
        [&] (const zlink::framework::message_flow_event_t &event) {
            {
                const std::lock_guard lock (events_mutex);
                events.push_back (event);
            }
            events_changed.notify_all ();
        });

    zlink::framework::zlink_builder_t builder;
    zlink::framework::detail::apply_dispatch_options (builder, dispatch);
    auto mesh = builder.add_route_mesh ("instance-trace");
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [] (zlink::framework::instance_spot_context_t context) {
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (
      builder, "instance-trace");
    ASSERT_TRUE (runtime);
    auto channel_runtime = zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ());
    channel_runtime.bind_serializers (serializers);

    const auto created = runtime->get_or_create_spot (
      "traced-player", zlink::framework::spot_id_t ("traced-player-1"));
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const std::string activation_flow_id =
      "019fc5b9-9df3-786b-bb69-d55358f6d48b";
    const auto event_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_event_t> ().serialize (traced_event_t{7}));
    const auto event_result = runtime->dispatch_instance_activation (
      zlink::framework::spot_id_t ("traced-player-1"), traced_event_t::packet_name,
      serializers.get<traced_event_t> ().content_type (), event_payload.to_bytes (), {}, false,
      "operation-send", provider,
      serializers, activation_flow_id, zlink::framework::flow_origin_t::application)
                                 .result ();
    ASSERT_TRUE (event_result);

    const auto request_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_request_t> ().serialize (traced_request_t{9}));
    const auto request_result = runtime->dispatch_instance_activation (
      zlink::framework::spot_id_t ("traced-player-1"), traced_request_t::packet_name,
      serializers.get<traced_request_t> ().content_type (), request_payload.to_bytes (), {}, true,
      "operation-request", provider,
      serializers, activation_flow_id, zlink::framework::flow_origin_t::application)
                                   .result ();
    ASSERT_TRUE (request_result);
    const auto decoded_reply = serializers.get<traced_reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (request_result.value ()));
    EXPECT_EQ (10, decoded_reply.value);

    {
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (
          lock, std::chrono::seconds (2), [&] { return events.size () >= 4; }));
    }

    const auto has_event_transition = [&] (std::string_view packet,
                                           zlink::framework::message_flow_outcome_t outcome,
                                           std::string_view correlation) {
        const std::lock_guard lock (events_mutex);
        return std::any_of (events.begin (), events.end (), [&] (const auto &event) {
            return event.packet_name && *event.packet_name == packet
                   && event.outcome == outcome
                   && event.surface == zlink::framework::dispatch_error_surface_t::spot_route
                   && event.spot_id && *event.spot_id == "traced-player-1"
                   && event.correlation_id && *event.correlation_id == correlation
                   && event.flow_id && *event.flow_id == activation_flow_id
                   && event.flow_origin
                   && *event.flow_origin == zlink::framework::flow_origin_t::application;
        });
    };
    EXPECT_TRUE (has_event_transition (
      traced_event_t::packet_name, zlink::framework::message_flow_outcome_t::received,
      "operation-send"));
    EXPECT_TRUE (has_event_transition (
      traced_event_t::packet_name, zlink::framework::message_flow_outcome_t::dispatched,
      "operation-send"));
    EXPECT_TRUE (has_event_transition (
      traced_request_t::packet_name, zlink::framework::message_flow_outcome_t::received,
      "operation-request"));
    EXPECT_TRUE (has_event_transition (
      traced_request_t::packet_name, zlink::framework::message_flow_outcome_t::replied,
      "operation-request"));
}

} // namespace
