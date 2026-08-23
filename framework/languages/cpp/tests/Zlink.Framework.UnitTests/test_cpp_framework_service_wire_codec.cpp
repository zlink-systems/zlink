/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/protocol/actor_join_recovery_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace protocol = zlink::framework::runtime::protocol;
namespace mesh = zlink::framework::runtime::mesh;
namespace messaging = zlink::framework::runtime::messaging;

static_assert (!std::is_same_v<zlink::framework::runtime::call_id_t,
                               protocol::wire_operation_id_t>);
static_assert (!std::is_convertible_v<zlink::framework::runtime::call_id_t,
                                      protocol::wire_operation_id_t>);

namespace
{
std::vector<std::uint8_t> from_hex (std::string_view value)
{
    assert (value.size () % 2 == 0);
    const auto digit = [] (char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t> (value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t> (value - 'a' + 10);
        assert (false);
        return 0;
    };
    std::vector<std::uint8_t> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2) {
        result.push_back (static_cast<std::uint8_t> (
          (digit (value[index]) << 4) | digit (value[index + 1])));
    }
    return result;
}

void put_u16 (std::vector<std::uint8_t> &out, std::uint16_t value)
{
    out.push_back (static_cast<std::uint8_t> (value >> 8));
    out.push_back (static_cast<std::uint8_t> (value));
}

void put_u32 (std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back (static_cast<std::uint8_t> (value >> shift));
}

void put_u64 (std::vector<std::uint8_t> &out, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back (static_cast<std::uint8_t> (value >> shift));
}

void put_text8 (std::vector<std::uint8_t> &out, std::string_view value)
{
    out.push_back (static_cast<std::uint8_t> (value.size ()));
    out.insert (out.end (), value.begin (), value.end ());
}

void put_text16 (std::vector<std::uint8_t> &out, std::string_view value)
{
    put_u16 (out, static_cast<std::uint16_t> (value.size ()));
    out.insert (out.end (), value.begin (), value.end ());
}

void put_body16 (std::vector<std::uint8_t> &out,
                 const std::vector<std::uint8_t> &body)
{
    put_u16 (out, static_cast<std::uint16_t> (body.size ()));
    out.insert (out.end (), body.begin (), body.end ());
}

std::vector<std::uint8_t> frozen_payload ()
{
    std::vector<std::uint8_t> body;
    put_text8 (body, "Packet");
    put_text8 (body, "application/json");
    put_u32 (body, 2);
    body.push_back (1);
    body.push_back (2);
    std::vector<std::uint8_t> result{1};
    put_u32 (result, static_cast<std::uint32_t> (body.size ()));
    result.insert (result.end (), body.begin (), body.end ());
    return result;
}

void put_actor_ref (std::vector<std::uint8_t> &out,
                    std::string_view actor = "actor")
{
    put_text8 (out, actor);
    put_u64 (out, 3);
}

void put_spot_ref (std::vector<std::uint8_t> &out,
                   std::string_view spot = "spot")
{
    put_text8 (out, spot);
    put_u64 (out, 4);
}

void put_actor_route (std::vector<std::uint8_t> &out)
{
    put_actor_ref (out);
    put_text8 (out, "node-t");
    put_u64 (out, 5);
    put_u64 (out, 6);
    put_u64 (out, 7);
}

void put_spot_route (std::vector<std::uint8_t> &out)
{
    put_spot_ref (out);
    put_text8 (out, "node-t");
    put_u64 (out, 5);
    put_u64 (out, 6);
    put_u64 (out, 7);
}

std::vector<std::uint8_t> frozen_source (std::uint8_t kind)
{
    std::vector<std::uint8_t> body;
    put_text8 (body, "node-s");
    put_u64 (body, 8);
    put_text8 (body, "owner-s");
    put_u64 (body, 9);
    if (kind == 2)
        put_text8 (body, "spot-s");
    else if (kind == 3 || kind == 4) {
        put_actor_ref (body, "actor-s");
        if (kind == 4) {
            put_text8 (body, "session-s");
            put_u64 (body, 10);
            put_u64 (body, 11);
        }
    }
    std::vector<std::uint8_t> result{kind};
    put_body16 (result, body);
    return result;
}

std::vector<std::uint8_t> make_frozen_record (
  std::uint8_t kind,
  std::uint8_t source_kind,
  std::uint32_t operation_kind,
  std::uint64_t operation_low,
  std::optional<std::uint64_t> reply_route,
  std::vector<std::uint8_t> body,
  bool metadata = false)
{
    std::vector<std::uint8_t> result{kind};
    const auto source = frozen_source (source_kind);
    result.insert (result.end (), source.begin (), source.end ());
    result.push_back (metadata ? 1 : 0);
    if (metadata) {
        result.push_back (1);
        result.push_back (1);
        put_text8 (result, "trace");
        put_text16 (result, "abc");
    }
    put_u64 (result, 0);
    put_u64 (result, operation_low);
    put_u32 (result, operation_kind);
    std::vector<std::uint8_t> reply;
    if (reply_route)
        put_u64 (reply, *reply_route);
    put_body16 (result, reply);
    result.insert (result.end (), body.begin (), body.end ());
    return result;
}

nlohmann::json load_fixture (const char *path)
{
    std::ifstream input (path);
    assert (input.good ());
    return nlohmann::json::parse (input);
}

void verify_generated_service_roundtrip (const std::vector<std::uint8_t> &bytes)
{
    assert (bytes.size () >= 5);
    switch (bytes[3]) {
        case 30:
            assert (protocol::encode_relocation_ready_30 (
                      protocol::decode_relocation_ready_30 (bytes))
                    == bytes);
            break;
        case 31:
            assert (protocol::encode_relocation_data_31 (
                      protocol::decode_relocation_data_31 (bytes))
                    == bytes);
            break;
        case 33: {
            const auto encoded = protocol::encode_reply_relay_33 (
              protocol::decode_reply_relay_33 ({bytes}));
            assert (encoded.size () == 1 && encoded.front () == bytes);
            break;
        }
        case 34:
            assert (protocol::encode_relocation_cutover_34 (
                      protocol::decode_relocation_cutover_34 (bytes))
                    == bytes);
            break;
        case 40:
            assert (protocol::encode_relocation_prepare_40 (
                      protocol::decode_relocation_prepare_40 (bytes))
                    == bytes);
            break;
        case 42:
            assert (protocol::encode_session_relocation_seal_42 (
                      protocol::decode_session_relocation_seal_42 (bytes))
                    == bytes);
            break;
        case 43:
            assert (protocol::encode_session_relocation_sealed_43 (
                      protocol::decode_session_relocation_sealed_43 (bytes))
                    == bytes);
            break;
        case 44:
            assert (protocol::encode_session_relocation_route_44 (
                      protocol::decode_session_relocation_route_44 (bytes))
                    == bytes);
            break;
        case 46:
            assert (protocol::encode_reply_relay_ack_46 (
                      protocol::decode_reply_relay_ack_46 (bytes))
                    == bytes);
            break;
        case 47:
            assert (protocol::encode_user_spot_create_47 (
                      protocol::decode_user_spot_create_47 (bytes))
                    == bytes);
            break;
        case 48:
            assert (protocol::encode_user_spot_close_48 (
                      protocol::decode_user_spot_close_48 (bytes))
                    == bytes);
            break;
        case 49:
            assert (protocol::encode_actor_create_49 (
                      protocol::decode_actor_create_49 (bytes))
                    == bytes);
            break;
        case 52:
            assert (protocol::encode_relocation_state_52 (
                      protocol::decode_relocation_state_52 (bytes))
                    == bytes);
            break;
        case 53:
            assert (protocol::encode_relocation_failed_53 (
                      protocol::decode_relocation_failed_53 (bytes))
                    == bytes);
            break;
        default:
            assert (false);
    }
}

void verify_runtime_service_roundtrip (const std::vector<std::uint8_t> &bytes)
{
    switch (static_cast<protocol::command> (bytes[3])) {
        case protocol::command::replyRelay:
            assert (protocol::encode_reply_relay (
                      protocol::decode_reply_relay (bytes))
                    == bytes);
            break;
        case protocol::command::replyRelayAck:
            assert (protocol::encode_reply_relay_ack (
                      protocol::decode_reply_relay_ack (bytes))
                    == bytes);
            break;
        case protocol::command::relocationReady:
        case protocol::command::relocationData:
        case protocol::command::relocationCutover:
        case protocol::command::relocationPrepare:
        case protocol::command::relocationState:
        case protocol::command::relocationFailed:
            assert (protocol::encode_relocation_control (
                      protocol::decode_relocation_control (bytes))
                    == bytes);
            break;
        case protocol::command::sessionRelocationSeal:
            assert (protocol::encode_session_relocation_seal (
                      protocol::decode_session_relocation_seal (bytes))
                    == bytes);
            break;
        case protocol::command::sessionRelocationSealed:
            assert (protocol::encode_session_relocation_sealed (
                      protocol::decode_session_relocation_sealed (bytes))
                    == bytes);
            break;
        case protocol::command::sessionRelocationRoute:
            assert (protocol::encode_session_relocation_route (
                      protocol::decode_session_relocation_route (bytes))
                    == bytes);
            break;
        case protocol::command::userSpotCreate:
            assert (protocol::encode_user_spot_create_header (
                      protocol::decode_user_spot_create_header (bytes))
                    == bytes);
            break;
        case protocol::command::userSpotClose:
            assert (protocol::encode_user_spot_close_header (
                      protocol::decode_user_spot_close_header (bytes))
                    == bytes);
            break;
        case protocol::command::actorCreate:
            assert (protocol::encode_actor_create_header (
                      protocol::decode_actor_create_header (bytes))
                    == bytes);
            break;
        default:
            assert (false);
    }
}

bool generated_service_rejects (const std::vector<std::uint8_t> &bytes)
{
    try {
        verify_generated_service_roundtrip (bytes);
    }
    catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

bool runtime_service_rejects (const std::vector<std::uint8_t> &bytes)
{
    try {
        verify_runtime_service_roundtrip (bytes);
    }
    catch (const protocol::service_wire_error_t &) {
        return true;
    }
    return false;
}

void verify_generated_adoption_goldens ()
{
    for (const auto path : {ZLINK_REPLY_RELAY_GOLDEN_PATH,
                            ZLINK_RELOCATION_CONTROL_GOLDEN_PATH,
                            ZLINK_SESSION_RELOCATION_BARRIER_GOLDEN_PATH}) {
        const auto fixture = load_fixture (path);
        for (const auto &item : fixture.at ("canonical")) {
            const auto bytes = from_hex (item.at ("hex").get<std::string> ());
            verify_generated_service_roundtrip (bytes);
            verify_runtime_service_roundtrip (bytes);
        }
        if (fixture.contains ("malformed")) {
            for (const auto &item : fixture.at ("malformed")) {
                const auto bytes = from_hex (
                  item.at ("hex").get<std::string> ());
                assert (generated_service_rejects (bytes));
                assert (runtime_service_rejects (bytes));
            }
        }
    }

    for (const auto path : {ZLINK_USER_SPOT_CREATE_GOLDEN_PATH,
                            ZLINK_USER_SPOT_CLOSE_GOLDEN_PATH,
                            ZLINK_ACTOR_CREATE_GOLDEN_PATH}) {
        const auto fixture = load_fixture (path);
        const auto canonical = from_hex (
          fixture.at ("canonical").at ("hex").get<std::string> ());
        verify_generated_service_roundtrip (canonical);
        verify_runtime_service_roundtrip (canonical);
        for (const auto &item : fixture.at ("malformed")) {
            const auto bytes = from_hex (item.at ("hex").get<std::string> ());
            assert (generated_service_rejects (bytes));
            assert (runtime_service_rejects (bytes));
        }
    }

    const auto zljr = load_fixture (ZLINK_ZLJR_GOLDEN_PATH);
    const auto canonical = from_hex (
      zljr.at ("canonical").at ("hex").get<std::string> ());
    const auto generated = protocol::decode_zljr_record_v1 (canonical);
    assert (protocol::encode_zljr_record_v1 (generated) == canonical);
    const auto frozen = protocol::decode_frozen_record (canonical);
    const auto recovery = protocol::decode_actor_join_recovery_saved_work (
      frozen);
    assert (recovery.has_value ());
    assert (protocol::encode_actor_join_recovery_saved_work (*recovery)
              .canonical_bytes
            == canonical);
    for (const auto &item : zljr.at ("malformed")) {
        const auto bytes = from_hex (item.at ("hex").get<std::string> ());
        bool generated_rejected = false;
        try {
            static_cast<void> (protocol::decode_zljr_record_v1 (bytes));
        }
        catch (const std::invalid_argument &) {
            generated_rejected = true;
        }
        assert (generated_rejected);
        bool runtime_rejected = false;
        try {
            const auto malformed_frozen = protocol::decode_frozen_record (bytes);
            runtime_rejected = !protocol::decode_actor_join_recovery_saved_work (
                                  malformed_frozen)
                                  .has_value ();
        }
        catch (const protocol::service_wire_error_t &) {
            runtime_rejected = true;
        }
        assert (runtime_rejected);
    }
}
}

int main ()
{
    verify_generated_adoption_goldens ();
    const protocol::actor_route_fence_t bound_actor{
      .actor_id = "actor-a",
      .object_generation = 1,
      .target_node_routing_id = {'a', 'c', 't', 'o', 'r', '-', 'o', 'w', 'n', 'e', 'r'},
      .target_node_generation = 2,
      .authority_owner_generation = 3,
      .owner_lease_generation = 4};
    const protocol::bound_session_send_t bound_send{
      .actor = bound_actor, .expected_binding_generation = 7};
    const auto encoded_bound_send =
      protocol::encode_bound_session_send (bound_send);
    assert (protocol::decode_bound_session_send (encoded_bound_send)
            == bound_send);

    for (const auto state : {protocol::bound_session_binding_state_t::active,
                             protocol::bound_session_binding_state_t::tombstone}) {
        const protocol::bound_session_bind_t bound_bind{
          .correlation = 9,
          .actor = bound_actor,
          .session_routing_id = {'s', 'e', 's', 's', 'i', 'o', 'n', '-', 'a'},
          .binding = {.state = state, .generation = 7}};
        const auto encoded = protocol::encode_bound_session_bind (bound_bind);
        assert (protocol::decode_bound_session_bind (encoded) == bound_bind);
    }

    std::ifstream replacement_fixture (
      ZLINK_BOUND_SESSION_REPLACED_GOLDEN_PATH);
    assert (replacement_fixture.good ());
    const auto replacement_vectors =
      nlohmann::json::parse (replacement_fixture);
    const auto canonical_replacement =
      replacement_vectors.at ("canonical").at ("bytes")
        .get<std::vector<std::uint8_t>> ();
    const protocol::bound_session_replaced_t replacement{
      .actor_authority = bound_actor,
      .retired_session = {
        .session_owner_node_routing_id =
          {'s', 'e', 's', 's', 'i', 'o', 'n', '-', 'o', 'w', 'n', 'e', 'r'},
        .session_owner_node_generation = 5,
        .session_owner_id = "session-runtime",
        .session_owner_lease_generation = 6,
        .session_routing_id =
          {'s', 'e', 's', 's', 'i', 'o', 'n', '-', 'a'},
        .retired_binding_generation = 7}};
    assert (protocol::encode_bound_session_replaced (replacement)
            == canonical_replacement);
    assert (protocol::decode_bound_session_replaced (canonical_replacement)
            == replacement);
    for (const auto &malformed : replacement_vectors.at ("malformed")) {
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_bound_session_replaced (
              malformed.at ("bytes").get<std::vector<std::uint8_t>> ()));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    for (auto malformed : std::vector<std::vector<std::uint8_t>>{
           [&] {
               auto value = encoded_bound_send;
               value.back () = 0;
               return value;
           } (),
           [&] {
               auto value = protocol::encode_bound_session_bind ({
                 .correlation = 9,
                 .actor = bound_actor,
                 .session_routing_id = {'s'},
                 .binding = {
                   .state = protocol::bound_session_binding_state_t::active,
                   .generation = 7}});
               value[value.size () - 10] = 3;
               return value;
           } ()}) {
        bool rejected = false;
        try {
            if (malformed[3]
                == static_cast<std::uint8_t> (
                  protocol::command::boundSessionSend)) {
                static_cast<void> (
                  protocol::decode_bound_session_send (malformed));
            } else {
                static_cast<void> (
                  protocol::decode_bound_session_bind (malformed));
            }
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    const auto node_send = protocol::encode_node_send_header ();
    const auto decoded_node_send = protocol::decode_header (node_send);
    assert (decoded_node_send.kind == protocol::command::nodeSend);
    assert (decoded_node_send.flags == 0);
    const auto channel_send =
      protocol::encode_channel_send_header ("alpha");
    assert (protocol::decode_channel_send_header (channel_send) == "alpha");
    const protocol::application_payload_t application{
      "Probe", "application/json", {1, 2, 3}};
    const auto application_wire =
      protocol::encode_application_payload (application);
    assert (protocol::decode_application_payload (application_wire)
            == application);
    assert (protocol::application_payload_hwm_bytes (application) == 3);
    assert (protocol::application_payload_hwm_bytes (application_wire) == 3);
    const protocol::application_payload_t multipart_application{
      protocol::framework_multipart_packet_name,
      protocol::framework_multipart_content_type,
      {0, 0, 0, 2,
       0, 0, 0, 3, 1, 2, 3,
       0, 0, 0, 4, 4, 5, 6, 7}};
    assert (protocol::application_payload_hwm_bytes (
              multipart_application)
            == 4);
    const auto multipart_wire =
      protocol::encode_application_payload (multipart_application);
    assert (protocol::application_payload_hwm_bytes (multipart_wire) == 4);
    auto truncated_multipart = multipart_application;
    truncated_multipart.payload.pop_back ();
    bool truncated_multipart_rejected = false;
    try {
        (void) protocol::application_payload_hwm_bytes (
          truncated_multipart);
    }
    catch (const protocol::service_wire_error_t &) {
        truncated_multipart_rejected = true;
    }
    assert (truncated_multipart_rejected);
    auto truncated_multipart_wire = multipart_wire;
    truncated_multipart_wire.pop_back ();
    bool truncated_multipart_wire_rejected = false;
    try {
        (void) protocol::application_payload_hwm_bytes (
          truncated_multipart_wire);
    }
    catch (const protocol::service_wire_error_t &) {
        truncated_multipart_wire_rejected = true;
    }
    assert (truncated_multipart_wire_rejected);
    const protocol::application_payload_t traced_application{
      "Probe", "application/json", {4, 5, 6},
      "019fc5b9-9df3-786b-bb69-d55358f6d48b",
      zlink::framework::flow_origin_t::application};
    const auto traced_application_wire =
      protocol::encode_application_payload (traced_application);
    assert (traced_application_wire.front () == 2);
    assert (protocol::decode_application_payload (traced_application_wire)
            == traced_application);
    /* spec 27 §4: capture_flow=false skips the observation-only flow pair
     * structurally — no validation, no materialization — so malformed flow
     * fields cannot reject a frame at Off or at frame-integrity guards. */
    {
        const auto stripped =
          protocol::decode_application_payload (traced_application_wire, false);
        assert (!stripped.flow_id && !stripped.flow_origin);
        assert (stripped.packet_name == traced_application.packet_name
                && stripped.payload == traced_application.payload);
        auto corrupted_wire = traced_application_wire;
        /* Corrupt the first flow-id byte (after the 1-byte text8 length). */
        const auto flow_position = corrupted_wire.size () - 37;
        corrupted_wire[flow_position] = static_cast<std::uint8_t> ('Z');
        bool corrupted_rejected = false;
        try {
            (void) protocol::decode_application_payload (corrupted_wire);
        }
        catch (const protocol::service_wire_error_t &) {
            corrupted_rejected = true;
        }
        assert (corrupted_rejected);
        const auto corrupted_stripped =
          protocol::decode_application_payload (corrupted_wire, false);
        assert (!corrupted_stripped.flow_id
                && corrupted_stripped.payload == traced_application.payload);
        /* HWM accounting stays mode-independent and structural. */
        assert (protocol::application_payload_hwm_bytes (
                  std::span<const std::uint8_t> (corrupted_wire))
                == traced_application.payload.size ());
    }
    auto admission_descriptor = mesh::service_node_descriptor_t{
      "codec-mesh", std::vector<std::uint8_t>{'n', 'o', 'd', 'e'},
      1, 7, "tcp://127.0.0.1:7000",
      {{"alpha", 100}}, mesh::service_node_state_t::serving};
    admission_descriptor.security_identity = "test";
    admission_descriptor.application_version = 42;
    for (const auto kind :
         {protocol::command::hello, protocol::command::admit,
          protocol::command::update}) {
        const auto encoded =
          protocol::encode_route_mesh_admission (kind, admission_descriptor);
        // The RouteMesh schema does not reserve four bytes for a message-size
        // limit; the v12+v13 capability advertisement is part of this wire size.
        assert (encoded.size () == 201);
        assert (protocol::decode_route_mesh_admission (
                  encoded, kind, admission_descriptor.node_routing_id)
                == admission_descriptor);
    }
    assert (protocol::decode_reject (protocol::encode_reject (7)) == 7);
    const protocol::client_server_client_admission_t client_admission{
      "alpha", "security-a", 1024 * 1024};
    assert (protocol::decode_client_server_client_admission (
              protocol::encode_client_server_client_admission (
                protocol::command::hello, client_admission),
              protocol::command::hello)
            == client_admission);
    const protocol::client_server_server_admission_t server_admission{
      "alpha",
      {'s', 'e', 'r', 'v', 'e', 'r'},
      3,
      7,
      100,
      mesh::service_node_state_t::serving,
      "security-a",
      1024 * 1024,
      "tcp://127.0.0.1:7002"};
    assert (protocol::decode_client_server_server_admission (
              protocol::encode_client_server_server_admission (
                protocol::command::admit, server_admission),
              protocol::command::admit)
            == server_admission);
    constexpr std::uint64_t correlation = 0x0102030405060708ULL;
    assert (protocol::decode_node_request_header (
              protocol::encode_node_request_header (correlation))
            == correlation);
    const auto channel_request = protocol::decode_channel_request_header (
      protocol::encode_channel_request_header (correlation, "alpha"));
    assert (channel_request.correlation == correlation);
    assert (channel_request.channel_name == "alpha");
    const auto reply = protocol::decode_reply_header (
      protocol::encode_reply_header (correlation, 0, 0));
    assert (reply.correlation == correlation);
    assert (reply.terminal_result == 0);
    assert (reply.failure_code == 0);

    // GOLDEN - service-wire-v1.schema.json reply(20) byte layout.
    // `request-specific-tail` is a conditional-union WITHOUT `bodyLengthType`,
    // so the tail is written inline: prefix(5) + u64 correlation +
    // u32 terminalResult + u32 failureCode + tail. An empty tail therefore
    // yields exactly 21 bytes with no u16 tail-length field. These vectors are
    // byte-identical across C++/Java/Node/.NET.
    const std::vector<std::uint8_t> golden_empty_tail_reply{
      0x5a, 0x4d, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert (protocol::encode_reply_header (7, 0, 0)
            == golden_empty_tail_reply);
    assert (golden_empty_tail_reply.size () == 21);
    const std::vector<std::uint8_t> golden_failure_reply{
      0x5a, 0x4d, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x08, 0x00, 0x00, 0x00, 0x66, 0x00, 0x00, 0x00, 0x0e};
    assert (protocol::encode_reply_header (8, 102, 14)
            == golden_failure_reply);

    // Every ClientServer framework error mapping must be a canonical reply
    // header pair accepted by every language's service-wire decoder.
    const std::vector<std::pair<
      protocol::request_terminal_result, protocol::framework_error_code>>
      client_server_failure_pairs{
        {protocol::request_terminal_result::notFound,
         protocol::framework_error_code::handlerNotFound},
        {protocol::request_terminal_result::protocolError,
         protocol::framework_error_code::payloadDecodeFailed},
        {protocol::request_terminal_result::internalError,
         protocol::framework_error_code::routeNotConnected},
        {protocol::request_terminal_result::notFound,
         protocol::framework_error_code::requestTargetNotFound},
        {protocol::request_terminal_result::rejected,
         protocol::framework_error_code::requestRejected},
        {protocol::request_terminal_result::protocolError,
         protocol::framework_error_code::requestProtocolError},
        {protocol::request_terminal_result::internalError,
         protocol::framework_error_code::requestFailed}};
    for (const auto &[terminal_result, failure_code] :
         client_server_failure_pairs) {
        const auto mapped = protocol::decode_reply_header (
          protocol::encode_reply_header (
            correlation, static_cast<std::uint32_t> (terminal_result),
            static_cast<std::uint32_t> (failure_code)));
        assert (mapped.terminal_result
                == static_cast<std::uint32_t> (terminal_result));
        assert (
          mapped.failure_code
          == static_cast<std::uint32_t> (failure_code));
    }

    const std::vector<std::pair<
      protocol::request_terminal_result, protocol::framework_error_code>>
      mismatched_reply_pairs{
        {protocol::request_terminal_result::notFound,
         protocol::framework_error_code::requestRejected},
        {protocol::request_terminal_result::rejected,
         protocol::framework_error_code::handlerNotFound}};
    for (const auto &[terminal_result, failure_code] :
         mismatched_reply_pairs) {
        bool encode_rejected = false;
        try {
            static_cast<void> (protocol::encode_reply_header (
              correlation, static_cast<std::uint32_t> (terminal_result),
              static_cast<std::uint32_t> (failure_code)));
        }
        catch (const protocol::service_wire_error_t &) {
            encode_rejected = true;
        }
        assert (encode_rejected);
    }

    const auto overwrite_u32 = [] (
      std::vector<std::uint8_t> &bytes,
      std::size_t offset,
      std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes[offset++] =
              static_cast<std::uint8_t> (value >> shift);
    };
    for (const auto &[terminal_result, failure_code] :
         mismatched_reply_pairs) {
        auto malformed = protocol::encode_reply_header (
          correlation, 106,
          static_cast<std::uint32_t> (
            protocol::framework_error_code::requestRejected));
        overwrite_u32 (
          malformed, malformed.size () - 8,
          static_cast<std::uint32_t> (terminal_result));
        overwrite_u32 (
          malformed, malformed.size () - 4,
          static_cast<std::uint32_t> (failure_code));
        bool decode_rejected = false;
        try {
            static_cast<void> (
              protocol::decode_reply_header (malformed));
        }
        catch (const protocol::service_wire_error_t &) {
            decode_rejected = true;
        }
        assert (decode_rejected);
    }

    // Channel error envelopes use the shared JSON wire (spec 51).  An Error
    // reply must name one of the canonical framework error codes; accepting a
    // missing or invented code would turn malformed remote input into a
    // fabricated application failure.
    const messaging::envelope_codec_t envelope_codec;
    const auto invalid_error_header = [&] (std::string_view error_code) {
        return envelope_codec.decode_header (zlink::message_t::from (
          std::string{"{\"formatMarker\":242,\"kind\":5,\"channelName\":\"test\",\"messageName\":\"reply\",\"contentType\":\"application/json\",\"correlationId\":\"request-1\",\"deadline\":null,\"topic\":null,"}
          + std::string (error_code)
          + ",\"errorMessage\":\"failed\",\"source\":null,\"metadata\":{}}"));
    };
    const auto missing_error_code = invalid_error_header (R"("errorCode":null)");
    assert (!missing_error_code
            && missing_error_code.error_kind ()
                 == zlink::framework::framework_error_kind_t::protocol_error);
    const auto unknown_error_code =
      invalid_error_header (R"("errorCode":"invented_error")");
    assert (!unknown_error_code
            && unknown_error_code.error_kind ()
                 == zlink::framework::framework_error_kind_t::protocol_error);
    const protocol::spot_route_fence_t spot_fence{
      {'s', 'p', 'o', 't'},
      3,
      {'n', 'o', 'd', 'e'},
      5,
      7,
      8};
    const auto spot_request = protocol::decode_spot_message_header (
      protocol::encode_spot_message_header (
        protocol::command::spotRequest, {'s', 'o', 'u', 'r', 'c', 'e'},
        spot_fence, {9, correlation}, correlation),
      protocol::command::spotRequest);
    assert (spot_request.correlation == correlation);
    assert ((spot_request.operation
             == protocol::wire_operation_id_t{9, correlation}));
    assert (spot_request.message_follow_hop_count == 0);
    assert (spot_request.target == spot_fence);
    const protocol::actor_route_fence_t actor_fence{
      "actor-1", 11, {'n', 'o', 'd', 'e'}, 5, 9, 10};
    const std::optional<std::pair<std::string, std::uint64_t>>
      source_actor{std::pair{"actor-0", 4}};
    const auto actor_send = protocol::decode_actor_message_header (
      protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, actor_fence,
        {9, correlation}),
      protocol::command::actorSend);
    assert (!actor_send.correlation);
    assert ((actor_send.operation
             == protocol::wire_operation_id_t{9, correlation}));
    assert (actor_send.message_follow_hop_count == 0);
    assert (actor_send.source_actor == source_actor);
    assert (actor_send.target == actor_fence);
    const protocol::actor_message_header_t::bound_session_source_t
      bound_session_source{{'s', 'e', 's', 's'}, 17, 23};
    const auto bound_actor_send = protocol::decode_actor_message_header (
      protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, actor_fence,
        {9, correlation}, std::nullopt, 0, bound_session_source),
      protocol::command::actorSend);
    assert (bound_actor_send.bound_session_source == bound_session_source);

    auto missing_source_spot_flag = protocol::encode_actor_message_header (
      protocol::command::actorSend, source_actor, actor_fence,
      {9, correlation}, std::nullopt, 0, bound_session_source);
    missing_source_spot_flag[4] =
      static_cast<std::uint8_t> (protocol::flag::boundSession);
    bool incomplete_bound_session_flags_rejected = false;
    try {
        static_cast<void> (protocol::decode_actor_message_header (
          missing_source_spot_flag, protocol::command::actorSend));
    }
    catch (const protocol::service_wire_error_t &) {
        incomplete_bound_session_flags_rejected = true;
    }
    assert (incomplete_bound_session_flags_rejected);

    bool zero_bound_session_sequence_rejected = false;
    try {
        static_cast<void> (protocol::encode_actor_message_header (
          protocol::command::actorSend, source_actor, actor_fence,
          {9, correlation}, std::nullopt, 0,
          protocol::actor_message_header_t::bound_session_source_t{
            {'s', 'e', 's', 's'}, 17, 0}));
    }
    catch (const protocol::service_wire_error_t &) {
        zero_bound_session_sequence_rejected = true;
    }
    assert (zero_bound_session_sequence_rejected);
    const protocol::actor_route_fence_t follow_target{
      "actor-1", 11, {'t', 'a', 'r', 'g', 'e', 't'}, 6, 12, 13};
    const protocol::message_follow_notice_t follow_notice{
      actor_fence,
      follow_target,
      1,
      1,
      4096,
      {9, correlation},
      77};
    assert (protocol::decode_message_follow (
               protocol::encode_message_follow (follow_notice))
             == follow_notice);
    const protocol::message_follow_notice_t one_way_follow_notice{
      actor_fence,
      follow_target,
      1,
      1,
      4096,
      {9, correlation},
      0};
    assert (protocol::decode_message_follow (
               protocol::encode_message_follow (one_way_follow_notice))
             == one_way_follow_notice);
    const protocol::user_spot_create_header_t user_spot_create{
      correlation,
      {4, 5},
      {'s', 'o', 'u', 'r', 'c', 'e'},
      7,
      {'s', 'p', 'o', 't'},
      "room",
      {"reservation-1",
       "store-1",
       9,
       11,
       {'t', 'a', 'r', 'g', 'e', 't'},
       13,
       "owner-1",
       15,
       1},
      1700000000000ULL};
    assert (protocol::decode_user_spot_create_header (
              protocol::encode_user_spot_create_header (
                user_spot_create))
            == user_spot_create);
    const protocol::instance_spot_activation_header_t instance_activation{
      {{'t', 'a', 'r', 'g', 'e', 't'},
       7,
       "spot-1",
       "main",
       "quest",
       "descriptor-9",
       1700000000000ULL},
      3,
      {'s', 'o', 'u', 'r', 'c', 'e'},
      std::string ("entry"),
      true,
      {0, 9},
      11,
      true};
    const auto encoded_instance_activation =
      protocol::encode_instance_spot_activation_header (
        instance_activation);
    assert (encoded_instance_activation[3]
            == static_cast<std::uint8_t> (
              protocol::command::instanceSpot));
    assert (protocol::decode_instance_spot_activation_header (
              encoded_instance_activation)
            == instance_activation);
    auto trailing_instance_activation = encoded_instance_activation;
    trailing_instance_activation.push_back (0);
    bool rejected_instance_activation = false;
    try {
        (void) protocol::decode_instance_spot_activation_header (
          trailing_instance_activation);
    }
    catch (const protocol::service_wire_error_t &) {
        rejected_instance_activation = true;
    }
    assert (rejected_instance_activation);
    const protocol::instance_activation_recovery_t instance_recovery{
      instance_activation,
      from_hex ("01010574726163650003616263"),
      {"quest.start", "application/json",
       {'{', '"', 'x', '"', ':', '1', '}'}}};
    const auto golden_instance_recovery = from_hex (
      "5a4c4941010000000000a00673706f742d31057175657374046d61696e067461"
      "7267657400000000000000070c64657363726970746f722d3906736f75726365"
      "00000000000000030105656e7472790200000000000000000000000000000009"
      "000000000000000b0000018bcfe5680001010105747261636500036162630100"
      "0000280b71756573742e7374617274106170706c69636174696f6e2f6a736f6e"
      "000000077b2278223a317de138c97b");
    assert (protocol::encode_instance_activation_recovery (
              instance_recovery)
            == golden_instance_recovery);
    assert (protocol::decode_instance_activation_recovery (
              golden_instance_recovery)
            == instance_recovery);
    auto corrupt_instance_recovery = golden_instance_recovery;
    corrupt_instance_recovery.back () ^= 1;
    bool rejected_instance_recovery = false;
    try {
        (void) protocol::decode_instance_activation_recovery (
          corrupt_instance_recovery);
    }
    catch (const protocol::service_wire_error_t &) {
        rejected_instance_recovery = true;
    }
    assert (rejected_instance_recovery);
    const protocol::user_spot_close_header_t user_spot_close{
      correlation,
      {6, 7},
      {'s', 'o', 'u', 'r', 'c', 'e'},
      7,
      {{'s', 'p', 'o', 't'},
       9,
       {'t', 'a', 'r', 'g', 'e', 't'},
       13,
       11,
       "store-2"},
      1700000001000ULL};
    assert (protocol::decode_user_spot_close_header (
              protocol::encode_user_spot_close_header (
                user_spot_close))
            == user_spot_close);
    const auto create_reply =
      protocol::decode_user_spot_create_reply (
        protocol::encode_user_spot_create_reply (
          correlation, 0, 0,
          protocol::user_spot_create_result_t::created,
          {'s', 'p', 'o', 't'}, 9));
    assert (create_reply.header.correlation == correlation);
    assert (create_reply.result
            == protocol::user_spot_create_result_t::created);
    assert (create_reply.object_generation == 9);
    const auto close_reply =
      protocol::decode_user_spot_close_reply (
        protocol::encode_user_spot_close_reply (
          correlation, 0, 0, true));
    assert (close_reply.closed);
    const auto stale_create_reply =
      protocol::decode_user_spot_create_reply (
        protocol::encode_user_spot_create_reply (
          correlation, 107,
          static_cast<std::uint32_t> (
            protocol::framework_error_code::spotGenerationStale),
          protocol::user_spot_create_result_t::rejected, {}, 0));
    assert (stale_create_reply.header.failure_code == 33);
    const auto type_mismatch_create_reply =
      protocol::decode_user_spot_create_reply (
        protocol::encode_user_spot_create_reply (
          correlation, 107,
          static_cast<std::uint32_t> (
            protocol::framework_error_code::spotTypeMismatch),
          protocol::user_spot_create_result_t::rejected, {}, 0));
    assert (
      type_mismatch_create_reply.header.failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::spotTypeMismatch));
    const auto moving_close_reply =
      protocol::decode_user_spot_close_reply (
        protocol::encode_user_spot_close_reply (
          correlation, 107,
          static_cast<std::uint32_t> (
            protocol::framework_error_code::spotMoving),
          false));
    assert (
      moving_close_reply.header.failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::spotMoving));
    const auto deadline_create_reply =
      protocol::decode_user_spot_create_reply (
        protocol::encode_user_spot_create_reply (
          correlation, 101, 0,
          protocol::user_spot_create_result_t::rejected, {}, 0));
    assert (deadline_create_reply.header.terminal_result == 101);
    const auto busy_create_reply =
      protocol::decode_user_spot_create_reply (
        protocol::encode_user_spot_create_reply (
          correlation, 108, 0,
          protocol::user_spot_create_result_t::rejected, {}, 0));
    assert (busy_create_reply.header.terminal_result == 108);
    auto trailing_user_spot_create =
      protocol::encode_user_spot_create_header (
        user_spot_create);
    trailing_user_spot_create.push_back (0);
    bool rejected_user_spot_create = false;
    try {
        (void) protocol::decode_user_spot_create_header (
          trailing_user_spot_create);
    }
    catch (const protocol::service_wire_error_t &) {
        rejected_user_spot_create = true;
    }
    assert (rejected_user_spot_create);
    for (auto malformed_payload : std::vector<std::vector<std::uint8_t>>{
           [&] { auto value = application_wire; value[0] = 2; return value; } (),
           [&] { auto value = application_wire; value[4] += 1; return value; } (),
           [&] { auto value = application_wire; value.push_back (0); return value; } (),
           [&] {
               auto value = application_wire;
               value[6] = 0xc0;
               return value;
           } ()}) {
        bool rejected = false;
        try {
            static_cast<void> (
              protocol::decode_application_payload (malformed_payload));
        } catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    const protocol::session_relocation_seal_t session_seal{
      {1, 2},
      {"coord", 3, {0xc1}, 4, "v5"},
      protocol::relocation_role_t::source,
      {"actor", 6, {0xe1}, 14, 10, 15},
      {0xa1},
      7,
      "owner",
      8,
      {0xb1},
      9};

    const protocol::reply_relay_t reply_relay{
      {1, 2},
      3,
      {4, 5},
      6,
      {"coordinator", 7,
       {'n', 'o', 'd', 'e', '-', 'a'}, 11, "store-3"},
      8,
      9,
      101,
      protocol::framework_error_code::none};
    const auto encoded_reply_relay =
      protocol::encode_reply_relay (reply_relay);
    assert (encoded_reply_relay == from_hex (
      "5a4d012100000000000000000100000000000000020000000000000003020054"
      "0000000000000004000000000000000500000000000000060b636f6f7264696e"
      "61746f720000000000000007066e6f64652d61000000000000000b000773746f"
      "72652d33000000000000000800000000000000090000006500000000"));
    assert (protocol::decode_reply_relay (encoded_reply_relay)
            == reply_relay);

    const protocol::reply_relay_ack_t reply_relay_ack{
      {4, 5},
      reply_relay.coordinator,
      {1, 2},
      3,
      {"source", 13, {'n', 'o', 'd', 'e', '-', 's'}, 17},
      protocol::reply_relay_ack_status_t::already_terminal};
    const auto encoded_reply_relay_ack =
      protocol::encode_reply_relay_ack (reply_relay_ack);
    assert (encoded_reply_relay_ack == from_hex (
      "5a4d012e00000000000000000400000000000000050b636f6f7264696e61746f"
      "720000000000000007066e6f64652d61000000000000000b000773746f72652d"
      "3300000000000000010000000000000002000000000000000306736f75726365000000000000000d"
      "066e6f64652d73000000000000001102"));
    assert (protocol::decode_reply_relay_ack (encoded_reply_relay_ack)
            == reply_relay_ack);
    for (auto malformed : {encoded_reply_relay, encoded_reply_relay_ack}) {
        malformed.push_back (0);
        bool rejected = false;
        try {
            if (malformed[3]
                == static_cast<std::uint8_t> (protocol::command::replyRelay))
                static_cast<void> (protocol::decode_reply_relay (malformed));
            else
                static_cast<void> (
                  protocol::decode_reply_relay_ack (malformed));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        auto invalid = reply_relay;
        invalid.terminal_result = 102;
        bool rejected = false;
        try {
            static_cast<void> (protocol::encode_reply_relay (invalid));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        auto invalid = encoded_reply_relay_ack;
        invalid.back () = 0;
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_reply_relay_ack (invalid));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        auto invalid = reply_relay_ack;
        invalid.reply_route_id = 0;
        bool rejected = false;
        try {
            static_cast<void> (protocol::encode_reply_relay_ack (invalid));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    const auto encoded_session_seal =
      protocol::encode_session_relocation_seal (session_seal);
    assert (protocol::decode_session_relocation_seal (
              encoded_session_seal)
            == session_seal);
    const protocol::session_relocation_sealed_t session_sealed{
      session_seal.relocation,
      session_seal.coordinator,
      session_seal.actor,
      session_seal.session_owner_node_routing_id,
      session_seal.session_owner_node_generation,
      session_seal.session_owner_id,
      session_seal.session_owner_lease_generation,
      session_seal.session_routing_id,
      session_seal.binding_generation};
    assert (protocol::decode_session_relocation_sealed (
              protocol::encode_session_relocation_sealed (
                session_sealed))
            == session_sealed);

    const protocol::session_relocation_route_t session_route{
      {1, 2},
      {"coord", 3, {0xc1}, 4, "v5"},
      protocol::relocation_role_t::target,
      {"actor", 6},
      {0xa1},
      7,
      "owner",
      8,
      {0xb1},
      9,
      {protocol::session_relocation_route_action_t::commit,
       10, 11, {0xd1}, 12, 0}};
    const auto encoded_session_route =
      protocol::encode_session_relocation_route (session_route);
    assert (encoded_session_route == from_hex (
      "5a4d012c00"
      "00000000000000010000000000000002"
      "05636f6f7264000000000000000301c1"
      "00000000000000040002763502"
      "056163746f72000000000000000601a1"
      "0000000000000007056f776e6572"
      "000000000000000801b10000000000000009"
      "01001a000000000000000a000000000000000b01d1"
      "000000000000000c"));
    assert (protocol::decode_session_relocation_route (
              encoded_session_route)
            == session_route);
    auto abort_session_route = session_route;
    abort_session_route.sender_role =
      protocol::relocation_role_t::source;
    abort_session_route.route = {
      protocol::session_relocation_route_action_t::abort,
      0, 0, {}, 0, 10};
    assert (protocol::decode_session_relocation_route (
              protocol::encode_session_relocation_route (
                abort_session_route))
            == abort_session_route);

    {
        std::ifstream fixture (
          ZLINK_SESSION_RELOCATION_BARRIER_GOLDEN_PATH);
        assert (fixture.good ());
        const auto vectors = nlohmann::json::parse (fixture);
        assert (vectors.at ("canonical").size () == 4);
        const auto &identity = vectors.at ("identity");
        const auto ordinal = [&identity] (const char *name) {
            return std::stoull (
              identity.at (name).get<std::string> ());
        };
        const auto bytes = [&identity] (const char *name) {
            const auto value = identity.at (name).get<std::string> ();
            return std::vector<std::uint8_t> (
              value.begin (), value.end ());
        };
        const auto canonical = [&vectors] (const char *name) {
            const auto &items = vectors.at ("canonical");
            const auto found = std::find_if (
              items.begin (), items.end (),
              [name] (const auto &item) {
                  return item.at ("name").template get<std::string> ()
                         == name;
              });
            assert (found != items.end ());
            const auto encoded = from_hex (
              found->at ("hex").template get<std::string> ());
            assert (encoded.size () > 3);
            assert (encoded[3]
                    == found->at ("command").template get<std::uint8_t> ());
            return encoded;
        };
        const protocol::relocation_id_t relocation{
          ordinal ("relocationHigh"), ordinal ("relocationLow")};
        const protocol::relocation_coordinator_fence_t coordinator{
          identity.at ("coordinatorOwnerId").get<std::string> (),
          ordinal ("coordinatorLeaseGeneration"),
          bytes ("coordinatorNodeRid"),
          ordinal ("coordinatorNodeGeneration"),
          identity.at ("expectedAuthorityStoreVersion")
            .get<std::string> ()};
        const protocol::actor_route_fence_t actor{
          identity.at ("actorId").get<std::string> (),
          ordinal ("objectGeneration"),
          bytes ("sourceNodeRid"),
          ordinal ("sourceNodeGeneration"),
          ordinal ("sourceAuthorityOwnerGeneration"),
          ordinal ("sourceOwnerLeaseGeneration")};
        const protocol::session_relocation_seal_t seal{
          relocation,
          coordinator,
          protocol::relocation_role_t::source,
          actor,
          bytes ("sessionOwnerNodeRid"),
          ordinal ("sessionOwnerNodeGeneration"),
          identity.at ("sessionOwnerId").get<std::string> (),
          ordinal ("sessionOwnerLeaseGeneration"),
          bytes ("sessionRid"),
          ordinal ("bindingGeneration")};
        const protocol::session_relocation_sealed_t sealed{
          relocation,
          coordinator,
          actor,
          seal.session_owner_node_routing_id,
          seal.session_owner_node_generation,
          seal.session_owner_id,
          seal.session_owner_lease_generation,
          seal.session_routing_id,
          seal.binding_generation};
        const protocol::session_relocation_route_t commit{
          relocation,
          coordinator,
          protocol::relocation_role_t::target,
          {actor.actor_id, actor.object_generation},
          seal.session_owner_node_routing_id,
          seal.session_owner_node_generation,
          seal.session_owner_id,
          seal.session_owner_lease_generation,
          seal.session_routing_id,
          seal.binding_generation,
          {protocol::session_relocation_route_action_t::commit,
           ordinal ("sourceAuthorityOwnerGeneration"),
           ordinal ("targetAuthorityOwnerGeneration"),
           bytes ("targetNodeRid"),
           ordinal ("targetNodeGeneration"),
           0}};
        auto abort = commit;
        abort.sender_role = protocol::relocation_role_t::source;
        abort.route = {
          protocol::session_relocation_route_action_t::abort,
          0,
          0,
          {},
          0,
          ordinal ("sourceAuthorityOwnerGeneration")};

        assert (protocol::encode_session_relocation_seal (seal)
                == canonical ("sessionRelocationSeal"));
        assert (protocol::encode_session_relocation_sealed (sealed)
                == canonical ("sessionRelocationSealed"));
        assert (protocol::encode_session_relocation_route (commit)
                == canonical ("sessionRelocationRouteCommit"));
        assert (protocol::encode_session_relocation_route (abort)
                == canonical ("sessionRelocationRouteAbort"));
    }
    {
        auto malformed = encoded_session_route;
        malformed.push_back (0);
        bool rejected = false;
        try {
            static_cast<void> (
              protocol::decode_session_relocation_route (malformed));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    const std::vector<std::string_view> relocation_control_golden{
      "5a4d011e000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33066e6f64652d62000000000000000c0c7461726765742d6f776e657200000000000000080200170673706f742d310000000000000009000000000000000a02",
      "5a4d011f000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a06010021016e00000000000000010e726571756573742d736f757263650000000000000006000000000000000000000000000000002a000000030008000000000000006304726f6f6d0000000000000001016d00000000000000020000000000000007000000000000000101000000290b4368617452657175657374106170706c69636174696f6e2f6a736f6e000000087b226964223a317d",
      "5a4d0122000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a0000000000000003e3069283",
      /* relocationPrepareRestore (golden): the manifest body carries
       * payloadTotalLength, payloadChunkCount, payloadChecksumCrc32c and
       * applicationVersion only (baseChecksumCrc32c removed). */
      "5a4d0128000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33066e6f64652d62000000000000000c0c7461726765742d6f776e65720000000000000008010200170673706f742d310000000000000009000000000000000a066e6f64652d61000000000000000b00000000000000180000000229bc87950000000000000001",
      /* relocationStateChunk (golden): no payloadStage byte; chunkOrdinal
       * follows object directly. */
      "5a4d0134000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a0000000000000010000102030405060708090a0b0c0d0e0f",
      "5a4d0134000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a00000001000000081011121314151617"};
    std::vector<protocol::relocation_control_t> decoded_controls;
    for (const auto fixture : relocation_control_golden) {
        const auto bytes = from_hex (fixture);
        const auto decoded = protocol::decode_relocation_control (bytes);
        assert (protocol::encode_relocation_control (decoded) == bytes);
        decoded_controls.push_back (decoded);
        auto truncated = bytes;
        truncated.pop_back ();
        bool rejected = false;
        try {
            static_cast<void> (
              protocol::decode_relocation_control (truncated));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    assert (std::holds_alternative<protocol::relocation_ready_t> (
      decoded_controls[0]));
    assert (std::holds_alternative<protocol::relocation_data_t> (
      decoded_controls[1]));
    assert (std::holds_alternative<protocol::relocation_cutover_t> (
      decoded_controls[2]));
    assert (std::holds_alternative<protocol::relocation_prepare_t> (
      decoded_controls[3]));
    assert (std::holds_alternative<protocol::relocation_state_t> (
      decoded_controls[4]));
    assert (std::holds_alternative<protocol::relocation_state_t> (
      decoded_controls[5]));
    const auto &ready = std::get<protocol::relocation_ready_t> (
      decoded_controls[0]);
    const auto &data = std::get<protocol::relocation_data_t> (
      decoded_controls[1]);
    const auto &cutover = std::get<protocol::relocation_cutover_t> (
      decoded_controls[2]);
    const auto &prepare = std::get<protocol::relocation_prepare_t> (
      decoded_controls[3]);
    const auto &state_chunk = std::get<protocol::relocation_state_t> (
      decoded_controls[4]);
    const auto &state_final_chunk = std::get<protocol::relocation_state_t> (
      decoded_controls[5]);
    assert (ready.sender_role == protocol::relocation_role_t::target);
    {
        const protocol::relocation_failed_t failure{
          ready.relocation, ready.target_attempt_generation,
          ready.coordinator, ready.target, ready.object,
          protocol::relocation_role_t::target,
          static_cast<std::uint32_t> (
            protocol::framework_error_code::relocationDataLost)};
        const auto encoded = protocol::encode_relocation_control (failure);
        assert (encoded[3] == static_cast<std::uint8_t> (
          protocol::command::relocationFailed));
        const auto decoded = protocol::decode_relocation_control (encoded);
        assert (std::get<protocol::relocation_failed_t> (decoded) == failure);
    }
    assert (data.sender_role == protocol::relocation_role_t::source);
    assert (data.record.kind
            == protocol::frozen_record_kind_t::spot_request);
    assert (cutover.sender_role == protocol::relocation_role_t::source);
    assert (cutover.boundary_record_count == 3);
    assert (cutover.boundary_checksum_crc32c == 0xe3069283u);
    assert (prepare.initiator_role == protocol::relocation_role_t::source);
    assert (prepare.payload_total_length == 24);
    assert (prepare.payload_chunk_count == 2);
    assert (prepare.payload_checksum_crc32c == 0x29bc8795u);
    assert (state_chunk.sender_role == protocol::relocation_role_t::source);
    assert (state_chunk.chunk_ordinal == 0);
    assert (state_chunk.chunk_data.size () == 16);
    assert (state_final_chunk.chunk_ordinal == 1);
    assert (state_final_chunk.chunk_data.size () == 8);
    {
        /* prepare manifest checksum matches the CRC-32C convention over the
         * 24-byte payload split into the two relocationState chunks. */
        std::vector<std::uint8_t> payload = state_chunk.chunk_data;
        payload.insert (payload.end (), state_final_chunk.chunk_data.begin (),
                        state_final_chunk.chunk_data.end ());
        assert (payload.size () == prepare.payload_total_length);
        assert (protocol::relocation_checksum_crc32c (payload)
                == prepare.payload_checksum_crc32c);
        const std::string_view check_input{"123456789"};
        assert (protocol::relocation_checksum_crc32c (
                  std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t *> (
                      check_input.data ()),
                    check_input.size ()})
                == 0xe3069283u);
    }
    {
        /* golden malformed: zero target attempt generation, truncated chunk
         * data, and a trailing byte are all rejected. */
        const std::vector<std::string_view> malformed{
          "5a4d0134000000000000000004000000000000000500000000000000000b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a0000000000000010000102030405060708090a0b0c0d0e0f",
          "5a4d0134000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a0000000000000010000102030405060708090a0b0c0d0e",
          "5a4d0134000000000000000004000000000000000500000000000000060b636f6f7264696e61746f720000000000000007066e6f64652d61000000000000000b000773746f72652d33010200170673706f742d310000000000000009000000000000000a0000000000000010000102030405060708090a0b0c0d0e0f00"};
        for (const auto fixture : malformed) {
            bool rejected = false;
            try {
                static_cast<void> (protocol::decode_relocation_control (
                  from_hex (fixture)));
            }
            catch (const protocol::service_wire_error_t &) {
                rejected = true;
            }
            assert (rejected);
        }
    }

    std::vector<std::vector<std::uint8_t>> frozen_records;
    frozen_records.push_back (make_frozen_record (
      1, 1, 0, 0, std::nullopt, frozen_payload (), true));
    frozen_records.push_back (make_frozen_record (
      2, 4, 1, 1, 12, frozen_payload (), true));
    {
        std::vector<std::uint8_t> body;
        put_text8 (body, "channel");
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          3, 2, 0, 0, std::nullopt, body, true));
        frozen_records.push_back (make_frozen_record (
          4, 3, 2, 2, 13, body, true));
    }
    {
        std::vector<std::uint8_t> body;
        put_spot_route (body);
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          5, 1, 0, 3, std::nullopt, body, true));
        frozen_records.push_back (make_frozen_record (
          6, 4, 3, 4, 14, body, true));
    }
    {
        std::vector<std::uint8_t> body;
        put_text8 (body, "channel");
        put_text8 (body, "topic");
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          7, 2, 0, 0, std::nullopt, body, true));
    }
    {
        std::vector<std::uint8_t> control_body;
        std::vector<std::uint8_t> snapshot;
        put_actor_ref (snapshot);
        put_spot_ref (snapshot);
        control_body.push_back (1);
        put_body16 (control_body, snapshot);
        frozen_records.push_back (make_frozen_record (
          8, 1, 7, 5, std::nullopt, control_body));
    }
    {
        std::vector<std::uint8_t> body;
        put_actor_route (body);
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          9, 3, 0, 6, std::nullopt, body, true));
        frozen_records.push_back (make_frozen_record (
          10, 4, 4, 7, 15, body, true));
    }
    {
        std::vector<std::uint8_t> body;
        put_u32 (body, 0);
        put_u32 (body, 0);
        body.push_back (1);
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          11, 3, 4, 8, 16, body));
    }
    {
        std::vector<std::uint8_t> body{1};
        std::vector<std::uint8_t> destination;
        put_text8 (destination, "node-t");
        put_body16 (body, destination);
        frozen_records.push_back (make_frozen_record (
          12, 1, 0, 0, std::nullopt, body));
    }
    {
        std::vector<std::uint8_t> body{4, 1};
        put_u64 (body, 4);
        put_u64 (body, 5);
        body.push_back (2);
        std::vector<std::uint8_t> object;
        put_text8 (object, "spot-1");
        put_u64 (object, 9);
        put_u64 (object, 10);
        put_body16 (body, object);
        put_u32 (body, 0);
        put_u32 (body, 0);
        frozen_records.push_back (make_frozen_record (
          13, 1, 0, 0, std::nullopt, body));
    }
    {
        std::vector<std::uint8_t> body{2};
        std::vector<std::uint8_t> route;
        put_text8 (route, "node-t");
        put_u64 (route, 5);
        put_text8 (route, "spot-1");
        put_text8 (route, "mesh");
        put_text8 (route, "instance-type");
        put_text8 (route, "descriptor-v1");
        put_u64 (route, 1000);
        put_body16 (body, route);
        put_u64 (body, 8);
        body.push_back (2);
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        frozen_records.push_back (make_frozen_record (
          14, 2, 12, 9, 17, body, true));
    }
    assert (frozen_records.size () == 14);
    for (std::size_t index = 0; index < frozen_records.size (); ++index) {
        const auto decoded = protocol::decode_frozen_record (
          frozen_records[index]);
        assert (static_cast<std::uint8_t> (decoded.kind) == index + 1);
        assert (protocol::encode_frozen_record (decoded)
                == frozen_records[index]);
        auto truncated = frozen_records[index];
        truncated.pop_back ();
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_frozen_record (truncated));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        protocol::frozen_application_record_t typed;
        typed.source_kind = protocol::frozen_source_kind_t::actor;
        typed.source = {
          .owner_id = "source-owner",
          .lease_generation = 7,
          .node_routing_id = {0x31},
          .node_generation = 9};
        typed.source_actor = std::pair{std::string{"source-actor"}, 3u};
        typed.metadata = {{"trace", "capture"}};
        typed.operation = {11, 12};
        typed.body = protocol::frozen_spot_application_body_t{
          .target =
            {.spot_id = "target-spot",
             .object_generation = 4,
             .target_node_routing_id = {0x41},
             .target_node_generation = 5,
             .authority_owner_generation = 6},
          .expected_owner_lease_generation = 7,
          .application = {"Packet", "application/json", {0x7b, 0x7d}}};
        for (const auto &[kind, operation_kind] : std::array{
               std::pair{protocol::frozen_record_kind_t::spot_send, 0u},
               std::pair{protocol::frozen_record_kind_t::spot_request, 3u}}) {
            typed.kind = kind;
            typed.operation_kind = operation_kind;
            typed.reply_route_id = operation_kind == 3
                                     ? std::make_optional<std::uint64_t> (44)
                                     : std::nullopt;
            const auto encoded = protocol::encode_frozen_application_record (
              typed);
            assert (encoded.kind == kind);
            assert (encoded.canonical_bytes
                    == protocol::encode_frozen_record (encoded));
            const auto summary =
              protocol::summarize_frozen_application_record (typed);
            auto expected_summary = encoded;
            expected_summary.canonical_bytes.clear ();
            assert (summary.canonical_bytes.empty ());
            assert (summary == expected_summary);
        }

        typed.body = protocol::frozen_actor_application_body_t{
          .target =
            {.actor_id = "target-actor",
             .object_generation = 4,
             .target_node_routing_id = {0x41},
             .target_node_generation = 5,
             .authority_owner_generation = 6,
             .owner_lease_generation = 7},
          .application = {"Packet", "application/json", {0x7b, 0x7d}}};
        for (const auto &[kind, operation_kind] : std::array{
               std::pair{protocol::frozen_record_kind_t::actor_send, 0u},
               std::pair{protocol::frozen_record_kind_t::actor_request, 4u}}) {
            typed.kind = kind;
            typed.operation_kind = operation_kind;
            typed.reply_route_id = operation_kind == 4
                                     ? std::make_optional<std::uint64_t> (45)
                                     : std::nullopt;
            const auto encoded = protocol::encode_frozen_application_record (
              typed);
            assert (encoded.kind == kind);
            assert (protocol::decode_frozen_record (encoded.canonical_bytes)
                    == encoded);
            const auto summary =
              protocol::summarize_frozen_application_record (typed);
            auto expected_summary = encoded;
            expected_summary.canonical_bytes.clear ();
            assert (summary.canonical_bytes.empty ());
            assert (summary == expected_summary);
        }

        typed.kind = protocol::frozen_record_kind_t::actor_request;
        typed.operation_kind = 4;
        typed.reply_route_id.reset ();
        bool missing_reply_rejected = false;
        try {
            static_cast<void> (
              protocol::encode_frozen_application_record (typed));
        }
        catch (const protocol::service_wire_error_t &) {
            missing_reply_rejected = true;
        }
        assert (missing_reply_rejected);
    }
    {
        const auto bound_request = protocol::decode_frozen_record (
          frozen_records[1]);
        assert (bound_request.source_kind
                == protocol::frozen_source_kind_t::bound_session);
        const std::optional<std::pair<std::string, std::uint64_t>>
          expected_actor{std::pair{std::string{"actor-s"},
                                   std::uint64_t{3}}};
        assert (bound_request.source_actor == expected_actor);
        assert (bound_request.source_binding_generation == 10);
        assert (bound_request.source_session_sequence == 11);
        assert (bound_request.reply_route_id == 12);
    }
    {
        auto mismatched = protocol::decode_frozen_record (
          frozen_records[1]);
        mismatched.operation_kind = 2;
        bool rejected = false;
        try {
            static_cast<void> (protocol::encode_frozen_record (mismatched));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        auto invalid = frozen_records[10];
        /* completion hasPayload byte follows terminal and failure fields */
        const auto decoded = protocol::decode_frozen_record (invalid);
        assert (decoded.kind == protocol::frozen_record_kind_t::completion);
        invalid.resize (invalid.size () - frozen_payload ().size ());
        invalid.back () = 2;
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_frozen_record (invalid));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        std::vector<std::uint8_t> snapshot;
        put_actor_ref (snapshot);
        put_spot_ref (snapshot);
        for (std::uint8_t lifecycle = 1; lifecycle <= 5; ++lifecycle) {
            std::vector<std::uint8_t> body{lifecycle};
            std::vector<std::uint8_t> lifecycle_body;
            if (lifecycle == 2) {
                lifecycle_body.push_back (0);
                put_u16 (lifecycle_body, 0);
                lifecycle_body.insert (lifecycle_body.end (),
                                       snapshot.begin (), snapshot.end ());
            }
            else {
                lifecycle_body.insert (lifecycle_body.end (),
                                       snapshot.begin (), snapshot.end ());
                if (lifecycle == 3)
                    lifecycle_body.insert (lifecycle_body.end (),
                                           snapshot.begin (), snapshot.end ());
            }
            put_body16 (body, lifecycle_body);
            const auto frozen = make_frozen_record (
              8, 1, 0, 0, std::nullopt, body);
            assert (protocol::encode_frozen_record (
                      protocol::decode_frozen_record (frozen))
                    == frozen);
        }
    }
    {
        for (std::uint8_t destination_kind = 1;
             destination_kind <= 5; ++destination_kind) {
            std::vector<std::uint8_t> body{destination_kind};
            std::vector<std::uint8_t> destination;
            if (destination_kind == 1)
                put_text8 (destination, "node-t");
            else if (destination_kind == 2)
                put_text8 (destination, "channel");
            else if (destination_kind == 3)
                put_spot_route (destination);
            else {
                put_actor_route (destination);
                if (destination_kind == 5)
                    put_u64 (destination, 12);
            }
            put_body16 (body, destination);
            const auto frozen = make_frozen_record (
              12, 1, 0, 0, std::nullopt, body);
            assert (protocol::encode_frozen_record (
                      protocol::decode_frozen_record (frozen))
                    == frozen);
        }
    }
    {
        std::vector<std::uint8_t> body{1};
        std::vector<std::uint8_t> route;
        put_text8 (route, "node-t");
        put_u64 (route, 5);
        put_text8 (route, "spot-1");
        put_u64 (route, 9);
        put_text8 (route, "owner-t");
        put_u64 (route, 10);
        put_u64 (route, 11);
        put_text16 (route, "store-1");
        put_body16 (body, route);
        put_u64 (body, 8);
        body.push_back (1);
        const auto payload = frozen_payload ();
        body.insert (body.end (), payload.begin (), payload.end ());
        const auto frozen = make_frozen_record (
          14, 3, 0, 0, std::nullopt, body, true);
        assert (protocol::encode_frozen_record (
                  protocol::decode_frozen_record (frozen))
                == frozen);
    }
    {
        auto data = std::get<protocol::relocation_data_t> (
          decoded_controls[1]);
        auto frozen = protocol::decode_frozen_record (frozen_records[1]);
        data.record = frozen;
        const auto encoded = protocol::encode_relocation_control (data);
        const auto decoded = std::get<protocol::relocation_data_t> (
          protocol::decode_relocation_control (encoded));
        assert (decoded.record == data.record);
    }
    for (auto invalid : std::vector<std::vector<std::uint8_t>>{
           [&] {
               auto value = frozen_records[0];
               value[0] = 15;
               return value;
           } (),
           [&] {
               auto value = frozen_records[0];
               value[1] = 5;
               return value;
           } (),
           make_frozen_record (
             1, 1, 0, 1, std::nullopt, frozen_payload (), true),
           make_frozen_record (
             2, 1, 1, 1, std::nullopt, frozen_payload (), true),
           [&] {
               std::vector<std::uint8_t> body;
               put_u32 (body, 101);
               put_u32 (body, 0);
               body.push_back (1);
               const auto payload = frozen_payload ();
               body.insert (body.end (), payload.begin (), payload.end ());
               return make_frozen_record (
                 11, 1, 1, 1, 12, body);
           } (),
           [&] {
               auto value = frozen_records[7];
               /* Metadata is forbidden for infrastructure records. */
               const auto source_length =
                 static_cast<std::size_t> ((value[2] << 8) | value[3]);
               value[4 + source_length] = 1;
               return value;
           } ()}) {
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_frozen_record (invalid));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        auto duplicate_metadata = frozen_records[0];
        const auto source_length = static_cast<std::size_t> (
          (duplicate_metadata[2] << 8) | duplicate_metadata[3]);
        const auto metadata_count = 4 + source_length + 2;
        assert (duplicate_metadata[metadata_count] == 1);
        duplicate_metadata[metadata_count] = 2;
        const auto entry_start = metadata_count + 1;
        const auto key_length = duplicate_metadata[entry_start];
        const auto value_length_offset = entry_start + 1 + key_length;
        const auto value_length = static_cast<std::size_t> (
          (duplicate_metadata[value_length_offset] << 8)
          | duplicate_metadata[value_length_offset + 1]);
        const auto entry_end = value_length_offset + 2 + value_length;
        duplicate_metadata.insert (
          duplicate_metadata.begin ()
            + static_cast<std::ptrdiff_t> (entry_end),
          duplicate_metadata.begin ()
            + static_cast<std::ptrdiff_t> (entry_start),
          duplicate_metadata.begin ()
            + static_cast<std::ptrdiff_t> (entry_end));
        bool rejected = false;
        try {
            static_cast<void> (
              protocol::decode_frozen_record (duplicate_metadata));
        }
        catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    constexpr std::uint64_t probe_id = 0x0102030405060708ULL;
    const auto probe = protocol::encode_liveness (protocol::command::livenessProbe, probe_id);
    const auto decoded_probe = protocol::decode_liveness (probe);
    assert (decoded_probe.kind == protocol::command::livenessProbe);
    assert (decoded_probe.probe_id == probe_id);

    const auto ack = protocol::encode_liveness (protocol::command::livenessAck,
                                                decoded_probe.probe_id);
    const auto decoded_ack = protocol::decode_liveness (ack);
    assert (decoded_ack.kind == protocol::command::livenessAck);
    assert (decoded_ack.probe_id == probe_id);

    for (auto malformed : std::vector<std::vector<std::uint8_t>>{
           std::vector<std::uint8_t> (probe.begin (), probe.end () - 1),
           [&] { auto value = probe; value.push_back (0); return value; } (),
           [&] { auto value = probe; value[0] = 0; return value; } (),
           [&] { auto value = probe; value[4] = 1; return value; } (),
           protocol::encode_liveness (protocol::command::livenessProbe, 1)}) {
        if (malformed.back () == 1 && malformed.size () == probe.size ()) {
            for (std::size_t index = 5; index < malformed.size (); ++index) malformed[index] = 0;
        }
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_liveness (malformed));
        } catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    for (auto malformed_header : std::vector<std::vector<std::uint8_t>>{
           std::vector<std::uint8_t> (node_send.begin (), node_send.end () - 1),
           [&] { auto value = node_send; value[0] = 0; return value; } (),
           [&] { auto value = node_send; value[2] = 2; return value; } ()}) {
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_header (malformed_header));
        } catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }
    {
        // node_request_header has no schema tail: any trailing byte is
        // protocol error.
        auto trailing = protocol::encode_node_request_header (correlation);
        trailing.push_back (0);
        bool rejected = false;
        try {
            static_cast<void> (
              protocol::decode_node_request_header (trailing));
        } catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    // GOLDEN - reply(20).tail is an inline `request-specific-tail`
    // conditional-union with no bodyLengthType (service-wire-v1.schema.json).
    // decode_reply_header cannot know the original operation kind, so it
    // MUST accept trailing tail bytes rather than rejecting them; the
    // operation-specific decoder is responsible for validating the tail's
    // structure. This mirrors Node's decodeReplyHeader (`frame.byteLength <
    // 21` bound, no exact-length check) and .NET's equivalent.
    {
        auto no_tail = protocol::encode_reply_header (correlation, 0, 0);
        const auto decoded_no_tail = protocol::decode_reply_header (no_tail);
        assert (decoded_no_tail.correlation == correlation);
        assert (decoded_no_tail.terminal_result == 0);
        assert (decoded_no_tail.failure_code == 0);

        auto with_tail = protocol::encode_reply_header (correlation, 0, 0);
        // Simulate an inline actorLookup-ok tail (arbitrary extra bytes);
        // decode_reply_header must not throw merely because a tail follows.
        with_tail.insert (
          with_tail.end (), {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
        const auto decoded_with_tail =
          protocol::decode_reply_header (with_tail);
        assert (decoded_with_tail.correlation == correlation);
        assert (decoded_with_tail.terminal_result == 0);
        assert (decoded_with_tail.failure_code == 0);

        // A frame truncated below the fixed 16-byte body is still rejected.
        auto truncated = no_tail;
        truncated.pop_back ();
        bool rejected = false;
        try {
            static_cast<void> (protocol::decode_reply_header (truncated));
        } catch (const protocol::service_wire_error_t &) {
            rejected = true;
        }
        assert (rejected);
    }

    // GOLDEN - service-wire-v1.schema.json: actor-join-reply-tail (reply(20)
    // tail for originalOperationKind actorJoin). Read the shared
    // cross-language fixture directly rather than copying hex vectors —
    // see the golden file's "notes" block for the receiveChunkLimitBytes
    // bound (relocationChunkBytes) and nonzero-u64 conventions.
    {
        std::ifstream fixture (ZLINK_ACTOR_JOIN_REPLY_GOLDEN_PATH);
        assert (fixture.good ());
        const auto vectors = nlohmann::json::parse (fixture);

        for (const auto &vector : vectors.at ("canonical")) {
            const auto bytes =
              from_hex (vector.at ("hex").template get<std::string> ());
            const auto decoded = protocol::decode_actor_join_reply (bytes);
            const auto &expected = vector.at ("decoded");
            assert (decoded.header.correlation
                    == std::stoull (
                      vector.at ("correlation").get<std::string> ()));
            assert (decoded.header.terminal_result == 0);
            assert (decoded.header.failure_code == 0);
            std::optional<protocol::actor_join_reply_spot_ref_t> spot;
            if (expected.contains ("spot")) {
                spot = protocol::actor_join_reply_spot_ref_t{
                  expected.at ("spot").at ("spotId").get<std::string> (),
                  std::stoull (expected.at ("spot")
                                 .at ("generation")
                                 .get<std::string> ())};
            }
            const auto join_result_name =
              expected.at ("joinResult").get<std::string> ();
            if (join_result_name == "accepted") {
                assert (decoded.join_result
                        == protocol::actor_join_result_t::accepted);
                assert (spot.has_value ());
                assert (decoded.spot.has_value ());
                assert (decoded.spot->spot_id == spot->spot_id);
                assert (decoded.spot->object_generation
                        == spot->object_generation);
                assert (decoded.membership_epoch
                        == std::stoull (expected.at ("membershipEpoch")
                                          .get<std::string> ()));
                assert (decoded.receive_chunk_limit_bytes
                        == expected.at ("receiveChunkLimitBytes")
                             .get<std::uint32_t> ());
                const auto reencoded = protocol::encode_actor_join_reply (
                  decoded.header.correlation, 0, 0,
                  protocol::actor_join_result_t::accepted, decoded.spot,
                  decoded.membership_epoch,
                  decoded.receive_chunk_limit_bytes);
                assert (reencoded == bytes);
            } else {
                assert (join_result_name == "rejected");
                assert (decoded.join_result
                        == protocol::actor_join_result_t::rejected);
                assert (decoded.spot.has_value () == spot.has_value ());
                if (spot) {
                    assert (decoded.spot->spot_id == spot->spot_id);
                    assert (decoded.spot->object_generation
                            == spot->object_generation);
                }
                const auto reencoded = protocol::encode_actor_join_reply (
                  decoded.header.correlation, 0, 0,
                  protocol::actor_join_result_t::rejected, decoded.spot, 0,
                  0);
                assert (reencoded == bytes);
            }
        }

        for (const auto &malformed : vectors.at ("malformed")) {
            const auto bytes =
              from_hex (malformed.at ("hex").template get<std::string> ());
            bool rejected = false;
            try {
                static_cast<void> (protocol::decode_actor_join_reply (bytes));
            }
            catch (const protocol::service_wire_error_t &) {
                rejected = true;
            }
            assert (rejected);
        }
    }

    // actorJoin(28) is generated code's sole encode/decode responsibility.
    // The shared fixture pins the generated multipart output to the legacy
    // wire bytes and keeps malformed input rejection typed at this boundary.
    {
        std::ifstream fixture (ZLINK_ACTOR_JOIN_REQUEST_GOLDEN_PATH);
        assert (fixture.good ());
        const auto vectors = nlohmann::json::parse (fixture);
        const auto parse_fence = [] (const nlohmann::json &value) {
            return protocol::service_wire_pilot_fence{
              value.at ("id").get<std::string> (),
              std::stoull (value.at ("generation").get<std::string> ()),
              from_hex (value.at ("targetNodeRidHex").get<std::string> ()),
              std::stoull (
                value.at ("targetNodeGeneration").get<std::string> ()),
              std::stoull (value.at ("expectedAuthorityOwnerGeneration")
                             .get<std::string> ()),
              std::stoull (value.at ("expectedOwnerLeaseGeneration")
                             .get<std::string> ())};
        };
        for (const auto &vector : vectors.at ("valid")) {
            const auto &input = vector.at ("input");
            protocol::service_wire_pilot_actor_join_28 request{
              std::stoull (input.at ("correlation").get<std::string> ()),
              parse_fence (input.at ("actor")), input.at ("entry").get<bool> (),
              parse_fence (input.at ("targetSpot")), std::nullopt};
            if (input.contains ("payload")) {
                const auto &payload = input.at ("payload");
                request.payload =
                  protocol::service_wire_pilot_application_payload_envelope_v1{
                    payload.at ("packetName").get<std::string> (),
                    payload.at ("contentType").get<std::string> (),
                    from_hex (payload.at ("payloadHex").get<std::string> ())};
            }
            std::vector<std::vector<std::uint8_t>> expected;
            for (const auto &frame : vector.at ("framesHex"))
                expected.push_back (from_hex (frame.get<std::string> ()));
            const auto encoded = protocol::encode_actor_join_28 (request);
            assert (encoded == expected);
            const auto decoded = protocol::decode_actor_join_28 (encoded);
            assert (decoded.correlation == request.correlation);
            assert (decoded.actor.id == request.actor.id);
            assert (decoded.actor.generation == request.actor.generation);
            assert (decoded.actor.target_node_rid == request.actor.target_node_rid);
            assert (decoded.actor.target_node_generation
                    == request.actor.target_node_generation);
            assert (decoded.actor.expected_authority_owner_generation
                    == request.actor.expected_authority_owner_generation);
            assert (decoded.actor.expected_owner_lease_generation
                    == request.actor.expected_owner_lease_generation);
            assert (decoded.entry == request.entry);
            assert (decoded.target_spot.id == request.target_spot.id);
            assert (decoded.target_spot.generation == request.target_spot.generation);
            assert (decoded.target_spot.target_node_rid
                    == request.target_spot.target_node_rid);
            assert (decoded.target_spot.target_node_generation
                    == request.target_spot.target_node_generation);
            assert (decoded.target_spot.expected_authority_owner_generation
                    == request.target_spot.expected_authority_owner_generation);
            assert (decoded.target_spot.expected_owner_lease_generation
                    == request.target_spot.expected_owner_lease_generation);
            assert (decoded.payload.has_value () == request.payload.has_value ());
            if (request.payload) {
                assert (decoded.payload->packet_name == request.payload->packet_name);
                assert (decoded.payload->content_type == request.payload->content_type);
                assert (decoded.payload->payload == request.payload->payload);
            }
        }
        for (const auto &malformed : vectors.at ("invalid")) {
            std::vector<std::vector<std::uint8_t>> frames;
            for (const auto &frame : malformed.at ("framesHex"))
                frames.push_back (from_hex (frame.get<std::string> ()));
            bool rejected = false;
            try {
                static_cast<void> (protocol::decode_actor_join_28 (frames));
            }
            catch (const std::invalid_argument &error) {
                rejected = true;
                assert (std::string_view (error.what ()).find (
                          malformed.at ("error").get<std::string> ())
                        != std::string_view::npos);
            }
            assert (rejected);
        }
    }
    return 0;
}
