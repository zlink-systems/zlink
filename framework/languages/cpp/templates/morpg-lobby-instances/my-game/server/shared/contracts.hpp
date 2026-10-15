#pragma once
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
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
struct Rejection
{
    static constexpr auto invalid_party = "invalid-party";
    static constexpr auto not_leader = "not-leader";
    static constexpr auto not_ready = "not-ready";
    static constexpr auto join_rejected = "join-rejected";
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
    std::string partyId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (CreatePartyRequest, partyId)
};
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
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyView, partyId, members, ready)
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
