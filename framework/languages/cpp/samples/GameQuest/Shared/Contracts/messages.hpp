/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <nlohmann/json.hpp>
#include <zlink/json_profile.hpp>

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

}

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::kill_monster_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::kill_monster_req_t::player_id),
      zlink::json::field ("monsterId", &zlink::samples::gamequest::kill_monster_req_t::monster_id),
      zlink::json::field ("areaId", &zlink::samples::gamequest::kill_monster_req_t::area_id),
      zlink::json::field ("idempotencyKey",
                          &zlink::samples::gamequest::kill_monster_req_t::idempotency_key)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::kill_monster_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("eventId", &zlink::samples::gamequest::kill_monster_res_t::event_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::collect_item_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::collect_item_req_t::player_id),
      zlink::json::field ("itemId", &zlink::samples::gamequest::collect_item_req_t::item_id),
      zlink::json::field ("count", &zlink::samples::gamequest::collect_item_req_t::count),
      zlink::json::field ("idempotencyKey",
                          &zlink::samples::gamequest::collect_item_req_t::idempotency_key)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::enter_area_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::enter_area_req_t::player_id),
      zlink::json::field ("areaId", &zlink::samples::gamequest::enter_area_req_t::area_id),
      zlink::json::field ("idempotencyKey",
                          &zlink::samples::gamequest::enter_area_req_t::idempotency_key)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::join_session_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::join_session_req_t::player_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::stored_quest_event_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("eventId", &zlink::samples::gamequest::stored_quest_event_t::event_id),
      zlink::json::field ("playerId", &zlink::samples::gamequest::stored_quest_event_t::player_id),
      zlink::json::field ("questId", &zlink::samples::gamequest::stored_quest_event_t::quest_id),
      zlink::json::field ("type", &zlink::samples::gamequest::stored_quest_event_t::type),
      zlink::json::field ("payload", &zlink::samples::gamequest::stored_quest_event_t::payload),
      zlink::json::field ("version", &zlink::samples::gamequest::stored_quest_event_t::version),
      zlink::json::field ("createdAtUnixMs",
                          &zlink::samples::gamequest::stored_quest_event_t::created_at_unix_ms),
      zlink::json::field ("sourceEventId",
                          &zlink::samples::gamequest::stored_quest_event_t::source_event_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::quest_progress_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::quest_progress_t::player_id),
      zlink::json::field ("questId", &zlink::samples::gamequest::quest_progress_t::quest_id),
      zlink::json::field ("status", &zlink::samples::gamequest::quest_progress_t::status),
      zlink::json::field ("currentCount",
                          &zlink::samples::gamequest::quest_progress_t::current_count),
      zlink::json::field ("requiredCount",
                          &zlink::samples::gamequest::quest_progress_t::required_count),
      zlink::json::field ("version", &zlink::samples::gamequest::quest_progress_t::version),
      zlink::json::field ("updatedAtUnixMs",
                          &zlink::samples::gamequest::quest_progress_t::updated_at_unix_ms),
      zlink::json::field ("lastSourceEventId",
                          &zlink::samples::gamequest::quest_progress_t::last_source_event_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::join_session_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &zlink::samples::gamequest::join_session_res_t::player_id),
      zlink::json::field ("activeQuests",
                          &zlink::samples::gamequest::join_session_res_t::active_quests)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::get_quest_progress_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "playerId", &zlink::samples::gamequest::get_quest_progress_req_t::player_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::get_quest_progress_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "activeQuests", &zlink::samples::gamequest::get_quest_progress_res_t::active_quests)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::sync_quest_progress_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "playerId", &zlink::samples::gamequest::sync_quest_progress_req_t::player_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::gamequest::sync_quest_progress_owner_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::sync_quest_progress_owner_req_t::player_id),
      zlink::json::field (
        "snapshotKillCount",
        &zlink::samples::gamequest::sync_quest_progress_owner_req_t::snapshot_kill_count)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::sync_quest_progress_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "updatedQuests", &zlink::samples::gamequest::sync_quest_progress_res_t::updated_quests)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::quest_progress_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::quest_progress_notify_t::player_id),
      zlink::json::field ("progress",
                          &zlink::samples::gamequest::quest_progress_notify_t::progress)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::quest_completed_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::quest_completed_notify_t::player_id),
      zlink::json::field ("progress",
                          &zlink::samples::gamequest::quest_completed_notify_t::progress),
      zlink::json::field ("rewardGranted",
                          &zlink::samples::gamequest::quest_completed_notify_t::reward_granted)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::gameplay_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("eventId", &zlink::samples::gamequest::gameplay_msg_t::event_id),
      zlink::json::field ("playerId", &zlink::samples::gamequest::gameplay_msg_t::player_id),
      zlink::json::field ("type", &zlink::samples::gamequest::gameplay_msg_t::type),
      zlink::json::field ("payload", &zlink::samples::gamequest::gameplay_msg_t::payload),
      zlink::json::field ("occurredAtUnixMs",
                          &zlink::samples::gamequest::gameplay_msg_t::occurred_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::close_player_quest_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &zlink::samples::gamequest::close_player_quest_msg_t::reason)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::notify_quest_progress_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::notify_quest_progress_msg_t::player_id),
      zlink::json::field ("projection",
                          &zlink::samples::gamequest::notify_quest_progress_msg_t::projection),
      zlink::json::field (
        "completedQuestId",
        &zlink::samples::gamequest::notify_quest_progress_msg_t::completed_quest_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::projection_admin_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::projection_admin_req_t::player_id),
      zlink::json::field ("questId", &zlink::samples::gamequest::projection_admin_req_t::quest_id),
      zlink::json::field ("operation",
                          &zlink::samples::gamequest::projection_admin_req_t::operation)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::projection_admin_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("ok", &zlink::samples::gamequest::projection_admin_res_t::ok),
      zlink::json::field ("projection",
                          &zlink::samples::gamequest::projection_admin_res_t::projection)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::unpublished_kill_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId",
                          &zlink::samples::gamequest::unpublished_kill_req_t::player_id),
      zlink::json::field ("count", &zlink::samples::gamequest::unpublished_kill_req_t::count)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::unpublished_kill_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("ok", &zlink::samples::gamequest::unpublished_kill_res_t::ok)};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::server_assertion_req_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<zlink::samples::gamequest::server_assertion_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("passed", &zlink::samples::gamequest::server_assertion_res_t::passed),
      zlink::json::field ("evidence",
                          &zlink::samples::gamequest::server_assertion_res_t::evidence)};
};

namespace zlink::samples::gamequest
{

} // namespace zlink::samples::gamequest

// Explicit JSON state-store conversion shares the typed messaging profile.
namespace zlink::samples::gamequest
{
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void to_json (nlohmann::json &out, const T &value)
{
    out = zlink::detail::json_profile::encode_value (value);
}
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void from_json (const nlohmann::json &in, T &value)
{
    value = zlink::detail::json_profile::decode_value<T> (in);
}
}
