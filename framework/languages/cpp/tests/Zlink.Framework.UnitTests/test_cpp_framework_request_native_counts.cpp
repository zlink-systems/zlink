/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "native_call_counter.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include <zlink/framework.hpp>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <future>
#include <thread>
#include <cstdio>
#include <zlink.h>
namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static bool complete_join (public_host_runtime_t &host,
                               pending_operation_t operation,
                               const std::vector<zlink::message_t> &parts)
    {
        return host.complete_local_actor_join (
          std::move (operation), "count-actor", {},
          result_t<actor_join_operation_result_t>::failure (framework_error_kind_t::unavailable,
                                                            "counted join failure"),
          parts);
    }
};
}
namespace
{
namespace fw = zlink::framework;
namespace wire = fw::runtime::protocol;
using fw::tests::native_call_scope_t;

bool service_control_message (const void *data, std::size_t size) noexcept
{
    const auto bytes = std::span (static_cast<const std::uint8_t *> (data), size);
    if (bytes.size () < wire::prefix_size
        || !std::equal (std::begin (wire::magic), std::end (wire::magic), bytes.begin ())
        || bytes[sizeof (wire::magic)] != wire::wire_major)
        return false;
    const auto header = wire::decode_header (bytes);
    return header.kind == wire::command::hello || header.kind == wire::command::admit
           || header.kind == wire::command::reject || header.kind == wire::command::update
           || header.kind == wire::command::livenessProbe
           || header.kind == wire::command::livenessAck;
}

TEST (RequestNativeCounts, BorrowedCodecViewDoesNotInitializeAnUnusedNativeMessage)
{
    auto message = zlink::message_t::from (std::string (128, 'x'));
    native_call_scope_t calls;
    {
        const auto payload = fw::detail::encoded_payload_from_raw (message);
        EXPECT_EQ (payload.bytes ().data (), message.bytes ().data ());
    }
    std::printf ("borrowed codec init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 0u);
}
TEST (RequestNativeCounts, OwnedCodecBridgeMovesItsNativeStorage)
{
    auto message = zlink::message_t::from (std::string (128, 'x'));
    const auto *address = message.bytes ().data ();
    native_call_scope_t calls;
    {
        auto payload = fw::detail::encoded_payload_from_raw (std::move (message));
        auto outgoing = fw::detail::encoded_payload_to_raw (std::move (payload));
        EXPECT_EQ (outgoing.bytes ().data (), address);
    }
    std::printf ("owned codec init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 1u);
}
TEST (RequestNativeCounts, PublicCopyOfBorrowedCodecPayloadOwnsItsBytes)
{
    auto message = zlink::message_t::from (std::string (128, 'x'));
    const auto borrowed = fw::detail::encoded_payload_from_raw (message);
    auto copied = borrowed;
    message = zlink::message_t::from (std::string (128, 'y'));
    EXPECT_EQ (copied.to_string (), std::string (128, 'x'));
    EXPECT_NE (copied.bytes ().data (), message.bytes ().data ());
}
TEST (RequestNativeCounts, RegistryReservationDoesNotCreateAPayloadPlaceholder)
{
    fw::runtime::foundation::operation_registry_t registry;
    native_call_scope_t calls;
    ASSERT_TRUE (registry.register_operation (
      {81, 1}, std::chrono::steady_clock::time_point::max (), [] (auto, auto) {}));
    std::printf ("registry reservation init=%zu copy=%zu close=%zu\n", calls[0], calls[3],
                 calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 0u);
    EXPECT_TRUE (registry.unregister ({81, 1}));
}
#ifndef J1672_BEFORE
TEST (RequestNativeCounts, PackedReplyViewsStayUnderTheirParentWithoutNativeSlices)
{
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::string (128, 'h')));
    parts.push_back (zlink::message_t::from (std::string (256, 'b')));
    auto packed = zlink::message_t::from (wire::encode_application_payload (
      wire::application_payload_t::from_parts (std::move (parts))));
    native_call_scope_t calls;
    auto views = wire::decode_application_part_views (wire::message_bytes (packed), false);
    ASSERT_EQ (views.size (), 2u);
    EXPECT_EQ (views[0].size (), 128u);
    EXPECT_EQ (views[1].size (), 256u);
    EXPECT_GE (views[0].data (), packed.bytes ().data ());
    EXPECT_LE (views[1].data () + views[1].size (), packed.bytes ().data () + packed.size ());
    EXPECT_EQ (calls[0] + calls[1] + calls[2] + calls[3] + calls[4], 0u);
    auto malformed = packed.to_bytes ();
    malformed.pop_back ();
    EXPECT_THROW ((void) wire::decode_application_part_views (malformed, false),
                  wire::service_wire_error_t);
    auto borrowed = fw::runtime::messaging::message_parts_t::borrow (views);
    auto owned = borrowed;
    EXPECT_EQ (owned.items ()[1].size (), 256u);
    packed = zlink::message_t{};
    EXPECT_EQ (owned.items ()[1].to_bytes (), std::vector<std::uint8_t> (256, 'b'));
}
TEST (RequestNativeCounts, WaitReadinessIsConsumedWithoutAnotherPollEvenAtPermitZero)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("readiness-owner");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("readiness-owner");
    state->core_context = std::make_shared<zlink::context_t> ();
    fw::detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    const auto discard = [] (const auto &, const auto &, auto, auto) {};
    {
        native_call_scope_t calls;
        const auto progressed =
          node.dispatch_ready (discard, false, {}, zlink::poll_event_flag_t::pollcompletion)
            .result ()
            .value ();
        EXPECT_GE (progressed, 1u);
        EXPECT_EQ (calls[5], 0u);
        const auto idle = node.dispatch_ready (discard, false, {}, zlink::poll_event_flag_t::none)
                            .result ()
                            .value ();
        EXPECT_EQ (idle, 0u);
        EXPECT_EQ (calls[5], 0u);
        (void) node.dispatch_ready (discard, false).result ().value ();
        EXPECT_GT (calls[5], 0u);
    }
    node.stop ();
}
#endif
struct count_payload_t
{
    static constexpr const char *packet_name = "CountPayload";
    int value = 7;
};

TEST (RequestNativeCounts, TypedChannelReplyBorrowsItsNativeBody)
{
    using parts_t = fw::runtime::messaging::message_parts_t;
    using transport_t = fw::result_t<parts_t>;
    fw::serializer_registry_t serializers;
    serializers.add<count_payload_t> (
      [] (const count_payload_t &v) {
          return fw::encoded_payload_t::from_string (std::to_string (v.value));
      },
      [] (const fw::encoded_payload_t &v) { return count_payload_t{std::stoi (v.to_string ())}; },
      "x/count");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    std::optional<fw::detail::request_receiver_t<transport_t>> selected;
    runtime.bind_mesh_node_transport ("m", {}, [&] (const auto &, auto, auto, auto receiver) {
        receiver.admitted->complete (fw::result_t<void>::success ());
        selected.emplace (std::move (receiver));
    });
    auto client = builder.route_client (serializers);
    auto reply = client.request_to_node ("m", zlink::routing_id_t::from ("peer"), count_payload_t{})
                   .async<count_payload_t> ();
    fw::runtime::messaging::client_call_codec_t codec;
    auto header =
      codec.create_envelope (fw::runtime::messaging::message_kind_t::response, "m", "CountPayload");
    auto parts = codec.encode_payload_parts (
      std::move (header),
      serializers.get<count_payload_t> ().serialize_with_content_type (count_payload_t{41}));
    auto terminal = fw::result_t<transport_t>::success (transport_t::success (std::move (parts)));
    native_call_scope_t calls;
    selected->complete (terminal);
    ASSERT_TRUE (reply.result ());
    EXPECT_EQ (reply.result ().value ().value, 41);
    std::printf ("typed channel init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 0u);
}
TEST (RequestNativeCounts, TypedEnvelopeReplyBorrowsItsBody)
{
    fw::serializer_registry_t serializers;
    serializers.add<count_payload_t> (
      [] (const count_payload_t &v) {
          return fw::encoded_payload_t::from_string (std::to_string (v.value));
      },
      [] (const fw::encoded_payload_t &v) { return count_payload_t{std::stoi (v.to_string ())}; },
      "x/count");
    fw::runtime::messaging::client_call_codec_t codec;
    auto header =
      codec.create_envelope (fw::runtime::messaging::message_kind_t::response, "m", "CountPayload");
    auto parts = codec.encode_payload_parts (
      std::move (header),
      serializers.get<count_payload_t> ().serialize_with_content_type (count_payload_t{41}));
    native_call_scope_t calls;
    auto decoded = codec.decode_envelope_reply<count_payload_t> (
      parts, serializers, "missing reply", "bad reply", "native count");
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().value, 41);
    std::printf ("typed envelope init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 0u);
}

TEST (RequestNativeCounts, HostResultProjectionBorrowsItsNativeParts)
{
    using reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("projection-count");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t operation;
    fw::runtime::host::operation_completion_t result;
    result.parts.push_back (zlink::message_t::from (std::string (128, 'x')));
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (result)));
    auto submission = fw::task_t<zlink::submit_result_t> (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    native_call_scope_t calls;
    auto output = fw::detail::request_task_access_t::receive_public<int, reply_t> (
      [&] (auto receiver) {
          runtime.compose_node_request_reply (operation, std::move (submission),
                                              std::move (receiver));
      },
      [] (const fw::result_t<reply_t> &terminal) {
          return fw::result_t<int>::success (
            static_cast<int> (terminal.value ().value ().items ()[0].size ()));
      });
    ASSERT_TRUE (output.result ());
    EXPECT_EQ (output.result ().value (), 128);
    std::printf ("host projection init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 0u);
    EXPECT_EQ (calls[3], 0u);
    EXPECT_EQ (calls[4], 0u);
}
TEST (RequestNativeCounts, LocalActorJoinMovesTheCapturedReplyParts)
{
    fw::completion_owner_t owner;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("join-copy-count");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("join-copy-count");
    state->object_role = fw::object_role_t::none;
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.start ().result ().value ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    auto host = runtime.native_node ().shared_from_this ();
    fw::runtime::host::pending_operation_t operation;
    operation.local_result = std::make_shared<fw::runtime::host::operation_completion_t> ();
    std::promise<void> finished;
    const auto registered = host->transport ()
                              .register_local_operation_async (
                                std::chrono::steady_clock::now () + std::chrono::seconds (5),
                                [&] (auto, auto) { finished.set_value (); })
                              .result ();
    ASSERT_TRUE (registered);
    ASSERT_TRUE (registered.value ());
    operation.id = *registered.value ();
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::string (128, 'j')));
    native_call_scope_t calls (service_control_message);
    ASSERT_TRUE (fw::runtime::host::public_host_runtime_test_access_t::complete_join (
      *host, operation, parts));
    ASSERT_EQ (finished.get_future ().wait_for (std::chrono::seconds (5)),
               std::future_status::ready);
    ASSERT_EQ (operation.local_result->parts.size (), 1u);
    EXPECT_EQ (operation.local_result->parts[0].to_string (), std::string (128, 'j'));
    EXPECT_EQ (parts[0].to_string (), std::string (128, 'j'));
    std::printf ("local actor join copies=%zu\n", calls[3]);
    EXPECT_EQ (calls[3], 1u); // Only the borrowed input's ownership copy.
}

struct relay_capture_t final : fw::hosted_service_t
{
    fw::detail::actor_gateway_state_t::relay_dispatcher_t relay;
    fw::detail::actor_gateway_runtime_t *gateway = nullptr;
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        gateway = &services.get_required<fw::detail::actor_gateway_runtime_t> ();
        relay = gateway->weak_state ().lock ()->relay_dispatcher;
        co_return;
    }
    void stop () noexcept override {}
};

TEST (RequestNativeCounts, AppRelayMovesItsOwnedPayloadIntoDelivery)
{
    auto app = fw::app_t::create ();
    auto capture = std::make_unique<relay_capture_t> ();
    auto *service = capture.get ();
    app.add_hosted_service (std::move (capture));
    app.use_metric_provider (opentelemetry::metrics::Provider::GetMeterProvider ());
    auto &options = app.add_zlink_framework ();
    options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
    options.add_route_mesh ("relay-copy-count")
      .set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("relay-copy-count"));
    std::atomic<bool> exited{false};
    std::thread runner ([&] {
        app.run (0, nullptr);
        exited.store (true);
    });
    struct stop_t
    {
        fw::app_t &app;
        std::thread &runner;
        ~stop_t ()
        {
            app.request_stop ();
            runner.join ();
        }
    } stop{app, runner};
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!app.is_ready () && !exited.load () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    ASSERT_TRUE (service->relay);
    const auto actor = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("missing-copy-target"), "count-actor", "a", 1);
    fw::detail::stream_header_t header;
    auto payload = zlink::message_t::from (std::string (128, 'r'));
    native_call_scope_t calls (service_control_message);
    auto delivered = fw::detail::request_submission (
      service->relay (actor, service->gateway->actor_context (actor), header, payload, {},
                      std::chrono::milliseconds (100)));
    const auto result = delivered.reply.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (payload.to_string (), std::string (128, 'r'));
    std::printf ("app relay copies=%zu\n", calls[3]);
    EXPECT_EQ (calls[3], 2u); // Borrowed relay input and the Mesh value parameter.
}

TEST (RequestNativeCounts, NativeCountersDetectAnInjectedCopyAndItsLifecycle)
{
    auto source = zlink::message_t::from (std::string (128, 'x'));
    zlink_msg_t consumed[2];
    for (auto &message : consumed)
        ASSERT_EQ (0, zlink_msg_init (&message));
    native_call_scope_t calls (service_control_message);
    {
        auto injected = source.copy ();
        EXPECT_EQ (injected.size (), 128u);
    }
    std::size_t index = 0;
    for (const auto command : {wire::command::livenessProbe, wire::command::livenessAck}) {
        auto bytes = wire::encode_liveness (command, 1);
        zlink_msg_t control, borrowed;
        EXPECT_EQ (0, zlink_msg_init_data (
                        &control, bytes.data (), bytes.size (), [] (void *, void *) {}, nullptr));
        EXPECT_EQ (0, zlink_msg_init (&borrowed));
        EXPECT_EQ (0, zlink_msg_copy (&borrowed, &control));
        // Core consumes the submitted view. Its empty close still belongs to
        // the control message, whose public owner retains the original bytes.
        EXPECT_EQ (0, zlink_msg_move (&consumed[index], &borrowed));
        EXPECT_EQ (0u, zlink_msg_size (&borrowed));
        EXPECT_EQ (0, zlink_msg_close (&borrowed));
        EXPECT_EQ (0, zlink_msg_close (&control));
        EXPECT_EQ (0, zlink_msg_close (&consumed[index++]));
    }
    std::printf ("injected copy init=%zu copy=%zu close=%zu\n", calls[0], calls[3], calls[4]);
    EXPECT_EQ (calls[0], 1u);
    EXPECT_EQ (calls[3], 1u);
    EXPECT_EQ (calls[4], 1u);
}
TEST (RequestNativeCounts, RealRoundTripReportsSourceNativeCallsPerRequest)
{
    using namespace std::chrono_literals;
    auto context = std::make_shared<zlink::context_t> ();
    auto source_state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("native-count");
    auto target_state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("native-count");
    source_state->listen_endpoint = target_state->listen_endpoint = "tcp://127.0.0.1:0";
    source_state->routing_id = zlink::routing_id_t::from ("count-source");
    target_state->routing_id = zlink::routing_id_t::from ("count-target");
    source_state->core_context = target_state->core_context = context;
    fw::detail::mesh_node_runtime_t source (source_state), target (target_state);
    target.start ().result ().value ();
    source.start ().result ().value ();
    const auto target_status = target.status ();
    source.connect_peer (target_status.routing_id (), target_status.local_endpoint (),
                         target_status.lifecycle_generation ());
    const auto discard = [] (const auto &, const auto &, auto, auto) {};
    const auto connected = std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (target_status.routing_id (),
                                       target_status.lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < connected) {
        source.dispatch_ready (discard).result ().value ();
        target.dispatch_ready (discard).result ().value ();
    }
    ASSERT_TRUE (source.has_admitted_peer (target_status.routing_id (),
                                           target_status.lifecycle_generation ()));
    ASSERT_TRUE (target.has_admitted_peer (source.status ().routing_id (),
                                           source.status ().lifecycle_generation ()));
    std::shared_ptr<fw::task_completion_source_t<void>> physical_admission;
    fw::serializer_registry_t serializers;
    serializers.add<count_payload_t> (
      [] (const count_payload_t &v) {
          return fw::encoded_payload_t::from_string (std::to_string (v.value));
      },
      [&physical_admission] (const fw::encoded_payload_t &v) {
          EXPECT_NE (fw::detail::current_completion_owner (), nullptr);
          EXPECT_TRUE (physical_admission && physical_admission->task ().await_ready ());
          return count_payload_t{std::stoi (v.to_string ())};
      },
      "x/count");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_mesh_node_transport (
      "native-count", {}, [&] (const auto &rid, auto parts, auto timeout, auto receiver) {
          physical_admission = receiver.admitted;
          source.request_node_reply (rid, std::move (parts).take_items (), timeout,
                                     std::move (receiver));
      });
    auto client = builder.route_client (serializers);
    constexpr std::size_t requests = 8;
    native_call_scope_t calls (service_control_message);
    for (std::size_t i = 0; i < requests; ++i) {
        auto reply =
          client.request_to_node ("native-count", target_status.routing_id (), count_payload_t{})
            .timeout (5s)
            .async<count_payload_t> ();
        fw::tests::count_native_calls.store (false);
        bool replied = false;
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!replied && std::chrono::steady_clock::now () < deadline) {
            const auto waited = target.native_node ().wait_for_dispatch_activity (
              std::chrono::ceil<std::chrono::milliseconds> (deadline
                                                            - std::chrono::steady_clock::now ()),
              true, {});
            auto receive = [&] (const auto &, const auto &record, auto parts, auto) {
                if (record.kind != fw::runtime::host::record_kind_t::node_request)
                    return;
                fw::runtime::messaging::envelope_codec_t codec;
                auto header =
                  codec
                    .decode_header (fw::runtime::messaging::message_parts_t (std::move (parts)),
                                    false)
                    .value ();
                header.kind = fw::runtime::messaging::message_kind_t::response;
                auto response =
                  codec.encode_raw_body_parts (header, zlink::message_t::from (std::string ("41")));
                replied = target.native_node ().reply (record.reply_token, response.items ());
            };
#ifdef J1672_BEFORE
            target.dispatch_ready (receive).result ().value ();
#else
            target.dispatch_ready (receive, true, {}, waited).result ().value ();
#endif
        }
        ASSERT_TRUE (replied);
        fw::tests::count_native_calls.store (true);
        while (!reply.await_ready () && std::chrono::steady_clock::now () < deadline) {
            const auto completions = fw::tests::native_completion_events.load ();
            const auto waited = source.native_node ().wait_for_dispatch_activity (
              std::chrono::ceil<std::chrono::milliseconds> (deadline
                                                            - std::chrono::steady_clock::now ()),
              false, {});
#ifdef J1672_BEFORE
            source.dispatch_ready (discard, false).result ().value ();
#else
            source.dispatch_ready (discard, false, {}, waited).result ().value ();
#endif
            if (fw::tests::native_completion_events.load () != completions) {
                ASSERT_TRUE (reply.result_for (std::chrono::ceil<std::chrono::milliseconds> (
                  deadline - std::chrono::steady_clock::now ())));
                break;
            }
        }
        ASSERT_TRUE (reply.await_ready ());
        ASSERT_TRUE (reply.result ());
        EXPECT_EQ (reply.result ().value ().value, 41);
        // Keep source cleanup in the measured window before excluding the next
        // target turn. These are lifecycle counts, not elapsed-time budgets.
#ifdef J1672_BEFORE
        constexpr std::size_t closes_per_request = 21;
#else
        constexpr std::size_t closes_per_request = 10;
#endif
        ASSERT_TRUE (calls.wait_for_closes ((i + 1) * closes_per_request, deadline));
    }
    source.stop ();
    fw::tests::count_native_calls.store (false);
    std::printf (
      "source-native requests=%zu init=%zu init_size=%zu init_data=%zu copy=%zu "
      "close=%zu poller_wait=%zu (excludes service controls; includes source shutdown)\n",
      requests, calls[0], calls[1], calls[2], calls[3], calls[4], calls[5]);
    // Binding retains two native send views. Framework adds two inner parts
    // and two service parts; the receive owner initializes two adoption slots.
    EXPECT_EQ (calls[3], requests * 2);
    EXPECT_EQ (calls[0] + calls[1] + calls[2], requests * 8);
    EXPECT_EQ (calls[4], requests * 10);
    target.stop ();
}

}
