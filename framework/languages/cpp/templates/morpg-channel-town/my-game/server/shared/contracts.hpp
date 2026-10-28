#pragma once
#include <zlink/json.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
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

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
};
struct JoinTownRequest
{
    static constexpr auto packet_name = "JoinTownRequest";
    std::string playerId;
    std::int64_t channel = 0;
};
struct ChangeChannelRequest
{
    static constexpr auto packet_name = "ChangeChannelRequest";
    std::int64_t channel = 0;
};
struct CreatePartyRequest
{
    static constexpr auto packet_name = "CreatePartyRequest";
};

struct JoinPartyRequest
{
    static constexpr auto packet_name = "JoinPartyRequest";
    std::string partyId;
};
enum class RunPhase { Forming, Preparing, Entering, Active, Completed };

struct PartyView
{
    std::string partyId;
    std::vector<std::string> members;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
};
struct TownView
{
    static constexpr auto packet_name = "TownView";
    std::string townId, nodeId;
    std::vector<std::string> players;
    std::vector<PartyView> parties;
};
struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    std::int64_t stage = 0;
    std::string runId;
    RunPhase phase = RunPhase::Entering;
};
struct PrepareDungeon
{
    static constexpr auto packet_name = "PrepareDungeon";
    std::string runId, partyId, ownerSpotId;
    std::vector<std::string> members;
};
struct DungeonPrepared
{
    static constexpr auto packet_name = "DungeonPrepared";
    std::string dungeonId, runId;
    std::optional<std::string> reason;
};
struct JoinDungeon
{
    static constexpr auto packet_name = "JoinDungeon";
    std::string dungeonId, runId;
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
};
struct RunStarted
{
    static constexpr auto packet_name = "RunStarted";
    std::string runId;
};
struct RunCompleted
{
    static constexpr auto packet_name = "RunCompleted";
    std::string runId;
};
struct PartyViewRequest
{
    static constexpr auto packet_name = "PartyViewRequest";
    std::string playerId;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::JoinTownRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinTownRequest::playerId),
      zlink::json::field ("channel", &::MyGame::JoinTownRequest::channel)
    };
};

template <> struct dto_traits<::MyGame::ChangeChannelRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("channel", &::MyGame::ChangeChannelRequest::channel)
    };
};

template <> struct dto_traits<::MyGame::JoinPartyRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::JoinPartyRequest::partyId)
    };
};

template <> struct dto_traits<::MyGame::PartyView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::PartyView::partyId),
      zlink::json::field ("members", &::MyGame::PartyView::members),
      zlink::json::field ("phase", &::MyGame::PartyView::phase),
      zlink::json::field ("runId", &::MyGame::PartyView::runId)
    };
};

template <> struct dto_traits<::MyGame::TownView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("townId", &::MyGame::TownView::townId),
      zlink::json::field ("nodeId", &::MyGame::TownView::nodeId),
      zlink::json::field ("players", &::MyGame::TownView::players),
      zlink::json::field ("parties", &::MyGame::TownView::parties)
    };
};

template <> struct dto_traits<::MyGame::DungeonView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonView::dungeonId),
      zlink::json::field ("nodeId", &::MyGame::DungeonView::nodeId),
      zlink::json::field ("partyId", &::MyGame::DungeonView::partyId),
      zlink::json::field ("members", &::MyGame::DungeonView::members),
      zlink::json::field ("stage", &::MyGame::DungeonView::stage),
      zlink::json::field ("runId", &::MyGame::DungeonView::runId),
      zlink::json::field ("phase", &::MyGame::DungeonView::phase)
    };
};

template <> struct dto_traits<::MyGame::PrepareDungeon>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("runId", &::MyGame::PrepareDungeon::runId),
      zlink::json::field ("partyId", &::MyGame::PrepareDungeon::partyId),
      zlink::json::field ("ownerSpotId", &::MyGame::PrepareDungeon::ownerSpotId),
      zlink::json::field ("members", &::MyGame::PrepareDungeon::members)
    };
};

template <> struct dto_traits<::MyGame::DungeonPrepared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonPrepared::dungeonId),
      zlink::json::field ("runId", &::MyGame::DungeonPrepared::runId),
      zlink::json::field ("reason", &::MyGame::DungeonPrepared::reason)
    };
};

template <> struct dto_traits<::MyGame::JoinDungeon>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::JoinDungeon::dungeonId),
      zlink::json::field ("runId", &::MyGame::JoinDungeon::runId)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::RunStarted>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("runId", &::MyGame::RunStarted::runId)
    };
};

template <> struct dto_traits<::MyGame::RunCompleted>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("runId", &::MyGame::RunCompleted::runId)
    };
};

template <> struct dto_traits<::MyGame::PartyViewRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PartyViewRequest::playerId)
    };
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::EnterDungeonRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::AdvanceRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::ViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::CreatePartyRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct enum_traits<::MyGame::RunPhase>
{
    static constexpr auto values = std::array{
      zlink::json::enum_value (::MyGame::RunPhase::Forming, "Forming"),
      zlink::json::enum_value (::MyGame::RunPhase::Preparing, "Preparing"),
      zlink::json::enum_value (::MyGame::RunPhase::Entering, "Entering"),
      zlink::json::enum_value (::MyGame::RunPhase::Active, "Active"),
      zlink::json::enum_value (::MyGame::RunPhase::Completed, "Completed")
    };
};
} // namespace zlink::json
