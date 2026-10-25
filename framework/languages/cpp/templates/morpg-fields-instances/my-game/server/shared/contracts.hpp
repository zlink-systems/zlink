#pragma once
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

namespace nlohmann
{
template <class T> struct adl_serializer<std::optional<T>>
{
    static void to_json (json &value, const std::optional<T> &item)
    {
        value = item ? json (*item) : json (nullptr);
    }
    static void from_json (const json &value, std::optional<T> &item)
    {
        if (value.is_null ())
            item.reset ();
        else
            item = value.template get<T> ();
    }
};
}

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
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason, retryAfterMs)
};
enum class RunPhase
{
    Forming,
    Preparing,
    Entering,
    Active,
    Completed
};
NLOHMANN_JSON_SERIALIZE_ENUM (RunPhase,
                              {{RunPhase::Forming, "Forming"},
                               {RunPhase::Preparing, "Preparing"},
                               {RunPhase::Entering, "Entering"},
                               {RunPhase::Active, "Active"},
                               {RunPhase::Completed, "Completed"}})

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
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    double x = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinWorldRequest, playerId, x)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double delta = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, delta, sequence)
};
struct EnterField
{
    double x = 0;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterField, x, sequence, remainingMs)
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string fieldId, nodeId;
    double x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (WorldState, fieldId, nodeId, x, sequence)
};
struct BorderPresence
{
    static constexpr auto packet_name = "BorderPresence";
    std::string sourceFieldId, playerId;
    double x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (BorderPresence, sourceFieldId, playerId, x, sequence)
};
struct BorderVisible
{
    static constexpr auto packet_name = "BorderVisible";
    std::string observerFieldId, sourceFieldId, playerId;
    double x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (
      BorderVisible, observerFieldId, sourceFieldId, playerId, x, sequence)
};
struct CreatePartyRequest
{
    static constexpr auto packet_name = "CreatePartyRequest";
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
EMPTY_PACKET_JSON (CreatePartyRequest)
EMPTY_PACKET_JSON (EnterDungeonRequest)
EMPTY_PACKET_JSON (AdvanceRequest)
EMPTY_PACKET_JSON (ViewRequest)
EMPTY_PACKET_JSON (SessionInfoRequest)
#undef EMPTY_PACKET_JSON
struct PartyView
{
    static constexpr auto packet_name = "PartyView";
    std::string partyId;
    std::vector<std::string> members;
    std::string leaderId;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyView, partyId, members, leaderId, phase, runId)
};
struct PartyCommand
{
    std::string actorId;
    bool join = false;
    bool enter = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyCommand, actorId, join, enter)
};
struct PartyCommandResult
{
    std::optional<PartyView> view;
    std::optional<ActionRejected> rejection;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyCommandResult, view, rejection)
};
struct DungeonMemberEntered
{
    std::string runId, actorId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonMemberEntered, runId, actorId)
};
struct RunStarted
{
    std::string runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RunStarted, runId)
};
struct RunCompleted
{
    std::string runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RunCompleted, runId)
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
    std::string runId;
    RunPhase phase = RunPhase::Preparing;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonView, dungeonId, nodeId, partyId, members, stage, runId, phase)
};
struct PrepareDungeon
{
    std::string runId, partyId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareDungeon, runId, partyId, members)
};
struct DungeonPrepared
{
    std::string dungeonId, runId;
    std::optional<ActionRejected> rejection;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonPrepared, dungeonId, runId, rejection)
};
struct JoinDungeon
{
    std::string dungeonId, runId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinDungeon, dungeonId, runId)
};
}
