/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Track F (SF-F2, SF-F3, SF-F7, SF-F11) wire contracts for a minimal
 * relocation-capable node pair added to the DiscoveryRegistryHa (Config 6)
 * harness. Tracks A-E test discovery/messaging only and never place a
 * stateful Actor, so this is a new, separate participant role
 * (Server/Relocation) rather than an extension of Server/Provider -- it does
 * not touch any Track A-E code or config.
 *
 * These scenarios exercise direct-transfer relocation only. Base/delta
 * capture was removed from the product; nothing here references it. */

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace zlink::e2e::store_failure_relocation
{

inline constexpr const char *mesh_name = "store-failure-relocation";
inline constexpr const char *entry_spot_id = "store-failure-relocation-entry";

/* Actor types. A single generic actor class is reused; behavior differs by
 * the relocation adapter selected through the type name at creation, the
 * same convention SpotActorTransfer uses for its transfer-fail-* types. */
inline constexpr const char *actor_type_stateful = "df-stateful";
/* Source-side explicit failure: capture() throws before any bytes leave the
 * source, i.e. before the relay-ready reply (SF-F11 common variant). */
inline constexpr const char *actor_type_fail_transfer_out = "df-fail-transfer-out";
/* Target-side explicit failure: restore() throws before target admission
 * commits, i.e. before the relay-ready reply is honored (SF-F2 failed-call
 * variant). */
inline constexpr const char *actor_type_fail_transfer_in = "df-fail-transfer-in";
/* Source-side capture is held on a named gate until the client releases it,
 * so a relocation can be kept running past several owner-lease renew
 * intervals (SF-F2 long-running variant). */
inline constexpr const char *actor_type_hold_capture = "df-hold-capture";

struct actor_create_req_t
{
    std::string actor_id;
    std::string actor_type = actor_type_stateful;
    /* Deterministic payload length in bytes. The node fills payload bytes
     * from a fixed formula (position-based), so two nodes independently
     * agree on content without transmitting it. */
    std::uint64_t payload_length = 16;
};

struct actor_create_res_t
{
    std::string actor_id;
    std::string actor_type;
    std::string node_rid;
    std::int64_t generation = 0;
};

struct create_spot_req_t
{
    std::string spot_id;
};

struct create_spot_res_t
{
    std::string spot_id;
    std::string node_rid;
};

struct relocate_req_t
{
    static constexpr const char *packet_name = "RelocateReq";
    std::string scenario;
    std::string target_spot_id;
};

struct relocate_res_t
{
    std::string scenario;
    std::string actor_id;
    bool accepted = false;
    std::string source_node_rid;
    std::string target_spot_id;
};

struct probe_state_req_t
{
    static constexpr const char *packet_name = "ProbeStateReq";
    std::string scenario;
    std::string marker;
};

struct probe_state_res_t
{
    std::string actor_id;
    std::string node_rid;
    std::string marker;
    std::uint64_t payload_length = 0;
    /* CRC32C over the payload bytes, same convention the framework uses for
     * relocation manifest checksums (protocol::relocation_checksum_crc32c),
     * so the fixture's own verification uses the same algorithm family the
     * production chunk assembly checks against. */
    std::uint32_t payload_checksum = 0;
};

struct gate_release_res_t
{
    std::string key;
    bool released = false;
};

struct relocation_evidence_t
{
    std::string scenario;
    std::string actor_id;
    std::string kind;
    std::string value;
    std::string node_rid;
};

inline void to_json (nlohmann::json &json, const actor_create_req_t &value)
{
    json = {{"actorId", value.actor_id},
            {"actorType", value.actor_type},
            {"payloadLength", value.payload_length}};
}

inline void from_json (const nlohmann::json &json, actor_create_req_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.actor_type = json.value ("actorType", std::string (actor_type_stateful));
    value.payload_length = json.value ("payloadLength", std::uint64_t{16});
}

inline void to_json (nlohmann::json &json, const actor_create_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"actorType", value.actor_type},
            {"nodeRid", value.node_rid},
            {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_create_res_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.actor_type = json.value ("actorType", "");
    value.node_rid = json.value ("nodeRid", "");
    value.generation = json.value ("generation", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const create_spot_req_t &value)
{
    json = {{"spotId", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, create_spot_req_t &value)
{
    value.spot_id = json.value ("spotId", "");
}

inline void to_json (nlohmann::json &json, const create_spot_res_t &value)
{
    json = {{"spotId", value.spot_id}, {"nodeRid", value.node_rid}};
}

inline void from_json (const nlohmann::json &json, create_spot_res_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.node_rid = json.value ("nodeRid", "");
}

inline void to_json (nlohmann::json &json, const relocate_req_t &value)
{
    json = {{"scenario", value.scenario}, {"targetSpotId", value.target_spot_id}};
}

inline void from_json (const nlohmann::json &json, relocate_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.target_spot_id = json.value ("targetSpotId", "");
}

inline void to_json (nlohmann::json &json, const relocate_res_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"accepted", value.accepted},
            {"sourceNodeRid", value.source_node_rid},
            {"targetSpotId", value.target_spot_id}};
}

inline void from_json (const nlohmann::json &json, relocate_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.accepted = json.value ("accepted", false);
    value.source_node_rid = json.value ("sourceNodeRid", "");
    value.target_spot_id = json.value ("targetSpotId", "");
}

inline void to_json (nlohmann::json &json, const probe_state_req_t &value)
{
    json = {{"scenario", value.scenario}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, probe_state_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const probe_state_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"nodeRid", value.node_rid},
            {"marker", value.marker},
            {"payloadLength", value.payload_length},
            {"payloadChecksum", value.payload_checksum}};
}

inline void from_json (const nlohmann::json &json, probe_state_res_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.marker = json.value ("marker", "");
    value.payload_length = json.value ("payloadLength", std::uint64_t{0});
    value.payload_checksum = json.value ("payloadChecksum", std::uint32_t{0});
}

inline void to_json (nlohmann::json &json, const gate_release_res_t &value)
{
    json = {{"key", value.key}, {"released", value.released}};
}

inline void from_json (const nlohmann::json &json, gate_release_res_t &value)
{
    value.key = json.value ("key", "");
    value.released = json.value ("released", false);
}

inline void to_json (nlohmann::json &json, const relocation_evidence_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"kind", value.kind},
            {"value", value.value},
            {"nodeRid", value.node_rid}};
}

inline void from_json (const nlohmann::json &json, relocation_evidence_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.kind = json.value ("kind", "");
    value.value = json.value ("value", "");
    value.node_rid = json.value ("nodeRid", "");
}

inline std::string evidence_text (const relocation_evidence_t &evidence)
{
    return evidence.scenario + "|" + evidence.actor_id + "|" + evidence.kind + "|"
           + evidence.value + "|" + evidence.node_rid;
}

/* CRC32C, the same polynomial the framework's relocation chunk checksum
 * uses (protocol::relocation_checksum_crc32c), computed independently here
 * so the e2e fixture does not need a framework-internal include. */
inline std::uint32_t crc32c (const std::vector<std::uint8_t> &bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Deterministic fill so source and target/probe agree on expected content
 * without transmitting the payload itself. */
inline std::vector<std::uint8_t> deterministic_payload (std::uint64_t length)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve (static_cast<std::size_t> (length));
    for (std::uint64_t index = 0; index < length; ++index) {
        bytes.push_back (static_cast<std::uint8_t> ((index * 31u + 7u) & 0xFFu));
    }
    return bytes;
}

template <typename T> zlink::message_t to_stream_payload (const T &value)
{
    return zlink::message_t::from_json (value);
}

template <typename T> void from_stream_payload (const zlink::message_t &payload, T &value)
{
    value = payload.parse_json<T> ();
}

} // namespace zlink::e2e::store_failure_relocation
