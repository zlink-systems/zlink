/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <gtest/gtest.h>
#include <zlink.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace framework = zlink::framework;
namespace mesh = framework::runtime::mesh;
namespace messaging = framework::runtime::messaging;
namespace wire = framework::runtime::protocol;
using namespace std::chrono_literals;

namespace
{
mesh::service_node_descriptor_t descriptor (const std::string &id)
{
    return {.mesh_name = "ended-route",
            .node_routing_id = {id.begin (), id.end ()},
            .lifecycle_generation = 1,
            .descriptor_revision = 1,
            .advertised_endpoint = "inproc://" + id,
            .channels = {{"orders"}},
            .state = mesh::service_node_state_t::serving};
}

struct order_handler_t
{
    static inline std::atomic<unsigned> calls{0};

    void send (const std::string &value, const framework::route_message_context_t &)
    {
        EXPECT_EQ (value, "1");
        ++calls;
    }
};

class EndedRouteClaim : public testing::Test
{
  protected:
    static auto now () { return mesh::service_liveness_registry_t::clock_t::now (); }

    static inline std::atomic<unsigned> next_id{0};
    framework::completion_owner_t completion_owner;
    const std::string id = "ended-route-" + std::to_string (next_id++);
    std::shared_ptr<zlink::context_t> context = std::make_shared<zlink::context_t> ();
    const mesh::service_node_descriptor_t remote = descriptor (id + "-source");
    mesh::raw_mesh_node_owner_t node{framework::test::runtime_failure_options (
                                       mesh::raw_mesh_node_options_t{descriptor (id + "-target")}),
                                     context};
    std::uint64_t ended_generation = 0;

    void SetUp () override
    {
        node.start ();
        node.expect_peer (remote);
    }

    void connect (zlink::dealer_socket_t &source)
    {
        source.set_routing_id (zlink::routing_id_t::from (remote.node_routing_id));
        source.connect (node.endpoint ());
    }

    void send_admission (zlink::dealer_socket_t &source, wire::command kind)
    {
        source.send ()
          .message (zlink::message_t::from (wire::encode_route_mesh_admission (kind, remote)))
          .submit ();
    }

    void send_order (zlink::dealer_socket_t &source)
    {
        messaging::envelope_header_t header;
        header.kind = messaging::message_kind_t::command;
        header.channel_name = "orders";
        header.message_name = "Order";
        auto parts = messaging::envelope_codec_t{}.encode_raw_body_parts (
          header, zlink::message_t::from ("1"));
        source.send ()
          .message (wire::encode_channel_send_header_message ("orders"))
          .message (wire::encode_application_payload_message (
            wire::application_payload_t::from_parts (std::move (parts).take_items ())))
          .submit ();
    }

    mesh::raw_mesh_node_owner_t::receive_turn_t await_record ()
    {
        const auto deadline = now () + 5s;
        do {
            auto turn = node.begin_receive_turn ();
            if (turn
                && (static_cast<short> (*turn)
                    & static_cast<short> (zlink::poll_event_flag_t::pollin))
                     != 0)
                return turn;
            std::this_thread::yield ();
        } while (now () < deadline);
        ADD_FAILURE () << "Expected Core to return a queued record";
        return {};
    }

    mesh::raw_mesh_pump_result_t receive ()
    {
        const auto turn = await_record ();
        return node.receive_next (turn, now ());
    }

    bool ended_is_claimable ()
    {
        return static_cast<bool> (
          node.topology ().claim_ingress_peer (remote.node_routing_id, ended_generation, true));
    }

    void admit (zlink::dealer_socket_t &source)
    {
        send_admission (source, wire::command::admit);
        ASSERT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
        ASSERT_TRUE (node.peer_route (remote.node_routing_id).second);
    }

    void observe_without_receiving ()
    {
        if (const auto ready = node.wait_for_activity (0ms, false))
            (void) node.begin_receive_turn (*ready);
    }

    std::uint64_t await_replacement ()
    {
        const auto deadline = now () + 5s;
        do {
            observe_without_receiving ();
            const auto generation = node.peer_route (remote.node_routing_id).first;
            if (generation != 0 && generation != ended_generation)
                return generation;
            std::this_thread::yield ();
        } while (now () < deadline);
        ADD_FAILURE () << "Expected Core to select a replacement generation";
        return 0;
    }

    void end (zlink::dealer_socket_t &source)
    {
        const auto route = node.peer_route (remote.node_routing_id);
        ASSERT_TRUE (route.second);
        ended_generation = route.first;
        // Leave the complete record in Core's queue while the route ends.
        source.close ();
        const auto deadline = now () + 5s;
        do {
            observe_without_receiving ();
            if (node.peer_route (remote.node_routing_id).first == 0) {
                EXPECT_FALSE (node.topology ().peer (remote.node_routing_id));
                return;
            }
            std::this_thread::yield ();
        } while (now () < deadline);
        FAIL () << "Expected route loss before claiming the queued record";
    }
};

TEST_F (EndedRouteClaim, DispatchesChannelSendFromEndedAdmittedRoute)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_order (source);
    ASSERT_NO_FATAL_FAILURE (end (source));

    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::application);
    auto claim = framework::test::claim_mailbox (
      node.mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
    ASSERT_TRUE (claim);
    const auto &record = *claim->records.front ();
    EXPECT_EQ (record.route_generation (), ended_generation);
    EXPECT_EQ (record.owner, mesh::service_mailbox_t::application_owner (
                               framework::runtime::host::owner_kind_t::channel, "orders"));
    EXPECT_EQ (record.source_node_generation, remote.lifecycle_generation);

    framework::service_collection_t services;
    auto provider = services.build_provider ();
    framework::serializer_registry_t serializers;
    serializers.add<std::string> (
      [] (const std::string &value) { return framework::encoded_payload_t::from_string (value); },
      [] (const framework::encoded_payload_t &value) { return value.to_string (); },
      "application/json");
    framework::detail::route_handler_registry_t handlers;
    handlers.on_send<order_handler_t, std::string> ("orders", "Order", &order_handler_t::send);
    framework::detail::no_route_internal_packet_dispatcher_t no_internal;
    framework::handler_registry_t filters;
    framework::dispatch_options_t options;
    framework::detail::route_packet_dispatcher_t dispatcher (
      "orders", provider, serializers, handlers, no_internal, options, &filters,
      framework::handler_dispatch_kind_t::channel_send,
      framework::handler_dispatch_kind_t::channel_request);
    auto parts = wire::decode_application_parts (
      wire::decode_application_payload (wire::message_bytes (record.parts ().back ()), true));
    messaging::message_parts_t encoded (std::move (parts));
    auto header = messaging::envelope_codec_t{}.decode_header (encoded, true).value ();
    order_handler_t::calls = 0;
    auto result = dispatcher
                    .dispatch ({zlink::routing_id_t::from (remote.node_routing_id),
                                record.correlation,
                                std::move (encoded),
                                {}},
                               std::move (header))
                    .result ();
    EXPECT_TRUE (result);
    EXPECT_EQ (order_handler_t::calls.load (), 1u);
    EXPECT_TRUE (node.mailbox ().release (*claim));
}

TEST_F (EndedRouteClaim, ReplacementRequiresAdmission)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_order (source);
    ASSERT_NO_FATAL_FAILURE (end (source));

    zlink::dealer_socket_t replacement (*context);
    connect (replacement);
    const auto generation = await_replacement ();
    ASSERT_NE (generation, 0u);
    EXPECT_TRUE (ended_is_claimable ());
    EXPECT_NE (generation, ended_generation);
    send_order (replacement);
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    EXPECT_EQ (node.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    send_admission (replacement, wire::command::hello);
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    const auto pending = node.topology ().peer (remote.node_routing_id);
    ASSERT_TRUE (pending);
    EXPECT_FALSE (pending->ready);
    EXPECT_TRUE (ended_is_claimable ());

    const auto claim_replacement_order = [&] {
        auto claim = framework::test::claim_mailbox (
          node.mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
        ASSERT_TRUE (claim);
        EXPECT_EQ (claim->records.front ()->route_generation (), generation);
        EXPECT_TRUE (node.mailbox ().release (*claim));
    };

    send_order (replacement);
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::application);
    ASSERT_NO_FATAL_FAILURE (claim_replacement_order ());

    ASSERT_NO_FATAL_FAILURE (admit (replacement));
    EXPECT_FALSE (ended_is_claimable ());
    send_order (replacement);
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::application);
    ASSERT_NO_FATAL_FAILURE (claim_replacement_order ());
    EXPECT_EQ (node.receive_next (node.begin_receive_turn (), now ()),
               mesh::raw_mesh_pump_result_t::no_data);
}

TEST_F (EndedRouteClaim, WithdrawalDiscardsAdmissionBeforeQueuedSendIsClaimed)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_order (source);
    ASSERT_NO_FATAL_FAILURE (end (source));

    EXPECT_TRUE (ended_is_claimable ());
    node.forget_peer (remote.node_routing_id, remote.advertised_endpoint);
    EXPECT_FALSE (ended_is_claimable ());
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    EXPECT_EQ (node.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application), 0u);
}

TEST_F (EndedRouteClaim, LifecycleChangeDiscardsEndedAdmission)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_order (source);
    ASSERT_NO_FATAL_FAILURE (end (source));

    EXPECT_TRUE (ended_is_claimable ());
    auto new_descriptor = remote;
    ++new_descriptor.lifecycle_generation;
    node.expect_peer (new_descriptor);
    EXPECT_FALSE (ended_is_claimable ());
    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    EXPECT_EQ (node.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application), 0u);
}

TEST_F (EndedRouteClaim, ExpiryOfPendingReplacementDiscardsEndedAdmission)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_order (source);
    ASSERT_NO_FATAL_FAILURE (end (source));

    zlink::dealer_socket_t replacement (*context);
    connect (replacement);
    send_admission (replacement, wire::command::hello);
    ASSERT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    const auto pending = node.topology ().peer (remote.node_routing_id);
    ASSERT_TRUE (pending);
    ASSERT_FALSE (pending->ready);
    ASSERT_TRUE (ended_is_claimable ());

    auto tick = node.tick_liveness (now () + 1min).result ();
    ASSERT_TRUE (tick);
    EXPECT_EQ (tick.value ().timed_out_nodes,
               (std::vector<std::vector<std::uint8_t>>{remote.node_routing_id}));
    EXPECT_FALSE (ended_is_claimable ());
}

TEST_F (EndedRouteClaim, DropsHandshakeFromEndedRoute)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    send_admission (source, wire::command::hello);
    send_admission (source, wire::command::admit);
    source.send ().message (zlink::message_t::from (wire::encode_reject (3))).submit ();
    ASSERT_NO_FATAL_FAILURE (end (source));

    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
        EXPECT_FALSE (node.topology ().peer (remote.node_routing_id));
    }
}

TEST_F (EndedRouteClaim, DropsReplyRelayFromEndedAdmittedRoute)
{
    zlink::dealer_socket_t source (*context);
    connect (source);
    ASSERT_NO_FATAL_FAILURE (admit (source));
    const wire::reply_relay_t relay{
      {1, 2}, 3, {4, 5}, 6, {"coordinator", 7, remote.node_routing_id, 1, "authority-v1"}, 8, 9};
    source.send ()
      .message (zlink::message_t::from (wire::encode_reply_relay (relay)))
      .message (wire::encode_application_payload_message ({"Reply", "application/json", {'1'}}))
      .submit ();
    ASSERT_NO_FATAL_FAILURE (end (source));

    EXPECT_EQ (receive (), mesh::raw_mesh_pump_result_t::infrastructure);
    EXPECT_EQ (node.mailbox ().pending_messages (mesh::service_mailbox_domain_t::infrastructure),
               0u);
    EXPECT_EQ (node.receive_next (node.begin_receive_turn (), now ()),
               mesh::raw_mesh_pump_result_t::no_data);
}
}
