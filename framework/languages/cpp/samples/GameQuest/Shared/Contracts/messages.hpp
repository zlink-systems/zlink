/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace zlink::samples::gamequest
{

struct quest_ids_t
{
    static constexpr const char *first_hunt = "first-hunt";
    static constexpr const char *herb_gathering = "herb-gathering";
    static constexpr const char *visit_ruins = "visit-ruins";
};

/* 공통 sample spec §11.4: 진행 중(Active) → 조건 충족(Completed) → 보상 지급(RewardGranted). */
struct quest_status_t
{
    static constexpr const char *active = "Active";
    static constexpr const char *completed = "Completed";
    static constexpr const char *reward_granted = "RewardGranted";
};

struct kill_monster_req_t
{
    static constexpr const char *packet_name = "KillMonsterReq";
    std::string player_id;
    std::string monster_id;
    std::string area_id;
    std::string idempotency_key;
};

struct kill_monster_res_t
{
    static constexpr const char *packet_name = "KillMonsterRes";
    std::string event_id;
};

struct collect_item_req_t
{
    static constexpr const char *packet_name = "CollectItemReq";
    std::string player_id;
    std::string item_id;
    int count = 0;
    std::string idempotency_key;
};

struct enter_area_req_t
{
    static constexpr const char *packet_name = "EnterAreaReq";
    std::string player_id;
    std::string area_id;
    std::string idempotency_key;
};

struct join_session_req_t
{
    static constexpr const char *packet_name = "JoinSessionReq";
    std::string player_id;
};

/* 공통 sample spec §11.3: quest domain event stream(append-only SoR). projection은 이 stream을
 * fold해 만든다. */
struct stored_quest_event_t
{
    static constexpr const char *progressed = "QuestProgressed";
    static constexpr const char *completed = "QuestCompleted";
    static constexpr const char *reward_granted = "QuestRewardGranted";
    static constexpr const char *reconciled = "QuestReconciled";

    std::string event_id;
    std::string player_id;
    std::string quest_id;
    std::string type;
    nlohmann::json payload = nlohmann::json::object ();
    std::optional<std::string> source_event_id;
    long long version = 0;
    long long created_at_unix_ms = 0;
};

struct quest_progress_t
{
    std::string player_id;
    std::string quest_id;
    std::string status;
    int current_count = 0;
    int required_count = 0;
    /* 이 진행을 만든 마지막 gameplay event(idempotency 판정용)와 event stream fold의 버전. */
    std::optional<std::string> last_source_event_id;
    long long version = 0;
    long long updated_at_unix_ms = 0;
};

struct join_session_res_t
{
    static constexpr const char *packet_name = "JoinSessionRes";
    std::string player_id;
    std::vector<quest_progress_t> active_quests;
};

struct get_quest_progress_req_t
{
    static constexpr const char *packet_name = "GetQuestProgressReq";
    std::string player_id;
};

struct get_quest_progress_res_t
{
    static constexpr const char *packet_name = "GetQuestProgressRes";
    std::vector<quest_progress_t> active_quests;
};

struct sync_quest_progress_req_t
{
    static constexpr const char *packet_name = "SyncQuestProgressReq";
    std::string player_id;
};

/* Internal server-to-server message. This is not part of the common client contract. GameApi
 * sends the authoritative snapshot to the owner Spot without adding server-only fields to the
 * public SyncQuestProgressReq. */
struct sync_quest_progress_owner_req_t
{
    static constexpr const char *packet_name = "SyncQuestProgressOwnerReq";
    std::string player_id;
    int snapshot_kill_count = 0;
};

struct sync_quest_progress_res_t
{
    static constexpr const char *packet_name = "SyncQuestProgressRes";
    std::vector<quest_progress_t> updated_quests;
};

struct quest_progress_notify_t
{
    static constexpr const char *packet_name = "QuestProgressNotify";
    std::string player_id;
    quest_progress_t progress;
};

struct quest_completed_notify_t
{
    static constexpr const char *packet_name = "QuestCompletedNotify";
    std::string player_id;
    quest_progress_t progress;
    bool reward_granted = false;
};

/* 공통 sample spec §11.2: entry-spot → owner spot 게임플레이 event는 응답 없는 one-way send다. */
struct gameplay_msg_t
{
    static constexpr const char *packet_name = "GameplayMsg";
    std::string event_id;
    std::string player_id;
    std::string type;
    nlohmann::json payload = nlohmann::json::object ();
    long long occurred_at_unix_ms = 0;
};

inline nlohmann::json gameplay_payload (const std::string &value, int count)
{
    return nlohmann::json{{"value", value}, {"count", count}};
}

inline nlohmann::json gameplay_payload (const gameplay_msg_t &message)
{
    return message.payload;
}

struct close_player_quest_msg_t
{
    static constexpr const char *packet_name = "ClosePlayerQuestMsg";
    std::optional<std::string> reason;
};

/* Internal server-to-server message. The owner Spot sends the projection to GameApi, and
 * GameApi relays the common QuestProgressNotify to the bound client session. The framework
 * session binding selects the destination; this message is not a client API. */
struct notify_quest_progress_msg_t
{
    static constexpr const char *packet_name = "NotifyQuestProgressMsg";
    std::string player_id;
    std::vector<quest_progress_t> projection;
    std::string completed_quest_id;
};

/* Test/evidence-only maintenance request. These messages exercise projection deletion,
 * rehydration and explicit owner deactivation from the sample self-check; they are not part of
 * the common GameQuest client contract. */
struct projection_admin_req_t
{
    static constexpr const char *packet_name = "GameQuestProjectionAdminReq";
    std::string player_id;
    std::string quest_id;
    std::string operation;
};

struct projection_admin_res_t
{
    static constexpr const char *packet_name = "GameQuestProjectionAdminRes";
    bool ok = false;
    std::vector<quest_progress_t> projection;
};

struct unpublished_kill_req_t
{
    static constexpr const char *packet_name = "GameQuestUnpublishedKillReq";
    std::string player_id;
    int count = 0;
};

struct unpublished_kill_res_t
{
    static constexpr const char *packet_name = "GameQuestUnpublishedKillRes";
    bool ok = false;
};

/* Test/evidence-only HTTP assertion request. It exposes runner evidence, not framework routing
 * identity or a supported application API. */
struct server_assertion_req_t
{
    static constexpr const char *packet_name = "GameQuestServerAssertReq";
};

struct server_assertion_res_t
{
    static constexpr const char *packet_name = "GameQuestServerAssertRes";
    bool passed = false;
    std::vector<std::string> evidence;
};

inline void to_json (nlohmann::json &json, const kill_monster_req_t &value)
{
    json = {{"playerId", value.player_id}, {"monsterId", value.monster_id},
            {"areaId", value.area_id}, {"idempotencyKey", value.idempotency_key}};
}
inline void from_json (const nlohmann::json &json, kill_monster_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("monsterId").get_to (value.monster_id);
    json.at ("areaId").get_to (value.area_id);
    json.at ("idempotencyKey").get_to (value.idempotency_key);
}
inline void to_json (nlohmann::json &json, const kill_monster_res_t &value)
{
    json = {{"eventId", value.event_id}};
}
inline void from_json (const nlohmann::json &json, kill_monster_res_t &value)
{
    json.at ("eventId").get_to (value.event_id);
}
inline void to_json (nlohmann::json &json, const collect_item_req_t &value)
{
    json = {{"playerId", value.player_id}, {"itemId", value.item_id},
            {"count", value.count}, {"idempotencyKey", value.idempotency_key}};
}
inline void from_json (const nlohmann::json &json, collect_item_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("itemId").get_to (value.item_id);
    json.at ("count").get_to (value.count);
    json.at ("idempotencyKey").get_to (value.idempotency_key);
}
inline void to_json (nlohmann::json &json, const enter_area_req_t &value)
{
    json = {{"playerId", value.player_id}, {"areaId", value.area_id},
            {"idempotencyKey", value.idempotency_key}};
}
inline void from_json (const nlohmann::json &json, enter_area_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("areaId").get_to (value.area_id);
    json.at ("idempotencyKey").get_to (value.idempotency_key);
}
inline void to_json (nlohmann::json &json, const join_session_req_t &value)
{
    json = {{"playerId", value.player_id}};
}
inline void from_json (const nlohmann::json &json, join_session_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
}
inline void to_json (nlohmann::json &json, const stored_quest_event_t &value)
{
    json = {{"eventId", value.event_id},
            {"playerId", value.player_id},
            {"questId", value.quest_id},
            {"type", value.type},
            {"payload", value.payload},
            {"sourceEventId", value.source_event_id ? nlohmann::json (*value.source_event_id)
                                                       : nlohmann::json (nullptr)},
            {"version", value.version},
            {"createdAtUnixMs", value.created_at_unix_ms}};
}
inline void from_json (const nlohmann::json &json, stored_quest_event_t &value)
{
    json.at ("eventId").get_to (value.event_id);
    json.at ("playerId").get_to (value.player_id);
    json.at ("questId").get_to (value.quest_id);
    json.at ("type").get_to (value.type);
    value.payload = json.value ("payload", nlohmann::json::object ());
    if (!json.contains ("sourceEventId") || json.at ("sourceEventId").is_null ()) {
        value.source_event_id = std::nullopt;
    } else {
        value.source_event_id = json.at ("sourceEventId").get<std::string> ();
    }
    value.version = json.value ("version", 0LL);
    value.created_at_unix_ms = json.value ("createdAtUnixMs", 0LL);
}
inline void to_json (nlohmann::json &json, const quest_progress_t &value)
{
    json = {{"playerId", value.player_id}, {"questId", value.quest_id},
            {"status", value.status}, {"currentCount", value.current_count},
            {"requiredCount", value.required_count},
            {"lastSourceEventId", value.last_source_event_id
                                      ? nlohmann::json (*value.last_source_event_id)
                                      : nlohmann::json (nullptr)},
            {"version", value.version},
            {"updatedAtUnixMs", value.updated_at_unix_ms}};
}
inline void from_json (const nlohmann::json &json, quest_progress_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("questId").get_to (value.quest_id);
    json.at ("status").get_to (value.status);
    json.at ("currentCount").get_to (value.current_count);
    json.at ("requiredCount").get_to (value.required_count);
    if (!json.contains ("lastSourceEventId") || json.at ("lastSourceEventId").is_null ()) {
        value.last_source_event_id = std::nullopt;
    } else {
        value.last_source_event_id = json.at ("lastSourceEventId").get<std::string> ();
    }
    value.version = json.value ("version", 0LL);
    json.at ("updatedAtUnixMs").get_to (value.updated_at_unix_ms);
}
inline void to_json (nlohmann::json &json, const join_session_res_t &value)
{
    json = {{"playerId", value.player_id}, {"activeQuests", value.active_quests}};
}
inline void from_json (const nlohmann::json &json, join_session_res_t &value)
{
    value.player_id = json.value ("playerId", std::string{});
    json.at ("activeQuests").get_to (value.active_quests);
}
inline void to_json (nlohmann::json &json, const get_quest_progress_req_t &value)
{
    json = {{"playerId", value.player_id}};
}
inline void from_json (const nlohmann::json &json, get_quest_progress_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
}
inline void to_json (nlohmann::json &json, const get_quest_progress_res_t &value)
{
    json = {{"activeQuests", value.active_quests}};
}
inline void from_json (const nlohmann::json &json, get_quest_progress_res_t &value)
{
    json.at ("activeQuests").get_to (value.active_quests);
}
inline void to_json (nlohmann::json &json, const sync_quest_progress_req_t &value)
{
    json = {{"playerId", value.player_id}};
}
inline void from_json (const nlohmann::json &json, sync_quest_progress_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
}

inline void to_json (nlohmann::json &json, const sync_quest_progress_owner_req_t &value)
{
    json = {{"playerId", value.player_id}, {"snapshotKillCount", value.snapshot_kill_count}};
}

inline void from_json (const nlohmann::json &json, sync_quest_progress_owner_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    value.snapshot_kill_count = json.value ("snapshotKillCount", 0);
}
inline void to_json (nlohmann::json &json, const sync_quest_progress_res_t &value)
{
    json = {{"updatedQuests", value.updated_quests}};
}
inline void from_json (const nlohmann::json &json, sync_quest_progress_res_t &value)
{
    json.at ("updatedQuests").get_to (value.updated_quests);
}
inline void to_json (nlohmann::json &json, const quest_progress_notify_t &value)
{
    json = {{"playerId", value.player_id}, {"progress", value.progress}};
}
inline void from_json (const nlohmann::json &json, quest_progress_notify_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("progress").get_to (value.progress);
}
inline void to_json (nlohmann::json &json, const quest_completed_notify_t &value)
{
    json = {{"playerId", value.player_id},
            {"progress", value.progress},
            {"rewardGranted", value.reward_granted}};
}
inline void from_json (const nlohmann::json &json, quest_completed_notify_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("progress").get_to (value.progress);
    json.at ("rewardGranted").get_to (value.reward_granted);
}
inline void to_json (nlohmann::json &json, const gameplay_msg_t &value)
{
    json = {{"eventId", value.event_id},
            {"playerId", value.player_id},
            {"type", value.type},
            {"payload", value.payload},
            {"occurredAtUnixMs", value.occurred_at_unix_ms}};
}
inline void from_json (const nlohmann::json &json, gameplay_msg_t &value)
{
    json.at ("eventId").get_to (value.event_id);
    json.at ("playerId").get_to (value.player_id);
    json.at ("type").get_to (value.type);
    json.at ("payload").get_to (value.payload);
    json.at ("occurredAtUnixMs").get_to (value.occurred_at_unix_ms);
}

inline void to_json (nlohmann::json &json, const close_player_quest_msg_t &value)
{
    json = nlohmann::json::object ();
    if (value.reason) {
        json["reason"] = *value.reason;
    }
}

inline void from_json (const nlohmann::json &json, close_player_quest_msg_t &value)
{
    if (!json.contains ("reason") || json.at ("reason").is_null ()) {
        value.reason = std::nullopt;
    } else {
        value.reason = json.at ("reason").get<std::string> ();
    }
}
inline void to_json (nlohmann::json &json, const notify_quest_progress_msg_t &value)
{
    json = {{"playerId", value.player_id},
            {"projection", value.projection},
            {"completedQuestId", value.completed_quest_id}};
}
inline void from_json (const nlohmann::json &json, notify_quest_progress_msg_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("projection").get_to (value.projection);
    value.completed_quest_id = json.value ("completedQuestId", std::string{});
}
inline void to_json (nlohmann::json &json, const projection_admin_req_t &value)
{
    json = {{"playerId", value.player_id},
            {"questId", value.quest_id},
            {"operation", value.operation}};
}
inline void from_json (const nlohmann::json &json, projection_admin_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("questId").get_to (value.quest_id);
    json.at ("operation").get_to (value.operation);
}
inline void to_json (nlohmann::json &json, const projection_admin_res_t &value)
{
    json = {{"ok", value.ok}, {"projection", value.projection}};
}
inline void from_json (const nlohmann::json &json, projection_admin_res_t &value)
{
    json.at ("ok").get_to (value.ok);
    json.at ("projection").get_to (value.projection);
}
inline void to_json (nlohmann::json &json, const unpublished_kill_req_t &value)
{
    json = {{"playerId", value.player_id}, {"count", value.count}};
}
inline void from_json (const nlohmann::json &json, unpublished_kill_req_t &value)
{
    json.at ("playerId").get_to (value.player_id);
    json.at ("count").get_to (value.count);
}
inline void to_json (nlohmann::json &json, const unpublished_kill_res_t &value)
{
    json = {{"ok", value.ok}};
}
inline void from_json (const nlohmann::json &json, unpublished_kill_res_t &value)
{
    json.at ("ok").get_to (value.ok);
}
inline void to_json (nlohmann::json &json, const server_assertion_req_t &)
{
    json = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, server_assertion_req_t &) {}
inline void to_json (nlohmann::json &json, const server_assertion_res_t &value)
{
    json = {{"passed", value.passed}, {"evidence", value.evidence}};
}
inline void from_json (const nlohmann::json &json, server_assertion_res_t &value)
{
    json.at ("passed").get_to (value.passed);
    json.at ("evidence").get_to (value.evidence);
}

} // namespace zlink::samples::gamequest
