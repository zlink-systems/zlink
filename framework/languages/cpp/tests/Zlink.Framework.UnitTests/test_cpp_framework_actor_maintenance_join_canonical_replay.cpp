/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_replay_access.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace deferred_join_reservation_probe
{
using namespace zlink::framework::test::actor_maintenance_join;

TEST (ActorMaintenanceJoin, CanonicalReplayMaterializesBodyWithoutInvocationCache)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                   .async<probe_t> ()
                   .result ());
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto actor =
      state->lane
        .run ([&] {
            return fw::detail::actor_ref_access_t::make (
              fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
              state->actor_generations.at (std::string (actor_type) + ":" + actor_id));
        })
        .get ();
    namespace wire = fw::runtime::protocol;
    fw::runtime::messaging::envelope_header_t header;
    header.kind = fw::runtime::messaging::message_kind_t::command;
    header.channel_name = mesh_name;
    header.message_name = record_t::packet_name;
    header.content_type = "application/json";
    auto encoded = fw::detail::encode_actor_packet (
      header, zlink::message_t::from (zlink::detail::json_profile::encode (record_t{73})));
    wire::frozen_application_record_t frozen;
    frozen.kind = wire::frozen_record_kind_t::actor_send;
    frozen.body = wire::frozen_actor_application_body_t{
      {}, wire::application_payload_t::from_parts (std::move (encoded).take_items ())};
    auto record = std::make_shared<fw::runtime::serial_application_record_t> ();
    record->payload ().emplace ();
    record->payload ()->application_record = std::move (frozen);
    int terminals = 0;
    record->native_reply = [&] (auto) {
        ++terminals;
        return true;
    };
    fw::detail::handoff_packet_t packet;
    packet.packet_name = record_t::packet_name;
    packet.content_type = header.content_type;
    packet.application_record = record;
    ASSERT_FALSE (record->invocation ());
    ASSERT_TRUE (packet.payload.empty ());
    fw::detail::spot_node_runtime_t runtime (state);
    ASSERT_TRUE (fw::detail::spot_node_runtime_test_access_t::replay (
                   runtime, actor, {std::move (packet)}, services)
                   .result ());
    EXPECT_EQ (terminals, 1);
    EXPECT_EQ (evidence.recorded, (std::vector<int>{73}));
}

TEST (ActorMaintenanceJoin, MalformedCanonicalReplaySettlesNativeTerminal)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                   .async<probe_t> ()
                   .result ());
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto actor =
      state->lane
        .run ([&] {
            return fw::detail::actor_ref_access_t::make (
              fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
              state->actor_generations.at (std::string (actor_type) + ":" + actor_id));
        })
        .get ();
    namespace wire = fw::runtime::protocol;
    fw::runtime::messaging::envelope_header_t header;
    header.kind = fw::runtime::messaging::message_kind_t::command;
    header.channel_name = mesh_name;
    header.message_name = record_t::packet_name;
    header.content_type = "application/json";
    wire::application_payload_t::multipart_t parts;
    parts.push_back (fw::runtime::messaging::envelope_codec_t{}.encode_header (header));
    wire::frozen_application_record_t frozen;
    frozen.kind = wire::frozen_record_kind_t::actor_send;
    frozen.body = wire::frozen_actor_application_body_t{
      {}, wire::application_payload_t::from_parts (std::move (parts))};
    auto record = std::make_shared<fw::runtime::serial_application_record_t> ();
    record->payload ().emplace ();
    record->payload ()->application_record = std::move (frozen);
    int terminals = 0;
    record->native_reply = [&] (auto) {
        ++terminals;
        return true;
    };
    fw::detail::handoff_packet_t packet;
    packet.packet_name = record_t::packet_name;
    packet.content_type = header.content_type;
    packet.application_record = record;
    ASSERT_FALSE (record->invocation ());
    ASSERT_TRUE (packet.payload.empty ());
    fw::detail::spot_node_runtime_t runtime (state);
    ASSERT_TRUE (fw::detail::spot_node_runtime_test_access_t::replay (
                   runtime, actor, {std::move (packet)}, services)
                   .result ());
    EXPECT_EQ (terminals, 1);
    EXPECT_TRUE (evidence.recorded.empty ());
}
} // namespace deferred_join_reservation_probe
