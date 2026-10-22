/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/channels/route_packet_dispatcher.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <gtest/gtest.h>

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;

// Access the owner's lane only in this white-box test. Closing it makes any
// additional topology turn fail deterministically, without a timing assertion.
struct topology_lane_access_t
{
    using member_t = zlink::framework::runtime::state_lane_t mesh::service_topology_registry_t::*;
    friend member_t topology_lane_member (topology_lane_access_t);
};

template <topology_lane_access_t::member_t Member> struct topology_lane_member_t
{
    friend topology_lane_access_t::member_t topology_lane_member (topology_lane_access_t)
    {
        return Member;
    }
};

template struct topology_lane_member_t<&mesh::service_topology_registry_t::_lane>;

TEST (ServiceTopologyPreparedFacts, ReadsDoNotRequestTopologyTurns)
{
    mesh::service_node_descriptor_t local{"prepared-facts",
                                          {'a'},
                                          1,
                                          1,
                                          "tcp://127.0.0.1:1",
                                          {{"X", 100}},
                                          mesh::service_node_state_t::serving};
    mesh::service_topology_registry_t topology (local);
    auto remote = local;
    remote.node_routing_id = {'b'};
    ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (remote, {1}));
    auto &lane = topology.*topology_lane_member (topology_lane_access_t{});
    lane.close ();

    // These are the facts read by the message receive/send paths (spec 08 §3 E2).
    EXPECT_NO_THROW (EXPECT_EQ (local, topology.local_descriptor ()));
    EXPECT_NO_THROW ({
        const auto peer = topology.peer (remote.node_routing_id);
        ASSERT_TRUE (peer);
        EXPECT_EQ (remote, peer->descriptor);
        EXPECT_EQ (std::vector<std::uint8_t>{1}, peer->connection_id);
    });
    EXPECT_NO_THROW (EXPECT_FALSE (topology.peer ({'c'})));
}

TEST (ServiceTopologyPreparedFacts, PublishesChangesBeforeNotification)
{
    mesh::service_node_descriptor_t local{"prepared-facts",
                                          {'a'},
                                          1,
                                          1,
                                          "tcp://127.0.0.1:1",
                                          {{"X", 100}},
                                          mesh::service_node_state_t::serving};
    mesh::service_topology_registry_t topology (local);
    auto remote = local;
    remote.node_routing_id = {'b'};
    std::shared_ptr<const mesh::admitted_peer_t> notified_peer;
    auto notified_local = local;
    int notifications = 0;
    topology.set_change_handler ([&] {
        notified_local = topology.local_descriptor ();
        notified_peer = topology.peer (remote.node_routing_id);
        ++notifications;
    });
    ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (remote, {1}));
    ASSERT_TRUE (notified_peer);
    EXPECT_EQ (remote, notified_peer->descriptor);
    const auto retained = notified_peer;

    ++remote.descriptor_revision;
    remote.channels.front ().weight = 50;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               topology.admit (remote, {1}, mesh::service_liveness_registry_t::clock_t::now (),
                               protocol::command::update));
    ASSERT_TRUE (notified_peer);
    EXPECT_EQ (remote, notified_peer->descriptor);
    EXPECT_GT (notified_peer->admission_epoch, retained->admission_epoch);
    EXPECT_EQ (100, retained->descriptor.channels.front ().weight);
    EXPECT_EQ (mesh::peer_admission_result_t::duplicate_connection, topology.admit (remote, {1}));
    EXPECT_FALSE (topology.disconnect (remote.node_routing_id, {2}));
    EXPECT_EQ (2, notifications);

    ++local.descriptor_revision;
    local.state = mesh::service_node_state_t::draining;
    topology.publish_local (local);
    EXPECT_EQ (local, notified_local);
    ASSERT_TRUE (notified_peer);
    EXPECT_EQ (remote, notified_peer->descriptor);
    EXPECT_TRUE (topology.disconnect (remote.node_routing_id, {1}));
    EXPECT_FALSE (notified_peer);
    EXPECT_EQ (4, notifications);
    topology.set_change_handler ({});
}

TEST (ServiceTopologyPreparedFacts, ConcurrentReadsRetainCompleteDescriptors)
{
    mesh::service_node_descriptor_t local{"prepared-facts",
                                          {'a'},
                                          1,
                                          1,
                                          "tcp://127.0.0.1:1",
                                          {{"X", 100}},
                                          mesh::service_node_state_t::serving};
    mesh::service_topology_registry_t topology (local);
    auto remote = local;
    remote.node_routing_id = {'b'};
    remote.active_capacity_used = static_cast<std::uint32_t> (remote.descriptor_revision);
    ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (remote, {1}));
    const auto remote_routing_id = remote.node_routing_id;
    std::promise<void> reader_started;
    std::atomic<bool> writer_done = false;
    std::atomic<bool> incomplete_descriptor = false;
    std::jthread reader ([&] {
        auto retained = topology.peer (remote_routing_id);
        reader_started.set_value ();
        do {
            const auto peer = topology.peer (remote_routing_id);
            if (!peer
                || peer->descriptor.active_capacity_used != peer->descriptor.descriptor_revision)
                incomplete_descriptor.store (true);
        } while (!writer_done.load ());
        if (!retained || retained->descriptor.descriptor_revision != 1
            || retained->descriptor.active_capacity_used != 1)
            incomplete_descriptor.store (true);
    });
    reader_started.get_future ().wait ();
    constexpr int update_count = 64;
    for (int update = 0; update < update_count; ++update) {
        ++remote.descriptor_revision;
        remote.active_capacity_used = static_cast<std::uint32_t> (remote.descriptor_revision);
        EXPECT_EQ (mesh::peer_admission_result_t::admitted,
                   topology.admit (remote, {1}, mesh::service_liveness_registry_t::clock_t::now (),
                                   protocol::command::update));
    }
    writer_done.store (true);
    reader.join ();
    EXPECT_FALSE (incomplete_descriptor.load ());
}

template <typename T> T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

bool pump_until (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target,
                 const std::function<bool ()> &condition)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!condition () && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::yield ();
    }
    return condition ();
}

std::optional<int> peer_channel_weight (mesh::raw_mesh_node_owner_t &source,
                                        const std::vector<std::uint8_t> &target_routing_id,
                                        const std::string &channel_name)
{
    const auto peer = source.topology ().peer (target_routing_id);
    if (!peer)
        return std::nullopt;
    for (const auto &channel : peer->descriptor.channels) {
        if (channel.name == channel_name)
            return channel.weight;
    }
    return std::nullopt;
}

void expect_successful_channel_request (mesh::raw_mesh_node_owner_t &source,
                                        mesh::raw_mesh_node_owner_t &target)
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    std::promise<completion_t> promise;
    auto completion = promise.get_future ();
    const protocol::application_payload_t request{"WeightRequest", "application/json",
                                                  bytes ("request")};
    ASSERT_TRUE (await_task (
      source.request_to_channel ("X", request, 2s,
                                 [&promise] (foundation::operation_terminal_t terminal,
                                             zlink::message_t native_payload) mutable {
                                     auto payload = native_payload.to_bytes ();
                                     promise.set_value ({terminal, std::move (payload)});
                                 })));

    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto request_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < request_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 64u * 1024u);
        if (!claim) {
            ASSERT_NE (
              mesh::raw_mesh_pump_result_t::protocol_error,
              await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ())));
        }
    }
    ASSERT_TRUE (claim);
    ASSERT_EQ (1u, claim->records.size ());
    const protocol::application_payload_t reply{"WeightReply", "application/json", bytes ("reply")};
    ASSERT_TRUE (target.reply (claim->records.front (), reply));
    ASSERT_TRUE (target.mailbox ().release (*claim));

    const auto reply_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < reply_deadline) {
        ASSERT_NE (
          mesh::raw_mesh_pump_result_t::protocol_error,
          await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false)));
    }
    ASSERT_EQ (std::future_status::ready, completion.wait_for (0ms));
    EXPECT_EQ (foundation::operation_terminal_t::completed, completion.get ().first);
}

TEST (CppFrameworkRouteMeshWeightUpdate, SoleTargetStopsAndResumesAfterLiveWeightChanges)
{
    auto context = std::make_shared<zlink::context_t> ();
    auto target_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("weight-update-mesh");
    target_state->core_context = context;
    target_state->listen_endpoint = "inproc://weight-update-target";
    target_state->routing_id = zlink::routing_id_t::from ("weight-update-target");
    target_state->channels.emplace (
      "X", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    zlink::framework::detail::mesh_node_runtime_t target_node (target_state);
    target_node.start ();
    auto &target = target_node.native_node ().transport ();

    mesh::service_node_descriptor_t source_descriptor;
    source_descriptor.mesh_name = "weight-update-mesh";
    source_descriptor.node_routing_id = bytes ("weight-update-source");
    source_descriptor.lifecycle_generation = 1;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.advertised_endpoint = "inproc://weight-update-source";
    mesh::raw_mesh_node_owner_t source (
      zlink::framework::test::runtime_failure_options (
        zlink::framework::runtime::mesh::raw_mesh_node_options_t{std::move (source_descriptor)}),
      context);
    source.start ();

    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_descriptor));
    ASSERT_TRUE (pump_until (source, target, [&] {
        const auto peer = source.topology ().peer (target_descriptor.node_routing_id);
        return peer && peer->ready;
    }));

    expect_successful_channel_request (source, target);

    target_node.set_channel_weight ("X", 0);
    ASSERT_TRUE (pump_until (
      source, target,
      [&] { return peer_channel_weight (source, target_descriptor.node_routing_id, "X") == 0; }))
      << "peer did not receive the runtime weight-zero descriptor update";
    const protocol::application_payload_t rejected{"WeightRequest", "application/json",
                                                   bytes ("request")};
    const auto rejected_request =
      source.request_to_channel ("X", rejected, 2s, [] (auto, auto) {}).result ();
    EXPECT_FALSE (rejected_request);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable,
               rejected_request.error_kind ());

    target_node.set_channel_weight ("X", 100);
    ASSERT_TRUE (pump_until (
      source, target,
      [&] { return peer_channel_weight (source, target_descriptor.node_routing_id, "X") == 100; }))
      << "peer did not receive the restored positive-weight descriptor update";
    expect_successful_channel_request (source, target);

    source.close ();
    target_node.stop ();
}

// A descriptor revision is read, published, and submitted to every admitted
// route in one owner turn, so a route never receives a lower revision after a
// higher one (wire-protocol §4 DescriptorRevision ordering).  The first
// publication pauses after the topology accepted it and before its UPDATE was
// submitted; a second weight change may not overtake it on the route.
TEST (CppFrameworkRouteMeshWeightUpdate, ConcurrentWeightChangesReachPeerInRevisionOrder)
{
    auto context = std::make_shared<zlink::context_t> ();
    auto target_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("weight-order-mesh");
    target_state->core_context = context;
    target_state->listen_endpoint = "inproc://weight-order-target";
    target_state->routing_id = zlink::routing_id_t::from ("weight-order-target");
    target_state->channels.emplace (
      "X", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    zlink::framework::detail::mesh_node_runtime_t target_node (target_state);
    target_node.start ();
    auto &target = target_node.native_node ().transport ();

    mesh::service_node_descriptor_t source_descriptor;
    source_descriptor.mesh_name = "weight-order-mesh";
    source_descriptor.node_routing_id = bytes ("weight-order-source");
    source_descriptor.lifecycle_generation = 1;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.advertised_endpoint = "inproc://weight-order-source";
    const auto source_routing_id = source_descriptor.node_routing_id;
    mesh::raw_mesh_node_owner_t source (
      zlink::framework::test::runtime_failure_options (
        zlink::framework::runtime::mesh::raw_mesh_node_options_t{std::move (source_descriptor)}),
      context);
    source.start ();

    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_descriptor));
    ASSERT_TRUE (pump_until (source, target, [&] {
        const auto source_peer = source.topology ().peer (target_descriptor.node_routing_id);
        const auto target_peer = target.topology ().peer (source_routing_id);
        return source_peer && source_peer->ready && target_peer && target_peer->ready;
    }));

    std::promise<void> first_published;
    std::promise<void> second_finished;
    auto second_finished_signal = second_finished.get_future ().share ();
    std::atomic<int> publications{0};
    target.topology ().set_change_handler ([&] {
        if (publications.fetch_add (1) != 0)
            return;
        first_published.set_value ();
        // Without one owner turn the second change completes here, before
        // the first UPDATE is submitted.  With it, the second change waits
        // for this turn, and the bounded wait only ends the pause.
        (void) second_finished_signal.wait_for (1s);
    });

    std::thread first ([&] { target_node.set_channel_weight ("X", 50); });
    first_published.get_future ().wait ();
    std::thread second ([&] {
        target_node.set_channel_weight ("X", 100);
        second_finished.set_value ();
    });
    second.join ();
    first.join ();
    target.topology ().set_change_handler ({});

    ASSERT_TRUE (pump_until (source, target, [&] {
        return peer_channel_weight (source, target_descriptor.node_routing_id, "X") == 100;
    }));
    // Drain every UPDATE the source received.  A lower revision after the
    // higher one is stale: the source answers REJECT and the target drops it.
    const auto drain_deadline = std::chrono::steady_clock::now () + 200ms;
    while (std::chrono::steady_clock::now () < drain_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::yield ();
    }
    EXPECT_EQ (100, peer_channel_weight (source, target_descriptor.node_routing_id, "X"));
    expect_successful_channel_request (source, target);
    EXPECT_TRUE (target.topology ().peer (source_routing_id) != nullptr)
      << "the target dropped the peer after it received descriptor revisions out of order";

    source.close ();
    target_node.stop ();
}


struct channel_flow_handler_t
{
    std::string request (const std::string &value,
                         const zlink::framework::route_message_context_t &)
    {
        return value;
    }
    void send (const std::string &, const zlink::framework::route_message_context_t &) {}
};

class ChannelMessageFlow : public ::testing::TestWithParam<bool>
{
};

TEST_P (ChannelMessageFlow, RouteMeshRequestAndSendRecordAllReceiverStages)
{
    using namespace zlink::framework;
    std::vector<message_flow_event_t> events;
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      options, [&] (const auto &event) { events.push_back (event); });
    auto context = std::make_shared<zlink::context_t> ();
    mesh::service_node_descriptor_t target_descriptor{"channel-flow",
                                                      bytes ("flow-target"),
                                                      1,
                                                      1,
                                                      "inproc://channel-flow-target",
                                                      {{"X", 100}},
                                                      mesh::service_node_state_t::serving};
    auto source_descriptor = target_descriptor;
    source_descriptor.node_routing_id = bytes ("flow-source");
    source_descriptor.advertised_endpoint = "inproc://channel-flow-source";
    source_descriptor.channels.clear ();
    mesh::raw_mesh_node_options_t target_options{target_descriptor};
    target_options.dispatch = options;
    mesh::raw_mesh_node_owner_t target (
      zlink::framework::test::runtime_failure_options (std::move (target_options)), context);
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
                                          mesh::raw_mesh_node_options_t{source_descriptor}),
                                        context);
    target.start ();
    source.start ();
    source.expect_peer (target_descriptor);
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_descriptor));
    ASSERT_TRUE (pump_until (source, target, [&] {
        auto peer = source.topology ().peer (target_descriptor.node_routing_id);
        return peer && peer->ready;
    }));
    service_collection_t services;
    services.add_singleton<channel_flow_handler_t> ();
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    serializers.add<std::string> (
      [] (const std::string &value) { return encoded_payload_t::from_string (value); },
      [] (const encoded_payload_t &value) { return value.to_string (); }, "application/json");
    detail::route_handler_registry_t handlers;
    handlers.on_request<channel_flow_handler_t, std::string, std::string> (
      "X", "FlowRequest", &channel_flow_handler_t::request);
    handlers.on_send<channel_flow_handler_t, std::string> ("X", "FlowSend",
                                                           &channel_flow_handler_t::send);
    detail::no_route_internal_packet_dispatcher_t no_internal;
    handler_registry_t filters;
    detail::route_packet_dispatcher_t dispatcher (
      "X", provider, serializers, handlers, no_internal, options, &filters,
      handler_dispatch_kind_t::channel_send, handler_dispatch_kind_t::channel_request);
    const bool request = GetParam ();
    events.clear ();
    runtime::messaging::envelope_header_t outbound;
    outbound.kind = request ? runtime::messaging::message_kind_t::request
                            : runtime::messaging::message_kind_t::command;
    outbound.channel_name = "X";
    outbound.message_name = request ? "FlowRequest" : "FlowSend";
    outbound.correlation_id = request ? "channel-flow-correlation" : "";
    outbound.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
    outbound.flow_origin = flow_origin_t::application;
    auto outbound_parts = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
      outbound, zlink::message_t::from ("body"));
    auto payload =
      protocol::application_payload_t::from_parts (std::move (outbound_parts).take_items ());
    auto replied = std::make_shared<std::promise<void>> ();
    auto reply = replied->get_future ();
    if (request)
        ASSERT_TRUE (await_task (source.request_to_channel (
          "X", payload, 2s, [replied] (auto, auto) { replied->set_value (); })));
    else
        ASSERT_TRUE (await_task (source.send_to_channel ("X", payload)));
    std::optional<mesh::service_mailbox_claim_t> claim;
    ASSERT_TRUE (pump_until (source, target, [&] {
        if (!claim)
            claim = target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1,
                                                 64u * 1024u);
        return claim.has_value ();
    }));
    auto &received = claim->records.front ();
    auto parts = protocol::decode_application_parts (
      protocol::decode_application_payload (protocol::message_bytes (received.parts[1]), true));
    runtime::messaging::message_parts_t encoded (std::move (parts));
    auto header = runtime::messaging::envelope_codec_t{}.decode_header (encoded, true).value ();
    header.kind = request ? runtime::messaging::message_kind_t::request
                          : runtime::messaging::message_kind_t::command;
    header.channel_name = "X";

    auto result = dispatcher.dispatch (
      {zlink::routing_id_t::from ("flow-source"), received.correlation, std::move (encoded), {}},
      std::move (header));
    ASSERT_TRUE (result);
    ASSERT_EQ (4u, events.size ());
    EXPECT_EQ (message_flow_outcome_t::received, events[0].outcome);
    EXPECT_EQ (message_flow_outcome_t::admitted, events[1].outcome);
    EXPECT_EQ (message_flow_outcome_t::dispatched, events[2].outcome);
    EXPECT_EQ (request ? message_flow_outcome_t::replied : message_flow_outcome_t::completed,
               events[3].outcome);
    for (const auto &event : events) {
        EXPECT_EQ (outbound.flow_id, event.flow_id);
        EXPECT_EQ (events.front ().correlation_id.value_or (""),
                   event.correlation_id.value_or (""));
    }
    if (request)
        ASSERT_TRUE (target.reply (received, protocol::application_payload_t{
                                               "FlowReply", "application/json", bytes ("reply")}));
    ASSERT_TRUE (target.mailbox ().release (*claim));
    if (request)
        ASSERT_TRUE (pump_until (
          source, target, [&] { return reply.wait_for (0ms) == std::future_status::ready; }));

    source.close ();
    target.close ();
}

INSTANTIATE_TEST_SUITE_P (Modes, ChannelMessageFlow, ::testing::Values (false, true));

} // namespace
