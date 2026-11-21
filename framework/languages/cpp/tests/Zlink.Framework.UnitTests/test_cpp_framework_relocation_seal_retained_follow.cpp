/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include "../support/relocation_seal_host_access.hpp"
#include "../support/relocation_seal_execution_fixture.hpp"
#include "../support/relocation_seal_native_records.hpp"

using namespace framework_relocation_seal_fixture;

using zlink::framework::test::begin_reserved_object;

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, source_fenced_relay_survives_target_authority_commit)
{
    for (const auto kind : {stateful::object_kind_t::actor, stateful::object_kind_t::user_spot}) {
        auto target_host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("relay-target"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        target_host->configure_stateful_dispatch (
          [] (const stateful::accepted_record_authority_query_t &)
            -> task_t<std::optional<stateful::accepted_record_authority_t>> {
              co_return std::nullopt;
          });
        const auto source_rid = zlink::routing_id_t::from ("relay-source").to_bytes ();
        const auto target_rid = zlink::routing_id_t::from ("relay-target").to_bytes ();
        const stateful::object_ref_t source{kind, "relay-object",         3,
                                            4,    "relocation-seal-test", "relay-source"};
        auto target = source;
        target.node_id = "relay-target";
        protocol::relocation_prepare_t prepare{};
        prepare.source_node_routing_id = source_rid;
        prepare.source_node_generation = 1;
        prepare.target = {target_rid, 1, "target-owner", 5};
        prepare.coordinator = {"source-owner", 7, source_rid, 1, "captured-version"};
        protocol::actor_message_header_t actor_header;
        actor_header.target = {source.key, 3, target_rid, 1, 4, 5};
        protocol::spot_message_header_t spot_header;
        spot_header.target = {source.key, 3, target_rid, 1, 4, 5};
        std::variant<protocol::actor_message_header_t, protocol::spot_message_header_t> header =
          actor_header;
        if (kind != stateful::object_kind_t::actor)
            header = spot_header;
        const auto address = [&] (std::uint64_t authority, std::uint64_t lease) {
            std::visit (
              [&] (auto &value) {
                  value.target.authority_owner_generation = authority;
                  value.target.owner_lease_generation = lease;
              },
              header);
        };
        const auto accept = [&] (const auto &rid) {
            return std::visit (
              [&] (const auto &value) {
                  return host::public_host_runtime_test_access_t::accept_relay (*target_host,
                                                                                target, value, rid)
                    .first;
              },
              header);
        };
        host::public_host_runtime_test_access_t::stage_relay (*target_host, prepare, source,
                                                              target);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        // The Store assigns an unrelated larger generation; the captured source route is unchanged.
        target.authority_owner_generation = 19;
        host::public_host_runtime_test_access_t::stage_relay (*target_host, prepare, source,
                                                              target);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        EXPECT_NE (accept (zlink::routing_id_t::from ("unrelated-source").to_bytes ()),
                   stateful::stateful_error_t::none);
        address (8, 5);
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        address (19, 5);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        address (19, 99);
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        address (4, 5);
        target.authority_owner_generation = 23;
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        target_host->close ();
    }
}

TEST (FrameworkRelocationSealContract, native_follow_increments_wire_hop_and_stops_at_eight)
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("hop-source"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("hop-target"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_node = source.topology ().local_descriptor ();
    const auto target_node = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_node));
    const auto ready = [&] {
        const auto one = source.topology ().peer (target_node.node_routing_id);
        const auto two = target.topology ().peer (source_node.node_routing_id);
        return one && one->ready && two && two->ready;
    };
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) source.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    ASSERT_TRUE (ready ());
    stateful::stateful_object_runtime_t objects;
    stateful::raw_stateful_dispatch_t dispatch (objects, source);
    const protocol::application_payload_t payload{
      "hop-test", "application/octet-stream", {1, 2, 3}};
    std::uint64_t sequence = 1;
    for (const auto kind :
         {protocol::frozen_record_kind_t::actor_send, protocol::frozen_record_kind_t::spot_send,
          protocol::frozen_record_kind_t::actor_request,
          protocol::frozen_record_kind_t::spot_request}) {
        SCOPED_TRACE (static_cast<int> (kind));
        const bool actor = kind == protocol::frozen_record_kind_t::actor_send
                           || kind == protocol::frozen_record_kind_t::actor_request;
        const bool request = kind == protocol::frozen_record_kind_t::actor_request
                             || kind == protocol::frozen_record_kind_t::spot_request;
        const stateful::object_ref_t owner{actor ? stateful::object_kind_t::actor
                                                 : stateful::object_kind_t::user_spot,
                                           "moving",
                                           7,
                                           1,
                                           "mesh",
                                           "source"};
        protocol::frozen_application_record_t record;
        record.kind = kind;
        record.source_kind =
          actor ? protocol::frozen_source_kind_t::node : protocol::frozen_source_kind_t::spot;
        if (!actor)
            record.source_spot_id = "producer";
        record.source = {"producer", 3, source_node.node_routing_id,
                         source_node.lifecycle_generation};
        record.operation = {33, sequence++};
        if (request) {
            record.operation_kind = actor ? 4 : 3;
            record.reply_route_id = 900 + sequence;
        }
        record.message_follow_hop_count = 7;
        if (actor)
            record.body = protocol::frozen_actor_application_body_t{
              {owner.key, 7, source_node.node_routing_id, source_node.lifecycle_generation, 1, 3},
              payload};
        else
            record.body = protocol::frozen_spot_application_body_t{
              {owner.key, 7, source_node.node_routing_id, source_node.lifecycle_generation, 1, 3},
              3,
              payload};
        stateful::turn_record_t turn;
        turn.application_record = record;
        ASSERT_TRUE (dispatch
                       .forward_accepted (owner, turn, target_node.node_routing_id,
                                          target_node.lifecycle_generation, 3,
                                          std::chrono::seconds (1), 2)
                       .result ()
                       .value ());
        zlink::framework::test::mailbox_claim_t claim;
        const auto receive_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
            (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
            claim = zlink::framework::test::claim_mailbox (
              target.mailbox (), mesh::service_mailbox_domain_t::application, 1, 65536);
            std::this_thread::yield ();
        }
        ASSERT_TRUE (claim);
        ASSERT_EQ (claim->records.size (), 1u);
        const auto &received = (*claim->records.front ());
        const auto bytes = protocol::message_bytes (received.parts ().front ());
        if (actor) {
            const auto header = protocol::decode_actor_message_header (
              bytes, request ? protocol::command::actorRequest : protocol::command::actorSend);
            EXPECT_EQ (header.message_follow_hop_count, 8);
            EXPECT_EQ (header.operation, record.operation);
            EXPECT_EQ (header.target.object_generation, 7u);
        } else {
            const auto header = protocol::decode_spot_message_header (
              bytes, request ? protocol::command::spotRequest : protocol::command::spotSend);
            EXPECT_EQ (header.message_follow_hop_count, 8);
            EXPECT_EQ (header.operation, record.operation);
            EXPECT_EQ (header.target.object_generation, 7u);
        }
        EXPECT_EQ (
          protocol::decode_application_payload (protocol::message_bytes (received.parts ()[1])),
          payload);
        if (request)
            ASSERT_TRUE (target.reply (received, payload));
        target.mailbox ().release (*claim);
        turn.application_record->message_follow_hop_count = 8;
        const auto extra =
          dispatch
            .forward_accepted (owner, turn, target_node.node_routing_id,
                               target_node.lifecycle_generation, 3, std::chrono::seconds (1), 2)
            .result ()
            .value ();
        EXPECT_FALSE (extra);
        // Drain an unexpected send so each regression row observes its own packet.
        if (extra) {
            zlink::framework::test::mailbox_claim_t unexpected;
            const auto drain_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (5);
            while (!unexpected && std::chrono::steady_clock::now () < drain_deadline) {
                (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
                unexpected = zlink::framework::test::claim_mailbox (
                  target.mailbox (), mesh::service_mailbox_domain_t::application, 1, 65536);
            }
            ASSERT_TRUE (unexpected);
            if (request)
                ASSERT_TRUE (target.reply ((*unexpected->records.front ()), payload));
            target.mailbox ().release (*unexpected);
        }
    }
    source.close ();
    target.close ();
}

TEST (FrameworkRelocationSealContract, exhausted_follow_request_settles_original_reply_once)
{
    mesh::raw_mesh_node_owner_t transport (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("exhausted-follow"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, owner, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (objects, transport);
    protocol::actor_message_header_t header;
    header.target = {owner.key, 7, {1}, 1, 1, 3};
    header.operation = {31, 41};
    header.correlation = 901;
    header.message_follow_hop_count = 8;
    auto accepted = std::make_shared<runtime::serial_application_record_t> ();
    accepted->payload ().emplace ();
    accepted->payload ()->application_record = stateful::accepted_actor_record (
      header, {"producer", 3, {1}, 1}, {"payload", "application/json", {1}}, true);
    int terminals = 0;
    std::optional<mesh::service_mailbox_record_t> taken_original;
    ASSERT_EQ (dispatch
                 .accept_local (owner, accepted, mesh::service_mailbox_record_t{}, taken_original,
                                [&] (const auto &reply) {
                                    ++terminals;
                                    EXPECT_FALSE (reply);
                                    return true;
                                })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({owner}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    ASSERT_EQ (sealed.seal.participants.front ().pending_application.size (), 1u);
    const auto turn = sealed.seal.participants.front ().pending_application.front ();
    const auto forwarded =
      dispatch.forward_accepted (owner, turn, {2}, 1, 4, std::chrono::seconds (1), 2).result ();
    ASSERT_TRUE (forwarded);
    EXPECT_TRUE (forwarded.value ());
    EXPECT_EQ (terminals, 1);
    EXPECT_FALSE (dispatch.forward_accepted (owner, turn, {2}, 1, 4, std::chrono::seconds (1), 2)
                    .result ()
                    .value ());
    EXPECT_EQ (terminals, 1);
}

TEST (FrameworkRelocationSealContract, accepted_follow_preserves_actor_and_spot_hop)
{
    protocol::actor_message_header_t actor;
    actor.message_follow_hop_count = 7;
    actor.target = {"actor", 7, {1}, 1, 2, 3};
    actor.operation = {1, 2};
    protocol::spot_message_header_t spot;
    spot.message_follow_hop_count = 8;
    spot.target = {"spot", 7, {1}, 1, 2, 3};
    spot.operation = {1, 3};
    spot.source_spot_id = "producer";
    const protocol::request_source_fence_t source{"owner", 3, {1}, 1};
    const protocol::application_payload_t payload{"hop-test", "application/json", {1}};
    const auto actor_record = stateful::accepted_actor_record (actor, source, payload, false);
    const auto spot_record = stateful::accepted_spot_record (spot, source, payload, false);
    EXPECT_EQ (actor_record.message_follow_hop_count, 7);
    EXPECT_EQ (spot_record.message_follow_hop_count, 8);
    for (const auto &record : {actor_record, spot_record}) {
        const auto frozen = protocol::encode_frozen_application_record (record);
        const auto decoded = protocol::decode_frozen_record (frozen.canonical_bytes);
        EXPECT_EQ (decoded.message_follow_hop_count,
                   protocol::summarize_frozen_application_record (record).message_follow_hop_count);
        EXPECT_EQ (decoded.operation, record.operation);
        auto oversized = record;
        oversized.metadata.push_back ({"oversized", std::string (protocol::metadataBytes, 'x')});
        EXPECT_THROW (protocol::encode_frozen_application_record (oversized),
                      protocol::service_wire_error_t);
        auto invalid = record;
        invalid.message_follow_hop_count = 9;
        EXPECT_THROW (protocol::encode_frozen_application_record (invalid),
                      protocol::service_wire_error_t);
    }
    actor.message_follow_hop_count = 0;
    EXPECT_TRUE (stateful::accepted_actor_record (actor, {}, {}, false).metadata.empty ());
}

TEST (FrameworkRelocationSealContract, retained_follow_rejects_previous_object_generation)
{
    auto descriptor = test_descriptor ("follow-generation");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (), [&] (const auto &query) {
          return host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    protocol::actor_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, recorded_source, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, recorded_source, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::generation_stale);
    host->close ();
}

TEST (FrameworkRelocationSealContract, retained_spot_rejects_previous_object_generation)
{
    auto descriptor = test_descriptor ("follow-generation");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::user_spot, "moving", 7, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (), [&] (const auto &query) {
          return host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    protocol::spot_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, recorded_source,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, recorded_source,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::generation_stale);
    host->close ();
}

TEST (FrameworkRelocationSealContract, retained_target_keeps_recorded_sender_without_store)
{
    auto descriptor = test_descriptor ("authority-order");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    int resolves = 0;
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (),
      [&] (const auto &query) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          ++resolves;
          EXPECT_EQ (query.target.object_generation, object.object_generation);
          co_return co_await host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    protocol::actor_message_header_t header;
    header.target = {object.key, 6, {1}, 1, 1, 3};
    // Ordinary Ready delivery keeps the existing current-object normalization.
    std::optional<stateful::retained_relocation_source_t> retained;
    ASSERT_TRUE (objects.find (object.kind, object.key, std::nullopt, &retained));
    EXPECT_EQ (
      dispatch.accepted_actor_authority (object, header, {2}, 1, recorded_source, false, retained)
        .result ()
        .value ()
        .first,
      stateful::stateful_error_t::none);
    EXPECT_EQ (resolves, 1);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_TRUE (objects.find (object.kind, object.key, std::nullopt, &retained));
    ASSERT_TRUE (retained);
    header.target.object_generation = 7;
    resolves = 0;
    const auto accept = [&] {
        return dispatch
          .accepted_actor_authority (object, header, {2}, 1, recorded_source, false, retained)
          .result ()
          .value ();
    };
    const auto accepted = accept ();
    ASSERT_EQ (accepted.first, stateful::stateful_error_t::none);
    ASSERT_TRUE (accepted.second);
    EXPECT_EQ (accepted.second->source.owner_id, "actual-sender");
    EXPECT_EQ (accepted.second->source.lease_generation, 11u);
    EXPECT_EQ (accepted.second->target_owner_lease_generation, 3u);
    EXPECT_EQ (accepted.second->source.node_routing_id, (std::vector<std::uint8_t>{2}));
    EXPECT_EQ (resolves, 1);
    ++header.target.authority_owner_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.authority_owner_generation;
    ++header.target.target_node_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.target_node_generation;
    ++header.target.owner_lease_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    EXPECT_EQ (resolves, 1);
    host->close ();
}

TEST (FrameworkRelocationSealContract,
      source_cleanup_failure_is_reported_with_flow_off_and_continues)
{
    auto state = std::make_shared<spot_node_builder_state_t> ("cleanup-failure");
    state->snapshot.routing_id = zlink::routing_id_t::from ("source");
    state->actor_types_by_id["bad"] = "actor";
    state->actor_generations["actor:bad"] = 2;
    state->actor_types_by_id["good"] = "actor";
    state->actor_generations["actor:good"] = 1;
    state->actor_instances["actor:bad"] = std::make_shared<int> (1);
    state->actor_instances["actor:good"] = std::make_shared<int> (2);
    state->dispatch.message_flow (message_flow_log_mode_t::off);
    state->monitoring = std::make_shared<monitoring_runtime_state_t> ();
    state->monitoring->runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    std::vector<std::exception_ptr> failures;
    state->monitoring->runtime_failures->bind_runtime_reporter (
      [&] (auto failure) { failures.push_back (failure); });
    detail::spot_node_runtime_t runtime (state);
    const auto source = zlink::routing_id_t::from ("source").to_string ();
    const std::vector<stateful::object_ref_t> sources{
      {stateful::object_kind_t::actor, "bad", 1, 1, "mesh", source},
      {stateful::object_kind_t::actor, "good", 1, 1, "mesh", source}};
    auto native = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cleanup-native"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = native->objects ();
    const auto created = begin_reserved_object (objects, sources.front (), "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed =
      objects.try_seal_relocation_aggregate ({sources.front ()}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (
      objects.begin_relocation_boundary (sealed.seal.token, [] (const auto &, const auto &) {})
        .first,
      stateful::stateful_error_t::none);
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    const auto retired = runtime.retire_relocation_source (sources).result ();
    // Shutdown's ordinary Actor destroy cannot reclaim a retained moving source.
    EXPECT_EQ (objects.destroy_actor (sources.front ()), stateful::stateful_error_t::moving);
    native->close ();
    EXPECT_TRUE (retired);
    ASSERT_EQ (failures.size (), 1u);
    EXPECT_NE (
      diagnostic_event_sink_t::exception_summary (failures.front ()).message.find ("generation"),
      std::string::npos);
    EXPECT_TRUE (state->actor_instances.contains ("actor:bad"));
    EXPECT_FALSE (state->actor_instances.contains ("actor:good"));
}

TEST (FrameworkRelocationSealContract, committed_payload_release_keeps_later_follow_request)
{
    using namespace zlink::framework::runtime;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("committed-source"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"original-requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "committed-spot", 1, 1, "mesh", "committed-source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto turn = [&] (std::uint64_t operation) {
        auto wire = native_spot_record (*host, spot, operation, operation + 70);
        const auto header = protocol::decode_spot_message_header (
          protocol::message_bytes (wire.parts ().front ()), protocol::command::spotRequest);
        const auto application =
          protocol::decode_application_payload (protocol::message_bytes (wire.parts ().back ()));
        stateful::turn_record_t record;
        record.sequence = operation;
        record.payload = protocol::encode_frozen_application_record (
                           stateful::accepted_spot_record (header, source, application, true))
                           .canonical_bytes;
        return record;
    };
    auto &dispatch = access_t::dispatch (*host);
    int committed_completions = 0, follow_completions = 0;
    ASSERT_EQ (dispatch
                 .stage_relocated (spot, turn (1),
                                   [&] (const auto &) {
                                       ++committed_completions;
                                       return true;
                                   })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ()
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, source)
                 .first,
               stateful::stateful_error_t::none);
    stateful::relocation_ingress_batch_t committed;
    ASSERT_EQ (host->objects ()
                 .finalize_relocation_cutover (sealed.seal.token, std::nullopt, &committed)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (committed.participants.size (), 1u);
    EXPECT_EQ (operations_of (committed.participants.front ().records),
               (std::vector<std::uint64_t>{1}));
    // This request belongs to the later Message Follow path, after the committed prefix.
    ASSERT_EQ (dispatch
                 .stage_relocated (spot, turn (2),
                                   [&] (const auto &) {
                                       ++follow_completions;
                                       return true;
                                   })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    dispatch.release_relocated_payloads (spot, committed.participants.front ().records);
    auto forwarded = protocol::decode_frozen_record (turn (2).payload);
    const auto relay = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 7}, 1, {"source", 3, {1}, 1, "committed-authority"}, 1, forwarded);
    EXPECT_TRUE (dispatch.complete_relocated_source_async (spot, 2, relay, forwarded.application)
                   .result ()
                   .value ());
    EXPECT_EQ (follow_completions, 1);
    const auto captured = protocol::decode_frozen_record (turn (1).payload);
    const auto retired = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 7}, 1, {"source", 3, {1}, 1, "committed-authority"}, 1, captured);
    // Source payload release preserves the original request terminal (Location runtime §9.3).
    EXPECT_TRUE (dispatch.complete_relocated_source_async (spot, 1, retired, captured.application)
                   .result ()
                   .value ());
    EXPECT_EQ (committed_completions, 1);
    EXPECT_FALSE (dispatch.complete_relocated_source_async (spot, 1, retired, captured.application)
                    .result ()
                    .value ());
    EXPECT_EQ (committed_completions, 1);
    host->close ();
}
