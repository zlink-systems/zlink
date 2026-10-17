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
    static constexpr auto town = "Town";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto stream = "players";
    static constexpr int maximum_party_members = 4;
    static std::string town_id (int channel)
    {
        if (channel != 1 && channel != 2)
            throw std::invalid_argument ("Channel must be 1 or 2");
        return "town-" + std::to_string (channel);
    }
    static std::string dungeon_id (const std::string &party) { return "dungeon-" + party; }
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
    static constexpr auto invalid_party = "INVALID_PARTY";
    static constexpr auto not_leader = "NOT_LEADER";
    static constexpr auto join_rejected = "JOIN_REJECTED";
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
    int channel = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinTownRequest, playerId, channel)
};
struct ChangeChannelRequest
{
    static constexpr auto packet_name = "ChangeChannelRequest";
    int channel = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ChangeChannelRequest, channel)
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
struct PartyView
{
    std::string partyId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyView, partyId, members)
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
