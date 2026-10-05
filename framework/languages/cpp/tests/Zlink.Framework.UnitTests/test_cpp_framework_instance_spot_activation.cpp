/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include "runtime_context_hooks_environment.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
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
        if (lookup_gate)
            co_await lookup_gate->task ();
        const auto found = addresses.find (spot_id);
        co_return found == addresses.end ()
          ? std::nullopt
          : std::optional<zlink::framework::runtime::spot_address_t> (found->second);
    }

    void invalidate_spot_address (std::string_view spot_id) override
    {
        addresses.erase (std::string (spot_id));
    }

    void invalidate_all_routes_after_store_recovery () override { addresses.clear (); }

    std::atomic_int reads{0};
    std::shared_ptr<zlink::framework::task_completion_source_t<void>> lookup_gate;
    std::map<std::string, zlink::framework::runtime::spot_address_t> addresses;
};

class traced_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit traced_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<&traced_instance_spot_t::on_event> ();
        _context.handlers ().add_handler<&traced_instance_spot_t::on_request> ();
    }

    void on_event (const traced_event_t &event) { last_event = event.value; }

    traced_reply_t on_request (const traced_request_t &request) { return {request.value + 1}; }

    int last_event = 0;

  private:
    zlink::framework::instance_spot_context_t _context;
};

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
          payload.to_bytes (), {}, true, "close-request-1", provider, serializers, std::nullopt,
          std::nullopt, &accepted_turn_terminal)
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
          payload.to_bytes (), {}, true, "close-request-2", provider, serializers)
        .result ();
    ASSERT_FALSE (second);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found, second.error_kind ());
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIntentActivatesOnceAndReadyOwnerIgnoresPlacementHints)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::atomic_int activations{0};
    std::atomic_int ready_requests{0};
    runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::optional<zlink::framework::runtime::spot_address_t> &cached_route,
           const std::string &, std::type_index,
           std::function<zlink::framework::serialized_payload_t (
             zlink::framework::serializer_registry_t &)>,
           const std::map<std::string, std::string> &, std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-17", std::string (spot_id));
          EXPECT_FALSE (cached_route);
          EXPECT_EQ (std::optional<std::string> ("commerce"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("shopping-cart"), intent.stable_type);
          ++activations;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          resolver.addresses.insert_or_assign ("cart-17", address);
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const auto &, const auto &, const auto &cached_route, std::string, std::type_index, auto,
           std::chrono::milliseconds, auto) {
          EXPECT_TRUE (cached_route);
          EXPECT_EQ ("commerce", cached_route->mesh_name);
          EXPECT_EQ ("cart-node", cached_route->node_rid.to_string ());
          ++ready_requests;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (
                serializers.get<reply_t> ().serialize (reply_t{71}))));
      });

    std::atomic_int sends{0};
    std::atomic_int requests{0};
    zlink::framework::runtime::messaging::envelope_codec_t envelopes;
    runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &node, const std::string &spot, std::uint64_t generation,
           zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-node", node.to_string ());
          EXPECT_EQ ("cart-17", spot);
          EXPECT_EQ (1u, generation);
          ++sends;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          ++requests;
          auto header = envelopes.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{71};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelopes.encode_parts (header, reply, serializers));
      });

    auto client = builder.route_client (serializers);
    const auto sent = client.send_to_spot ("cart-17", event_t{1})
                        .instance_spot ("shopping-cart")
                        .in_mesh ("commerce")
                        .async ()
                        .result ();
    ASSERT_TRUE (sent);

    const auto reply = client.request_to_spot ("cart-17", request_t{2})
                         .instance_spot ("different-type")
                         .in_mesh ("different-mesh")
                         .async<reply_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (71, reply.value ().value);
    EXPECT_EQ (1, activations.load ());
    EXPECT_EQ (0, sends.load ());
    EXPECT_EQ (0, requests.load ());
    EXPECT_EQ (1, ready_requests.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, MissingWithoutIntentDoesNotActivate)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure, "must not activate");
      },
      [&] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          ++activations;
          return zlink::framework::task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, "must not activate"));
      });

    const auto result =
      builder.route_client (serializers).send_to_spot ("missing", event_t{1}).async ().result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found, result.error_kind ());
    EXPECT_EQ (0, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, ActivationKeepsSubmissionStartAcrossRouteLookup)
{
    namespace fw = zlink::framework;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    resolver.lookup_gate = std::make_shared<fw::task_completion_source_t<void>> ();
    runtime.bind_spot_address_resolver (resolver);
    std::optional<std::chrono::system_clock::time_point> observed_start;
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point started_at) -> fw::task_t<fw::result_t<void>> {
          observed_start = started_at;
          co_return fw::result_t<void>::success ();
      },
      [] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          return fw::task_t<zlink::message_t> (fw::result_t<zlink::message_t>::failure (
            fw::framework_error_kind_t::internal_failure, "unused request activation"));
      });
    const auto before = std::chrono::system_clock::now ();
    auto send = builder.route_client (serializers)
                  .send_to_spot ("cold", event_t{1})
                  .instance_spot ("traced-player")
                  .async ();
    const auto lookup_started = std::chrono::system_clock::now ();
    EXPECT_EQ (1, resolver.reads.load ());
    EXPECT_FALSE (send.await_ready ());
    EXPECT_FALSE (observed_start);
    resolver.lookup_gate->complete (fw::result_t<void>::success ());
    ASSERT_TRUE (send.result ());
    ASSERT_TRUE (observed_start);
    EXPECT_GE (*observed_start, before);
    EXPECT_LE (*observed_start, lookup_started);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      OneWayColdActivationUsesSourceRequestBudgetWithoutEndingSend)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    auto &options = app.add_zlink_framework ();
    class lookup_gate_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
        {
            if (key.value.find ("budget-player-1") != std::string::npos && !held.exchange (true)) {
                entered.set_value ();
                co_await release.task ();
            }
            co_return co_await inner.read (std::move (key));
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
        {
            return inner.scan (std::move (request));
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
        {
            return inner.write (std::move (request));
        }
        fw::runtime::in_memory_location_store_t inner;
        std::atomic_bool held{false};
        std::promise<void> entered;
        fw::task_completion_source_t<void> release;
    };
    auto store = std::make_shared<lookup_gate_store_t> ();
    auto lookup_entered = store->entered.get_future ();
    options.add_location_store (store);
    options.add_relocation_store (std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    options.configure_locations ().polling_interval = 10ms;
    std::shared_ptr<traced_instance_spot_t> instance;
    options.add_route_mesh ("activation-budget")
      .set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("activation-budget-node"))
      .listen ("tcp://127.0.0.1:0")
      .set_default_request_timeout (80ms)
      .add_instance_spot_factory<traced_instance_spot_t> (
        "traced-player",
        [&] (fw::instance_spot_context_t context) {
            instance = std::make_shared<traced_instance_spot_t> (std::move (context));
            return instance;
        },
        [] (auto &factory) { factory.disable_relocation (); });
    char command[] = "instance-activation-budget";
    char *argv[] = {command};
    int exit_code = -1;
    std::thread host ([&] { exit_code = app.run (1, argv); });
    auto cleanup =
      std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
          store->release.complete (fw::result_t<void>::success ());
          app.stop ();
          host.join ();
      });
    const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    auto provider = app.advanced ().services ().build_provider ();
    auto &client = provider.get_required<fw::route_client_t> ();
    auto send = client.send_to_spot ("budget-player-1", traced_event_t{11})
                  .instance_spot ("traced-player")
                  .in_mesh ("activation-budget")
                  .async ();
    ASSERT_EQ (std::future_status::ready, lookup_entered.wait_for (3s));
    std::this_thread::sleep_for (240ms);
    EXPECT_FALSE (send.await_ready ());
    store->release.complete (fw::result_t<void>::success ());
    // The expired activation deadline reaches the target in ZLIA; send admission succeeds.
    const auto admitted = send.result_for (1s);
    ASSERT_TRUE (admitted.has_value ());
    ASSERT_TRUE (*admitted);
    const auto reply = client.request_to_spot ("budget-player-1", traced_request_t{7})
                         .instance_spot ("traced-player")
                         .in_mesh ("activation-budget")
                         .timeout (3s)
                         .async<traced_reply_t> ()
                         .result_for (4s);
    ASSERT_TRUE (reply.has_value ());
    ASSERT_TRUE (*reply);
    ASSERT_TRUE (instance);
    EXPECT_EQ (0, instance->last_event);
    EXPECT_TRUE (send.result ());
    cleanup.reset ();
    EXPECT_EQ (0, exit_code);
}

TEST (ZLinkFrameworkInstanceSpotActivation, MissingRequestUsesDefaultTimeoutForColdActivation)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);

    std::chrono::milliseconds observed_timeout{0};
    runtime.bind_instance_spot_activator (
      [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
          std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "unused one-way activation");
      },
      [&observed_timeout] (const auto &, const auto &, const auto &, auto, auto, auto,
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
                          .async<reply_t> ()
                          .result ();

    EXPECT_FALSE (result);
    EXPECT_EQ (std::chrono::seconds (30), observed_timeout);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIsNotCachedAndEachOperationAttemptsActivationOnce)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    runtime.bind_spot_address_resolver (resolver);
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
          return zlink::framework::task_t<zlink::message_t> (
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

TEST (ZLinkFrameworkInstanceSpotActivation, ClosingOwnerTerminalInvalidatesBeforeNextColdActivation)
{
    namespace messaging = zlink::framework::runtime::messaging;

    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    resolver_t resolver;
    resolver.addresses.insert_or_assign (
      "player-alice",
      zlink::framework::runtime::spot_address_t{
        "gamequest", zlink::routing_id_t::from ("quest-mission"), "player-alice", 7});
    runtime.bind_spot_address_resolver (resolver);

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
        auto) -> zlink::framework::task_t<zlink::message_t> {
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
        -> zlink::framework::task_t<zlink::framework::result_t<messaging::message_parts_t>> {
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
    EXPECT_FALSE (resolver.addresses.contains ("player-alice"));

    const auto rehydrated = client.request_to_spot ("player-alice", request_t{2})
                              .instance_spot ("player-quest")
                              .in_mesh ("gamequest")
                              .async<reply_t> ()
                              .result ();
    ASSERT_TRUE (rehydrated);
    EXPECT_EQ (3, rehydrated.value ().value);
    EXPECT_EQ (0, direct_requests.load ());
    EXPECT_EQ (1, ready_failures.load ());
    EXPECT_EQ (1, cold_activations.load ());
    EXPECT_EQ (2, resolver.reads.load ());
}

// Failover policy §4.4: an Instance intent request whose cached Ready route the
// owner fence refused before admission reads the authority once. Missing continues
// as cold activation; a present authority or another terminal ends the operation.
TEST (ZLinkFrameworkInstanceSpotActivation, CachedRouteFenceRefusalReadsAuthorityOnce)
{
    namespace messaging = zlink::framework::runtime::messaging;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::spot_address_t;

    class authority_resolver_t final : public zlink::framework::runtime::spot_address_resolver_t
    {
      public:
        zlink::framework::task_t<std::optional<spot_address_t>>
        resolve_spot_address (std::string, std::string) override
        {
            if (cached)
                co_return cached;
            ++authority_reads;
            co_return authority;
        }

        void invalidate_spot_address (std::string_view) override { cached.reset (); }

        void invalidate_all_routes_after_store_recovery () override { cached.reset (); }

        std::optional<spot_address_t> cached;
        std::optional<spot_address_t> authority;
        int authority_reads = 0;
    };

    const messaging::request_failure_mapper_t mapper;
    // The owner's Ready owner-fence refusal, and the Unavailable terminal of a message
    // the owner accepted (a relocating host ending the Instance intent messages it kept).
    const auto refusal = *mapper.target_failure_reply (
      framework_error_kind_t::unavailable,
      static_cast<std::uint32_t> (
        zlink::framework::runtime::protocol::framework_error_code::spotMoving));
    const auto accepted = *mapper.target_failure_reply (framework_error_kind_t::unavailable);
    struct case_t
    {
        messaging::request_wire_failure_t owner_failure;
        bool authority_present;
        bool replied;
        std::vector<bool> cached_routes;
        int authority_reads;
    };
    for (const auto &scenario :
         {case_t{refusal, false, true, {true, false}, 1}, case_t{refusal, true, false, {true}, 1},
          case_t{accepted, false, false, {true}, 0}}) {
        zlink::framework::serializer_registry_t serializers;
        zlink::framework::zlink_builder_t builder =
          zlink::framework::test::runtime_failure_builder ();
        auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        const spot_address_t route{"gamequest", zlink::routing_id_t::from ("quest-mission"),
                                   "player-alice", 7};
        authority_resolver_t resolver;
        resolver.cached = route;
        if (scenario.authority_present)
            resolver.authority = route;
        runtime.bind_spot_address_resolver (resolver);
        std::vector<bool> cached_routes;
        runtime.bind_instance_spot_activator (
          [] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
              std::chrono::system_clock::time_point)
            -> zlink::framework::task_t<zlink::framework::result_t<void>> {
              co_return zlink::framework::result_t<void>::failure (
                framework_error_kind_t::internal_failure, "unused one-way activation");
          },
          [&] (const auto &, const auto &, const std::optional<spot_address_t> &cached_route,
               std::string, std::type_index, auto, std::chrono::milliseconds,
               auto) -> zlink::framework::task_t<zlink::message_t> {
              cached_routes.push_back (cached_route.has_value ());
              if (cached_route)
                  co_return zlink::framework::detail::result_access_t::failure<zlink::message_t> (
                    mapper.reply_header_exception (scenario.owner_failure.terminal_result,
                                                   scenario.owner_failure.failure_code,
                                                   "Instance Spot request"));
              co_return zlink::framework::result_t<zlink::message_t>::success (
                zlink::framework::detail::encoded_payload_to_raw (
                  serializers.get<reply_t> ().serialize (reply_t{9})));
          });

        const auto reply = builder.route_client (serializers)
                             .request_to_spot ("player-alice", request_t{1})
                             .instance_spot ("player-quest")
                             .async<reply_t> ()
                             .result ();
        EXPECT_EQ (scenario.replied, static_cast<bool> (reply));
        if (reply) {
            EXPECT_EQ (9, reply.value ().value);
        } else {
            EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
            ASSERT_TRUE (reply.error ());
            EXPECT_EQ (scenario.owner_failure.failure_code,
                       zlink::framework::detail::failure_code (*reply.error ()));
        }
        EXPECT_EQ (scenario.cached_routes, cached_routes);
        EXPECT_EQ (scenario.authority_reads, resolver.authority_reads);
    }
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
                              .advertised_endpoint = "tcp://127.0.0.1:0"}}});
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

    EXPECT_TRUE (
      runtime->dispatch_mesh_record (owner, record, request_parts, provider, serializers));
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
      dispatch, [&] (const zlink::framework::message_flow_event_t &event) {
          {
              const std::lock_guard lock (events_mutex);
              events.push_back (event);
          }
          events_changed.notify_all ();
      });

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    zlink::framework::detail::apply_dispatch_options (builder, dispatch);
    auto mesh = builder.add_route_mesh ("instance-trace");
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [] (zlink::framework::instance_spot_context_t context) {
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder, "instance-trace");
    ASSERT_TRUE (runtime);
    auto channel_runtime =
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    channel_runtime.bind_serializers (serializers);

    const auto created = runtime->get_or_create_spot (
      "traced-player", zlink::framework::spot_id_t ("traced-player-1"));
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const std::string activation_flow_id = "019fc5b9-9df3-786b-bb69-d55358f6d48b";
    const auto event_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_event_t> ().serialize (traced_event_t{7}));
    const auto event_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_event_t::packet_name,
          serializers.get<traced_event_t> ().content_type (), event_payload.to_bytes (), {}, false,
          "operation-send", provider, serializers, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (event_result);

    const auto request_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_request_t> ().serialize (traced_request_t{9}));
    const auto request_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_request_t::packet_name,
          serializers.get<traced_request_t> ().content_type (), request_payload.to_bytes (), {},
          true, "operation-request", provider, serializers, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (request_result);
    const auto decoded_reply = serializers.get<traced_reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (request_result.value ()));
    EXPECT_EQ (10, decoded_reply.value);

    {
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (lock, std::chrono::seconds (2),
                                              [&] { return events.size () >= 4; }));
    }

    const auto has_event_transition = [&] (std::string_view packet,
                                           zlink::framework::message_flow_outcome_t outcome,
                                           std::string_view correlation) {
        const std::lock_guard lock (events_mutex);
        return std::any_of (events.begin (), events.end (), [&] (const auto &event) {
            return event.packet_name && *event.packet_name == packet && event.outcome == outcome
                   && event.surface == zlink::framework::dispatch_error_surface_t::spot_route
                   && event.spot_id && *event.spot_id == "traced-player-1" && event.correlation_id
                   && *event.correlation_id == correlation && event.flow_id
                   && *event.flow_id == activation_flow_id && event.flow_origin
                   && *event.flow_origin == zlink::framework::flow_origin_t::application;
        });
    };
    EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::received,
                                       "operation-send"));
    EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::dispatched,
                                       "operation-send"));
    EXPECT_TRUE (has_event_transition (traced_request_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::received,
                                       "operation-request"));
    EXPECT_TRUE (has_event_transition (traced_request_t::packet_name,
                                       zlink::framework::message_flow_outcome_t::replied,
                                       "operation-request"));
}

} // namespace
