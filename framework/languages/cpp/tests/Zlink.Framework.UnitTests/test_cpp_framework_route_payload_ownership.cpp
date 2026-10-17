/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include <zlink/framework.hpp>
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
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
#include <filesystem>
#include <fstream>
#include <sstream>
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
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    // A consumer of the parsed header must never consult its wire frame again.
    parts[0] = zlink::message_t::from (std::string ("invalid header"));
    detail::mesh_record_dispatcher_t dispatcher (provider, serializers, handlers, filters);
    ASSERT_TRUE (dispatcher.dispatch (record, std::move (parts), std::move (*decoded.value ())));
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
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
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
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    EXPECT_NO_THROW ((void) decoded.value ());
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
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
      spots.dispatch_mesh_record (owner, record, parts, decoded.value (), provider, serializers));
    ASSERT_TRUE (decoded);
    ASSERT_TRUE (decoded.value ());
    EXPECT_EQ (decoded.value ()->message_name, "payload");
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
        auto decoded = runtime::decode_mesh_record_header (record, parts, true);
        ASSERT_TRUE (decoded);
        EXPECT_FALSE (decoded.value ());
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
        auto decoded = runtime::decode_mesh_record_header (record, parts, true);
        EXPECT_FALSE (decoded);
        EXPECT_EQ (decoded.error_kind (), framework_error_kind_t::protocol_error);
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
    }
}

TEST (RoutePayloadOwnership, MeshIngressRepliesToInvalidRequestHeadersForEveryOwner)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "header-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    msg::envelope_codec_t codec;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    std::vector<message_dispatch_error_event_t> errors;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options, [&] (const message_dispatch_error_event_t &event) { errors.push_back (event); });
    for (auto kind : {host::record_kind_t::actor_request, host::record_kind_t::spot_request,
                      host::record_kind_t::node_request, host::record_kind_t::channel_request}) {
        SCOPED_TRACE (static_cast<int> (kind));
        host::receive_record_t record;
        record.kind = kind;
        record.channel_name = "mesh";
        int replies = 0;
        std::vector<zlink::message_t> reply_parts;
        record.reply_token.host = reply_host;
        record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
            ++replies;
            reply_parts = parts;
            return true;
        };
        msg::envelope_header_t header;
        header.channel_name = "mesh";
        header.message_name = "request";
        header.correlation_id = "request-correlation";
        header.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
        header.flow_origin = flow_origin_t::application;
        auto wire = codec.encode_header (header).to_string ();
        auto invalid_flow = wire;
        invalid_flow.replace (invalid_flow.find (*header.flow_id), 4, "ZZZZ");
        auto invalid_flow_type = wire;
        const auto flow_at = invalid_flow_type.find (*header.flow_id);
        invalid_flow_type.replace (flow_at - 1, header.flow_id->size () + 2, "7");
        auto invalid_metadata = wire;
        const std::string empty_metadata = "\"metadata\":{}";
        invalid_metadata.replace (invalid_metadata.find (empty_metadata), empty_metadata.size (),
                                  "\"metadata\":[]");
        auto invalid_content_type = wire;
        const auto content_at = invalid_content_type.find (header.content_type);
        invalid_content_type.replace (content_at - 1, header.content_type.size () + 2, "7");
        auto invalid_deadline = wire;
        const std::string deadline = "\"deadline\":null";
        invalid_deadline.replace (invalid_deadline.find (deadline), deadline.size (),
                                  "\"deadline\":7");
        for (const auto &invalid : {invalid_flow, invalid_flow_type, invalid_metadata,
                                    invalid_content_type, invalid_deadline}) {
            SCOPED_TRACE (invalid);
            const auto previous_errors = errors.size ();
            const auto previous_replies = replies;
            reply_parts.clear ();
            auto parts = std::vector<zlink::message_t>{zlink::message_t::from (invalid),
                                                       zlink::message_t::from ("42")};
            auto rejected = runtime::decode_mesh_record_header (record, parts, true, options);
            EXPECT_FALSE (rejected);
            EXPECT_EQ (rejected.error_kind (), framework_error_kind_t::protocol_error);
            EXPECT_EQ (replies, previous_replies + 1);
            ASSERT_EQ (errors.size (), previous_errors + 1);
            EXPECT_EQ (errors.back ().action, dispatch_error_action_t::reply_error);
            EXPECT_EQ (errors.back ().reason, dispatch_error_reason_t::invalid_frame);
            EXPECT_EQ (errors.back ().correlation_id, header.correlation_id);
            EXPECT_EQ (errors.back ().flow_id,
                       invalid == invalid_metadata ? header.flow_id : std::nullopt);
            ASSERT_EQ (reply_parts.size (), 2u);
            auto reply = codec.decode_header (msg::message_parts_t (reply_parts));
            ASSERT_TRUE (reply);
            EXPECT_EQ (reply.value ().kind, msg::message_kind_t::error);
            EXPECT_EQ (reply.value ().error_code, "protocol_error");
            EXPECT_EQ (reply.value ().correlation_id, header.correlation_id);
            EXPECT_EQ (reply.value ().channel_name, header.channel_name);
            EXPECT_EQ (reply.value ().message_name, header.message_name);
        }
        // Without readable correlation there is no request to attach a reply to.
        const auto previous_replies = replies;
        const auto previous_errors = errors.size ();
        auto unreadable = std::vector<zlink::message_t>{zlink::message_t::from ("invalid header")};
        EXPECT_FALSE (runtime::decode_mesh_record_header (record, unreadable, true, options));
        EXPECT_EQ (replies, previous_replies);
        ASSERT_EQ (errors.size (), previous_errors + 1);
        EXPECT_EQ (errors.back ().action, dispatch_error_action_t::drop);
        EXPECT_FALSE (errors.back ().correlation_id);
    }
}

TEST (RoutePayloadOwnership, MeshIngressDropsInvalidSendsAndReportsOnceForEveryOwner)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    std::vector<message_dispatch_error_event_t> errors;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options, [&] (const message_dispatch_error_event_t &event) { errors.push_back (event); });
    for (auto kind : {host::record_kind_t::actor_send, host::record_kind_t::spot_send,
                      host::record_kind_t::node_send, host::record_kind_t::channel_send}) {
        SCOPED_TRACE (static_cast<int> (kind));
        host::receive_record_t record;
        record.kind = kind;
        record.channel_name = "mesh";
        int replies = 0;
        record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &) {
            ++replies;
            return true;
        };
        auto parts = std::vector<zlink::message_t>{zlink::message_t::from ("invalid header")};
        const auto previous_errors = errors.size ();
        auto decoded = runtime::decode_mesh_record_header (record, parts, true, options);
        EXPECT_FALSE (decoded);
        EXPECT_EQ (decoded.error_kind (), framework_error_kind_t::protocol_error);
        EXPECT_EQ (replies, 0);
        ASSERT_EQ (errors.size (), previous_errors + 1);
        EXPECT_EQ (errors.back ().reason, dispatch_error_reason_t::invalid_frame);
        EXPECT_EQ (errors.back ().action, dispatch_error_action_t::drop);
        EXPECT_EQ (errors.back ().message_kind, dispatch_message_kind_t::send);
        EXPECT_EQ (errors.back ().surface,
                   kind == host::record_kind_t::actor_send  ? dispatch_error_surface_t::spot_actor
                   : kind == host::record_kind_t::spot_send ? dispatch_error_surface_t::spot_route
                   : kind == host::record_kind_t::node_send
                     ? dispatch_error_surface_t::node
                     : dispatch_error_surface_t::route_mesh_channel);
        EXPECT_FALSE (errors.back ().flow_id);
        EXPECT_FALSE (errors.back ().correlation_id);
        ASSERT_EQ (parts.size (), 1u);
        EXPECT_EQ (parts[0].to_string (), "invalid header");
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

TEST (RoutePayloadOwnership, MalformedReplyRouteMatchesSharedFourLanguageContract)
{
    namespace host = runtime::host;
    namespace msg = runtime::messaging;
    auto root = std::filesystem::path (__FILE__).parent_path ();
    for (int depth = 0; depth < 4; ++depth)
        root = root.parent_path ();
    std::ifstream fixture (root / "test/fixtures/malformed-envelope-reply-route.tsv");
    ASSERT_TRUE (fixture.is_open ());
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "header-rejection",
                              .node_routing_id = zlink::routing_id_t::from ("reply").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    std::string row;
    int count = 0;
    while (std::getline (fixture, row)) {
        if (!row.empty () && row.back () == '\r')
            row.pop_back ();
        if (row.empty () || row.front () == '#')
            continue;
        std::istringstream input (row);
        std::string id, request, json, action;
        std::getline (input, id, '\t');
        std::getline (input, request, '\t');
        std::getline (input, json, '\t');
        std::getline (input, action, '\t');
        SCOPED_TRACE (id);
        for (auto kind :
             request == "true"
               ? std::vector{host::record_kind_t::node_request,
                             host::record_kind_t::channel_request,
                             host::record_kind_t::spot_request, host::record_kind_t::actor_request}
               : std::vector{host::record_kind_t::node_send, host::record_kind_t::channel_send,
                             host::record_kind_t::spot_send, host::record_kind_t::actor_send}) {
            SCOPED_TRACE (static_cast<int> (kind));
            host::receive_record_t record;
            record.kind = kind;
            record.channel_name = "play";
            int replies = 0;
            std::vector<zlink::message_t> reply_parts;
            if (request == "true") {
                record.reply_token.host = reply_host;
                record.reply_token.local_reply = [&] (const std::vector<zlink::message_t> &parts) {
                    ++replies;
                    reply_parts = parts;
                    return true;
                };
            }
            dispatch_options_t options;
            options.message_flow (message_flow_log_mode_t::normal);
            std::vector<message_dispatch_error_event_t> errors;
            detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
              options,
              [&] (const message_dispatch_error_event_t &event) { errors.push_back (event); });
            auto parts = std::vector<zlink::message_t>{zlink::message_t::from (json),
                                                       zlink::message_t::from ("request")};
            auto result = runtime::decode_mesh_record_header (record, parts, false, options);
            EXPECT_FALSE (result);
            EXPECT_EQ (replies, action == "reply_error" ? 1 : 0);
            ASSERT_EQ (errors.size (), 1u);
            EXPECT_EQ (errors[0].reason, dispatch_error_reason_t::invalid_frame);
            EXPECT_EQ (errors[0].action, action == "reply_error"
                                           ? dispatch_error_action_t::reply_error
                                           : dispatch_error_action_t::drop);
            if (!reply_parts.empty ()) {
                auto reply =
                  msg::envelope_codec_t{}.decode_header (msg::message_parts_t (reply_parts), false);
                ASSERT_TRUE (reply);
                EXPECT_EQ (reply.value ().kind, msg::message_kind_t::error);
                EXPECT_EQ (reply.value ().error_code, "protocol_error");
                EXPECT_EQ (reply.value ().correlation_id,
                           nlohmann::json::parse (json).at ("correlationId").get<std::string> ());
            }
        }
        ++count;
    }
    EXPECT_EQ (count, 22);
}

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

TEST (RoutePayloadOwnership, UnconnectedObjectClientNodeDirectTargetIsNotFound)
{
    // 02-channel-messaging section 8: an Object Client RID is not a Node
    // direct target. A target the topology does not know is classified from
    // the Location Store, so the result does not depend on a connection.
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("mesh");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->routing_id = zlink::routing_id_t::from ("actor-node");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->object_role = object_role_t::server;
    detail::mesh_node_runtime_t source (state);
    auto store = std::make_shared<client_node_store_t> ();
    source.configure_user_spot_operations (store, {});
    source.start ();
    const auto client_rid = zlink::routing_id_t::from ("session-node");
    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("payload"))};
    EXPECT_EQ (std::move (source.send_to_node (client_rid, parts)).result ().value (),
               zlink::submit_result_t::not_found);
    runtime::host::pending_operation_t operation;
    EXPECT_EQ (std::move (source.request_to_node (client_rid, parts, operation,
                                                  std::chrono::milliseconds (500)))
                 .result ()
                 .value (),
               zlink::submit_result_t::not_found);
    EXPECT_EQ (store->lists, 2);
    source.stop ();
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
