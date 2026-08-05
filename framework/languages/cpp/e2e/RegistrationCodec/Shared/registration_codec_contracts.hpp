/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace zlink::framework::e2e::registration_codec
{

inline constexpr const char *api_channel = "registration.codec.api";
inline constexpr const char *handler_group = "registration-codec";

struct echo_auto_req_t
{
    static constexpr const char *packet_name = "EchoAutoReq";
    std::string value;
};

struct echo_auto_res_t
{
    std::string value;
};

struct echo_auto_msg_t
{
    static constexpr const char *packet_name = "EchoAutoMsg";
    std::string value;
};

struct echo_attr_req_t
{
    static constexpr const char *packet_name = "EchoAttr";
    std::string value;
};

struct echo_attr_res_t
{
    std::string value;
    std::string packet_name;
    std::string content_type;
};

struct echo_attr_msg_t
{
    static constexpr const char *packet_name = "EchoAttrMsg";
    std::string value;
};

struct echo_manual_req_t
{
    static constexpr const char *packet_name = "EchoManual";
    std::string value;
};

struct echo_manual_res_t
{
    std::string value;
    std::string packet_name;
    std::string content_type;
};

struct echo_manual_msg_t
{
    static constexpr const char *packet_name = "EchoManualMsg";
    std::string value;
};

struct json_roundtrip_req_t
{
    static constexpr const char *packet_name = "JsonRoundtripReq";
    std::string value;
};

struct json_roundtrip_res_t
{
    std::string value;
    std::string content_type;
};

struct json_codec_msg_t
{
    static constexpr const char *packet_name = "JsonCodecMsg";
    std::string value;
};

struct protobuf_roundtrip_req_t
{
    static constexpr const char *packet_name = "ProtobufRoundtripReq";
    std::string value;
};

struct protobuf_roundtrip_res_t
{
    std::string value;
    std::string content_type;
};

struct protobuf_codec_msg_t
{
    static constexpr const char *packet_name = "ProtobufCodecMsg";
    std::string value;
};

struct messagepack_roundtrip_req_t
{
    static constexpr const char *packet_name = "MessagePackRoundtripReq";
    std::string value;
};

struct messagepack_roundtrip_res_t
{
    std::string value;
    std::string content_type;
};

struct messagepack_codec_msg_t
{
    static constexpr const char *packet_name = "MessagePackCodecMsg";
    std::string value;
};

struct custom_roundtrip_req_t
{
    static constexpr const char *packet_name = "CustomRoundtripReq";
    std::string value;
};

struct custom_roundtrip_res_t
{
    std::string value;
};

struct mismatch_roundtrip_req_t
{
    static constexpr const char *packet_name = "MismatchRoundtripReq";
    std::string value;
};

struct mismatch_roundtrip_res_t
{
    std::string value;
};

struct scoped_lifecycle_req_t
{
    static constexpr const char *packet_name = "ScopedLifecycleReq";
    std::string value;
};

struct scoped_lifecycle_res_t
{
    int scoped_id = 0;
    int singleton_id = 0;
    int destroyed_before = 0;
};

struct scoped_lifecycle_stats_req_t
{
    static constexpr const char *packet_name = "ScopedLifecycleStatsReq";
    std::string value;
};

struct scoped_lifecycle_stats_res_t
{
    int destroyed_count = 0;
};

struct filter_order_req_t
{
    static constexpr const char *packet_name = "FilterOrderReq";
    std::string value;
};

struct filter_order_res_t
{
    std::string value;
    std::vector<std::string> order;
};

struct lifecycle_scenario_res_t
{
    scoped_lifecycle_res_t first;
    scoped_lifecycle_res_t second;
    scoped_lifecycle_stats_res_t stats;
};

struct codec_roundtrip_scenario_res_t
{
    json_roundtrip_res_t json;
    protobuf_roundtrip_res_t protobuf;
    messagepack_roundtrip_res_t messagepack;
};

struct codec_coexistence_scenario_res_t
{
    json_roundtrip_res_t json;
    custom_roundtrip_res_t custom;
    protobuf_roundtrip_res_t protobuf;
    messagepack_roundtrip_res_t messagepack;
};

struct operation_status_t
{
    std::string status;
};

struct evidence_entry_t
{
    std::string marker;
    std::string value;
};

struct evidence_snapshot_t
{
    std::vector<evidence_entry_t> entries;
};

inline void to_json (nlohmann::json &json, const echo_auto_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_auto_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_auto_res_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_auto_res_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_auto_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_auto_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_attr_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_attr_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_attr_res_t &value)
{
    json = nlohmann::json{{"value", value.value},
                          {"packet_name", value.packet_name},
                          {"content_type", value.content_type}};
}

inline void from_json (const nlohmann::json &json, echo_attr_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("packet_name").get_to (value.packet_name);
    json.at ("content_type").get_to (value.content_type);
}

inline void to_json (nlohmann::json &json, const echo_attr_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_attr_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_manual_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_manual_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const echo_manual_res_t &value)
{
    json = nlohmann::json{{"value", value.value},
                          {"packet_name", value.packet_name},
                          {"content_type", value.content_type}};
}

inline void from_json (const nlohmann::json &json, echo_manual_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("packet_name").get_to (value.packet_name);
    json.at ("content_type").get_to (value.content_type);
}

inline void to_json (nlohmann::json &json, const echo_manual_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, echo_manual_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const json_roundtrip_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, json_roundtrip_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const json_roundtrip_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"content_type", value.content_type}};
}

inline void from_json (const nlohmann::json &json, json_roundtrip_res_t &value)
{
    json.at ("value").get_to (value.value);
    if (json.contains ("content_type")) {
        json.at ("content_type").get_to (value.content_type);
    }
}

inline void to_json (nlohmann::json &json, const json_codec_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, json_codec_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const protobuf_roundtrip_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, protobuf_roundtrip_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const protobuf_roundtrip_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"content_type", value.content_type}};
}

inline void from_json (const nlohmann::json &json, protobuf_roundtrip_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("content_type").get_to (value.content_type);
}

inline void to_json (nlohmann::json &json, const protobuf_codec_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, protobuf_codec_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const messagepack_roundtrip_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, messagepack_roundtrip_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const messagepack_roundtrip_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"content_type", value.content_type}};
}

inline void from_json (const nlohmann::json &json, messagepack_roundtrip_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("content_type").get_to (value.content_type);
}

inline void to_json (nlohmann::json &json, const messagepack_codec_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, messagepack_codec_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const custom_roundtrip_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, custom_roundtrip_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const custom_roundtrip_res_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, custom_roundtrip_res_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const mismatch_roundtrip_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, mismatch_roundtrip_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const mismatch_roundtrip_res_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, mismatch_roundtrip_res_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const scoped_lifecycle_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, scoped_lifecycle_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const scoped_lifecycle_res_t &value)
{
    json = nlohmann::json{{"scoped_id", value.scoped_id},
                          {"singleton_id", value.singleton_id},
                          {"destroyed_before", value.destroyed_before}};
}

inline void from_json (const nlohmann::json &json, scoped_lifecycle_res_t &value)
{
    json.at ("scoped_id").get_to (value.scoped_id);
    json.at ("singleton_id").get_to (value.singleton_id);
    json.at ("destroyed_before").get_to (value.destroyed_before);
}

inline void to_json (nlohmann::json &json, const scoped_lifecycle_stats_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, scoped_lifecycle_stats_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const scoped_lifecycle_stats_res_t &value)
{
    json = nlohmann::json{{"destroyed_count", value.destroyed_count}};
}

inline void from_json (const nlohmann::json &json, scoped_lifecycle_stats_res_t &value)
{
    json.at ("destroyed_count").get_to (value.destroyed_count);
}

inline void to_json (nlohmann::json &json, const filter_order_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, filter_order_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const filter_order_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"order", value.order}};
}

inline void from_json (const nlohmann::json &json, filter_order_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("order").get_to (value.order);
}

inline void to_json (nlohmann::json &json, const lifecycle_scenario_res_t &value)
{
    json = nlohmann::json{{"first", value.first}, {"second", value.second}, {"stats", value.stats}};
}

inline void from_json (const nlohmann::json &json, lifecycle_scenario_res_t &value)
{
    json.at ("first").get_to (value.first);
    json.at ("second").get_to (value.second);
    json.at ("stats").get_to (value.stats);
}

inline void to_json (nlohmann::json &json, const codec_roundtrip_scenario_res_t &value)
{
    json = nlohmann::json{{"json", value.json},
                          {"protobuf", value.protobuf},
                          {"messagepack", value.messagepack}};
}

inline void from_json (const nlohmann::json &json, codec_roundtrip_scenario_res_t &value)
{
    json.at ("json").get_to (value.json);
    json.at ("protobuf").get_to (value.protobuf);
    json.at ("messagepack").get_to (value.messagepack);
}

inline void to_json (nlohmann::json &json, const codec_coexistence_scenario_res_t &value)
{
    json = nlohmann::json{{"json", value.json},
                          {"custom", value.custom},
                          {"protobuf", value.protobuf},
                          {"messagepack", value.messagepack}};
}

inline void from_json (const nlohmann::json &json, codec_coexistence_scenario_res_t &value)
{
    json.at ("json").get_to (value.json);
    json.at ("custom").get_to (value.custom);
    json.at ("protobuf").get_to (value.protobuf);
    json.at ("messagepack").get_to (value.messagepack);
}

inline void to_json (nlohmann::json &json, const operation_status_t &value)
{
    json = nlohmann::json{{"status", value.status}};
}

inline void from_json (const nlohmann::json &json, operation_status_t &value)
{
    json.at ("status").get_to (value.status);
}

inline void to_json (nlohmann::json &json, const evidence_entry_t &value)
{
    json = nlohmann::json{{"marker", value.marker}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, evidence_entry_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const evidence_snapshot_t &value)
{
    json = nlohmann::json{{"entries", value.entries}};
}

inline void from_json (const nlohmann::json &json, evidence_snapshot_t &value)
{
    json.at ("entries").get_to (value.entries);
}

} // namespace zlink::framework::e2e::registration_codec
