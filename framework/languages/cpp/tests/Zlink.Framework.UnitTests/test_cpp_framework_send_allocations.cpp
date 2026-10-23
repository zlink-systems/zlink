/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"

#include <cstdio>
#include <limits>
#include <memory>
#include <optional>

namespace
{
namespace fw = zlink::framework;
namespace detail = fw::detail;
namespace mesh = fw::runtime::mesh;

struct route_payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
};

TEST (SendAllocations, TypedRouteEncoderUsesOnlyTheBorrowedFunctionContainer)
{
    fw::serializer_registry_t serializers;
    std::size_t before_serializer = 0;
    serializers.add<route_payload_t> (
      [&] (const route_payload_t &payload) {
          before_serializer = fw::tests::tracked_allocation_count;
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_mesh_node_transport ("m",
                                      [] (const auto &, auto) -> fw::task_t<fw::result_t<void>> {
                                          co_return fw::result_t<void>::success ();
                                      },
                                      {});
    auto client = builder.route_client (serializers);
    const auto target = zlink::routing_id_t::from ("n");
    const auto payload = std::make_shared<route_payload_t> ();
    const fw::dispatch_options_t dispatch;
    fw::task_completion_source_t<void> encoded;
    ASSERT_TRUE (encoded.complete (fw::result_t<void>::success ()));
    // Both paths create one transport-boundary task and encode the same envelope.
    // The baseline uses the standard allocation-free reference_wrapper target.
    auto baseline_submit = [&] (const std::string &packet_name,
                                const auto &metadata) -> fw::task_t<void> {
        auto encode = [payload] (fw::serializer_registry_t &registry) {
            return registry.get<route_payload_t> ().serialize_with_content_type (*payload);
        };
        fw::route_client_t::payload_encoder_t borrowed (std::ref (encode));
        fw::runtime::messaging::client_call_codec_t codec;
        (void) codec.encode_node_send_parts ("m", packet_name, target, metadata, dispatch,
                                             serializers, borrowed);
        co_await encoded.task ();
    };
    const std::string packet_name = "p";
    const fw::route_send_call_t::metadata_map_t metadata;
    auto actual = client.send_to_node ("m", target, route_payload_t{});
    baseline_submit (packet_name, metadata).result ().value ();
    client.send_to_node ("m", target, route_payload_t{}).async ().result ().value ();
    (void) fw::tests::count_allocations (
      [&] { baseline_submit (packet_name, metadata).result ().value (); });
    const auto expected = before_serializer;
    (void) fw::tests::count_allocations ([&] { actual.async ().result ().value (); });
    EXPECT_EQ (before_serializer, expected);
    std::printf ("route encoder allocations before serializer: baseline=%zu typed=%zu\n", expected,
                 before_serializer);
}

TEST (SendAllocations, TypedSendEncodesBeforePendingTransportAndPreservesItsFailure)
{
    for (const bool channel_send : {false, true}) {
        fw::serializer_registry_t serializers;
        unsigned encoded = 0;
        serializers.add<route_payload_t> (
          [&] (const route_payload_t &payload) {
              ++encoded;
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<void> release;
        const auto transport = [&] (auto) -> fw::task_t<fw::result_t<void>> {
            co_await release.task ();
            throw std::runtime_error ("delayed send failure");
            co_return fw::result_t<void>::success ();
        };
        runtime.bind_mesh_channel_transport ("c", transport, {});
        runtime.bind_mesh_node_transport (
          "m", [&] (const auto &, auto parts) { return transport (std::move (parts)); }, {});
        auto client = builder.route_client (serializers);
        auto call = channel_send ? client.send_to_channel ("c", route_payload_t{})
                                 : client.send_to_node ("m", zlink::routing_id_t::from ("n"),
                                                        route_payload_t{});
        auto pending = call.async ();
        EXPECT_EQ (encoded, 1u);
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_TRUE (release.complete (fw::result_t<void>::success ()));
        const auto &result = pending.result ();
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
        ASSERT_NE (result.error (), nullptr);
        EXPECT_STREQ (result.error ()->what (), "delayed send failure");
        EXPECT_EQ (encoded, 1u);
    }
}

std::shared_ptr<detail::mesh_node_builder_state_t> mesh_state ()
{
    auto state =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("send-allocation");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->routing_id = zlink::routing_id_t::from ("source");
    return state;
}

TEST (SendAllocations, KnownTargetForwardsTheNativeTaskWithoutAnotherResult)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    const auto target = zlink::routing_id_t::from ("target");
    auto peer = node.native_node ().transport ().topology ().local_descriptor ();
    peer.node_routing_id = target.to_bytes ();
    peer.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (node.native_node ().transport ().topology ().admit (peer, {1}),
               mesh::peer_admission_result_t::admitted);
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    const auto native = [&] {
        (void) node.native_node ().transport ().classify_node_target (target.to_bytes ());
        return node.native_node ().send_to_node (target, parts).result ();
    };
    const auto forward = [&] {
        return node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    };
    ASSERT_TRUE (native ());
    ASSERT_TRUE (forward ());
    std::optional<fw::result_t<zlink::submit_result_t>> expected;
    std::optional<fw::result_t<zlink::submit_result_t>> actual;
    const auto baseline = fw::tests::measure_allocations ([&] { expected.emplace (native ()); });
    const auto measured = fw::tests::measure_allocations ([&] { actual.emplace (forward ()); });
    ASSERT_TRUE (*expected);
    ASSERT_TRUE (*actual);
    EXPECT_EQ (expected->value (), zlink::submit_result_t::not_connected);
    EXPECT_EQ (actual->value (), expected->value ());
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    std::printf ("known send allocations: native=%zu/%zu forwarded=%zu/%zu\n", baseline.count,
                 baseline.bytes, measured.count, measured.bytes);
    node.stop ();
}

class pending_listing_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    using page_t = fw::location_page_t<fw::mesh_node_descriptor_t>;
    fw::task_completion_source_t<page_t> completion;
    unsigned calls = 0;
    fw::task_t<page_t> list_mesh_nodes (std::string, fw::location_page_request_t = {}) override
    {
        ++calls;
        return completion.task ();
    }
};

TEST (SendAllocations, UnknownTargetPreservesLocationLookupAndFailureClassification)
{
    for (const bool move_parts : {false, true}) {
        for (const bool failed_listing : {false, true}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            const auto target = zlink::routing_id_t::from ("unknown");
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            EXPECT_TRUE (store->completion.complete (
              failed_listing ? fw::result_t<pending_listing_t::page_t>::failure (
                                 fw::framework_error_kind_t::unavailable, "listing unavailable")
                             : fw::result_t<pending_listing_t::page_t>::success ({})));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, UnknownTargetKeepsValuesAndFailuresAcrossLocationSuspension)
{
    enum class scenario_t
    {
        replaced_target,
        cleared_parts,
        failed_listing
    };
    for (const bool move_parts : {false, true}) {
        for (const auto scenario :
             {scenario_t::replaced_target, scenario_t::cleared_parts, scenario_t::failed_listing}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            auto target = zlink::routing_id_t::from ("unknown");
            const auto submitted_target = target;
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            if (scenario == scenario_t::replaced_target)
                target = zlink::routing_id_t::from ("changed");
            else
                parts.clear ();
            pending_listing_t::page_t page;
            if (scenario == scenario_t::replaced_target) {
                fw::mesh_node_descriptor_t client;
                client.mesh_name = "send-allocation";
                client.rid = submitted_target;
                client.object_role = fw::object_role_t::client;
                page.items.push_back (std::move (client));
            }
            EXPECT_TRUE (store->completion.complete (
              scenario == scenario_t::failed_listing
                ? fw::result_t<pending_listing_t::page_t>::failure (
                    fw::framework_error_kind_t::unavailable, "listing unavailable")
                : fw::result_t<pending_listing_t::page_t>::success (std::move (page))));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), scenario == scenario_t::replaced_target
                                          ? zlink::submit_result_t::not_found
                                          : zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, StoppedNodeKeepsItsTaskFailureBoundary)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    const auto target = zlink::routing_id_t::from ("target");
    const std::vector<zlink::message_t> parts;
    const auto result = node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    EXPECT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::protocol_error);
}
TEST (SendAllocations, ReceiveTurnForwardsTheNativeTaskWithoutAnotherResult)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    const auto native = [&] {
        return node.native_node ()
          .dispatch_ready ([] (const auto &, const auto &, auto) {}, false)
          .result ();
    };
    const auto forward = [&] {
        return node.dispatch_ready ([] (const auto &, const auto &, auto) {}, false).result ();
    };
    ASSERT_TRUE (native ());
    ASSERT_TRUE (forward ());
    std::optional<fw::result_t<std::size_t>> expected;
    std::optional<fw::result_t<std::size_t>> actual;
    const auto baseline = fw::tests::measure_allocations ([&] { expected.emplace (native ()); });
    const auto measured = fw::tests::measure_allocations ([&] { actual.emplace (forward ()); });
    ASSERT_TRUE (*expected);
    ASSERT_TRUE (*actual);
    EXPECT_EQ (expected->value (), 0u);
    EXPECT_EQ (actual->value (), expected->value ());
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    std::printf ("receive forwarding allocations: native=%zu/%zu forwarded=%zu/%zu\n",
                 baseline.count, baseline.bytes, measured.count, measured.bytes);
    node.stop ();
}

TEST (SendAllocations, ReceiveTurnValidationKeepsItsTaskFailureBoundary)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    auto invalid = node.dispatch_ready ({}, false);
    EXPECT_TRUE (invalid.await_ready ());
    const auto &result = invalid.result ();
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::protocol_error);
    EXPECT_STREQ (result.error ()->what (), "MeshNode dispatch callback is required");
    node.stop ();
}

TEST (SendAllocations, ReceiveTurnRetainsCallbacksAcrossNativeSuspension)
{
    namespace host = fw::runtime::host;
    namespace stateful = fw::runtime::stateful;
    namespace wire = fw::runtime::protocol;
    for (const bool forwarded : {false, true}) {
        detail::mesh_node_runtime_t node (mesh_state ());
        fw::task_completion_source_t<std::optional<fw::location_owner_token_t>> owner;
        unsigned owner_calls = 0;
        unsigned bind_calls = 0;
        node.configure_bound_session_operations (
          {.session_owner =
             [&] (const auto &, auto) {
                 ++owner_calls;
                 return owner.task ();
             },
           .bind =
             [&] (const auto &, const auto &, auto, const auto &, auto) {
                 ++bind_calls;
                 return host::bound_session_bind_operation_result_t{
                   stateful::stateful_error_t::none, std::nullopt};
             },
           .send = [] (const auto &, auto) { return stateful::stateful_error_t::conflict; },
           .replaced = [] (const auto &) { ADD_FAILURE () << "unexpected Session replacement"; },
           .commit_relocation_route =
             [] (const auto &, const auto &, const auto &) {
                 ADD_FAILURE () << "unexpected relocation commit";
                 return false;
             },
           .prepare_relocation_target_route =
             [] (const auto &, auto) {
                 ADD_FAILURE () << "unexpected relocation preparation";
                 return false;
             }});
        node.start ();
        auto &runtime = node.native_node ();
        const auto descriptor = runtime.transport ().topology ().local_descriptor ();
        const stateful::object_ref_t actor{
          stateful::object_kind_t::actor,
          "actor",
          1,
          1,
          descriptor.mesh_name,
          zlink::routing_id_t::from (descriptor.node_routing_id).to_string ()};
        const auto created = runtime.objects ().begin_reserved_object (actor, "actor", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (runtime.objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        const wire::bound_session_bind_t bind{
          .correlation = 1,
          .actor = {.actor_id = "actor",
                    .object_generation = 1,
                    .target_node_routing_id = descriptor.node_routing_id,
                    .target_node_generation = descriptor.lifecycle_generation,
                    .authority_owner_generation = 1,
                    .owner_lease_generation = 1},
          .session_routing_id = {'s'},
          .binding = {.generation = 1}};
        mesh::service_mailbox_record_t record;
        record.owner = "control";
        record.domain = mesh::service_mailbox_domain_t::infrastructure;
        record.parts.push_back (zlink::message_t::from (wire::encode_bound_session_bind (bind)));
        record.source_routing_id = {'s'};
        record.source_node_generation = 1;
        ASSERT_TRUE (runtime.transport ().mailbox ().try_enqueue (std::move (record)));
        auto lifetime = std::make_shared<int> (1);
        std::weak_ptr<int> held = lifetime;
        auto callback = [lifetime] (const auto &, const auto &, auto) {
            ADD_FAILURE () << "unexpected application callback";
        };
        auto next = [lifetime] { return false; };
        auto running = forwarded
                         ? node.dispatch_ready (std::move (callback), false, std::move (next))
                         : runtime.dispatch_ready (std::move (callback), false, std::move (next));
        lifetime.reset ();
        EXPECT_FALSE (running.await_ready ());
        EXPECT_EQ (owner_calls, 1u);
        EXPECT_EQ (bind_calls, 0u);
        EXPECT_FALSE (held.expired ());
        EXPECT_TRUE (
          owner.complete (fw::result_t<std::optional<fw::location_owner_token_t>>::success (
            fw::location_owner_token_t{"session-owner", 1})));
        const auto settled = running.result ();
        ASSERT_TRUE (settled);
        EXPECT_EQ (settled.value (), 1u);
        EXPECT_EQ (bind_calls, 1u);
        EXPECT_TRUE (runtime.transport ().mailbox ().try_claim_owner (
                       mesh::service_mailbox_domain_t::infrastructure, "control", 1,
                       std::numeric_limits<std::size_t>::max ())
                     == nullptr);
        node.stop ();
    }
}
} // namespace
