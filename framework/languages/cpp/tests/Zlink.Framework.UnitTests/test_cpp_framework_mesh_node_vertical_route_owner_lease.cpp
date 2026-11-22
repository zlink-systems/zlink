/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/mesh_node_vertical_location_resolvers.hpp"
#include "../support/mesh_node_vertical_transport_fixture.hpp"
#include "../support/session_disconnect_probe_spot.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/actors/actor_ref_access.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include <runtime/locations/location_repository.hpp>
#include <service_wire_codec.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace
{
zlink::message_t make_route_multicast_frame (std::string_view packet_name,
                                             std::string_view topic,
                                             std::string_view payload)
{
    namespace messaging = zlink::framework::runtime::messaging;
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::publish;
    header.channel_name = "work";
    header.message_name = packet_name;
    header.content_type = messaging::envelope_codec_t::default_content_type;
    header.topic = std::string (topic);
    header.source = header.channel_name;
    const auto header_bytes = messaging::envelope_codec_t{}.encode_header (header).to_bytes ();
    std::vector<std::uint8_t> frame{'Z', 'L', 'F', 'E'};
    const auto header_size = static_cast<std::uint32_t> (header_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> (header_size >> 24));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 16));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 8));
    frame.push_back (static_cast<std::uint8_t> (header_size));
    frame.insert (frame.end (), header_bytes.begin (), header_bytes.end ());
    frame.insert (frame.end (), payload.begin (), payload.end ());
    return zlink::message_t::from (std::move (frame));
}

struct owner_lease_node_send_message_t
{
    std::string value;
};

struct owner_lease_node_send_state_t
{
    std::shared_ptr<std::promise<void>> dispatched;
};

class owner_lease_node_send_handler_t
{
  public:
    explicit owner_lease_node_send_handler_t (owner_lease_node_send_state_t &state) : _state (state)
    {
    }

    void handle (const owner_lease_node_send_message_t &,
                 const zlink::framework::route_message_context_t &)
    {
        _state.dispatched->set_value ();
    }

  private:
    owner_lease_node_send_state_t &_state;
};

void enqueue_application_route (
  const std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t> &node,
  zlink::framework::runtime::host::ready_record_t owner,
  zlink::framework::runtime::host::receive_record_t record,
  std::vector<zlink::message_t> parts)
{
    namespace mesh = zlink::framework::runtime::mesh;
    mesh::service_mailbox_record_t mailbox_record;
    mailbox_record.owner = mesh::service_mailbox_t::application_owner (owner);
    mailbox_record.domain = mesh::service_mailbox_domain_t::application;
    mailbox_record.before_application_handler = [] {};
    mailbox_record.application.emplace (
      zlink::framework::runtime::host::local_application_dispatch_t{
        std::move (owner), std::move (record), std::move (parts)});
    assert (node->native_node ().transport ().mailbox ().try_enqueue (std::move (mailbox_record)));
}
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_route_internal_packets_precede_application_dispatch ()
{
    zlink::framework::completion_owner_t completion_owner;
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    auto registration = make_node ("tcp://127.0.0.1:0", "internal-route-owner");
    auto node_send_dispatched = std::make_shared<std::promise<void>> ();
    auto node_send_dispatched_future = node_send_dispatched->get_future ();
    registration->handlers
      .on_send<owner_lease_node_send_handler_t, owner_lease_node_send_message_t> (
        "vertical-mesh", "OwnerLeaseProbe", &owner_lease_node_send_handler_t::handle);
    auto worker = std::make_shared<runtime::offload_executor_t> (1, "rel-358-internal-route");
    registration->spot_state->worker_executor = worker;
    spot_node_runtime_t spots (registration->spot_state);
    spots.set_route_client (route_client_t{});

    auto context = std::make_shared<spot_context_state_t> ();
    context->node = registration->spot_state;
    context->lane_owner = registration->spot_state;
    context->node_rid = node_rid_t::from_string ("internal-route-owner");
    context->mesh_name = "vertical-mesh";
    context->spot_id = spot_id_t ("internal-route-spot");
    context->spot_name = "internal-route-spot";
    context->serial_executor = worker;

    auto application_jobs = std::make_shared<runtime::application_job_queue_t> (
      runtime::application_job_queue_configuration_t{application_job_queue_profile_t::balanced,
                                                     std::uint32_t{1}, 1, 1});
    auto ingress_supply = application_jobs->try_reserve_supply ();
    assert (ingress_supply);
    auto ingress_entered = std::make_shared<std::latch> (1);
    auto multicast_received = std::make_shared<std::promise<std::string>> ();
    auto multicast_future = multicast_received->get_future ();
    context->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::subscription, "Rel358MulticastPayload", "rel-358-topic",
      std::type_index (typeid (int)), std::type_index (typeid (void)),
      std::type_index (typeid (void)), std::type_index (typeid (void)), "work"});
    context->handler_invokers.push_back (
      [multicast_received, ingress_entered,
       application_jobs] (void *, void *, service_provider_t &, serializer_registry_t &,
                          const zlink::message_t &message,
                          const spot_inbound_message_t &inbound) -> task_t<zlink::message_t> {
          assert (ingress_entered->try_wait ());
          const auto capacity = application_jobs->snapshot ();
          assert (capacity.effective_max_queued_application_jobs == 1);
          assert (capacity.queued_application_jobs == 0);
          assert (inbound.channel_name == "work");
          assert (
            (inbound.values
             == std::map<std::string, std::string>{{"trace-key", "native-receive-snapshot"}}));
          multicast_received->set_value (message.to_string ());
          co_return zlink::message_t{};
      });

    std::atomic_bool disconnect_called{false};
    spot_actor_admission_callbacks_t actor_callbacks;
    actor_callbacks.on_disconnect_actor = [&disconnect_called] (void *, void *) -> task_t<void> {
        disconnect_called.store (true, std::memory_order_release);
        co_return;
    };
    test::register_session_disconnect_probe (context,
                                             std::move (actor_callbacks.on_disconnect_actor));
    registration->spot_state->spot_contexts_by_id.emplace (context->spot_id,
                                                           spot_context_access_t::create (context));

    spot_node_builder_state_t::actor_factory_registration_t actor_factory;
    actor_factory.actor_type = std::type_index (typeid (int));
    actor_factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    actor_factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    registration->spot_state->actor_factories.emplace ("vertical.actor", std::move (actor_factory));
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("internal-route-owner"),
                                                 "vertical.actor", "internal-route-actor", 1);
    registration->spot_state->actor_instances.emplace ("vertical.actor:internal-route-actor",
                                                       std::make_shared<int> (7));
    registration->spot_state->actor_spot_ids.emplace ("vertical.actor:internal-route-actor",
                                                      context->spot_id);
    registration->spot_state->actor_generations.emplace ("vertical.actor:internal-route-actor", 1);
    record_actor_instance_index_unlocked (
      *registration->spot_state, actor,
      registration->spot_state->actor_instances.at ("vertical.actor:internal-route-actor").get ());

    serializer_registry_t serializers;
    serializers.add<owner_lease_node_send_message_t> (
      [] (const owner_lease_node_send_message_t &message) {
          return encoded_payload_t::from_string (message.value);
      },
      [] (const encoded_payload_t &payload) {
          return owner_lease_node_send_message_t{payload.to_string ()};
      });
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    services.add_singleton<owner_lease_node_send_state_t> (
      std::make_unique<owner_lease_node_send_state_t> (node_send_dispatched));
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &location_store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (location_store));
    location_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    auto &actor_gateway = provider.get_required<actor_gateway_runtime_t> ();
    auto &location_runtime = provider.get_required<runtime::location_runtime_t> ();
    location_runtime.start (*registration->routing_id);
    runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers, {}, {}, application_jobs);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();
    auto native_spot = std::make_shared<host::spot_handle_t> (
      node->native_node ()
        .get_or_create_spot_async (std::string (context->spot_id))
        .result ()
        .value ());
    registration->spot_state->lane.run ([&] { context->native_spot = native_spot; }).get ();

    native_spot->set_subscription ("work", "rel-358-topic");
    namespace wire = runtime::protocol::generated::detail;
    wire::logicalMulticast_23_t multicast;
    multicast.flags = static_cast<std::uint8_t> (runtime::protocol::flag::metadata);
    multicast.channelName.value = "work";
    multicast.topic.value = "rel-358-topic";
    multicast.sourceSpotId.value = "source-spot";
    wire::writer_t multicast_header;
    assert (wire::encode_head_logicalMulticast_23 (multicast_header, multicast)
            == wire::error_code::ok);
    const auto multicast_owner =
      runtime::mesh::service_mailbox_t::application_owner (host::owner_kind_t::node);
    const auto multicast_source = zlink::routing_id_t::from ("route-source").to_bytes ();
    auto ingress_permit =
      std::make_shared<runtime::application_job_queue_t::permit_t> (std::move (*ingress_supply));
    ingress_supply.reset ();
    ingress_permit->mark_queued ();
    auto before_multicast_handler = [permit = std::move (ingress_permit), ingress_entered] {
        permit->release_for_handler_entry ();
        ingress_entered->count_down ();
    };
    const auto queued_ingress = application_jobs->snapshot ();
    assert (queued_ingress.effective_max_queued_application_jobs == 1);
    assert (queued_ingress.queued_application_jobs == 1);
    assert (queued_ingress.permits_in_use == 1);
    std::vector<zlink::message_t> multicast_parts{
      zlink::message_t::from (std::move (multicast_header.data)),
      zlink::message_t::from (
        mesh_metadata_codec_t::encode ({{"trace-key", "native-receive-snapshot"}})),
      zlink::message_t::from (runtime::protocol::encode_application_payload (
        runtime::protocol::application_payload_t::from_parts ({make_route_multicast_frame (
          "Rel358MulticastPayload", "rel-358-topic", "multicast-through-host")})))};
    auto multicast_record = runtime::mesh::service_mailbox_record_t::from_parts (
      multicast_owner, runtime::mesh::service_mailbox_domain_t::application,
      std::move (multicast_parts), multicast_source, std::nullopt, 0, {}, {},
      std::move (before_multicast_handler));
    assert (
      node->native_node ().transport ().mailbox ().try_enqueue (std::move (multicast_record)));
    assert (multicast_future.wait_for (1s) == std::future_status::ready);
    assert (multicast_future.get () == "multicast-through-host");

    messaging::client_call_codec_t route_codec;
    auto disconnect_header = route_codec.create_envelope (
      messaging::message_kind_t::request, "work", spot_actor_packet_route_request_t::packet_name);
    auto disconnect_parts = route_codec.encode_envelope_parts (
      disconnect_header,
      make_spot_actor_packet_route_request (
        actor, context->spot_id, runtime::protocol::session_disconnected_packet_name,
        zlink::message_t::from (runtime::protocol::encode_session_disconnected (
          {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7})),
        {}),
      serializers);
    auto disconnect_reply = std::make_shared<std::promise<std::vector<zlink::message_t>>> ();
    auto disconnect_reply_future = disconnect_reply->get_future ();
    host::ready_record_t node_owner;
    node_owner.owner_kind = host::owner_kind_t::node;
    host::receive_record_t node_record;
    node_record.kind = host::record_kind_t::node_request;
    node_record.source_node_rid = zlink::routing_id_t::from ("route-source");
    node_record.reply_token.host = node->native_node ().shared_from_this ();
    node_record.reply_token.local_reply =
      [disconnect_reply] (const std::vector<zlink::message_t> &parts) {
          disconnect_reply->set_value (parts);
          return true;
      };
    const auto binding_before = actor_gateway.bound_session_route_async (actor).result ();
    assert (binding_before && !binding_before.value ());
    runtime::location_runtime_test_access_t::set_deadline (
      location_runtime, std::chrono::steady_clock::time_point::min ());
    assert (!location_runtime.owner_lease_usable ());
    messaging::client_call_codec_t node_send_codec;
    auto node_send_header = node_send_codec.create_envelope (messaging::message_kind_t::command,
                                                             "vertical-mesh", "OwnerLeaseProbe");
    auto node_send_parts = node_send_codec.encode_envelope_parts (
      node_send_header, owner_lease_node_send_message_t{"owner lease independent"}, serializers);
    auto node_send_record = runtime::mesh::service_mailbox_record_t::from_parts (
      runtime::mesh::service_mailbox_t::application_owner (host::owner_kind_t::node),
      runtime::mesh::service_mailbox_domain_t::application,
      {zlink::message_t::from (runtime::protocol::encode_node_send_header ()),
       zlink::message_t::from (runtime::protocol::encode_application_payload (
         runtime::protocol::application_payload_t::from_parts (
           std::move (node_send_parts).take_items ())))},
      zlink::routing_id_t::from ("route-source").to_bytes (), std::nullopt, 0, {}, {}, [] {});
    assert (
      node->native_node ().transport ().mailbox ().try_enqueue (std::move (node_send_record)));
    assert (node_send_dispatched_future.wait_for (1s) == std::future_status::ready);
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));

    enqueue_application_route (node, std::move (node_owner), std::move (node_record),
                               std::move (disconnect_parts).take_items ());
    assert (disconnect_reply_future.wait_for (1s) == std::future_status::ready);
    auto reply_parts = messaging::message_parts_t (disconnect_reply_future.get ());
    const auto reply = route_codec.decode_envelope_reply<spot_actor_packet_route_reply_t> (
      reply_parts, serializers, "disconnect reply is empty", "disconnect reply is invalid",
      "actor disconnect");
    assert (reply && reply.value ().has_reply && reply.value ().payload.empty ());
    assert (disconnect_called.load (std::memory_order_acquire));
    const auto binding_after = actor_gateway.bound_session_route_async (actor).result ();
    assert (binding_after && !binding_after.value ());
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));

    service.stop ();
    location_runtime.stop ();
}
} // namespace framework_mesh_node_vertical_tests
