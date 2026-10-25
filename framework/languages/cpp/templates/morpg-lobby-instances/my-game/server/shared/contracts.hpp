#pragma once
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>

namespace MyGame
{
enum class RunPhase { Forming, Preparing, Entering, Active, Completed };
NLOHMANN_JSON_SERIALIZE_ENUM(RunPhase, {{RunPhase::Forming, "Forming"}, {RunPhase::Preparing, "Preparing"}, {RunPhase::Entering, "Entering"}, {RunPhase::Active, "Active"}, {RunPhase::Completed, "Completed"}})
inline std::string new_id(const std::string &prefix) {
    auto value = boost::uuids::to_string(boost::uuids::random_generator()());
    std::erase(value, '-'); return prefix + value;
}
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto player = "Player";
    static constexpr auto party = "Party";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto stream = "players";
    static constexpr auto party_prefix = "party-";
    static constexpr auto dungeon_prefix = "dungeon-";
    static constexpr std::size_t maximum_members = 4;
    static constexpr std::int64_t maximum_integer = 9007199254740991LL;
    static constexpr std::int64_t final_stage = 3;
    static std::string dungeon_id(const std::string &run) { return dungeon_prefix + run; }
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
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};
inline void to_json (nlohmann::json &j, const SessionInfoRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, SessionInfoRequest &)
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
struct Rejection
{
    static constexpr auto invalid_party = "invalid-party";
    static constexpr auto not_leader = "not-leader";
    static constexpr auto not_ready = "not-ready";
    static constexpr auto join_rejected = "join-rejected";
    static constexpr auto run_mismatch = "run-mismatch";
    static constexpr auto run_not_active = "run-not-active";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
};
inline bool nonblank (const std::string &value)
{
    return value.find_first_not_of (" \t\r\n") != std::string::npos;
}
struct LobbyView
{
    static constexpr auto packet_name = "LobbyView";
};
inline void to_json (nlohmann::json &j, const LobbyView &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, LobbyView &)
{
}
struct ReadyRequest
{
    static constexpr auto packet_name = "ReadyRequest";
};
inline void to_json (nlohmann::json &j, const ReadyRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, ReadyRequest &)
{
}
struct EnterDungeonRequest
{
    static constexpr auto packet_name = "EnterDungeonRequest";
};
inline void to_json (nlohmann::json &j, const EnterDungeonRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, EnterDungeonRequest &)
{
}
struct AdvanceRequest
{
    static constexpr auto packet_name = "AdvanceRequest";
};
inline void to_json (nlohmann::json &j, const AdvanceRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, AdvanceRequest &)
{
}
struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};
inline void to_json (nlohmann::json &j, const ViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, ViewRequest &)
{
}
struct JoinLobbyRequest
{
    static constexpr auto packet_name = "JoinLobbyRequest";
    std::string playerId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinLobbyRequest, playerId)
};
struct CreatePartyRequest
{
    static constexpr auto packet_name = "CreatePartyRequest";
};
inline void to_json(nlohmann::json &j, const CreatePartyRequest&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, CreatePartyRequest&) {}
struct JoinPartyRequest
{
    static constexpr auto packet_name = "JoinPartyRequest";
    std::string partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinPartyRequest, partyId)
};
struct PartyJoin
{
    static constexpr auto packet_name = "PartyJoin";
    std::string partyId;
    bool create = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyJoin, partyId, create)
};
struct PartyView
{
    static constexpr auto packet_name = "PartyView";
    std::string partyId;
    std::vector<std::string> members;
    std::vector<bool> ready;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
};
inline void to_json(nlohmann::json &j, const PartyView& v) {
    j["partyId"] = v.partyId;
    j["members"] = v.members;
    j["ready"] = v.ready;
    j["phase"] = v.phase;
    j["runId"] = v.runId ? nlohmann::json(*v.runId) : nlohmann::json(nullptr);
}
inline void from_json(const nlohmann::json &j, PartyView& v) {
    j.at("partyId").get_to(v.partyId);
    j.at("members").get_to(v.members);
    j.at("ready").get_to(v.ready);
    j.at("phase").get_to(v.phase);
    if (j.at("runId").is_null()) v.runId.reset(); else v.runId = j.at("runId").get<std::string>();
}

struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    std::int64_t stage = 0;
    std::optional<std::string> runId;
    RunPhase phase = RunPhase::Preparing;
};
inline void to_json(nlohmann::json &j, const DungeonView& v) {
    j["dungeonId"] = v.dungeonId;
    j["nodeId"] = v.nodeId;
    j["partyId"] = v.partyId;
    j["members"] = v.members;
    j["stage"] = v.stage;
    j["runId"] = v.runId ? nlohmann::json(*v.runId) : nlohmann::json(nullptr);
    j["phase"] = v.phase;
}
inline void from_json(const nlohmann::json &j, DungeonView& v) {
    j.at("dungeonId").get_to(v.dungeonId);
    j.at("nodeId").get_to(v.nodeId);
    j.at("partyId").get_to(v.partyId);
    j.at("members").get_to(v.members);
    j.at("stage").get_to(v.stage);
    if (j.at("runId").is_null()) v.runId.reset(); else v.runId = j.at("runId").get<std::string>();
    j.at("phase").get_to(v.phase);
}

struct PrepareDungeon
{
    static constexpr auto packet_name = "PrepareDungeon";
    std::string partyId;
    std::vector<std::string> members;
    std::string runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareDungeon, partyId, members, runId)
};
struct DungeonPrepared
{
    static constexpr auto packet_name = "DungeonPrepared";
    std::string dungeonId, runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonPrepared, dungeonId, runId)
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
struct DungeonMemberEntered { static constexpr auto packet_name = "DungeonMemberEntered"; std::string runId, playerId; NLOHMANN_DEFINE_TYPE_INTRUSIVE(DungeonMemberEntered, runId, playerId) };
struct RunStarted { static constexpr auto packet_name = "RunStarted"; std::string runId; NLOHMANN_DEFINE_TYPE_INTRUSIVE(RunStarted, runId) };
struct RunCompleted { static constexpr auto packet_name = "RunCompleted"; std::string runId; NLOHMANN_DEFINE_TYPE_INTRUSIVE(RunCompleted, runId) };
struct EnterPartyRun { static constexpr auto packet_name = "EnterPartyRun"; std::string playerId; NLOHMANN_DEFINE_TYPE_INTRUSIVE(EnterPartyRun, playerId) };
}
