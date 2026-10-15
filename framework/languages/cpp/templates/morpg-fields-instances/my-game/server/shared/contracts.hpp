#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto field = "Field";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto border = "border";
    static constexpr auto stream = "players";
    static constexpr auto timer = "border-tick";
    static constexpr auto field_prefix = "field-";
    static constexpr int field_width = 100;
    static constexpr int field_count = 4;
    static constexpr std::size_t maximum_party_members = 4;
    static bool valid_id (const std::string &id)
    {
        return id.find_first_not_of (" \t\r\n") != std::string::npos;
    }
    static std::string dungeon_id (const std::string &party) { return "dungeon-" + party; }
    static constexpr int border_radius = 10;
    static constexpr auto tick_period = std::chrono::milliseconds (100);
    static std::string field_id (int x)
    {
        if (x < 0 || x >= field_width * field_count)
            throw std::out_of_range ("Position outside world");
        return field_prefix + std::to_string (x / field_width + 1);
    }
    static std::vector<std::string> adjacent (const std::string &field_id)
    {
        auto n = std::stoi (field_id.substr (std::string (field_prefix).size ()));
        std::vector<std::string> result;
        if (n > 1)
            result.push_back (field_prefix + std::to_string (n - 1));
        if (n < field_count)
            result.push_back (field_prefix + std::to_string (n + 1));
        return result;
    }
    static std::string topic (const std::string &from, const std::string &to)
    {
        return from + ":" + to;
    }
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
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
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    int x = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinWorldRequest, playerId, x)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, x, sequence)
};
struct EnterField
{
    static constexpr auto packet_name = "EnterField";
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterField, x, sequence)
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string fieldId, nodeId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (WorldState, fieldId, nodeId, x, sequence)
};
struct BorderPresence
{
    static constexpr auto packet_name = "BorderPresence";
    std::string sourceFieldId, playerId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (BorderPresence, sourceFieldId, playerId, x, sequence)
};
struct BorderVisible
{
    static constexpr auto packet_name = "BorderVisible";
    std::string observerFieldId, sourceFieldId, playerId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (
      BorderVisible, observerFieldId, sourceFieldId, playerId, x, sequence)
};
struct Rejection
{
    static constexpr auto invalid_party = "INVALID_PARTY";
    static constexpr auto not_leader = "NOT_LEADER";
};
struct CreatePartyRequest
{
    static constexpr auto packet_name = "CreatePartyRequest";
    std::string partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (CreatePartyRequest, partyId)
};
struct JoinPartyRequest
{
    static constexpr auto packet_name = "JoinPartyRequest";
    std::string partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinPartyRequest, partyId)
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
#define EMPTY_PACKET_JSON(Type)                                                                    \
    inline void to_json (nlohmann::json &j, const Type &)                                          \
    {                                                                                              \
        j = nlohmann::json::object ();                                                             \
    }                                                                                              \
    inline void from_json (const nlohmann::json &, Type &)                                         \
    {                                                                                              \
    }
EMPTY_PACKET_JSON (EnterDungeonRequest)
EMPTY_PACKET_JSON (AdvanceRequest)
EMPTY_PACKET_JSON (ViewRequest)
#undef EMPTY_PACKET_JSON
struct PartyView
{
    std::string partyId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyView, partyId, members)
};
struct FieldView
{
    static constexpr auto packet_name = "FieldView";
    std::string fieldId, nodeId;
    std::vector<std::string> players;
    std::vector<PartyView> parties;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldView, fieldId, nodeId, players, parties)
};
struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    int stage = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonView, dungeonId, nodeId, partyId, members, stage)
};
struct PrepareDungeon
{
    static constexpr auto packet_name = "PrepareDungeon";
    std::string partyId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareDungeon, partyId, members)
};
struct DungeonPrepared
{
    static constexpr auto packet_name = "DungeonPrepared";
    std::string dungeonId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonPrepared, dungeonId)
};
struct JoinDungeon
{
    static constexpr auto packet_name = "JoinDungeon";
    std::string dungeonId, partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinDungeon, dungeonId, partyId)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason)
};
}
