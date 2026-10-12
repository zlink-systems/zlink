/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <gtest/gtest.h>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include <zlink/framework.hpp>
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "../support/runtime_failure_fixture.hpp"

#include <chrono>
#include <thread>

namespace
{
using namespace zlink::framework;

struct payload_handler_t
{
    const zlink::message_t *payload = nullptr;
    int references = 0;

    void send (const int &, const route_message_context_t &) { references = payload->ref_count (); }

    task_t<int> request (const int &value, const route_message_context_t &)
    {
        references = payload->ref_count ();
        co_return value;
    }
};

TEST (RoutePayloadOwnership, SendBorrowsPayloadForTheAwaitedHandler)
{
    service_collection_t services;
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_send<payload_handler_t, int> ("mesh", "payload", &payload_handler_t::send);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string (128, ' ') + "42");
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &payload;
    const auto references = payload.ref_count ();
    detail::route_handler_invoker_t invoker;
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    auto invoked = invoker
                     .invoke_send (handlers, filters, handler_dispatch_kind_t::node_direct_send,
                                   "mesh", "payload", provider, serializers, payload, context)
                     .result ();
    ASSERT_TRUE (invoked);
    EXPECT_EQ (handler.references, references);
    EXPECT_EQ (payload.ref_count (), references);
}

TEST (RoutePayloadOwnership, RequestBorrowsPayloadForTheAwaitedHandler)
{
    service_collection_t services;
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_request<payload_handler_t, int, int> ("mesh", "payload",
                                                      &payload_handler_t::request);
    handler_registry_t filters;
    auto payload = zlink::message_t::from (std::string (128, ' ') + "42");
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &payload;
    const auto references = payload.ref_count ();
    detail::route_handler_invoker_t invoker;
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    auto invoked =
      invoker
        .invoke_request (handlers, filters, handler_dispatch_kind_t::node_direct_request, "mesh",
                         "payload", provider, serializers, payload, context)
        .result ();
    ASSERT_TRUE (invoked);
    EXPECT_EQ (handler.references, references);
    EXPECT_EQ (payload.ref_count (), references);
}

TEST (RoutePayloadOwnership, ApplicationDispatcherConsumesTheDecodedHeader)
{
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_singleton<payload_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handlers.on_send<payload_handler_t, int> ("mesh", "payload", &payload_handler_t::send);
    handler_registry_t filters;
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto encoded = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
      header, zlink::message_t::from (std::string (128, ' ') + "42"));
    auto parts = std::move (encoded).take_items ();
    auto &handler = provider.get_required<payload_handler_t> ();
    handler.payload = &parts[1];
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    spots.set_route_client (route_client_t{});
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::node_send;
    record.channel_name = "mesh";
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded, provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (*decoded);
    // A consumer of the parsed header must never consult its wire frame again.
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    detail::mesh_record_dispatcher_t dispatcher (provider, serializers, handlers, filters);
    ASSERT_TRUE (dispatcher.dispatch (record, std::move (parts), std::move (*decoded)));
    EXPECT_GT (handler.references, 0);
}

TEST (RoutePayloadOwnership, MeshIngressProvidesHeaderWithoutARouteClient)
{
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::node_send;
    runtime::messaging::envelope_header_t header;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto parts = runtime::messaging::envelope_codec_t{}
                   .encode_raw_body_parts (header, zlink::message_t::from ("42"))
                   .take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded, provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (*decoded);
    EXPECT_EQ (decoded->value ().message_name, "payload");
}

TEST (RoutePayloadOwnership, ActorSendFallbackProvidesHeader)
{
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::node;
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::actor_send;
    runtime::messaging::envelope_header_t header;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto parts = runtime::messaging::envelope_codec_t{}
                   .encode_raw_body_parts (header, zlink::message_t::from ("42"))
                   .take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded, provider, serializers));
    EXPECT_NO_THROW ((void) decoded.value ());
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (*decoded);
    EXPECT_EQ (decoded->value ().message_name, "payload");
}

TEST (RoutePayloadOwnership, ActorSendWithoutTerminalPreservesIngressHeaderAndFrames)
{
    service_collection_t services;
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    auto state = std::make_shared<detail::spot_node_builder_state_t> ("node");
    state->actor_types_by_id["actor"] = "player";
    detail::spot_node_runtime_t spots (state);
    runtime::host::ready_record_t owner;
    owner.owner_kind = runtime::host::owner_kind_t::actor;
    owner.actor =
      detail::actor_ref_access_t::make (node_rid_t::from_string ("node"), "player", "actor", 1);
    runtime::host::receive_record_t record;
    record.kind = runtime::host::record_kind_t::actor_send;
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "mesh";
    header.message_name = "payload";
    auto parts = runtime::messaging::envelope_codec_t{}
                   .encode_raw_body_parts (header, zlink::message_t::from ("42"))
                   .take_items ();
    auto decoded = runtime::decode_mesh_record_header (record, parts, false);
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    EXPECT_FALSE (
      spots.dispatch_mesh_record (owner, record, parts, decoded, provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (*decoded);
    EXPECT_EQ (decoded->value ().message_name, "payload");
    ASSERT_EQ (parts.size (), 2u);
    EXPECT_EQ (parts[0].to_string (), "invalid header");
    EXPECT_EQ (parts[1].to_string (), "42");
}

TEST (RoutePayloadOwnership, MeshIngressSkipsRecordsWithoutEnvelopeHeaders)
{
    runtime::host::receive_record_t record;
    auto parts =
      std::vector<zlink::message_t>{zlink::message_t::from (std::string ("invalid header"))};
    for (auto kind : {runtime::host::record_kind_t::spot_multicast,
                      runtime::host::record_kind_t::spot_control}) {
        record.kind = kind;
        EXPECT_FALSE (runtime::decode_mesh_record_header (record, parts, true));
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
    }
}

TEST (RoutePayloadOwnership, MeshIngressRejectsMalformedRouteAndActorHeaders)
{
    runtime::host::receive_record_t record;
    for (auto kind :
         {runtime::host::record_kind_t::node_send, runtime::host::record_kind_t::actor_send,
          runtime::host::record_kind_t::actor_request}) {
        record.kind = kind;
        auto parts =
          std::vector<zlink::message_t>{zlink::message_t::from (std::string ("invalid header"))};
        std::optional<result_t<runtime::messaging::envelope_header_t>> decoded;
        ASSERT_NO_THROW (decoded = runtime::decode_mesh_record_header (record, parts, true));
        ASSERT_TRUE (decoded);
        EXPECT_FALSE (*decoded);
        EXPECT_EQ (decoded->error_kind (), framework_error_kind_t::protocol_error);
        service_collection_t services;
        auto provider = services.build_provider ();
        serializer_registry_t serializers;
        detail::spot_node_runtime_t spots (
          std::make_shared<detail::spot_node_builder_state_t> ("node"));
        runtime::host::ready_record_t owner;
        owner.owner_kind = runtime::host::owner_kind_t::node;
        EXPECT_FALSE (
          spots.dispatch_mesh_record (owner, record, parts, decoded, provider, serializers));
        ASSERT_EQ (parts.size (), 1u);
        if (kind == runtime::host::record_kind_t::node_send) {
            detail::route_handler_registry_t handlers;
            handler_registry_t filters;
            detail::mesh_record_dispatcher_t dispatcher (provider, serializers, handlers, filters);
            auto rejected = dispatcher.dispatch (record, std::move (parts), std::move (*decoded));
            EXPECT_FALSE (rejected);
            EXPECT_EQ (rejected.error_kind (), framework_error_kind_t::protocol_error);
        }
    }
}

class client_node_store_t : public runtime::in_memory_location_repository_t
{
  public:
    int lists = 0;
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name, location_page_request_t = {}) override
    {
        ++lists;
        mesh_node_descriptor_t target;
        target.mesh_name = std::move (mesh_name);
        target.rid = zlink::routing_id_t::from ("session-node");
        target.object_role = object_role_t::client;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (target));
        co_return page;
    }
};

TEST (RoutePayloadOwnership, BoundSessionInternalSendSkipsUserTargetClassification)
{
    auto make_node = [] (const char *rid, object_role_t role) {
        auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("mesh");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->routing_id = zlink::routing_id_t::from (rid);
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->listen_port.reset ();
        state->object_role = role;
        state->channels.emplace ("work", detail::mesh_channel_registration_t{100, {}, true, true});
        return state;
    };
    auto source_state = make_node ("actor-node", object_role_t::server);
    auto target_state = make_node ("session-node", object_role_t::client);
    detail::mesh_node_runtime_t source (source_state), target (target_state);
    auto store = std::make_shared<client_node_store_t> ();
    source.configure_user_spot_operations (store, {});
    source.start ();
    target.start ();
    source.connect_peer (*target_state->routing_id, target.status ().local_endpoint ());
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    auto pump = [] (detail::mesh_node_runtime_t &node) {
        node.dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ();
    };
    while ((source.admitted_peer_count () == 0 || target.admitted_peer_count () == 0)
           && std::chrono::steady_clock::now () < deadline) {
        pump (source);
        pump (target);
        std::this_thread::yield ();
    }
    ASSERT_EQ (source.admitted_peer_count (), 1u);
    ASSERT_EQ (target.admitted_peer_count (), 1u);
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    auto client = builder.route_client (serializers);
    detail::channel_runtime_t::from (builder.message_bus ())
      .bind_mesh_node_transport (
        "mesh",
        [&source] (const auto &rid,
                   runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
            const auto sent = co_await source.send_to_node (rid, std::move (parts).take_items ());
            if (sent == zlink::submit_result_t::ok)
                co_return result_t<void>::success ();
            co_return result_t<void>::failure (
              runtime::messaging::map_submit_result_error_kind (sent), "node send failed");
        },
        {});
    detail::actor_gateway_runtime_t gateway;
    const auto actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("actor-node"),
                                                         "player", "actor", 1);
    const auto user_send =
      client.send_to_node ("mesh", *target_state->routing_id, 42).async ().result ();
    ASSERT_FALSE (user_send);
    EXPECT_EQ (user_send.error_kind (), framework_error_kind_t::not_found);
    ASSERT_TRUE (gateway.bind_session_route (
      actor, detail::spot_node_runtime_t (source_state->spot_state).native_node (), serializers,
      "mesh", *target_state->routing_id));
    store->lists = 0;
    const auto sent = gateway.dispatch_bound_session_send (actor, "reply", stream_codec_t::json,
                                                           zlink::message_t::from ("42"));
    EXPECT_TRUE (sent) << (sent.error () ? sent.error ()->what () : "");
    EXPECT_EQ (store->lists, 0);
    bool delivered = false;
    const auto receive_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (sent && !delivered && std::chrono::steady_clock::now () < receive_deadline) {
        target
          .dispatch_ready ([&] (const auto &, const auto &, auto parts) {
              auto decoded = runtime::messaging::envelope_codec_t{}.decode_header (
                runtime::messaging::message_parts_t (std::move (parts)), false);
              delivered = decoded
                          && decoded.value ().message_name
                               == detail::actor_bound_session_route_request_t::packet_name;
          })
          .result ()
          .value ();
        std::this_thread::yield ();
    }
    EXPECT_TRUE (delivered);
    source.stop ();
    target.stop ();
}

TEST (RoutePayloadOwnership, ServiceWirePartsRetainTheTransferredFrame)
{
    namespace protocol = runtime::protocol;
    std::vector<zlink::message_t> source;
    source.push_back (zlink::message_t::from (std::string ("header")));
    source.push_back (zlink::message_t::from (std::string (1024, 'x')));
    auto frame = protocol::encode_application_payload (
      protocol::application_payload_t::from_parts (std::move (source)));
    const auto start = reinterpret_cast<std::uintptr_t> (frame.data ());
    const auto end = start + frame.size ();
    detail::backend::raw_message_t input;
    input.push_back (std::move (frame));
    auto native = detail::backend::materialize_binding_parts (std::move (input));
    auto parts = protocol::decode_application_parts (std::move (native[0]), false);
    ASSERT_EQ (parts.size (), 2u);
    EXPECT_EQ (parts[0].to_string (), "header");
    EXPECT_EQ (parts[1].to_string (), std::string (1024, 'x'));
    for (const auto &part : parts) {
        const auto address = reinterpret_cast<std::uintptr_t> (part.data ());
        EXPECT_GE (address, start);
        EXPECT_LE (address + part.size (), end);
    }
}

TEST (RoutePayloadOwnership, ServiceWireRejectsTruncatedTransferredFrame)
{
    namespace protocol = runtime::protocol;
    auto frame = protocol::encode_application_payload (protocol::application_payload_t::from_parts (
      std::vector<zlink::message_t>{zlink::message_t::from (std::string ("payload"))}));
    frame.pop_back ();
    detail::backend::raw_message_t input;
    input.push_back (std::move (frame));
    auto native = detail::backend::materialize_binding_parts (std::move (input));
    EXPECT_THROW (protocol::decode_application_parts (std::move (native[0]), false),
                  protocol::service_wire_error_t);
}
}
