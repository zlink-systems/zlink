#pragma once
#include <zlink/json.hpp>
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

inline bool insert_sorted_unique (std::vector<std::string> &values, const std::string &value)
{
    const auto position = std::lower_bound (values.begin (), values.end (), value);
    if (position != values.end () && *position == value)
        return false;
    values.insert (position, value);
    return true;
}

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

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
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

struct ReadyRequest
{
    static constexpr auto packet_name = "ReadyRequest";
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

struct JoinLobbyRequest
{
    static constexpr auto packet_name = "JoinLobbyRequest";
    std::string playerId;
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
struct PartyJoin
{
    static constexpr auto packet_name = "PartyJoin";
    std::string partyId;
    bool create = false;
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

struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    std::int64_t stage = 0;
    std::optional<std::string> runId;
    RunPhase phase = RunPhase::Preparing;
};

struct PrepareDungeon
{
    static constexpr auto packet_name = "PrepareDungeon";
    std::string partyId;
    std::vector<std::string> members;
    std::string runId;
};
struct DungeonPrepared
{
    static constexpr auto packet_name = "DungeonPrepared";
    std::string dungeonId, runId;
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
struct DungeonMemberEntered { static constexpr auto packet_name = "DungeonMemberEntered"; std::string runId, playerId;                                                                       };
struct RunStarted { static constexpr auto packet_name = "RunStarted"; std::string runId;                                                   };
struct RunCompleted { static constexpr auto packet_name = "RunCompleted"; std::string runId;                                                     };
struct EnterPartyRun { static constexpr auto packet_name = "EnterPartyRun"; std::string playerId;                                                         };
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

template <> struct dto_traits<::MyGame::JoinLobbyRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinLobbyRequest::playerId)
    };
};

template <> struct dto_traits<::MyGame::JoinPartyRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::JoinPartyRequest::partyId)
    };
};

template <> struct dto_traits<::MyGame::PartyJoin>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::PartyJoin::partyId),
      zlink::json::field ("create", &::MyGame::PartyJoin::create)
    };
};

template <> struct dto_traits<::MyGame::PrepareDungeon>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::PrepareDungeon::partyId),
      zlink::json::field ("members", &::MyGame::PrepareDungeon::members),
      zlink::json::field ("runId", &::MyGame::PrepareDungeon::runId)
    };
};

template <> struct dto_traits<::MyGame::DungeonPrepared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonPrepared::dungeonId),
      zlink::json::field ("runId", &::MyGame::DungeonPrepared::runId)
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

template <> struct dto_traits<::MyGame::DungeonMemberEntered>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("runId", &::MyGame::DungeonMemberEntered::runId),
      zlink::json::field ("playerId", &::MyGame::DungeonMemberEntered::playerId)
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

template <> struct dto_traits<::MyGame::EnterPartyRun>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::EnterPartyRun::playerId)
    };
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::LobbyView>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::ReadyRequest>
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

template <> struct dto_traits<::MyGame::PartyView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::PartyView::partyId),
      zlink::json::field ("members", &::MyGame::PartyView::members),
      zlink::json::field ("ready", &::MyGame::PartyView::ready),
      zlink::json::field ("phase", &::MyGame::PartyView::phase),
      zlink::json::field ("runId", &::MyGame::PartyView::runId)
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
