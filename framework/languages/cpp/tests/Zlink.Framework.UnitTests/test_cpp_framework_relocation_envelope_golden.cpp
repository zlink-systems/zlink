/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Golden conformance pin for the relocation-envelope-v1 logical stream
// (28-relocation-flow.md §4.2). This test consumes
// golden/relocation-envelope-v1.json directly, decodes the logical hex
// stream with the production codec, verifies every decoded field against
// the golden's declared decode, re-encodes the decoded model byte-exactly,
// and checks the store-derived participant inventory ordering
// (participantId = sorted-UTF-8-authority-key index + 1) against the
// golden's declared inventory, following the store-record golden test's
// conventions.

#include "runtime/protocol/relocation_envelope_codec.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;

std::vector<std::uint8_t> from_hex (const std::string &value)
{
    const auto digit = [] (char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9')
            return static_cast<unsigned char> (ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<unsigned char> (ch - 'a' + 10);
        assert (false);
        return 0;
    };
    assert (value.size () % 2 == 0);
    std::vector<std::uint8_t> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (static_cast<std::uint8_t> (
          (digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

std::uint64_t u64 (const nlohmann::json &value)
{
    return std::stoull (value.get<std::string> ());
}

std::vector<std::uint8_t> fixture_bytes (const nlohmann::json &value)
{
    const auto text = value.get<std::string> ();
    return {text.begin (), text.end ()};
}

int failures = 0;

void require (bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

} // namespace

int main ()
{
    std::ifstream input (ZLINK_RELOCATION_ENVELOPE_GOLDEN_PATH);
    require (input.good (), "golden fixture must open");
    if (!input.good ())
        return EXIT_FAILURE;
    nlohmann::json golden;
    input >> golden;
    require (golden.at ("format").get<std::string> ()
               == "relocation-envelope-v1",
             "golden format tag must match");

    const auto logical =
      from_hex (golden.at ("logicalHex").get<std::string> ());
    protocol::relocation_envelope_t envelope;
    try {
        envelope = protocol::decode_relocation_envelope (logical);
    }
    catch (const std::exception &error) {
        std::cerr << "decode failed: " << error.what () << '\n';
        return EXIT_FAILURE;
    }

    const auto &decoded = golden.at ("decoded");
    require (envelope.relocation.high == u64 (decoded.at ("relocationHigh"))
               && envelope.relocation.low
                    == u64 (decoded.at ("relocationLow")),
             "relocation id must match");
    const auto &object = decoded.at ("object");
    require (object.at ("objectKind").get<std::string> () == "userSpot"
               && envelope.object.kind
                    == protocol::relocation_object_kind_t::user_spot,
             "object kind must match");
    require (envelope.object.object_id
               == object.at ("spotIdUtf8Fixture").get<std::string> (),
             "object id must match");
    require (envelope.object.object_generation
               == u64 (object.at ("spotGeneration")),
             "object generation must match");
    require (envelope.object.expected_authority_owner_generation
               == u64 (object.at ("expectedAuthorityOwnerGeneration")),
             "object owner generation must match");
    require (envelope.application_version
               == static_cast<std::int64_t> (
                 u64 (decoded.at ("applicationVersion"))),
             "application version must match");

    const auto &states = decoded.at ("applicationStates");
    require (envelope.application_states.size () == states.size (),
             "application state count must match");
    for (std::size_t index = 0;
         index < states.size ()
         && index < envelope.application_states.size ();
         ++index) {
        const auto &expected = states[index];
        const auto &actual = envelope.application_states[index];
        require (actual.participant_id
                   == u64 (expected.at ("participantId")),
                 "state participant id must match");
        const auto &application = expected.at ("applicationState");
        const auto has_state =
          application.at ("hasState").get<bool> ();
        require (actual.has_state == has_state,
                 "state presence must match");
        if (has_state)
            require (actual.state
                       == fixture_bytes (
                         application.at ("payloadUtf8Fixture")),
                     "state payload must match");
        else
            require (actual.state.empty (),
                     "an absent state must decode empty");
    }

    const auto &saved_work = decoded.at ("savedWork");
    require (envelope.saved_work.size () == saved_work.size (),
             "saved work count must match");
    for (std::size_t index = 0;
         index < saved_work.size () && index < envelope.saved_work.size ();
         ++index) {
        const auto &expected = saved_work[index];
        const auto &actual = envelope.saved_work[index];
        require (actual.participant_id
                   == u64 (expected.at ("participantId")),
                 "saved work participant must match");
        require (actual.order == u64 (expected.at ("order")),
                 "saved work order must match");
        const auto &record = expected.at ("record");
        require (record.at ("recordKind").get<std::string> ()
                     == "spotRequest"
                   && actual.record.kind
                        == protocol::frozen_record_kind_t::spot_request,
                 "record kind must match");
        const auto &source = record.at ("source");
        require (source.at ("sourceKind").get<std::string> () == "node"
                   && actual.record.source_kind
                        == protocol::frozen_source_kind_t::node,
                 "record source kind must match");
        require (actual.record.source.node_routing_id
                   == fixture_bytes (
                     source.at ("sourceNodeRidUtf8Fixture")),
                 "record source node rid must match");
        require (actual.record.source.node_generation
                   == u64 (source.at ("sourceNodeGeneration")),
                 "record source node generation must match");
        require (actual.record.source.owner_id
                   == source.at ("sourceOwnerId").get<std::string> (),
                 "record source owner must match");
        require (actual.record.source.lease_generation
                   == u64 (source.at ("sourceOwnerLeaseGeneration")),
                 "record source lease must match");
        require (record.at ("metadata").is_null ()
                   == !actual.record.has_metadata,
                 "record metadata presence must match");
        require (actual.record.operation.high
                     == u64 (record.at ("operationId").at ("high"))
                   && actual.record.operation.low
                        == u64 (record.at ("operationId").at ("low")),
                 "record operation id must match");
        require (record.at ("operationKind").get<std::string> ()
                     == "spotRequest"
                   && actual.record.operation_kind == 3,
                 "record operation kind must match");
        require (actual.record.reply_route_id
                   == std::optional<std::uint64_t>{
                     u64 (record.at ("replyRouteId"))},
                 "record reply route must match");
        const auto &body = record.at ("body");
        const auto &target_spot = body.at ("targetSpot");
        require (actual.record.target.has_value (),
                 "record target must decode");
        if (actual.record.target) {
            require (actual.record.target->object_id
                         == target_spot.at ("spotIdUtf8Fixture")
                              .get<std::string> ()
                       && actual.record.target->object_generation
                            == u64 (target_spot.at ("spotGeneration"))
                       && actual.record.target->target_node_routing_id
                            == fixture_bytes (target_spot.at (
                              "targetNodeRidUtf8Fixture"))
                       && actual.record.target->target_node_generation
                            == u64 (
                              target_spot.at ("targetNodeGeneration"))
                       && actual.record.target->authority_owner_generation
                            == u64 (target_spot.at (
                              "expectedAuthorityOwnerGeneration"))
                       && actual.record.target->owner_lease_generation
                            == u64 (target_spot.at (
                              "expectedOwnerLeaseGeneration")),
                     "record target fence must match");
        }
        const auto &payload = body.at ("payload");
        require (actual.record.application.has_value (),
                 "record payload must decode");
        if (actual.record.application) {
            require (actual.record.application->packet_name
                         == payload.at ("packetName").get<std::string> ()
                       && actual.record.application->content_type
                            == payload.at ("contentType")
                                 .get<std::string> ()
                       && actual.record.application->payload
                            == fixture_bytes (
                              payload.at ("payloadUtf8Fixture")),
                     "record application payload must match");
        }
    }

    const auto &timers = decoded.at ("timerRegistrations");
    require (envelope.timer_registrations.size () == timers.size (),
             "timer registration count must match");
    for (std::size_t index = 0;
         index < timers.size ()
         && index < envelope.timer_registrations.size ();
         ++index) {
        const auto &expected = timers[index];
        const auto &actual = envelope.timer_registrations[index];
        require (
          actual.participant_id == u64 (expected.at ("participantId"))
            && actual.name == expected.at ("name").get<std::string> ()
            && actual.handler_type
                 == expected.at ("handlerType").get<std::string> ()
            && actual.period_milliseconds
                 == u64 (expected.at ("periodMilliseconds"))
            && (expected.at ("overrunPolicy").get<std::string> ()
                  != "skipLateTicks"
                || actual.overrun_policy == 1)
            && actual.max_catch_up_ticks
                 == u64 (expected.at ("maxCatchUpTicks"))
            && actual.stop_on_unhandled_exception
                 == expected.at ("stopOnUnhandledException").get<bool> ()
            && actual.last_completed_delivery_index
                 == u64 (expected.at ("lastCompletedDeliveryIndex"))
            && actual.last_completed_scheduled_index
                 == u64 (expected.at ("lastCompletedScheduledIndex"))
            && actual.next_scheduled_at_unix_milliseconds
                 == u64 (expected.at ("nextScheduledAtUnixMilliseconds")),
          "timer registration fields must match");
    }

    const auto &pending = decoded.at ("pendingTimerTicks");
    require (envelope.pending_timer_ticks.size () == pending.size (),
             "pending tick count must match");
    for (std::size_t index = 0;
         index < pending.size ()
         && index < envelope.pending_timer_ticks.size ();
         ++index) {
        const auto &expected = pending[index];
        const auto &actual = envelope.pending_timer_ticks[index];
        require (
          actual.participant_id == u64 (expected.at ("participantId"))
            && actual.order == u64 (expected.at ("order"))
            && actual.timer_name
                 == expected.at ("timerName").get<std::string> ()
            && actual.delivery_index
                 == u64 (expected.at ("deliveryIndex"))
            && actual.scheduled_index
                 == u64 (expected.at ("scheduledIndex"))
            && actual.scheduled_at_unix_milliseconds
                 == u64 (expected.at ("scheduledAtUnixMilliseconds"))
            && actual.skipped_ticks == u64 (expected.at ("skippedTicks")),
          "pending timer tick fields must match");
    }

    // Byte-exact re-encode of every decoded golden field.
    try {
        require (protocol::encode_relocation_envelope (envelope) == logical,
                 "re-encoded stream must be byte-exact");
    }
    catch (const std::exception &error) {
        std::cerr << "re-encode failed: " << error.what () << '\n';
        ++failures;
    }

    // Store-derived participant inventory ordering: participantId is the
    // zero-based index plus one after sorting by UTF-8 authority-key bytes.
    // The golden's inventory fixtures are "actor:z" < "actor:é" < "spot:a".
    {
        const auto &inventory =
          golden.at ("participantAuthorityKeyInventory");
        require (inventory.size () == 3
                   && inventory[0].at ("authorityKeyUtf8Fixture")
                          .get<std::string> ()
                        == "actor:z"
                   && inventory[1].at ("authorityKeyUtf8Fixture")
                          .get<std::string> ()
                        == "actor:é"
                   && inventory[2].at ("authorityKeyUtf8Fixture")
                          .get<std::string> ()
                        == "spot:a",
                 "golden inventory fixture must be the declared order");
        const auto identity = [] (stateful::object_kind_t kind,
                                  std::string key) {
            stateful::relocation_participant_identity_t value;
            value.owner.kind = kind;
            value.owner.key = std::move (key);
            value.owner.object_generation = 1;
            value.owner.authority_owner_generation = 7;
            value.owner.mesh_name = "mesh";
            value.owner.node_id = "node";
            value.stable_type = "type";
            return value;
        };
        // Deliberately shuffled input: the mapping must sort it into the
        // golden's canonical order before assigning participant ids.
        const auto materialized =
          stateful::maintenance_runtime_t::materialize_envelope (
            envelope,
            {identity (stateful::object_kind_t::user_spot, "a"),
             identity (stateful::object_kind_t::actor, "é"),
             identity (stateful::object_kind_t::actor, "z")});
        require (materialized.has_value ()
                   && materialized->size () == 3,
                 "golden inventory must materialize");
        if (materialized && materialized->size () == 3) {
            require ((*materialized)[0].owner.key == "z"
                       && (*materialized)[0].owner.kind
                            == stateful::object_kind_t::actor,
                     "participant 1 must be actor:z");
            require ((*materialized)[1].owner.key == "é"
                       && (*materialized)[1].owner.kind
                            == stateful::object_kind_t::actor,
                     "participant 2 must be actor:é");
            require ((*materialized)[2].owner.key == "a"
                       && (*materialized)[2].owner.kind
                            == stateful::object_kind_t::user_spot,
                     "participant 3 must be spot:a");
            require ((*materialized)[1].pending_application.size () == 1
                       && (*materialized)[1]
                              .pending_application.front ()
                              .sequence
                            == 1,
                     "saved work must land on participant 2 (actor:é)");
            require ((*materialized)[2].timers.size () == 1
                       && (*materialized)[2]
                              .timers.front ()
                              .pending_ticks.size ()
                            == 1,
                     "the timer and its pending tick must land on spot:a");
        }
    }

    if (failures != 0) {
        std::cerr << failures << " golden conformance failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
