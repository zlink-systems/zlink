/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <charconv>
#include <system_error>
#include <optional>
#include <span>
#include <stdexcept>
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

struct json_golden_req_t
{
    static constexpr const char *packet_name = "JsonGolden";
    std::string display_name;
    std::string status;
    std::int64_t balance = 0;
    std::vector<std::uint8_t> payload;
    std::int32_t score = 0;
    double ratio = 0;
    std::optional<std::string> optional_note;
};

struct json_golden_res_t
{
    std::string display_name;
    std::string status;
    std::int64_t balance = 0;
    std::vector<std::uint8_t> payload;
    std::int32_t score = 0;
    double ratio = 0;
    std::optional<std::string> optional_note;
    std::string content_type;
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

inline std::string encode_base64 (std::span<const std::uint8_t> bytes)
{
    constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve ((bytes.size () + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size (); index += 3) {
        const auto remaining = bytes.size () - index;
        const auto first = bytes[index];
        const auto second = remaining > 1 ? bytes[index + 1] : 0;
        const auto third = remaining > 2 ? bytes[index + 2] : 0;
        result.push_back (alphabet[first >> 2]);
        result.push_back (alphabet[((first & 0x03u) << 4) | (second >> 4)]);
        result.push_back (remaining > 1 ? alphabet[((second & 0x0fu) << 2) | (third >> 6)] : '=');
        result.push_back (remaining > 2 ? alphabet[third & 0x3fu] : '=');
    }
    return result;
}

inline std::uint8_t decode_base64_digit (char value)
{
    if (value >= 'A' && value <= 'Z')
        return static_cast<std::uint8_t> (value - 'A');
    if (value >= 'a' && value <= 'z')
        return static_cast<std::uint8_t> (value - 'a' + 26);
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t> (value - '0' + 52);
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    throw std::invalid_argument ("invalid base64 digit");
}

inline std::vector<std::uint8_t> decode_base64 (const std::string &encoded)
{
    if (encoded.size () % 4 != 0)
        throw std::invalid_argument ("base64 value must be padded");
    std::vector<std::uint8_t> result;
    result.reserve (encoded.size () / 4 * 3);
    for (std::size_t index = 0; index < encoded.size (); index += 4) {
        const auto first = decode_base64_digit (encoded[index]);
        const auto second = decode_base64_digit (encoded[index + 1]);
        const auto third_padded = encoded[index + 2] == '=';
        const auto fourth_padded = encoded[index + 3] == '=';
        if (fourth_padded && !third_padded && index + 4 != encoded.size ())
            throw std::invalid_argument ("invalid base64 padding");
        const auto third = third_padded ? 0 : decode_base64_digit (encoded[index + 2]);
        const auto fourth = fourth_padded ? 0 : decode_base64_digit (encoded[index + 3]);
        if (third_padded && !fourth_padded)
            throw std::invalid_argument ("invalid base64 padding");
        if (third_padded && (second & 0x0fu) != 0)
            throw std::invalid_argument ("non-zero base64 padding bits");
        if (fourth_padded && !third_padded && (third & 0x03u) != 0)
            throw std::invalid_argument ("non-zero base64 padding bits");
        result.push_back (static_cast<std::uint8_t> ((first << 2) | (second >> 4)));
        if (!third_padded)
            result.push_back (static_cast<std::uint8_t> ((second << 4) | (third >> 2)));
        if (!fourth_padded)
            result.push_back (static_cast<std::uint8_t> ((third << 6) | fourth));
    }
    return result;
}

inline std::int64_t parse_json_int64 (const nlohmann::json &value)
{
    if (value.is_number_integer ())
        return value.get<std::int64_t> ();
    if (!value.is_string ())
        throw std::invalid_argument ("JSON int64 value must be a decimal string or integer");
    const auto text = value.get<std::string> ();
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars (text.data (), text.data () + text.size (), result);
    if (error != std::errc{} || end != text.data () + text.size ())
        throw std::invalid_argument ("JSON int64 value is not a decimal integer");
    return result;
}

inline void from_json (const nlohmann::json &json, json_golden_req_t &value)
{
    json.at ("displayName").get_to (value.display_name);
    json.at ("status").get_to (value.status);
    value.balance = parse_json_int64 (json.at ("balance"));
    value.payload = decode_base64 (json.at ("payload").get<std::string> ());
    json.at ("score").get_to (value.score);
    json.at ("ratio").get_to (value.ratio);
    if (!json.contains ("optionalNote") || json.at ("optionalNote").is_null ())
        value.optional_note.reset ();
    else
        value.optional_note = json.at ("optionalNote").get<std::string> ();
}

inline void to_json (nlohmann::json &json, const json_golden_req_t &value)
{
    json = nlohmann::json{{"displayName", value.display_name},
                          {"status", value.status},
                          {"balance", std::to_string (value.balance)},
                          {"payload", encode_base64 (value.payload)},
                          {"score", value.score},
                          {"ratio", value.ratio},
                          {"optionalNote", value.optional_note
                                               ? nlohmann::json (*value.optional_note)
                                               : nlohmann::json (nullptr)}};
}

inline void from_json (const nlohmann::json &json, json_golden_res_t &value)
{
    json.at ("displayName").get_to (value.display_name);
    json.at ("status").get_to (value.status);
    value.balance = parse_json_int64 (json.at ("balance"));
    value.payload = decode_base64 (json.at ("payload").get<std::string> ());
    json.at ("score").get_to (value.score);
    json.at ("ratio").get_to (value.ratio);
    if (!json.contains ("optionalNote") || json.at ("optionalNote").is_null ())
        value.optional_note.reset ();
    else
        value.optional_note = json.at ("optionalNote").get<std::string> ();
    json.at ("contentType").get_to (value.content_type);
}

inline void to_json (nlohmann::json &json, const json_golden_res_t &value)
{
    json = nlohmann::json{{"displayName", value.display_name},
                          {"status", value.status},
                          {"balance", std::to_string (value.balance)},
                          {"payload", encode_base64 (value.payload)},
                          {"score", value.score},
                          {"ratio", value.ratio},
                          {"optionalNote", value.optional_note
                                               ? nlohmann::json (*value.optional_note)
                                               : nlohmann::json (nullptr)},
                          {"contentType", value.content_type}};
}

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
