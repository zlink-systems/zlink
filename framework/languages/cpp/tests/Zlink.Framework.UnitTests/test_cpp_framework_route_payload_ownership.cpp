/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <gtest/gtest.h>
#include <zlink/framework.hpp>
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

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
    std::optional<result_t<runtime::messaging::envelope_header_t>> decoded;
    EXPECT_FALSE (spots.dispatch_mesh_record (owner, record, parts, provider, serializers, {},
                                              nullptr, {}, &decoded));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (*decoded);
    // A consumer of the parsed header must never consult its wire frame again.
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    detail::mesh_record_dispatcher_t dispatcher (provider, serializers, handlers, filters);
    ASSERT_TRUE (dispatcher.dispatch (record, std::move (parts), std::move (decoded)));
    EXPECT_GT (handler.references, 0);
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
