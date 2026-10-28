#pragma once
#include <zlink/json.hpp>
#include <optional>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto field = "Field";
    static constexpr auto party = "Party";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto border = "border";
    static constexpr auto stream = "players";
    static constexpr auto timer = "border-tick";
    static constexpr auto field_prefix = "field-";
    static constexpr int field_width = 100;
    static constexpr int field_count = 4;
    static constexpr int final_stage = 3;
    static constexpr std::int64_t max_safe_integer = 9'007'199'254'740'991LL;
    static constexpr std::size_t maximum_party_members = 4;
    static constexpr int border_radius = 10;
    static constexpr auto tick_period = std::chrono::milliseconds (100);

    static bool valid_id (const std::string &id)
    {
        return id.find_first_not_of (" \t\r\n") != std::string::npos;
    }
    static std::string new_id (std::string_view prefix)
    {
        std::random_device random;
        std::array<unsigned char, 16> bytes{};
        for (auto &byte : bytes)
            byte = static_cast<unsigned char> (random () & 0xffU);
        std::ostringstream value;
        value << std::hex << std::setfill ('0');
        for (const auto byte : bytes)
            value << std::setw (2) << static_cast<unsigned int> (byte);
        return std::string (prefix) + value.str ();
    }
    static std::string dungeon_id (const std::string &run) { return "dungeon-" + run; }
    static std::string field_id (double x)
    {
        if (!std::isfinite (x) || x < 0 || x >= field_width * field_count)
            throw std::out_of_range ("Position outside world");
        return field_prefix + std::to_string (static_cast<int> (x / field_width) + 1);
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
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto safe_shutdown = message_follow + relocation;
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
    static constexpr auto run_mismatch = "run-mismatch";
    static constexpr auto stale_sequence = "stale-sequence";
    static constexpr auto non_finite = "non-finite";
    static constexpr auto outside_world = "outside-world";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
    static constexpr auto step_too_large = "step-too-large";
    static constexpr auto move_budget = "move-budget";
    static constexpr auto run_not_active = "run-not-active";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
    static constexpr auto run_in_progress = "run-in-progress";
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
};
enum class RunPhase
{
    Forming,
    Preparing,
    Entering,
    Active,
    Completed
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
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    double x = 0;
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double delta = 0;
    std::int64_t sequence = 0;
};
struct EnterField
{
    double x = 0;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string fieldId, nodeId;
    double x = 0;
    std::int64_t sequence = 0;
};
struct BorderPresence
{
    static constexpr auto packet_name = "BorderPresence";
    std::string sourceFieldId, playerId;
    double x = 0;
    std::int64_t sequence = 0;
};
struct BorderVisible
{
    static constexpr auto packet_name = "BorderVisible";
    std::string observerFieldId, sourceFieldId, playerId;
    double x = 0;
    std::int64_t sequence = 0;

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
#undef EMPTY_PACKET_JSON
struct PartyView
{
    static constexpr auto packet_name = "PartyView";
    std::string partyId;
    std::vector<std::string> members;
    std::string leaderId;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
};
struct PartyCommand
{
    std::string actorId;
    bool join = false;
    bool enter = false;
};
struct PartyCommandResult
{
    std::optional<PartyView> view;
    std::optional<ActionRejected> rejection;
};
struct DungeonMemberEntered
{
    std::string runId, actorId;
};
struct RunStarted
{
    std::string runId;
};
struct RunCompleted
{
    std::string runId;
};
struct DungeonView
{
    static constexpr auto packet_name = "DungeonView";
    std::string dungeonId, nodeId, partyId;
    std::vector<std::string> members;
    int stage = 0;
    std::string runId;
    RunPhase phase = RunPhase::Preparing;
};
struct PrepareDungeon
{
    std::string runId, partyId;
    std::vector<std::string> members;
};
struct DungeonPrepared
{
    std::string dungeonId, runId;
    std::optional<ActionRejected> rejection;
};
struct JoinDungeon
{
    std::string dungeonId, runId;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::JoinWorldRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinWorldRequest::playerId),
      zlink::json::field ("x", &::MyGame::JoinWorldRequest::x)
    };
};

template <> struct dto_traits<::MyGame::MoveRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("delta", &::MyGame::MoveRequest::delta),
      zlink::json::field ("sequence", &::MyGame::MoveRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::EnterField>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("x", &::MyGame::EnterField::x),
      zlink::json::field ("sequence", &::MyGame::EnterField::sequence),
      zlink::json::field ("remainingMs", &::MyGame::EnterField::remainingMs)
    };
};

template <> struct dto_traits<::MyGame::WorldState>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("fieldId", &::MyGame::WorldState::fieldId),
      zlink::json::field ("nodeId", &::MyGame::WorldState::nodeId),
      zlink::json::field ("x", &::MyGame::WorldState::x),
      zlink::json::field ("sequence", &::MyGame::WorldState::sequence)
    };
};

template <> struct dto_traits<::MyGame::BorderPresence>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sourceFieldId", &::MyGame::BorderPresence::sourceFieldId),
      zlink::json::field ("playerId", &::MyGame::BorderPresence::playerId),
      zlink::json::field ("x", &::MyGame::BorderPresence::x),
      zlink::json::field ("sequence", &::MyGame::BorderPresence::sequence)
    };
};

template <> struct dto_traits<::MyGame::BorderVisible>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("observerFieldId", &::MyGame::BorderVisible::observerFieldId),
      zlink::json::field ("sourceFieldId", &::MyGame::BorderVisible::sourceFieldId),
      zlink::json::field ("playerId", &::MyGame::BorderVisible::playerId),
      zlink::json::field ("x", &::MyGame::BorderVisible::x),
      zlink::json::field ("sequence", &::MyGame::BorderVisible::sequence)
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
      zlink::json::field ("leaderId", &::MyGame::PartyView::leaderId),
      zlink::json::field ("phase", &::MyGame::PartyView::phase),
      zlink::json::field ("runId", &::MyGame::PartyView::runId)
    };
};

template <> struct dto_traits<::MyGame::PartyCommand>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("actorId", &::MyGame::PartyCommand::actorId),
      zlink::json::field ("join", &::MyGame::PartyCommand::join),
      zlink::json::field ("enter", &::MyGame::PartyCommand::enter)
    };
};

template <> struct dto_traits<::MyGame::PartyCommandResult>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("view", &::MyGame::PartyCommandResult::view),
      zlink::json::field ("rejection", &::MyGame::PartyCommandResult::rejection)
    };
};

template <> struct dto_traits<::MyGame::DungeonMemberEntered>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("runId", &::MyGame::DungeonMemberEntered::runId),
      zlink::json::field ("actorId", &::MyGame::DungeonMemberEntered::actorId)
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
      zlink::json::field ("members", &::MyGame::PrepareDungeon::members)
    };
};

template <> struct dto_traits<::MyGame::DungeonPrepared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonPrepared::dungeonId),
      zlink::json::field ("runId", &::MyGame::DungeonPrepared::runId),
      zlink::json::field ("rejection", &::MyGame::DungeonPrepared::rejection)
    };
};

template <> struct dto_traits<::MyGame::JoinDungeon>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::JoinDungeon::dungeonId),
      zlink::json::field ("runId", &::MyGame::JoinDungeon::runId)
    };
};

template <> struct dto_traits<::MyGame::CreatePartyRequest>
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

template <> struct dto_traits<::MyGame::SessionInfoRequest>
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
