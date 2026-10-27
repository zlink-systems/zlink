#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlohmann
{
template <typename T> struct adl_serializer<std::optional<T>>
{
    static void to_json (json &j, const std::optional<T> &value)
    {
        j = value ? json (*value) : json (nullptr);
    }
    static void from_json (const json &j, std::optional<T> &value)
    {
        if (j.is_null ())
            value.reset ();
        else
            value = j.get<T> ();
    }
};
}

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto town = "Town";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto stream = "players";
    static constexpr int maximum_party_members = 4;
    static constexpr std::int64_t channel_count = 2;
    static constexpr std::int64_t maximum_integer = 9'007'199'254'740'991LL;
    static constexpr std::int64_t final_stage = 3;
    static std::string town_id (std::int64_t channel)
    {
        if (channel != 1 && channel != 2)
            throw std::invalid_argument ("Channel must be 1 or 2");
        return "town-" + std::to_string (channel);
    }
    static std::string dungeon_id (const std::string &run_id) { return "dungeon-" + run_id; }
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
struct Control
{
    static constexpr auto ready = "READY ";
    static constexpr auto drain = "drain";
    static constexpr auto shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED";
    static constexpr auto relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN";
    static constexpr auto stopped = "STOPPED";
};
struct Rejection
{
    static constexpr auto invalid_party = "invalid-party";
    static constexpr auto not_leader = "not-leader";
    static constexpr auto join_rejected = "join-rejected";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};
struct EnterDungeonRequest
{
    static constexpr auto packet_name = "EnterDungeonRequest";
};
struct AdvanceRequest
{
    static constexpr auto packet_name = "AdvanceRequest";
};
struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};
inline void to_json (nlohmann::json &j, const SessionInfoRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, SessionInfoRequest &)
{
}
inline void to_json (nlohmann::json &j, const EnterDungeonRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, EnterDungeonRequest &)
{
}
inline void to_json (nlohmann::json &j, const AdvanceRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, AdvanceRequest &)
{
}
inline void to_json (nlohmann::json &j, const ViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, ViewRequest &)
{
}
struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct JoinTownRequest
{
    static constexpr auto packet_name = "JoinTownRequest";
    std::string playerId;
    std::int64_t channel = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinTownRequest, playerId, channel)
};
struct ChangeChannelRequest
{
    static constexpr auto packet_name = "ChangeChannelRequest";
    std::int64_t channel = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ChangeChannelRequest, channel)
};
struct CreatePartyRequest
{
    static constexpr auto packet_name = "CreatePartyRequest";
};
inline void to_json (nlohmann::json &j, const CreatePartyRequest &) { j = nlohmann::json::object (); }
inline void from_json (const nlohmann::json &, CreatePartyRequest &) {}
struct JoinPartyRequest
{
    static constexpr auto packet_name = "JoinPartyRequest";
    std::string partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinPartyRequest, partyId)
};
enum class RunPhase { Forming, Preparing, Entering, Active, Completed };
NLOHMANN_JSON_SERIALIZE_ENUM (RunPhase,
                              {{RunPhase::Forming, "Forming"},
                               {RunPhase::Preparing, "Preparing"},
                               {RunPhase::Entering, "Entering"},
                               {RunPhase::Active, "Active"},
                               {RunPhase::Completed, "Completed"}})
struct PartyView
{
    std::string partyId;
    std::vector<std::string> members;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyView, partyId, members, phase, runId)
};
struct TownView
{
    static constexpr auto packet_name = "TownView";
    std::string townId, nodeId;
    std::vector<std::string> players;
    std::vector<PartyView> parties;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (TownView, townId, nodeId, players, parties)
};
struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    std::int64_t stage = 0;
    std::string runId;
    RunPhase phase = RunPhase::Entering;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonView, dungeonId, nodeId, partyId, members, stage, runId, phase)
};
struct PrepareDungeon
{
    static constexpr auto packet_name = "PrepareDungeon";
    std::string runId, partyId, ownerSpotId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareDungeon, runId, partyId, ownerSpotId, members)
};
struct DungeonPrepared
{
    static constexpr auto packet_name = "DungeonPrepared";
    std::string dungeonId, runId;
    std::optional<std::string> reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonPrepared, dungeonId, runId, reason)
};
struct JoinDungeon
{
    static constexpr auto packet_name = "JoinDungeon";
    std::string dungeonId, runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinDungeon, dungeonId, runId)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason, retryAfterMs)
};
struct RunStarted
{
    static constexpr auto packet_name = "RunStarted";
    std::string runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RunStarted, runId)
};
struct RunCompleted
{
    static constexpr auto packet_name = "RunCompleted";
    std::string runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RunCompleted, runId)
};
struct PartyViewRequest
{
    static constexpr auto packet_name = "PartyViewRequest";
    std::string playerId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyViewRequest, playerId)
};
}
