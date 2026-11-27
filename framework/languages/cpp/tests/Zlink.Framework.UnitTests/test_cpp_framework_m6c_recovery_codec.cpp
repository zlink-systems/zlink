/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "runtime/protocol/actor_join_recovery_codec.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

namespace
{

std::vector<std::uint8_t> decode_hex_vector (std::string_view value)
{
    const auto nibble = [] (char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9')
            return static_cast<std::uint8_t> (ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<std::uint8_t> (ch - 'a' + 10);
        return static_cast<std::uint8_t> (ch - 'A' + 10);
    };
    std::vector<std::uint8_t> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index != value.size (); index += 2)
        result.push_back (
          static_cast<std::uint8_t> ((nibble (value[index]) << 4u) | nibble (value[index + 1])));
    return result;
}

} // namespace

void test_envelope_round_trip (test_context_t &test)
{
    namespace protocol = zlink::framework::runtime::protocol;
    // One canonical saved-work record, exactly as the seal canonicalizes an
    // accepted Spot request (Stage A retains the typed admission capture).
    protocol::frozen_application_record_t admitted;
    admitted.kind = protocol::frozen_record_kind_t::spot_request;
    admitted.source_kind = protocol::frozen_source_kind_t::node;
    admitted.source = {"request-source", 6, {'n'}, 1};
    admitted.operation = {0, 42};
    admitted.operation_kind = 3;
    admitted.reply_route_id = 99;
    admitted.body = protocol::frozen_spot_application_body_t{
      {"spot-a", 3, {'m'}, 2, 9, 0},
      1,
      protocol::application_payload_t{
        "ChatRequest", "application/json", {'{', '"', 'i', 'd', '"', ':', '1', '}'}}};
    const auto canonical = protocol::encode_frozen_application_record (admitted);

    frozen_object_state_t frozen{
      .owner = {.kind = object_kind_t::user_spot,
                .key = "spot-a",
                .object_generation = 3,
                .authority_owner_generation = 9,
                .mesh_name = "mesh",
                .node_id = "node-a"},
      .stable_type = "spot",
      .application_state = {7, 8, 9},
      .pending_application = {turn_record_t{
        4, canonical.canonical_bytes,
        protocol::application_payload_hwm_bytes (*canonical.application), std::nullopt, canonical}},
      .timers = {{11, 100, 50, 3}}};

    const protocol::relocation_id_t relocation{0, 9};
    bool unmatched_binding_rejected = false;
    try {
        const relocation_session_binding_t binding{{"missing-actor", 1}, {}};
        (void) maintenance_runtime_t::encode_envelope ({frozen}, relocation, 1, {binding});
    }
    catch (const protocol::service_wire_error_t &) {
        unmatched_binding_rejected = true;
    }
    test.require (unmatched_binding_rejected,
                  "an unmatched Session binding must report a protocol error");
    const auto encoded = maintenance_runtime_t::encode_envelope ({frozen}, relocation);
    test.require (!encoded.empty (), "the schema relocation envelope must encode");
    const auto envelope = maintenance_runtime_t::decode_envelope (encoded);
    test.require (envelope.has_value (), "the schema relocation envelope must decode");
    test.require (
      envelope && envelope->relocation == relocation
        && envelope->object.kind == protocol::relocation_object_kind_t::user_spot
        && envelope->object.object_id == "spot-a" && envelope->object.object_generation == 3
        && envelope->object.expected_authority_owner_generation == 9
        && envelope->application_states.size () == 1 && envelope->saved_work.size () == 1
        && envelope->saved_work.front ().order == 4 && envelope->timer_registrations.size () == 1,
      "the stream must carry the declared schema sections");
    // Re-encoding the decoded model must be byte-exact.
    test.require (envelope && protocol::encode_relocation_envelope (*envelope) == encoded,
                  "the schema relocation envelope must re-encode byte-exactly");

    const auto materialized = maintenance_runtime_t::materialize_envelope (
      *envelope,
      {relocation_participant_identity_t{frozen.owner, frozen.stable_type, std::nullopt}});
    test.require (materialized.has_value () && materialized->size () == 1
                    && materialized->front () == frozen,
                  "queue and timer state must round-trip through the schema stream");

    auto unordered = frozen;
    unordered.pending_application.push_back (unordered.pending_application.front ());
    test.require (maintenance_runtime_t::encode_envelope ({unordered}, relocation).empty (),
                  "encoder must reject duplicate or unordered queue sequences");

    // A two-participant aggregate: the canonical inventory order is the
    // UTF-8 authority-key byte order, so the Actor precedes the Spot.
    frozen_object_state_t actor{.owner = {.kind = object_kind_t::actor,
                                          .key = "actor-b",
                                          .object_generation = 5,
                                          .authority_owner_generation = 2,
                                          .mesh_name = "mesh",
                                          .node_id = "node-a"},
                                .stable_type = "actor",
                                .application_state = {1},
                                .pending_application = {},
                                .timers = {}};
    const auto aggregate_encoded =
      maintenance_runtime_t::encode_envelope ({frozen, actor}, relocation);
    test.require (!aggregate_encoded.empty (), "the aggregate schema envelope must encode");
    const auto aggregate_envelope = maintenance_runtime_t::decode_envelope (aggregate_encoded);
    test.require (aggregate_envelope && aggregate_envelope->application_states.size () == 2
                    && aggregate_envelope->object.object_id == "spot-a"
                    && aggregate_envelope->saved_work.size () == 1
                    && aggregate_envelope->saved_work.front ().participant_id == 2,
                  "participant ids must follow the sorted authority-key inventory");
    const relocation_session_binding_t binding{{actor.owner.key, actor.owner.object_generation},
                                               {0, {'s'}, 7, "session-owner", 9, {'r'}, 11}};
    const auto bound_envelope = maintenance_runtime_t::decode_envelope (
      maintenance_runtime_t::encode_envelope ({frozen, actor}, relocation, 1, {binding}));
    auto expected_binding = binding.second;
    expected_binding.participant_id = 1;
    test.require (
      bound_envelope && bound_envelope->session_bindings == std::vector{expected_binding},
      "Session binding fields use the Actor's canonical participant id without a route");
    const auto aggregate_materialized = maintenance_runtime_t::materialize_envelope (
      *aggregate_envelope,
      {relocation_participant_identity_t{frozen.owner, frozen.stable_type, std::nullopt},
       relocation_participant_identity_t{actor.owner, actor.stable_type,
                                         std::pair{std::string ("spot-a"), std::uint64_t{3}}}});
    test.require (aggregate_materialized && aggregate_materialized->size () == 2
                    && aggregate_materialized->front () == actor
                    && aggregate_materialized->back () == frozen,
                  "aggregate participants must round-trip in inventory order");

    // Tampering with the stream must be rejected by the decoder.
    auto truncated = encoded;
    truncated.pop_back ();
    test.require (!maintenance_runtime_t::decode_envelope (truncated),
                  "decoder must reject a truncated stream");
    test.require (!maintenance_runtime_t::decode_envelope ({}),
                  "decoder must reject an empty stream");
    test.require (maintenance_runtime_t::crc32c (encoded) != 0,
                  "CRC32C must be computed for the retained payload");
}

void test_actor_join_recovery_round_trip (test_context_t &test)
{
    namespace protocol = zlink::framework::runtime::protocol;
    const std::string handoff_id = "00112233445566778899aabbccddeeff";
    const auto relocation = protocol::actor_join_relocation_id (handoff_id);
    protocol::actor_join_recovery_t recovery{
      .actor_id = "actor-a",
      .actor_type = "sample.Actor",
      .handoff_id = handoff_id,
      .source_spot_id = "source-spot",
      .source_node_routing_id = {'s', 'r', 'c'},
      .actor_generation = 7,
      .actor_authority_owner_generation = 11,
      .actor_node_generation = 13,
      .expected_owner_lease_generation = 17,
      .relocation = relocation,
      .relocation_content_type = std::string (protocol::actor_join_snapshot_content_type),
      .request_content_type = "application/json",
      .request = {'{', '}'},
      .reservation_token = handoff_id,
      .reserved_payload_bytes =
        protocol::actor_join_reserved_payload_bytes (2, protocol::actor_join_snapshot_content_type),
      .target_spot_id = "target-spot",
      .target_node_routing_id = {'d', 's', 't'},
      .target_node_generation = 19,
      .target_spot_generation = 23,
      .target_authority_owner_generation = 12,
      .target_spot_authority_owner_generation = 29,
      .coordinator = {"owner-a", 17, {'s', 'r', 'c'}, 13, "store-v1"},
      .operation = {31, 37},
      .reply_content_type = "application/json",
      .reply = {'[', ']'}};
    auto empty_reply = recovery;
    empty_reply.reply.clear ();
    empty_reply.reply_content_type.clear ();
    const auto empty_restored = protocol::decode_actor_join_recovery_saved_work (
      protocol::encode_actor_join_recovery_saved_work (empty_reply));
    test.require (empty_restored && empty_restored->reply_content_type.empty ()
                    && empty_restored->reply.empty ()
                    && empty_restored->operation.high == recovery.operation.high
                    && empty_restored->operation.low == recovery.operation.low,
                  "empty Join reply must preserve absent metadata and its completion operation");
    auto typed_reply = recovery;
    typed_reply.reply_content_type = protocol::framework_multipart_content_type;
    const auto typed_restored = protocol::decode_actor_join_recovery_saved_work (
      protocol::encode_actor_join_recovery_saved_work (typed_reply));
    test.require (typed_restored
                    && typed_restored->reply_content_type
                         == protocol::framework_multipart_content_type
                    && typed_restored->reply == recovery.reply,
                  "typed Join reply must preserve the fixed outer profile through recovery");
    const auto frozen = protocol::encode_actor_join_recovery_saved_work (recovery);
    /* Fixed Node vector from encodeCanonicalActorJoinRecoverySavedWork in
     * actor-join-recovery-codec.ts, using this test's recovery fields.  This
     * is intentionally not a C++ encode/decode round trip: changing JSON
     * property order must fail before persisted ZLJR records diverge. */
    constexpr std::string_view node_zljr_frozen_hex =
      "0101001f06373337323633000000000000000d076f776e65722d6100000000000000110000000000000000000000"
      "0000000000000000000000000100"
      "000767225f5f7a6c696e6b2e6163746f722e726f757465645f6a6f696e2e7265636f76657279316170706c696361"
      "74696f6e2f782d7a6c696e6b2d61"
      "63746f722d726f757465642d6a6f696e2d7265636f766572792d76310000070e5a4c4a5201000006f90000000200"
      "0000027b2252657175657374223a"
      "7b224163746f724964223a226163746f722d61222c224163746f7254797065223a2273616d706c652e4163746f72"
      "222c2248616e646f66664964223a"
      "223030313132323333343435353636373738383939616162626363646465656666222c22426f756e645365737369"
      "6f6e4e6f6465526964223a6e756c"
      "6c2c22426f756e6453657373696f6e526964223a6e756c6c2c2252656c6f636174696f6e436f6e74656e74547970"
      "65223a226170706c69636174696f"
      "6e2f766e642e7a6c696e6b2e6163746f722d72656c6f636174696f6e2e736e617073686f74222c2252656c6f6361"
      "74696f6e5265666572656e636522"
      "3a2270656e64696e67222c2252656c6f636174696f6e436865636b73756d437263333263223a302c2252656c6f63"
      "6174696f6e416767726567617465"
      "4964223a2230303131323233332d343435352d363637372d383839392d616162626363646465656666222c225265"
      "6c6f636174696f6e416767726567"
      "61746547656e65726174696f6e223a312c2252656c6f636174696f6e496e76656e746f7279446967657374223a22"
      "4141414141414141414141414141"
      "41414141414141414141414141414141414141414141414141414141413d222c2252657175657374436f6e74656e"
      "7454797065223a226170706c6963"
      "6174696f6e2f6a736f6e222c2252657175657374223a22222c2248616e646f66664672616d6573223a5b5d2c2253"
      "6f7572636553706f744964223a22"
      "736f757263652d73706f74222c22536f757263654e6f6465526964223a2263334a6a222c224163746f7247656e65"
      "726174696f6e223a372c22416374"
      "6f72417574686f726974794f776e657247656e65726174696f6e223a31312c22426f756e6453657373696f6e4269"
      "6e64696e67546f6b656e223a6e75"
      "6c6c2c22426f756e6453657373696f6e42696e64696e6747656e65726174696f6e223a302c22426f756e64536573"
      "73696f6e4f626a65637447656e65"
      "726174696f6e223a302c22426f756e6453657373696f6e417574686f726974794f776e657247656e65726174696f"
      "6e223a302c22426f756e64536573"
      "73696f6e4d6573684e616d65223a6e756c6c2c22426f756e6453657373696f6e5461726765744e6f646547656e65"
      "726174696f6e223a302c22426f75"
      "6e6453657373696f6e4f776e65724c6561736547656e65726174696f6e223a302c22426f756e6453657373696f6e"
      "4f776e65724e6f646547656e6572"
      "6174696f6e223a302c22426f756e6453657373696f6e4163636570746564486967685761746572223a302c22426f"
      "756e6453657373696f6e53657373"
      "696f6e4f776e65724964223a6e756c6c2c22426f756e6453657373696f6e53657373696f6e4f776e65724c656173"
      "6547656e65726174696f6e223a30"
      "2c225265736572766174696f6e546f6b656e223a2230303131323233333434353536363737383839396161626263"
      "63646465656666222c2252657365"
      "727665645061796c6f61644279746573223a38333935313631382c225461726765744e6f6465526964223a225a48"
      "4e30222c225461726765744e6f64"
      "6547656e65726174696f6e223a31392c2254617267657453706f7447656e65726174696f6e223a32332c22546172"
      "676574417574686f726974794f77"
      "6e657247656e65726174696f6e223a31322c2254617267657453706f74417574686f726974794f776e657247656e"
      "65726174696f6e223a32392c2252"
      "656c6f636174696f6e436f6f7264696e61746f724f776e65724964223a226f776e65722d61222c2252656c6f6361"
      "74696f6e436f6f7264696e61746f"
      "724c6561736547656e65726174696f6e223a31372c2252656c6f636174696f6e436f6f7264696e61746f724e6f64"
      "65526964223a2263334a6a222c22"
      "52656c6f636174696f6e436f6f7264696e61746f724e6f646547656e65726174696f6e223a31332c2252656c6f63"
      "6174696f6e436f6f7264696e6174"
      "6f724578706563746564417574686f7269747953746f726556657273696f6e223a2273746f72652d7631222c2241"
      "63746f724e6f646547656e657261"
      "74696f6e223a31332c2245787065637465644f776e65724c6561736547656e65726174696f6e223a31377d2c2254"
      "617267657453706f744964223a22"
      "7461726765742d73706f74222c225461726765744e6f6465526964223a225a484e30222c225461726765744e6f64"
      "6547656e65726174696f6e223a31"
      "392c2254617267657453706f7447656e65726174696f6e223a32332c22546172676574417574686f726974794f77"
      "6e657247656e65726174696f6e22"
      "3a31322c224f7065726174696f6e496448696768223a33312c224f7065726174696f6e49644c6f77223a33372c22"
      "5265706c79436f6e74656e745479"
      "7065223a226170706c69636174696f6e2f6a736f6e222c225265706c79223a22227d7b7d5b5d";
    test.require (frozen.canonical_bytes == decode_hex_vector (node_zljr_frozen_hex),
                  "ZLJR saved-work must match the fixed Node byte vector");
    const auto decoded = protocol::decode_actor_join_recovery_saved_work (frozen);
    test.require (decoded && decoded->actor_id == recovery.actor_id
                    && decoded->actor_type == recovery.actor_type
                    && decoded->handoff_id == handoff_id && decoded->relocation == relocation
                    && decoded->source_spot_id == recovery.source_spot_id
                    && decoded->source_node_routing_id == recovery.source_node_routing_id
                    && decoded->target_spot_id == recovery.target_spot_id
                    && decoded->target_node_routing_id == recovery.target_node_routing_id
                    && decoded->coordinator == recovery.coordinator
                    && decoded->request == recovery.request && decoded->reply == recovery.reply,
                  "ZLJR saved-work must preserve the Node/.NET byte contract and identity fields");

    auto independent_recovery = recovery;
    independent_recovery.handoff_id = "source-issued-handoff";
    independent_recovery.reservation_token = "target-issued-reservation";
    independent_recovery.reserved_payload_bytes = 97;
    const auto independently_issued = protocol::decode_actor_join_recovery_saved_work (
      protocol::encode_actor_join_recovery_saved_work (independent_recovery));
    test.require (
      independently_issued && independently_issued->relocation == independent_recovery.relocation
        && independently_issued->handoff_id == independent_recovery.handoff_id
        && independently_issued->reservation_token == independent_recovery.reservation_token
        && independently_issued->reserved_payload_bytes
             == independent_recovery.reserved_payload_bytes,
      "ZLJR must preserve independently issued handoff and admission values");

    frozen_object_state_t actor{.owner = {object_kind_t::actor, "actor-a", 7, 11, "mesh", "src"},
                                .stable_type = "sample.Actor",
                                .application_state = {1, 2, 3},
                                .pending_application = {turn_record_t{
                                  1, frozen.canonical_bytes, std::nullopt, std::nullopt, frozen}},
                                .timers = {}};
    const auto envelope_bytes = maintenance_runtime_t::encode_envelope ({actor}, relocation, 42);
    const auto envelope = maintenance_runtime_t::decode_envelope (envelope_bytes);
    test.require (
      envelope && envelope->application_version == 42 && envelope->saved_work.size () == 1
        && protocol::decode_actor_join_recovery_saved_work (envelope->saved_work.front ().record)
             .has_value (),
      "Join relocation must carry descriptor applicationVersion and one canonical ZLJR record");
}

} // namespace m6c_test
