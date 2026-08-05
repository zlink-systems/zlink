/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace zlink::framework::e2e::pubsub
{

inline constexpr const char *event_channel = "pubsub.events";
inline constexpr const char *handler_group = "pubsub";
inline constexpr const char *topic_fanout = "fanout";
inline constexpr const char *topic_alpha = "alpha";
inline constexpr const char *topic_beta = "beta";

/* spec §3.1: 정식 DTO는 wire 이름을 스스로 선언한다(typeid 폴백 금지). */
struct event_msg_t
{
    static constexpr const char *packet_name = "EventMsg";
    std::string value;
};

/* handler가 등록되지 않은 packet 이름으로 발행해 dispatch 오류(handlerMissing)를
 * 만들어 내는 negative wire 변형. */
struct missing_event_msg_t
{
    static constexpr const char *packet_name = "MissingEventMsg";
    std::string value;
};

struct evidence_event_t
{
    std::string subscriber_id;
    std::string topic;
    std::string value;
};

struct dispatch_error_evidence_t
{
    std::string message_kind;
    std::string reason;
    std::string action;
    std::string packet_name;
    std::string topic;
    std::string error_message;
};

struct evidence_snapshot_t
{
    std::string subscriber_id;
    std::vector<evidence_event_t> events;
    std::vector<evidence_event_t> ignored_events;
    std::vector<dispatch_error_evidence_t> errors;
};

struct evidence_wait_req_t
{
    std::vector<std::string> contains_all;
    std::vector<std::vector<std::string>> contains_any_groups;
    std::vector<std::vector<std::string>> contains_all_line_groups;
    std::vector<std::vector<std::string>> contains_any_line_groups;
    int timeout_milliseconds = 10000;
};

inline void to_json (nlohmann::json &json, const event_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, event_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const missing_event_msg_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, missing_event_msg_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const evidence_event_t &value)
{
    json = nlohmann::json{
      {"subscriber_id", value.subscriber_id}, {"topic", value.topic}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, evidence_event_t &value)
{
    json.at ("subscriber_id").get_to (value.subscriber_id);
    json.at ("topic").get_to (value.topic);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const dispatch_error_evidence_t &value)
{
    json = nlohmann::json{{"message_kind", value.message_kind},
                          {"reason", value.reason},
                          {"action", value.action},
                          {"packet_name", value.packet_name},
                          {"topic", value.topic},
                          {"error_message", value.error_message}};
}

inline void from_json (const nlohmann::json &json, dispatch_error_evidence_t &value)
{
    json.at ("message_kind").get_to (value.message_kind);
    json.at ("reason").get_to (value.reason);
    json.at ("action").get_to (value.action);
    json.at ("packet_name").get_to (value.packet_name);
    json.at ("topic").get_to (value.topic);
    if (json.contains ("error_message")) {
        json.at ("error_message").get_to (value.error_message);
    }
}

inline void to_json (nlohmann::json &json, const evidence_snapshot_t &value)
{
    json = nlohmann::json{{"subscriber_id", value.subscriber_id},
                          {"events", value.events},
                          {"ignored_events", value.ignored_events},
                          {"errors", value.errors}};
}

inline void from_json (const nlohmann::json &json, evidence_snapshot_t &value)
{
    json.at ("subscriber_id").get_to (value.subscriber_id);
    json.at ("events").get_to (value.events);
    if (json.contains ("ignored_events")) {
        json.at ("ignored_events").get_to (value.ignored_events);
    }
    json.at ("errors").get_to (value.errors);
}

inline void to_json (nlohmann::json &json, const evidence_wait_req_t &value)
{
    json = nlohmann::json{{"contains_all", value.contains_all},
                          {"contains_any_groups", value.contains_any_groups},
                          {"contains_all_line_groups", value.contains_all_line_groups},
                          {"contains_any_line_groups", value.contains_any_line_groups},
                          {"timeout_milliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, evidence_wait_req_t &value)
{
    if (json.contains ("contains_all")) {
        json.at ("contains_all").get_to (value.contains_all);
    }
    if (json.contains ("contains_any_groups")) {
        json.at ("contains_any_groups").get_to (value.contains_any_groups);
    }
    if (json.contains ("contains_all_line_groups")) {
        json.at ("contains_all_line_groups").get_to (value.contains_all_line_groups);
    }
    if (json.contains ("contains_any_line_groups")) {
        json.at ("contains_any_line_groups").get_to (value.contains_any_line_groups);
    }
    if (json.contains ("timeout_milliseconds")) {
        json.at ("timeout_milliseconds").get_to (value.timeout_milliseconds);
    }
}

} // namespace zlink::framework::e2e::pubsub
